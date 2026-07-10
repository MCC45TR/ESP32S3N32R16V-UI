#include "spi_c3_master.h"
#include "src/config/pin_config.h"
#include "src/platform/mros_gpio.h"
#include "src/platform/mros_time.h"
#include "src/drivers/utils/mros_console.h"
#include <esp_err.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <stddef.h>
#include <string.h>

// Packet definition (must match C3 side)
#define C3S3_MARKER 0xCCAA55EEu
#define S3C3_CMD_MARKER 0x5333434Du
#define C3_SPI_FRAME_SIZE 16
// Run SPI at higher speed for PID feedback; keep wiring short and grounded.
#define C3_SPI_CLOCK_HZ 1000000
#define C3_SPI_PERIOD_FAST_MS 2
#define C3_SPI_PERIOD_MISSING_MS 100
#define C3_SPI_BAD_BURST_MIN 3

#pragma pack(push, 1)
struct C3toS3_Packet {
  uint32_t marker;
  int32_t position_x100;
  int16_t speed_x10;
  int16_t accel_x10;
  uint8_t status_flags;
  uint8_t loop_hz;
  uint16_t crc16;
};

struct S3toC3_Command {
  uint32_t marker;
  uint8_t reset_turret;
  int8_t failsafe_option;
  uint8_t cmd_flags;
  uint8_t reserved0;
  int32_t pid_setpoint_x100;
  uint16_t reserved1;
  uint16_t crc16;
};
#pragma pack(pop)
static_assert(sizeof(C3toS3_Packet) == C3_SPI_FRAME_SIZE,
              "C3toS3_Packet must be 16 bytes");
static_assert(offsetof(C3toS3_Packet, crc16) == 14,
              "C3toS3_Packet.crc16 must be the last field");
static_assert(sizeof(S3toC3_Command) == C3_SPI_FRAME_SIZE,
              "S3toC3_Command must be 16 bytes");
static_assert(offsetof(S3toC3_Command, crc16) == 14,
              "S3toC3_Command.crc16 must be the last field");

// SPI handle
static spi_device_handle_t spi_handle = NULL;
static spi_transaction_t spi_trans = {};
static bool trans_queued = false;

// DMA aligned buffers
WORD_ALIGNED_ATTR static uint8_t dma_rx_buf[C3_SPI_FRAME_SIZE];
WORD_ALIGNED_ATTR static uint8_t dma_tx_buf[C3_SPI_FRAME_SIZE]; // Dummy TX

// Decoded values (float, converted on S3 which has FPU)
static volatile float c3_position_deg = 0.0f;
static volatile float c3_speed_deg_s = 0.0f;
static volatile float c3_accel_deg_s2 = 0.0f;
static volatile uint8_t c3_loop_hz = 0;

// Diagnostics
static volatile uint32_t crc_error_count = 0;
static volatile uint32_t marker_error_count = 0;
static volatile uint32_t total_rx_count = 0;
static volatile uint32_t transaction_queued_count = 0;
static volatile uint32_t transaction_completed_count = 0;
static volatile uint32_t transaction_timeout_count = 0;
static volatile uint32_t transaction_fail_count = 0;
static volatile uint32_t effective_period_ms = C3_SPI_PERIOD_FAST_MS;
static volatile unsigned long last_good_rx_ms = 0;
static unsigned long last_queue_ms = 0;
static bool link_synced = false;
static uint32_t good_packet_count = 0;
static uint8_t consecutive_bad_frames = 0;
static int last_alive_level = -1;
static unsigned long last_alive_edge_ms = 0;
static volatile bool pending_encoder_reset_cmd = false;
static volatile int8_t failsafe_option_cmd = C3_FAILSAFE_T41_QSPI;
static volatile uint8_t cmd_flags_cmd = C3_CMD_FLAG_NONE;
static volatile int32_t pid_setpoint_x100_cmd = 0;
static volatile bool c3_espnow_active = false;
static volatile bool c3_espnow_connected = false;
static volatile bool c3_espnow_recent_cmd = false;
static TaskHandle_t g_spi_c3_task_handle = nullptr;

static inline void increment_counter(volatile uint32_t* counter) {
  if (counter == nullptr) {
    return;
  }
  *counter = *counter + 1U;
}

// CRC16-CCITT (must match C3)
static uint16_t calc_crc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int j = 0; j < 8; j++) {
      if (crc & 0x8000)
        crc = (crc << 1) ^ 0x1021;
      else
        crc <<= 1;
    }
  }
  return crc;
}

