#include "i2c_pca9685.h"
#include "src/config/pin_config.h"
#include <algorithm>
#include <cmath>
#include <math.h>
#include <string.h>
#include <driver/gpio.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "src/drivers/utils/mros_console.h"
#include "src/platform/mros_i2c.h"
#include "src/platform/mros_nvs.h"
#include "src/platform/mros_time.h"

// --- PCA9685 Constants ---
#define PCA9685_I2C_ADDR_DEFAULT 0x40 // 7-bit base address (A0-A5 all open)
static uint32_t g_pca9685_osc_freq = 25000000UL; // Standard internal RC oscillator
static float g_current_freq = 50.0f;
static float g_us_to_tick_scale = (4096.0f * 50.0f) / 1000000.0f;
#define PCA9685_I2C_FREQ_HZ 100000UL // Debug-stable speed for noisy wiring
#define PCA9685_I2C_FALLBACK_SDA 41
#define PCA9685_I2C_FALLBACK_SCL 42
static constexpr int kPcaI2cTimeoutMs = 20;
static constexpr uint8_t kPcaWriteChunkSizeDefault = 4U;
static constexpr unsigned long kPcaInitRetryCooldownMs = 15000UL;
static constexpr unsigned long kPcaInitSkipLogCooldownMs = 15000UL;

// Register addresses
#define PCA9685_MODE1 0x00
#define PCA9685_MODE2 0x01
#define PCA9685_LED0_ON_L 0x06
#define PCA9685_PRESCALE 0xFE

// MODE1 bits
#define MODE1_RESTART 0x80
#define MODE1_AI 0x20 // Auto-Increment
#define MODE1_SLEEP 0x10

// Servo defaults
#define SERVO_FREQ_HZ 50.0f
#define DEFAULT_ANGLE_MIN_US 500.0f
#define DEFAULT_ANGLE_MAX_US 2500.0f
#define DEFAULT_SPEED_MIN_US 500.0f
#define DEFAULT_SPEED_CTR_US 1500.0f
#define DEFAULT_SPEED_MAX_US 2500.0f

// --- Module State ---
static bool g_pca_initialized = false;
static bool g_oe_enabled = false;
static PCA9685_ChannelCal_t g_cal[PCA_TOTAL_CHANNELS];
static uint16_t g_last_on[PCA_TOTAL_CHANNELS] = {0};
static uint16_t g_last_off[PCA_TOTAL_CHANNELS] = {0};
static bool g_last_valid[PCA_TOTAL_CHANNELS] = {false};
static unsigned long g_last_i2c_err_log_ms = 0;
static int g_i2c_sda_pin = PIN_I2C_SDA;
static int g_i2c_scl_pin = PIN_I2C_SCL;
static uint8_t g_last_i2c_err_code = 0xFF;
static char g_last_i2c_err_tag[24] = {0};
static bool g_i2c_bus_ready = false;
static uint8_t g_pca9685_addr = PCA9685_I2C_ADDR_DEFAULT;
static unsigned long g_last_addr_recover_ms = 0;
static unsigned long g_last_bus_recover_ms = 0;
static unsigned long g_last_init_fail_ms = 0;
static unsigned long g_last_init_skip_log_ms = 0;
static uint32_t g_init_fail_count = 0;
static TaskHandle_t g_pca_task_handle = nullptr;
static constexpr UBaseType_t kPcaQueueLength = 24;
static QueueHandle_t g_pca_cmd_queue = nullptr;
static volatile uint32_t g_pca_enqueue_count = 0;
static volatile uint32_t g_pca_process_count = 0;
static volatile uint32_t g_pca_duplicate_skip_count = 0;
static volatile uint32_t g_pca_coalesced_count = 0;
static volatile uint32_t g_pca_drop_oldest_count = 0;
static volatile uint32_t g_pca_drop_count = 0;
static volatile uint32_t g_pca_queue_high_watermark = 0;
static volatile uint32_t g_pca_shadow_update_count = 0;
static volatile uint32_t g_pca_shadow_coalesce_count = 0;
static volatile uint32_t g_pca_shadow_flush_count = 0;

static inline void increment_counter(volatile uint32_t *counter) {
  if (counter == nullptr) {
    return;
  }
  *counter = *counter + 1U;
}

enum class PcaCommandType : uint8_t {
  PwmGroup = 0,
  AngleGroup,
  SpeedGroup,
  SetFrequency,
  SetOutputEnable,
};

struct PcaCommand {
  PcaCommandType type = PcaCommandType::PwmGroup;
  uint8_t start_channel = 0;
  uint8_t count = 0;
  int speed = 0;
  bool enable = false;
  float freq_hz = 0.0f;
  uint16_t off_ticks[PCA_TOTAL_CHANNELS] = {0};
  float angles[PCA_TOTAL_CHANNELS] = {0.0f};
};

static PcaCommand g_pca_last_enqueued_command = {};
static bool g_pca_has_last_enqueued_command = false;
static PcaCommand g_pca_latest_group_shadow = {};
static bool g_pca_has_latest_group_shadow = false;

static bool pca9685_set_pwm_direct(uint8_t channel, uint16_t on, uint16_t off);
static bool pca9685_set_pwm_group_direct(uint8_t start_channel, const uint16_t *off_ticks,
                                         uint8_t count);
static bool pca9685_set_servo_angle_direct(uint8_t channel, float angle);
static bool pca9685_set_servo_speed_direct(uint8_t channel, int speed);
static bool pca9685_set_servo_speed_group_direct(uint8_t start_channel, uint8_t count,
                                                 int speed);
static bool pca9685_set_servo_angle_group_direct(uint8_t start_channel, const float *angles,
                                                 uint8_t count);
static bool pca9685_set_frequency_direct(float freq_hz);
static void pca9685_set_output_enable_direct(bool enable);
static bool pca_commands_can_coalesce(const PcaCommand &current,
                                      const PcaCommand &next);