static bool alive_heartbeat_ok(unsigned long now_ms) {
  int level = mros::platform::mros_gpio_read(PIN_C3_ALIVE);
  if (last_alive_level < 0) {
    last_alive_level = level;
    last_alive_edge_ms = now_ms;
  } else if (level != last_alive_level) {
    last_alive_level = level;
    last_alive_edge_ms = now_ms;
  }
  return (now_ms - last_alive_edge_ms) < 1500;
}

static uint32_t select_c3_spi_period_ms(bool alive_ok) {
  if (pending_encoder_reset_cmd || cmd_flags_cmd != C3_CMD_FLAG_NONE ||
      pid_setpoint_x100_cmd != 0) {
    return C3_SPI_PERIOD_FAST_MS;
  }
  return alive_ok ? C3_SPI_PERIOD_FAST_MS : C3_SPI_PERIOD_MISSING_MS;
}

static void parse_rx_packet(unsigned long now_ms, bool alive_ok) {
  if (!alive_ok) {
    link_synced = false;
    good_packet_count = 0;
    consecutive_bad_frames = 0;
    return;
  }

  increment_counter(&total_rx_count);

  C3toS3_Packet pkt = {};
  memcpy(&pkt, dma_rx_buf, sizeof(pkt));

  bool all_zero = true;
  bool all_ff = true;
  for (int i = 0; i < C3_SPI_FRAME_SIZE; i++) {
    if (dma_rx_buf[i] != 0x00)
      all_zero = false;
    if (dma_rx_buf[i] != 0xFF)
      all_ff = false;
  }
  if (all_zero || all_ff) {
    // Ignore line-idle garbage (floating or un-driven bus sample).
    if (link_synced && consecutive_bad_frames < 255)
      consecutive_bad_frames++;
    return;
  }

  if (pkt.marker != C3S3_MARKER) {
    if (link_synced && consecutive_bad_frames < 255)
      consecutive_bad_frames++;
    if (link_synced && consecutive_bad_frames >= C3_SPI_BAD_BURST_MIN)
      increment_counter(&marker_error_count);
    return;
  }

  uint16_t calc = calc_crc16(reinterpret_cast<const uint8_t *>(&pkt),
                            sizeof(pkt) - sizeof(pkt.crc16));
  if (calc != pkt.crc16) {
    if (link_synced && consecutive_bad_frames < 255)
      consecutive_bad_frames++;
    if (link_synced && consecutive_bad_frames >= C3_SPI_BAD_BURST_MIN)
      increment_counter(&crc_error_count);
    return;
  }

  consecutive_bad_frames = 0;
  // Keep C3 values as raw encoder values.
  c3_position_deg = pkt.position_x100 / 100.0f;
  c3_speed_deg_s = pkt.speed_x10 / 10.0f;
  c3_accel_deg_s2 = pkt.accel_x10 / 10.0f;
  c3_loop_hz = pkt.loop_hz;
  c3_espnow_active = (pkt.status_flags & C3_STATUS_ESPNOW_ACTIVE) != 0U;
  c3_espnow_connected = (pkt.status_flags & C3_STATUS_ESPNOW_CONNECTED) != 0U;
  c3_espnow_recent_cmd = (pkt.status_flags & C3_STATUS_ESPNOW_RECENT_CMD) != 0U;
  last_good_rx_ms = now_ms;

  if (!link_synced) {
    good_packet_count++;
    if (good_packet_count >= 5) {
      // After stable lock, zero counters so UI reflects real ongoing errors.
      link_synced = true;
      marker_error_count = 0;
      crc_error_count = 0;
      consecutive_bad_frames = 0;
    }
  }
}