static bool pca_queue_ready() {
  if (g_pca_cmd_queue != nullptr) {
    return true;
  }
  g_pca_cmd_queue = xQueueCreate(kPcaQueueLength, sizeof(PcaCommand));
  return g_pca_cmd_queue != nullptr;
}

static bool pca_should_execute_direct() {
  return (g_pca_task_handle == nullptr) || (xTaskGetCurrentTaskHandle() == g_pca_task_handle);
}

static void note_pca_init_failed() {
  g_last_init_fail_ms = mros::platform::mros_millis();
  if (g_init_fail_count != 0xFFFFFFFFUL) {
    ++g_init_fail_count;
  }
}

static bool enqueue_pca_command(const PcaCommand &command) {
  if (!pca_queue_ready()) {
    increment_counter(&g_pca_drop_count);
    return false;
  }
  if (g_pca_has_last_enqueued_command &&
      memcmp(&g_pca_last_enqueued_command, &command, sizeof(PcaCommand)) == 0) {
    increment_counter(&g_pca_duplicate_skip_count);
    return true;
  }
  if (command.type == PcaCommandType::PwmGroup ||
      command.type == PcaCommandType::AngleGroup ||
      command.type == PcaCommandType::SpeedGroup) {
    g_pca_latest_group_shadow = command;
    g_pca_has_latest_group_shadow = true;
    increment_counter(&g_pca_shadow_update_count);
  }
  if (xQueueSend(g_pca_cmd_queue, &command, 0) != pdTRUE) {
    PcaCommand buffered[kPcaQueueLength] = {};
    UBaseType_t retained = 0;
    PcaCommand pending = {};
    while (xQueueReceive(g_pca_cmd_queue, &pending, 0) == pdTRUE) {
      if (pca_commands_can_coalesce(pending, command)) {
        increment_counter(&g_pca_shadow_coalesce_count);
        continue;
      }
      if (retained < kPcaQueueLength) {
        buffered[retained++] = pending;
      }
    }
    for (UBaseType_t i = 0; i < retained; ++i) {
      (void)xQueueSend(g_pca_cmd_queue, &buffered[i], 0);
    }
    if (xQueueSend(g_pca_cmd_queue, &command, 0) == pdTRUE) {
      increment_counter(&g_pca_enqueue_count);
      g_pca_last_enqueued_command = command;
      g_pca_has_last_enqueued_command = true;
      const UBaseType_t depth = uxQueueMessagesWaiting(g_pca_cmd_queue);
      if (depth > g_pca_queue_high_watermark) {
        g_pca_queue_high_watermark = depth;
      }
      return true;
    }
    PcaCommand dropped = {};
    if (xQueueReceive(g_pca_cmd_queue, &dropped, 0) == pdTRUE) {
      increment_counter(&g_pca_drop_oldest_count);
      if (xQueueSend(g_pca_cmd_queue, &command, 0) != pdTRUE) {
        increment_counter(&g_pca_drop_count);
        return false;
      }
    } else {
      increment_counter(&g_pca_drop_count);
      return false;
    }
  }
  increment_counter(&g_pca_enqueue_count);
  g_pca_last_enqueued_command = command;
  g_pca_has_last_enqueued_command = true;
  const UBaseType_t depth = uxQueueMessagesWaiting(g_pca_cmd_queue);
  if (depth > g_pca_queue_high_watermark) {
    g_pca_queue_high_watermark = depth;
  }
  return true;
}

static bool pca_commands_can_coalesce(const PcaCommand &current,
                                      const PcaCommand &next) {
  if (current.type != next.type) {
    return false;
  }
  switch (current.type) {
    case PcaCommandType::PwmGroup:
    case PcaCommandType::AngleGroup:
    case PcaCommandType::SpeedGroup:
      return current.start_channel == next.start_channel &&
             current.count == next.count;
    case PcaCommandType::SetFrequency:
    case PcaCommandType::SetOutputEnable:
      return true;
  }
  return false;
}

static void execute_pca_command(const PcaCommand &command) {
  if (command.type == PcaCommandType::PwmGroup ||
      command.type == PcaCommandType::AngleGroup ||
      command.type == PcaCommandType::SpeedGroup) {
    increment_counter(&g_pca_shadow_flush_count);
  }
  switch (command.type) {
    case PcaCommandType::PwmGroup:
      pca9685_set_pwm_group_direct(command.start_channel, command.off_ticks,
                                   command.count);
      break;
    case PcaCommandType::AngleGroup:
      pca9685_set_servo_angle_group_direct(command.start_channel, command.angles,
                                           command.count);
      break;
    case PcaCommandType::SpeedGroup:
      pca9685_set_servo_speed_group_direct(command.start_channel, command.count,
                                           command.speed);
      break;
    case PcaCommandType::SetFrequency:
      pca9685_set_frequency_direct(command.freq_hz);
      break;
    case PcaCommandType::SetOutputEnable:
      pca9685_set_output_enable_direct(command.enable);
      break;
  }
}

static const char *i2c_err_to_text(uint8_t code) {
  switch (code) {
  case 0:
    return "OK";
  case 1:
    return "DATA_TOO_LONG";
  case 2:
    return "NACK_ADDR";
  case 3:
    return "NACK_DATA";
  case 4:
    return "OTHER";
  case 5:
    return "TIMEOUT";
  default:
    return "UNKNOWN";
  }
}

static uint8_t i2c_err_from_esp(const esp_err_t err) {
  return mros::platform::mros_i2c_legacy_error_code(err);
}