static bool queue_transaction(unsigned long now_ms) {
  if (!spi_handle || trans_queued)
    return false;

  memset(&spi_trans, 0, sizeof(spi_trans));
  S3toC3_Command tx_cmd = {};
  tx_cmd.marker = S3C3_CMD_MARKER;

  bool send_reset_cmd = pending_encoder_reset_cmd;
  if (send_reset_cmd) {
    tx_cmd.reset_turret = 0xA5;
  }
  tx_cmd.failsafe_option = failsafe_option_cmd;
  tx_cmd.cmd_flags = cmd_flags_cmd;
  tx_cmd.pid_setpoint_x100 = pid_setpoint_x100_cmd;
  tx_cmd.crc16 = calc_crc16(reinterpret_cast<const uint8_t *>(&tx_cmd),
                            sizeof(tx_cmd) - sizeof(tx_cmd.crc16));

  memcpy(dma_tx_buf, &tx_cmd, sizeof(tx_cmd));
  spi_trans.length = C3_SPI_FRAME_SIZE * 8; // bits
  spi_trans.tx_buffer = dma_tx_buf;
  spi_trans.rx_buffer = dma_rx_buf;

  esp_err_t ret = spi_device_queue_trans(spi_handle, &spi_trans, 0);
  if (ret != ESP_OK) {
    increment_counter(&transaction_fail_count);
    return false;
  }

  if (send_reset_cmd)
    pending_encoder_reset_cmd = false;
  trans_queued = true;
  last_queue_ms = now_ms;
  increment_counter(&transaction_queued_count);
  return true;
}

void spi_c3_master_init() {
  if (PIN_C3_SPI_SCK < 0 || PIN_C3_SPI_MOSI < 0 || PIN_C3_SPI_MISO < 0 ||
      PIN_C3_SPI_CS < 0 || PIN_C3_ALIVE < 0) {
    mros_console.println(
        "[SPI-C3] Disabled: PIN_C3_* not assigned for current topology.");
    return;
  }

  // ALIVE pin as input
  (void)mros::platform::mros_gpio_config(PIN_C3_ALIVE,
                                         mros::platform::GpioMode::InputPullup);
  (void)mros::platform::mros_gpio_config(PIN_C3_SPI_MISO,
                                         mros::platform::GpioMode::InputPullup);
  last_alive_level = mros::platform::mros_gpio_read(PIN_C3_ALIVE);
  last_alive_edge_ms = mros::platform::mros_millis();

  // SPI bus configuration
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = PIN_C3_SPI_MOSI;
  buscfg.miso_io_num = PIN_C3_SPI_MISO;
  buscfg.sclk_io_num = PIN_C3_SPI_SCK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = C3_SPI_FRAME_SIZE;

  // Initialize SPI3_HOST (VSPI) as Master with DMA
  esp_err_t err = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    mros_console.printf("[C3-SPI] Bus Init Failed: %s\n", esp_err_to_name(err));
    return;
  }

  // Device configuration
  spi_device_interface_config_t devcfg = {};
  devcfg.clock_speed_hz = C3_SPI_CLOCK_HZ;
  devcfg.mode = 0; // CPOL=0, CPHA=0
  devcfg.spics_io_num = PIN_C3_SPI_CS;
  devcfg.queue_size = 2;
  devcfg.flags = SPI_DEVICE_NO_DUMMY;
  devcfg.cs_ena_pretrans = 4;
  devcfg.cs_ena_posttrans = 4;

  err = spi_bus_add_device(SPI3_HOST, &devcfg, &spi_handle);
  if (err != ESP_OK) {
    mros_console.printf("[C3-SPI] Add Device Failed: %s\n", esp_err_to_name(err));
    spi_handle = NULL;
    return;
  }
  mros_console.println("[C3-SPI] Initialized Master on SPI3 (FSPI) at 1MHz.");
  gpio_set_drive_capability((gpio_num_t)PIN_C3_SPI_SCK, GPIO_DRIVE_CAP_0);
  gpio_set_drive_capability((gpio_num_t)PIN_C3_SPI_MOSI, GPIO_DRIVE_CAP_0);
  gpio_set_drive_capability((gpio_num_t)PIN_C3_SPI_CS, GPIO_DRIVE_CAP_0);

  // Keep TX zeroed (command channel reserved)
  memset(dma_tx_buf, 0, sizeof(dma_tx_buf));
  memset(dma_rx_buf, 0, sizeof(dma_rx_buf));

  // Prime first DMA transaction immediately.
  queue_transaction(mros::platform::mros_millis());
}

void spi_c3_set_task_handle(TaskHandle_t task_handle) {
  g_spi_c3_task_handle = task_handle;
}

void spi_c3_service_cycle(unsigned long now_ms) {
  (void)g_spi_c3_task_handle;
  if (!spi_handle) {
    return;
  }

  bool alive_ok = alive_heartbeat_ok(now_ms);
  if (last_good_rx_ms != 0 && (now_ms - last_good_rx_ms) > 1000) {
    link_synced = false;
    good_packet_count = 0;
  }

  if (trans_queued) {
    spi_transaction_t *ret_trans = NULL;
    esp_err_t ret = spi_device_get_trans_result(spi_handle, &ret_trans, 0);
    if (ret == ESP_OK) {
      trans_queued = false;
      increment_counter(&transaction_completed_count);
      parse_rx_packet(now_ms, alive_ok);
    } else if (ret == ESP_ERR_TIMEOUT) {
      if (now_ms - last_queue_ms > 100) {
        trans_queued = false;
        increment_counter(&transaction_timeout_count);
      }
    } else {
      trans_queued = false;
      increment_counter(&transaction_fail_count);
    }
  }

  effective_period_ms = select_c3_spi_period_ms(alive_ok);
  if (!trans_queued && (now_ms - last_queue_ms >= effective_period_ms)) {
    queue_transaction(now_ms);
  }
}

void spi_c3_get_diag_snapshot(C3SpiDiagSnapshot *snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  memset(snapshot, 0, sizeof(*snapshot));
  const unsigned long now = mros::platform::mros_millis();
  const bool alive_ok = alive_heartbeat_ok(now);
  snapshot->effective_period_ms = effective_period_ms;
  snapshot->transactions_queued = transaction_queued_count;
  snapshot->transactions_completed = transaction_completed_count;
  snapshot->transaction_timeout_count = transaction_timeout_count;
  snapshot->transaction_fail_count = transaction_fail_count;
  snapshot->crc_error_count = crc_error_count;
  snapshot->marker_error_count = marker_error_count;
  snapshot->total_rx_count = total_rx_count;
  snapshot->last_good_rx_ms = static_cast<uint32_t>(last_good_rx_ms);
  snapshot->connected = (last_good_rx_ms != 0 && (now - last_good_rx_ms < 1000));
  snapshot->alive_ok = alive_ok;
  snapshot->link_synced = link_synced;
}

uint32_t spi_c3_get_effective_period_ms() {
  return effective_period_ms;
}

void spi_c3_master_poll() {
  spi_c3_service_cycle(mros::platform::mros_millis());
}

void spi_c3_request_encoder_reset() { pending_encoder_reset_cmd = true; }

void spi_c3_set_failsafe_option(int8_t option) {
  if (option < C3_FAILSAFE_T41_QSPI) option = C3_FAILSAFE_T41_QSPI;
  if (option > C3_FAILSAFE_RESERVED) option = C3_FAILSAFE_RESERVED;
  failsafe_option_cmd = option;
}

int8_t spi_c3_get_failsafe_option() { return failsafe_option_cmd; }

void spi_c3_set_cmd_flags(uint8_t flags) { cmd_flags_cmd = flags; }

uint8_t spi_c3_get_cmd_flags() { return cmd_flags_cmd; }

void spi_c3_set_pid_setpoint_x100(int32_t setpoint_x100) {
  pid_setpoint_x100_cmd = setpoint_x100;
}

int32_t spi_c3_get_pid_setpoint_x100() { return pid_setpoint_x100_cmd; }

float spi_c3_get_position_deg() { return c3_position_deg; }
float spi_c3_get_speed_deg_s() { return c3_speed_deg_s; }
float spi_c3_get_accel_deg_s2() { return c3_accel_deg_s2; }
uint8_t spi_c3_get_loop_hz() { return c3_loop_hz; }

bool spi_c3_is_connected() {
  unsigned long now = mros::platform::mros_millis();
  (void)alive_heartbeat_ok(now);
  if (last_good_rx_ms == 0)
    return false;
  return (now - last_good_rx_ms < 1000);
}

bool spi_c3_is_espnow_active() {
  if (!spi_c3_is_connected()) return false;
  return c3_espnow_active;
}

bool spi_c3_is_espnow_connected() {
  if (!spi_c3_is_connected()) return false;
  return c3_espnow_connected;
}

bool spi_c3_has_recent_espnow_cmd() {
  if (!spi_c3_is_connected()) return false;
  return c3_espnow_recent_cmd;
}

uint32_t spi_c3_get_crc_errors() { return crc_error_count; }
uint32_t spi_c3_get_marker_errors() { return marker_error_count; }
uint32_t spi_c3_get_total_rx() { return total_rx_count; }
uint32_t spi_c3_get_last_good_rx_ms() {
  return static_cast<uint32_t>(last_good_rx_ms);
}
void spi_c3_reset_error_counters() {
  crc_error_count = 0;
  marker_error_count = 0;
}