static void log_i2c_err_throttled(const char *tag, uint8_t code) {
  unsigned long now = mros::platform::mros_millis();
  unsigned long min_interval_ms = 250UL;
  if ((now - g_last_i2c_err_log_ms) < min_interval_ms)
    return;
  g_last_i2c_err_log_ms = now;
  g_last_i2c_err_code = code;
  strncpy(g_last_i2c_err_tag, tag, sizeof(g_last_i2c_err_tag) - 1);
  g_last_i2c_err_tag[sizeof(g_last_i2c_err_tag) - 1] = '\0';
  mros_console.printf(
      "[PCA9685][%s] I2C err=%u (%s) SDA=%d SCL=%d OE=%d freq=%lu\n", tag,
      (unsigned)code, i2c_err_to_text(code), g_i2c_sda_pin, g_i2c_scl_pin,
      PIN_PCA_OE, (unsigned long)PCA9685_I2C_FREQ_HZ);
}

static void debug_i2c_scan(const bool force = false) {
  if (!g_i2c_bus_ready) {
    mros_console.println("[PCA9685] I2C scan skipped (bus not ready)");
    return;
  }
  if (!force && !mros::platform::mros_i2c_debug_scan_enabled()) {
    return;
  }
  mros_console.println("[PCA9685] I2C scan start...");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    const uint8_t code = i2c_err_from_esp(mros::platform::mros_i2c_probe(addr));
    if (code == 0) {
      mros_console.printf("[PCA9685] I2C device found: 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) {
    mros_console.println("[PCA9685] I2C scan: no device found");
  }
}

static bool i2c_probe_addr(uint8_t addr, uint8_t *probe_code) {
  const uint8_t code =
      i2c_err_from_esp(mros::platform::mros_i2c_probe(addr));
  if (probe_code) {
    *probe_code = code;
  }
  return (code == 0);
}

static bool i2c_autodetect_pca_addr(uint8_t *probe_code) {
  uint8_t code = 0xFF;

  // Try current/default address first.
  if (i2c_probe_addr(g_pca9685_addr, &code)) {
    if (probe_code) {
      *probe_code = 0;
    }
    return true;
  }

  // Scan valid PCA9685 address range (0x40-0x7F).
  for (uint8_t addr = 0x40; addr <= 0x7F; addr++) {
    if (addr == g_pca9685_addr) {
      continue;
    }
    if (i2c_probe_addr(addr, &code)) {
      g_pca9685_addr = addr;
      if (probe_code) {
        *probe_code = 0;
      }
      mros_console.printf("[PCA9685] Auto-detected address: 0x%02X\n",
                          g_pca9685_addr);
      return true;
    }
  }

  if (probe_code) {
    *probe_code = code;
  }
  return false;
}

static bool i2c_begin_and_probe(int sda, int scl, uint8_t *probe_code) {
  g_i2c_sda_pin = sda;
  g_i2c_scl_pin = scl;
  if (g_i2c_bus_ready) {
    mros::platform::mros_i2c_deinit();
    g_i2c_bus_ready = false;
    mros::platform::mros_delay_ms(1);
  }
  mros::platform::I2cBusConfig config = {};
  config.port = I2C_NUM_0;
  config.sda_pin = g_i2c_sda_pin;
  config.scl_pin = g_i2c_scl_pin;
  config.freq_hz = PCA9685_I2C_FREQ_HZ;
  config.timeout_ms = kPcaI2cTimeoutMs;
  config.write_chunk_size = kPcaWriteChunkSizeDefault;
  const bool begin_ok = mros::platform::mros_i2c_init(config);
  if (!begin_ok) {
    if (probe_code)
      *probe_code = 4;
    mros_console.printf("[PCA9685] I2C begin failed on SDA=%d SCL=%d\n",
                        g_i2c_sda_pin, g_i2c_scl_pin);
    return false;
  }
  g_i2c_bus_ready = true;
  mros::platform::mros_delay_ms(2);
  uint8_t code = 0xFF;
  bool ok = i2c_autodetect_pca_addr(&code);
  if (probe_code)
    *probe_code = code;
  if (!ok) {
    mros_console.printf("[PCA9685] I2C probe failed. Last code: %d\n", code);
  } else {
    mros_console.printf("[PCA9685] I2C probe OK at 0x%02X.\n", g_pca9685_addr);
  }
  return ok;
}

static bool pca_try_recover_on_addr_nack(const char *tag) {
  const unsigned long now = mros::platform::mros_millis();
  if ((now - g_last_addr_recover_ms) < 300UL) {
    return false;
  }
  g_last_addr_recover_ms = now;

  uint8_t probe = 0xFF;
  if (g_i2c_bus_ready && i2c_autodetect_pca_addr(&probe)) {
    g_pca_initialized = true;
    mros_console.printf("[PCA9685][%s] Address recovery OK: 0x%02X\n", tag,
                        g_pca9685_addr);
    return true;
  }

  g_pca_initialized = false;
  log_i2c_err_throttled("RECOVER_ADDR", probe);
  return false;
}

static bool pca_try_recover_on_bus_error(const char *tag, uint8_t code) {
  if (code == 2) {
    return pca_try_recover_on_addr_nack(tag);
  }
  if (code != 3 && code != 4 && code != 5) {
    return false;
  }
  const unsigned long now = mros::platform::mros_millis();
  if ((now - g_last_bus_recover_ms) < 300UL) {
    return false;
  }
  g_last_bus_recover_ms = now;

  uint8_t probe = 0xFF;
  bool ok = i2c_begin_and_probe(g_i2c_sda_pin, g_i2c_scl_pin, &probe);
  if (!ok) {
    g_pca_initialized = false;
    log_i2c_err_throttled("RECOVER_BUS", probe);
    return false;
  }
  g_pca_initialized = true;
  mros_console.printf("[PCA9685][%s] Bus recover OK SDA=%d SCL=%d addr=0x%02X\n",
                      tag, g_i2c_sda_pin, g_i2c_scl_pin, g_pca9685_addr);
  return true;
}

// --- Low-level I2C helpers ---
static bool write_reg(uint8_t reg, uint8_t val) {
  if (!g_i2c_bus_ready) {
    log_i2c_err_throttled("WRITE_REG_STATE", 4);
    return false;
  }
  esp_err_t err =
      mros::platform::mros_i2c_write_reg_u8(g_pca9685_addr, reg, val);
  uint8_t code = i2c_err_from_esp(err);
  if (code != 0 && pca_try_recover_on_bus_error("WRITE_REG", code)) {
    err = mros::platform::mros_i2c_write_reg_u8(g_pca9685_addr, reg, val);
    code = i2c_err_from_esp(err);
  }
  if (code != 0) {
    log_i2c_err_throttled("WRITE_REG", code);
    return false;
  }
  return true;
}

static bool read_reg(uint8_t reg, uint8_t *val) {
  if (!g_i2c_bus_ready) {
    log_i2c_err_throttled("READ_REG_STATE", 4);
    return false;
  }
  esp_err_t err = mros::platform::mros_i2c_read_reg_u8(g_pca9685_addr, reg, val);
  uint8_t code = i2c_err_from_esp(err);
  if (code != 0 && pca_try_recover_on_bus_error("READ_REG_ADDR", code)) {
    err = mros::platform::mros_i2c_read_reg_u8(g_pca9685_addr, reg, val);
    code = i2c_err_from_esp(err);
  }
  if (code != 0) {
    log_i2c_err_throttled("READ_REG_ADDR", code);
    return false;
  }
  return err == ESP_OK;
}

static void normalize_cal(PCA9685_ChannelCal_t *c) {
  if (!c)
    return;

  float a_min = c->angle_min_us;
  float a_max = c->angle_max_us;
  bool angle_ok = std::isfinite(a_min) && std::isfinite(a_max) && (a_min >= 500.0f) &&
                  (a_max <= 2500.0f) && (a_min < a_max);
  if (!angle_ok) {
    a_min = DEFAULT_ANGLE_MIN_US;
    a_max = DEFAULT_ANGLE_MAX_US;
  }

  float s_min = c->speed_min_us;
  float s_ctr = c->speed_center_us;
  float s_max = c->speed_max_us;
  // Migrate legacy turret/speed window (1000/1500/2000) to full 500/1500/2500.
  if (fabsf(s_min - 1000.0f) < 1.0f && fabsf(s_ctr - 1500.0f) < 1.0f &&
      fabsf(s_max - 2000.0f) < 1.0f) {
    s_min = DEFAULT_SPEED_MIN_US;
    s_ctr = DEFAULT_SPEED_CTR_US;
    s_max = DEFAULT_SPEED_MAX_US;
  }
  bool speed_ok = std::isfinite(s_min) && std::isfinite(s_ctr) && std::isfinite(s_max) &&
                  (s_min >= 500.0f) && (s_max <= 2500.0f) &&
                  (s_min < s_ctr) && (s_ctr < s_max);
  if (!speed_ok) {
    s_min = DEFAULT_SPEED_MIN_US;
    s_ctr = DEFAULT_SPEED_CTR_US;
    s_max = DEFAULT_SPEED_MAX_US;
  }

  c->angle_min_us = a_min;
  c->angle_max_us = a_max;
  c->speed_min_us = s_min;
  c->speed_center_us = s_ctr;
  c->speed_max_us = s_max;
}

static void resolve_speed_cal(const PCA9685_ChannelCal_t *c, float *s_min,
                              float *s_ctr, float *s_max) {
  float mn = DEFAULT_SPEED_MIN_US;
  float ct = DEFAULT_SPEED_CTR_US;
  float mx = DEFAULT_SPEED_MAX_US;

  if (c) {
    mn = c->speed_min_us;
    ct = c->speed_center_us;
    mx = c->speed_max_us;
  }

  bool valid_range = std::isfinite(mn) && std::isfinite(ct) && std::isfinite(mx) &&
                     (mn >= 500.0f) && (mx <= 2500.0f) && (mn < ct) &&
                     (ct < mx);
  if (!valid_range) {
    mn = DEFAULT_SPEED_MIN_US;
    ct = DEFAULT_SPEED_CTR_US;
    mx = DEFAULT_SPEED_MAX_US;
  }

  // Avoid too narrow deadband windows that prevent motion.
  if ((ct - mn) < 40.0f || (mx - ct) < 40.0f) {
    float base = ct;
    if (!std::isfinite(base) || base < 500.0f || base > 2500.0f)
      base = DEFAULT_SPEED_CTR_US;
    mn = base - 500.0f;
    mx = base + 500.0f;
    if (mn < 500.0f)
      mn = 500.0f;
    if (mx > 2500.0f)
      mx = 2500.0f;
    ct = base;
  }

  *s_min = mn;
  *s_ctr = ct;
  *s_max = mx;
}

// --- Helper: microseconds to tick ---
static uint16_t us_to_tick(float us) {
  uint16_t tick = (uint16_t)((us * g_us_to_tick_scale) + 0.5f);
  if (tick > 4095)
    tick = 4095;
  return tick;
}

static bool write_pwm_block(uint8_t start_channel, const uint16_t *on_vals,
                             const uint16_t *off_vals, uint8_t count) {
  if (!g_pca_initialized || !on_vals || !off_vals || count == 0)
    return false;
  if (start_channel < 1 || start_channel > 16)
    return false;
  if ((uint16_t)start_channel + (uint16_t)count - 1 > 16)
    return false;

  bool any_changed = false;
  for (uint8_t i = 0; i < count; i++) {
    uint8_t idx = (uint8_t)(start_channel - 1 + i);
    if (!g_last_valid[idx] || g_last_on[idx] != on_vals[i] ||
        g_last_off[idx] != off_vals[i]) {
      any_changed = true;
      break;
    }
  }
  if (!any_changed)
    return true;

  if (!g_i2c_bus_ready) {
    log_i2c_err_throttled("WRITE_BLOCK_STATE", 4);
    return false;
  }

  // I2C CHUNKING: Standard Wire buffer is 32 bytes.
  // Reg(1) + 4 * channels. 4 channels = 17 bytes (Safe). 7 channels = 29 bytes (Limit).
  // We use 4-channel chunks for categorical timing stability.
  uint8_t chunk_size = mros::platform::mros_i2c_config().write_chunk_size;
  if (chunk_size == 0U) {
    chunk_size = kPcaWriteChunkSizeDefault;
  }
  bool success = true;

  for (uint8_t i = 0; i < count; i += chunk_size) {
    const uint8_t current_chunk = std::min(chunk_size, static_cast<uint8_t>(count - i));
    uint8_t reg_base = PCA9685_LED0_ON_L + 4 * (start_channel - 1 + i);
    uint8_t payload[1U + (4U * PCA_TOTAL_CHANNELS)] = {0};
    payload[0] = reg_base;
    for (uint8_t j = 0; j < current_chunk; j++) {
      uint16_t on = on_vals[i + j];
      uint16_t off = off_vals[i + j];
      const size_t base = 1U + static_cast<size_t>(j) * 4U;
      payload[base + 0U] = static_cast<uint8_t>(on & 0xFF);
      payload[base + 1U] = static_cast<uint8_t>((on >> 8) & 0x0F);
      payload[base + 2U] = static_cast<uint8_t>(off & 0xFF);
      payload[base + 3U] = static_cast<uint8_t>((off >> 8) & 0x0F);
    }
    esp_err_t err = mros::platform::mros_i2c_transmit(
        g_pca9685_addr, payload, 1U + static_cast<size_t>(current_chunk) * 4U);
    uint8_t code = i2c_err_from_esp(err);
    if (code != 0 &&
        pca_try_recover_on_bus_error("WRITE_BLOCK_CHUNK", code)) {
      err = mros::platform::mros_i2c_transmit(
          g_pca9685_addr, payload, 1U + static_cast<size_t>(current_chunk) * 4U);
      code = i2c_err_from_esp(err);
    }
    if (code != 0) {
      log_i2c_err_throttled("WRITE_BLOCK_CHUNK", code);
      success = false;
      break;
    }
  }

  if (success) {
    for (uint8_t i = 0; i < count; i++) {
      uint8_t idx = (uint8_t)(start_channel - 1 + i);
      g_last_on[idx] = on_vals[i];
      g_last_off[idx] = off_vals[i];
      g_last_valid[idx] = true;
    }
  }
  return success;
}

// --- Calibration defaults ---
static void set_defaults(PCA9685_ChannelCal_t *c) {
  c->angle_min_us = DEFAULT_ANGLE_MIN_US;
  c->angle_max_us = DEFAULT_ANGLE_MAX_US;
  c->speed_min_us = DEFAULT_SPEED_MIN_US;
  c->speed_center_us = DEFAULT_SPEED_CTR_US;
  c->speed_max_us = DEFAULT_SPEED_MAX_US;
}

// --- Public API ---

bool pca9685_init() {
  const unsigned long now_ms = mros::platform::mros_millis();
  if (g_pca_initialized) {
    return true;
  }
  if (g_last_init_fail_ms != 0UL &&
      (now_ms - g_last_init_fail_ms) < kPcaInitRetryCooldownMs) {
    if ((now_ms - g_last_init_skip_log_ms) >= kPcaInitSkipLogCooldownMs) {
      g_last_init_skip_log_ms = now_ms;
      mros_console.printf(
          "[PCA9685] Init skipped: previous probe failed, retry in %lu ms (failures=%lu)\n",
          static_cast<unsigned long>(kPcaInitRetryCooldownMs -
                                     (now_ms - g_last_init_fail_ms)),
          static_cast<unsigned long>(g_init_fail_count));
    }
    return false;
  }

  g_pca_initialized = false;
  g_pca9685_addr = PCA9685_I2C_ADDR_DEFAULT;
  // 0. Load calibration from flash (or defaults)
  pca9685_load_cal();
  mros_console.printf("[PCA9685] Init start addr=0x%02X SDA=%d SCL=%d OE=%d freq=%lu\n",
                g_pca9685_addr, PIN_I2C_SDA, PIN_I2C_SCL, PIN_PCA_OE,
                (unsigned long)PCA9685_I2C_FREQ_HZ);
  mros_console.println(
      "[PCA9685] Not: GPIO1/2 bazi S3 kartlarda kristal/ozel islevde olabilir.");

  // 1. Start I2C and check device presence on primary pins
  uint8_t probe = 0xFF;
  if (!i2c_begin_and_probe(PIN_I2C_SDA, PIN_I2C_SCL, &probe)) {
    log_i2c_err_throttled("PROBE_PRIMARY", probe);
    debug_i2c_scan(true);
    // 2. Fallback pins for S3 boards where GPIO1/2 are problematic
    mros_console.printf("[PCA9685] Primary probe failed. Trying fallback SDA=%d SCL=%d\n",
                  PCA9685_I2C_FALLBACK_SDA, PCA9685_I2C_FALLBACK_SCL);
    if (!i2c_begin_and_probe(PCA9685_I2C_FALLBACK_SDA,
                             PCA9685_I2C_FALLBACK_SCL, &probe)) {
      log_i2c_err_throttled("PROBE_FALLBACK", probe);
      debug_i2c_scan(true);
      g_pca_initialized = false;
      note_pca_init_failed();
      return false;
    }
    mros_console.printf("[PCA9685] Fallback probe OK on SDA=%d SCL=%d\n",
                  g_i2c_sda_pin, g_i2c_scl_pin);
  } else {
    mros_console.println("[PCA9685] Probe OK");
  }

  // 3. Software reset (general call)
  if (!g_i2c_bus_ready) {
    log_i2c_err_throttled("SWRST_STATE", 4);
    g_pca_initialized = false;
    note_pca_init_failed();
    return false;
  }
  const uint8_t swrst_cmd = 0x06;
  uint8_t swrst = i2c_err_from_esp(
      mros::platform::mros_i2c_transmit(0x00, &swrst_cmd, 1U));
  if (swrst != 0) {
    mros_console.printf("[PCA9685] SWRST failed: %s (%u)\n", i2c_err_to_text(swrst), swrst);
  }
  mros::platform::mros_delay_ms(10);

  // 4. Wake up + Auto-Increment (retry up to 3 times)
  for (int i = 0; i < 3; i++) {
    write_reg(PCA9685_MODE1, 0x00); // Clear SLEEP
    mros::platform::mros_delay_ms(10);
    write_reg(PCA9685_MODE1, MODE1_AI); // Enable Auto-Increment
    mros::platform::mros_delay_ms(10);

    uint8_t mode1_val = 0;
    read_reg(PCA9685_MODE1, &mode1_val);
    if ((mode1_val & MODE1_SLEEP) == 0) {
      g_pca_initialized = true;
      break;
    }
    mros::platform::mros_delay_ms(10);
  }

  if (!g_pca_initialized) {
    mros_console.println("[PCA9685] Critical: Wakeup timeout - Device not responding to MODE1 writes.");
    note_pca_init_failed();
    return false;
  }

  // 5. Totem-pole output (push-pull)
  if (!write_reg(PCA9685_MODE2, 0x04)) {
    mros_console.println("[PCA9685] MODE2 write failed");
    g_pca_initialized = false;
    note_pca_init_failed();
    return false;
  }

  // 6. Set servo frequency (50 Hz)
  if (!pca9685_set_frequency_direct(SERVO_FREQ_HZ)) {
    mros_console.println("[PCA9685] Frequency setup failed");
    g_pca_initialized = false;
    note_pca_init_failed();
    return false;
  }

  // 7. OE pin setup: Configure and enable by default (pull LOW).
  gpio_config_t oe_config = {};
  oe_config.pin_bit_mask = (1ULL << PIN_PCA_OE);
  oe_config.mode = GPIO_MODE_OUTPUT;
  oe_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  oe_config.pull_up_en = GPIO_PULLUP_DISABLE;
  oe_config.intr_type = GPIO_INTR_DISABLE;
  (void)gpio_config(&oe_config);
  (void)gpio_set_level(static_cast<gpio_num_t>(PIN_PCA_OE), 0); // Explicitly enable motors on boot to match UI default.
  g_oe_enabled = true; 
  mros_console.printf("[PCA9685] Init Success. Freq=%.1f Osc=%lu OE=%s\n", 
                SERVO_FREQ_HZ, g_pca9685_osc_freq, g_oe_enabled ? "ACTIVE" : "STOPPED");
  g_last_init_fail_ms = 0;
  g_last_init_skip_log_ms = 0;

  return true;
}

static bool pca9685_set_pwm_direct(uint8_t channel, uint16_t on, uint16_t off) {
  uint16_t on_arr[1] = {on};
  uint16_t off_arr[1] = {off};
  return write_pwm_block(channel, on_arr, off_arr, 1);
}

static bool pca9685_set_pwm_group_direct(uint8_t start_channel,
                                         const uint16_t *off_ticks,
                                         uint8_t count) {
  if (!off_ticks || count == 0)
    return false;
  uint16_t on_vals[PCA_TOTAL_CHANNELS] = {0};
  uint16_t off_vals[PCA_TOTAL_CHANNELS] = {0};
  for (uint8_t i = 0; i < count; i++) {
    off_vals[i] = off_ticks[i];
  }
  return write_pwm_block(start_channel, on_vals, off_vals, count);
}

static bool pca9685_set_servo_angle_direct(uint8_t channel, float angle) {
  if (channel < 1 || channel > 16)
    return false;
  if (angle < 0.0f)
    angle = 0.0f;
  if (angle > 180.0f)
    angle = 180.0f;

  PCA9685_ChannelCal_t *c = &g_cal[channel - 1];

  // Linear interpolation between calibrated min and max
  float us =
      c->angle_min_us + (angle / 180.0f) * (c->angle_max_us - c->angle_min_us);

  return pca9685_set_pwm_direct(channel, 0, us_to_tick(us));
}

static bool pca9685_set_servo_speed_direct(uint8_t channel, int speed) {
  if (channel < 1 || channel > 16)
    return false;
  if (speed > 100)
    speed = 100;
  if (speed < -100)
    speed = -100;

  PCA9685_ChannelCal_t *c = &g_cal[channel - 1];
  float s_min, s_ctr, s_max;
  resolve_speed_cal(c, &s_min, &s_ctr, &s_max);

  if (speed == 0) {
    // Continuous-rotation servo stop should be center pulse.
    return pca9685_set_pwm_direct(channel, 0, us_to_tick(s_ctr));
  }

  float us;
  if (speed > 0) {
    // Positive: center -> max
    us = s_ctr + ((float)speed / 100.0f) * (s_max - s_ctr);
  } else {
    // Negative: center -> min
    us = s_ctr + ((float)speed / 100.0f) * (s_ctr - s_min);
  }

  return pca9685_set_pwm_direct(channel, 0, us_to_tick(us));
}

static bool pca9685_set_servo_speed_group_direct(uint8_t start_channel,
                                                 uint8_t count,
                                                 int speed) {
  if (count == 0)
    return false;
  if (speed > 100)
    speed = 100;
  if (speed < -100)
    speed = -100;

  uint16_t on_vals[PCA_TOTAL_CHANNELS] = {0};
  uint16_t off_vals[PCA_TOTAL_CHANNELS] = {0};
  for (uint8_t i = 0; i < count; i++) {
    uint8_t ch = (uint8_t)(start_channel + i);
    if (ch < 1 || ch > 16)
      return false;
    PCA9685_ChannelCal_t *c = &g_cal[ch - 1];
    float s_min, s_ctr, s_max;
    resolve_speed_cal(c, &s_min, &s_ctr, &s_max);
    if (speed == 0) {
      off_vals[i] = us_to_tick(s_ctr);
      continue;
    }

    float us;
    if (speed > 0) {
      us = s_ctr + ((float)speed / 100.0f) * (s_max - s_ctr);
    } else {
      us = s_ctr + ((float)speed / 100.0f) * (s_ctr - s_min);
    }
    off_vals[i] = us_to_tick(us);
  }
  return write_pwm_block(start_channel, on_vals, off_vals, count);
}

static bool pca9685_set_servo_angle_group_direct(uint8_t start_channel,
                                                 const float *angles,
                                                 uint8_t count) {
  if (!angles || count == 0)
    return false;
  uint16_t on_vals[PCA_TOTAL_CHANNELS] = {0};
  uint16_t off_vals[PCA_TOTAL_CHANNELS] = {0};
  for (uint8_t i = 0; i < count; i++) {
    uint8_t ch = (uint8_t)(start_channel + i);
    if (ch < 1 || ch > 16)
      return false;
    float angle = angles[i];
    if (angle < 0.0f)
      angle = 0.0f;
    if (angle > 180.0f)
      angle = 180.0f;

    PCA9685_ChannelCal_t *c = &g_cal[ch - 1];
    float us = c->angle_min_us +
               (angle / 180.0f) * (c->angle_max_us - c->angle_min_us);
    off_vals[i] = us_to_tick(us);
  }
  return write_pwm_block(start_channel, on_vals, off_vals, count);
}

static bool pca9685_set_frequency_direct(float freq_hz) {
  if (!g_pca_initialized)
    return false;
  if (freq_hz < 24.0f || freq_hz > 1526.0f)
    return false;

  // prescale = round(OSC / (4096 * freq)) - 1
  uint8_t prescale =
      (uint8_t)(((float)g_pca9685_osc_freq / (4096.0f * freq_hz)) + 0.5f) - 1;

  // PRESCALE can only be written in SLEEP mode
  write_reg(PCA9685_MODE1, MODE1_SLEEP | MODE1_AI); // Enter sleep
  mros::platform::mros_delay_ms(2);
  write_reg(PCA9685_PRESCALE, prescale); // Write prescaler
  mros::platform::mros_delay_ms(2);
  write_reg(PCA9685_MODE1, MODE1_AI); // Wake up
  mros::platform::mros_delay_ms(5);   // Oscillator stabilization
  write_reg(PCA9685_MODE1, MODE1_AI | MODE1_RESTART); // Restart PWM
  mros::platform::mros_delay_ms(2);

  g_current_freq = freq_hz;
  g_us_to_tick_scale = (4096.0f * g_current_freq) / 1000000.0f;
  return true;
}

float pca9685_get_frequency() { return g_current_freq; }

void pca9685_set_osc_freq(uint32_t freq_hz) {
  g_pca9685_osc_freq = freq_hz;
  // Re-apply frequency to update prescaler with new oscillator value
  if (pca_should_execute_direct()) {
    pca9685_set_frequency_direct(g_current_freq);
  } else {
    PcaCommand command = {};
    command.type = PcaCommandType::SetFrequency;
    command.freq_hz = g_current_freq;
    enqueue_pca_command(command);
  }
}

uint32_t pca9685_get_osc_freq() { return g_pca9685_osc_freq; }

static void pca9685_set_output_enable_direct(bool enable) {
  // Drive physical PCA9685 OE pin (LOW = enabled, HIGH = disabled)
  (void)gpio_set_level(static_cast<gpio_num_t>(PIN_PCA_OE), enable ? 0 : 1);
  g_oe_enabled = enable;
}

void pca9685_set_task_handle(TaskHandle_t task_handle) {
  g_pca_task_handle = task_handle;
  (void)pca_queue_ready();
}

void pca9685_process_queue() {
  if (!pca_queue_ready()) {
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }

  PcaCommand command = {};
  if (xQueueReceive(g_pca_cmd_queue, &command, portMAX_DELAY) != pdTRUE) {
    return;
  }

  PcaCommand next = {};
  while (xQueueReceive(g_pca_cmd_queue, &next, 0) == pdTRUE) {
    if (pca_commands_can_coalesce(command, next)) {
      command = next;
      increment_counter(&g_pca_coalesced_count);
      continue;
    }
    execute_pca_command(command);
    increment_counter(&g_pca_process_count);
    command = next;
  }
  execute_pca_command(command);
  increment_counter(&g_pca_process_count);
}

void pca9685_get_diag_snapshot(PCA9685_DiagSnapshot_t *snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->queue_capacity = kPcaQueueLength;
  snapshot->queue_depth =
      (g_pca_cmd_queue != nullptr) ? uxQueueMessagesWaiting(g_pca_cmd_queue) : 0U;
  snapshot->queue_high_watermark = g_pca_queue_high_watermark;
  snapshot->enqueue_count = g_pca_enqueue_count;
  snapshot->process_count = g_pca_process_count;
  snapshot->duplicate_skip_count = g_pca_duplicate_skip_count;
  snapshot->coalesced_count = g_pca_coalesced_count;
  snapshot->drop_oldest_count = g_pca_drop_oldest_count;
  snapshot->drop_count = g_pca_drop_count;
  snapshot->shadow_update_count = g_pca_shadow_update_count;
  snapshot->shadow_coalesce_count = g_pca_shadow_coalesce_count;
  snapshot->shadow_flush_count = g_pca_shadow_flush_count;
  snapshot->shadow_valid = g_pca_has_latest_group_shadow;
  snapshot->shadow_type = static_cast<uint8_t>(g_pca_latest_group_shadow.type);
  snapshot->shadow_start_channel = g_pca_latest_group_shadow.start_channel;
  snapshot->shadow_count = g_pca_latest_group_shadow.count;
}

bool pca9685_set_pwm(uint8_t channel, uint16_t on, uint16_t off) {
  if (pca_should_execute_direct()) {
    return pca9685_set_pwm_direct(channel, on, off);
  }
  PcaCommand command = {};
  command.type = PcaCommandType::PwmGroup;
  command.start_channel = channel;
  command.count = 1;
  command.off_ticks[0] = off;
  return enqueue_pca_command(command);
}

bool pca9685_set_pwm_group(uint8_t start_channel, const uint16_t *off_ticks,
                           uint8_t count) {
  if (pca_should_execute_direct()) {
    return pca9685_set_pwm_group_direct(start_channel, off_ticks, count);
  }
  if (!off_ticks || count == 0 || count > PCA_TOTAL_CHANNELS) {
    return false;
  }
  PcaCommand command = {};
  command.type = PcaCommandType::PwmGroup;
  command.start_channel = start_channel;
  command.count = count;
  memcpy(command.off_ticks, off_ticks, sizeof(uint16_t) * count);
  return enqueue_pca_command(command);
}

bool pca9685_set_servo_angle(uint8_t channel, float angle) {
  if (pca_should_execute_direct()) {
    return pca9685_set_servo_angle_direct(channel, angle);
  }
  PcaCommand command = {};
  command.type = PcaCommandType::AngleGroup;
  command.start_channel = channel;
  command.count = 1;
  command.angles[0] = angle;
  return enqueue_pca_command(command);
}

bool pca9685_set_servo_speed(uint8_t channel, int speed) {
  if (pca_should_execute_direct()) {
    return pca9685_set_servo_speed_direct(channel, speed);
  }
  PcaCommand command = {};
  command.type = PcaCommandType::SpeedGroup;
  command.start_channel = channel;
  command.count = 1;
  command.speed = speed;
  return enqueue_pca_command(command);
}

bool pca9685_set_servo_speed_group(uint8_t start_channel, uint8_t count,
                                   int speed) {
  if (pca_should_execute_direct()) {
    return pca9685_set_servo_speed_group_direct(start_channel, count, speed);
  }
  PcaCommand command = {};
  command.type = PcaCommandType::SpeedGroup;
  command.start_channel = start_channel;
  command.count = count;
  command.speed = speed;
  return enqueue_pca_command(command);
}

bool pca9685_set_servo_angle_group(uint8_t start_channel, const float *angles,
                                   uint8_t count) {
  if (pca_should_execute_direct()) {
    return pca9685_set_servo_angle_group_direct(start_channel, angles, count);
  }
  if (angles == nullptr || count == 0 || count > PCA_TOTAL_CHANNELS) {
    return false;
  }
  PcaCommand command = {};
  command.type = PcaCommandType::AngleGroup;
  command.start_channel = start_channel;
  command.count = count;
  memcpy(command.angles, angles, sizeof(float) * count);
  return enqueue_pca_command(command);
}

bool pca9685_set_frequency(float freq_hz) {
  if (pca_should_execute_direct()) {
    return pca9685_set_frequency_direct(freq_hz);
  }
  PcaCommand command = {};
  command.type = PcaCommandType::SetFrequency;
  command.freq_hz = freq_hz;
  return enqueue_pca_command(command);
}

void pca9685_set_output_enable(bool enable) {
  if (pca_should_execute_direct()) {
    pca9685_set_output_enable_direct(enable);
    return;
  }
  PcaCommand command = {};
  command.type = PcaCommandType::SetOutputEnable;
  command.enable = enable;
  (void)enqueue_pca_command(command);
}

bool pca9685_is_ready() { return g_pca_initialized; }

bool pca9685_get_output_enable() { return g_oe_enabled; }

// --- Calibration API ---

PCA9685_ChannelCal_t *pca9685_get_cal(uint8_t channel) {
  if (channel < 1 || channel > PCA_TOTAL_CHANNELS)
    return NULL;
  return &g_cal[channel - 1];
}

void pca9685_set_cal(uint8_t channel, const PCA9685_ChannelCal_t *cal) {
  if (channel < 1 || channel > PCA_TOTAL_CHANNELS)
    return;
  if (!cal)
    return;
  g_cal[channel - 1] = *cal;
  normalize_cal(&g_cal[channel - 1]);
}

void pca9685_save_cal() {
  mros::platform::NvsNamespace cal_prefs;
  if (!cal_prefs.open("pca_cal", false,
                      mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    return;
  }
  cal_prefs.set_blob("data", g_cal, sizeof(g_cal));
  cal_prefs.set_u32("osc", g_pca9685_osc_freq);
}

void pca9685_load_cal() {
  // Set defaults first
  pca9685_reset_cal();
  g_pca9685_osc_freq = 29331500UL; //Do not change this value. It is calibrated for 50Hz.

  mros::platform::NvsNamespace cal_prefs;
  if (!cal_prefs.open("pca_cal", true,
                      mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    return;
  }
  size_t len = cal_prefs.get_blob_size("data");
  if (len == sizeof(g_cal)) {
    cal_prefs.get_blob("data", g_cal, sizeof(g_cal));
    for (int i = 0; i < PCA_TOTAL_CHANNELS; i++) {
      normalize_cal(&g_cal[i]);
    }
  }
  uint32_t saved_osc = 0;
  (void)cal_prefs.get_u32("osc", &saved_osc);
  if (saved_osc != 0) {
    g_pca9685_osc_freq = saved_osc;
  }
}

void pca9685_reset_cal() {
  for (int i = 0; i < PCA_TOTAL_CHANNELS; i++) {
    set_defaults(&g_cal[i]);
    g_last_valid[i] = false;
  }
}
