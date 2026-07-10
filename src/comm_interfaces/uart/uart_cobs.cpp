#include "uart_cobs.h"
#include "src/config/pin_config.h"
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "WString.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string.h>
#include "src/drivers/utils/mros_console.h"
#include "src/comm_interfaces/protocols/protocol_def.h"
#include "src/platform/mros_uart.h"
#include "src/security/uart_secure.h"
#include "src/shell/mshell_remote.h"
#include "src/web/server/web_server.h"
#include "src/web/server/wifi_manager.h"

static unsigned long uart_timer = 0;
volatile uint32_t systemLogsVersion = 0;
static constexpr const char* kUartTag = "UART";
static constexpr size_t kSystemLogsCapacityBytes = 256 * 1024;
#ifndef MROS_USE_STATIC_PSRAM_SYSTEM_LOGS
#define MROS_USE_STATIC_PSRAM_SYSTEM_LOGS 0
#endif
#if MROS_USE_STATIC_PSRAM_SYSTEM_LOGS && \
    defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
static EXT_RAM_BSS_ATTR char g_system_logs_storage[kSystemLogsCapacityBytes];
static char *g_system_logs = g_system_logs_storage;
#else
static char *g_system_logs = nullptr;
#endif
static size_t g_system_logs_head = 0;
static size_t g_system_logs_size = 0;
static size_t g_system_logs_total_written = 0;
static SemaphoreHandle_t g_system_logs_mutex = nullptr;
[[maybe_unused]] static bool g_system_logs_init_failed = false;
static constexpr size_t kT41CobsFrameBufferSize = 1536U;
static uint8_t g_cobs_frame_buf_t41[kT41CobsFrameBufferSize] = {};
static size_t g_cobs_frame_len_t41 = 0U;
static bool g_cobs_frame_truncated_t41 = false;
static volatile uint32_t g_system_logs_append_count = 0;
static volatile uint32_t g_system_logs_full_copy_count = 0;
static volatile uint32_t g_system_logs_since_copy_count = 0;
static volatile uint32_t g_system_logs_snapshot_string_count = 0;
static volatile uint32_t g_system_logs_lock_fail_count = 0;
static volatile UartLogNoiseMode g_uart_log_noise_mode = UartLogNoiseMode::Normal;

static inline void increment_counter(volatile uint32_t* counter) {
  if (counter == nullptr) {
    return;
  }
  *counter = *counter + 1U;
}

static bool lockSystemLogs(TickType_t timeout = pdMS_TO_TICKS(20)) {
  if (g_system_logs_mutex == nullptr) {
    return false;
  }
  return xSemaphoreTake(g_system_logs_mutex, timeout) == pdTRUE;
}

static void unlockSystemLogs() {
  if (g_system_logs_mutex != nullptr) {
    xSemaphoreGive(g_system_logs_mutex);
  }
}

static bool ensureSystemLogsStorageReady() {
  if (g_system_logs_mutex == nullptr) {
    g_system_logs_mutex = xSemaphoreCreateMutex();
    if (g_system_logs_mutex == nullptr) {
      return false;
    }
  }

#if MROS_USE_STATIC_PSRAM_SYSTEM_LOGS && \
    defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
  return true;
#else
  if (g_system_logs != nullptr) {
    return true;
  }

  if (esp_psram_get_size() == 0) {
    if (!g_system_logs_init_failed) {
      ESP_LOGW(kUartTag, "PSRAM unavailable, system logs ring disabled");
      g_system_logs_init_failed = true;
    }
    return false;
  }

  if (!lockSystemLogs(pdMS_TO_TICKS(100))) {
    return false;
  }
  if (g_system_logs == nullptr) {
    g_system_logs = static_cast<char *>(
        heap_caps_malloc(kSystemLogsCapacityBytes,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_system_logs == nullptr) {
      if (!g_system_logs_init_failed) {
        ESP_LOGW(kUartTag, "Failed to allocate %u-byte PSRAM system log ring",
                 static_cast<unsigned>(kSystemLogsCapacityBytes));
        g_system_logs_init_failed = true;
      }
    } else {
      g_system_logs_head = 0;
      g_system_logs_size = 0;
      g_system_logs_init_failed = false;
    }
  }
  const bool ok = (g_system_logs != nullptr);
  unlockSystemLogs();
  return ok;
#endif
}

static inline void writeSystemLogChar(char c) {
  g_system_logs[g_system_logs_head] = c;
  g_system_logs_head = (g_system_logs_head + 1) % kSystemLogsCapacityBytes;
  if (g_system_logs_size < kSystemLogsCapacityBytes) {
    g_system_logs_size++;
  }
  g_system_logs_total_written++;
}

static void writeSystemLogBytes(const char *data, size_t len) {
  if (data == nullptr || len == 0 || g_system_logs == nullptr) {
    return;
  }
  for (size_t i = 0; i < len; i++) {
    writeSystemLogChar(data[i]);
  }
}

static bool isReadableLogChar(uint8_t c) {
  return (c == '\t') || (c >= 32 && c <= 126);
}

static bool isLikelyNoiseLine(const String &line) {
  int len = 0;
  int changes = 0;
  char prev = 0;
  for (size_t i = 0; i < line.length(); i++) {
    char c = line[i];
    if (c == '\r' || c == '\n')
      continue;
    len++;
    if (len > 1 && c != prev)
      changes++;
    prev = c;
  }
  if (len <= 0)
    return true;
  // Repetitive or too long lines are usually binary noise over UART.
  if (len > 260)
    return true;
  if (len >= 40 && changes < (len / 12))
    return true;
  return false;
}

static String sanitizeLogLine(const String &line) {
  String out;
  out.reserve(line.length() + 1);
  for (size_t i = 0; i < line.length(); i++) {
    uint8_t c = (uint8_t)line[i];
    if (c == '\r')
      continue;
    if (c == '\n' || isReadableLogChar(c)) {
      out += (char)c;
    } else {
      out += '?';
    }
  }
  if (!out.endsWith("\n")) {
    out += '\n';
  }
  if (out.length() > 280) {
    out = out.substring(0, 280);
    if (!out.endsWith("\n"))
      out += '\n';
  }
  return out;
}

static String sanitizeT41LogLine(const String &line) {
  String out;
  out.reserve(line.length() + 1);
  for (size_t i = 0; i < line.length(); i++) {
    uint8_t c = (uint8_t)line[i];
    if (c == '\r') continue;
    if (c == '\n' || c == '\t' || (c >= 32 && c <= 126)) {
      out += (char)c;
    } else {
      out += '?';
    }
  }
  if (!out.endsWith("\n")) out += '\n';
  // Keep t41 log lines larger to avoid dropping detailed traces.
  if (out.length() > 1024) {
    out = out.substring(0, 1024);
    if (!out.endsWith("\n")) out += '\n';
  }
  return out;
}

static bool isImportantLogLine(const String &line) {
  return line.indexOf("ERR") >= 0 || line.indexOf("Error") >= 0 ||
         line.indexOf("ERROR") >= 0 || line.indexOf("WARN") >= 0 ||
         line.indexOf("FAIL") >= 0 || line.indexOf("PANIC") >= 0 ||
         line.indexOf("[RTOS]") >= 0 || line.indexOf("[STORAGE]") >= 0;
}

// --- CRC8 Algorithm ---
static uint8_t calc_crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if ((crc & 0x80) != 0)
        crc = (uint8_t)((crc << 1) ^ 0x07);
      else
        crc <<= 1;
    }
  }
  return crc;
}

// --- COBS Encoder ---
static size_t COBS_Encode(const void *data, size_t length, uint8_t *buffer) {
  if (buffer == nullptr || (data == nullptr && length != 0U)) return 0U;
  const uint8_t* input = static_cast<const uint8_t*>(data);
  size_t read_index = 0U;
  size_t write_index = 1U;
  size_t code_index = 0U;
  uint8_t code = 1U;
  while (read_index < length) {
    if (input[read_index] == 0U) {
      buffer[code_index] = code;
      code = 1U;
      code_index = write_index++;
      ++read_index;
      continue;
    }
    buffer[write_index++] = input[read_index++];
    ++code;
    if (code == 0xFFU) {
      buffer[code_index] = code;
      code = 1U;
      code_index = write_index++;
    }
  }
  buffer[code_index] = code;
  return write_index;
}

static bool COBS_Decode(const uint8_t* data,
                        const size_t length,
                        uint8_t* out,
                        const size_t out_capacity,
                        size_t* out_len) {
  if (data == nullptr || out == nullptr || out_len == nullptr) {
    return false;
  }
  *out_len = 0U;
  size_t read_index = 0U;
  size_t write_index = 0U;
  while (read_index < length) {
    const uint8_t code = data[read_index++];
    if (code == 0U) {
      return false;
    }
    const size_t copy_len = static_cast<size_t>(code - 1U);
    if (read_index + copy_len > length || write_index + copy_len > out_capacity) {
      return false;
    }
    for (size_t i = 0U; i < copy_len; ++i) {
      out[write_index++] = data[read_index++];
    }
    if (code != 0xFFU && read_index < length) {
      if (write_index >= out_capacity) {
        return false;
      }
      out[write_index++] = 0U;
    }
  }
  *out_len = write_index;
  return true;
}

static bool write_cobs_payload(const uint8_t* data, const size_t len) {
  if ((data == nullptr && len != 0U) || len > kT41CobsFrameBufferSize) return false;
  uint8_t encoded[kT41CobsFrameBufferSize + 8U] = {};
  const size_t encoded_len = COBS_Encode(data, len, encoded);
  if (encoded_len == 0U || encoded_len + 1U > sizeof(encoded)) return false;
  encoded[encoded_len] = 0U;
  return mros::platform::mros_uart_write(UART_NUM_1, encoded, encoded_len + 1U) ==
         static_cast<int>(encoded_len + 1U);
}

static bool seal_and_write(const uint8_t frame_type, const uint8_t* data, const size_t len) {
  std::vector<uint8_t> sealed;
  std::string error;
  if (!mros::security::uart_secure::seal(frame_type, data, len, &sealed, &error)) {
    ESP_LOGW(kUartTag, "secure UART seal failed: %s", error.c_str());
    return false;
  }
  return write_cobs_payload(sealed.data(), sealed.size());
}

static mros::platform::UartConfig uart1_config() {
  mros::platform::UartConfig config = {};
  config.port = UART_NUM_1;
  config.tx_pin = PIN_UART1_TX;
  config.rx_pin = PIN_UART1_RX;
  config.rts_pin = PIN_UART1_RTS;
  config.cts_pin = PIN_UART1_CTS;
  config.baud_rate = 5000000;
  config.flow_control = (PIN_UART1_CTS >= 0) ? UART_HW_FLOWCTRL_CTS
                                             : UART_HW_FLOWCTRL_DISABLE;
  config.rx_buffer_size = 4096;
  config.tx_buffer_size = 4096;
  config.queue_size = 20;
  return config;
}

static void copy_status_string(char* dst, size_t capacity, const char* src) {
  if (dst == nullptr || capacity == 0U) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  std::strncpy(dst, src, capacity - 1U);
  dst[capacity - 1U] = '\0';
}

static void fill_status_ip(uint8_t ip[4], const String& text) {
  if (ip == nullptr) {
    return;
  }
  unsigned int a = 0U;
  unsigned int b = 0U;
  unsigned int c = 0U;
  unsigned int d = 0U;
  if (std::sscanf(text.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
    ip[0] = static_cast<uint8_t>(a);
    ip[1] = static_cast<uint8_t>(b);
    ip[2] = static_cast<uint8_t>(c);
    ip[3] = static_cast<uint8_t>(d);
  }
}

void uart1_cobs_init() {
  mros::security::uart_secure::init();
  QueueHandle_t event_queue = nullptr;
  if (!mros::platform::mros_uart_init(uart1_config(), &event_queue)) {
    ESP_LOGE(kUartTag, "UART1 init failed");
  } else {
    (void)event_queue;
    (void)uart1_cobs_send_text_line("STATUS:BOOT");
  }
  (void)ensureSystemLogsStorageReady();
}

bool uart1_cobs_install_driver(QueueHandle_t* out_event_queue) {
  return mros::platform::mros_uart_init(uart1_config(), out_event_queue);
}

static void sendUARTStatus() {
  UART_Wifi_Status_t stat_pkt = {};
  stat_pkt.header0 = UART_HEADER_WIFI_STAT;
  stat_pkt.header1 = UART_HEADER_WIFI_STAT_2;
  stat_pkt.protocol_ver = UART_LINK_PROTOCOL_VERSION;
  stat_pkt.type = UART_TYPE_WIFI_STAT;
  stat_pkt.payload_len = static_cast<uint8_t>(sizeof(UART_Wifi_Status_t) - 6U - 1U);
  stat_pkt.peer_id = UART_PEER_TEENSY41;
  WifiManagerSnapshot wifi = {};
  wifi_manager_get_snapshot(&wifi);

  if (!wifi_manager_is_enabled()) {
    stat_pkt.wifi_status = 0;
    stat_pkt.rssi = 0;
    copy_status_string(stat_pkt.ssid, sizeof(stat_pkt.ssid), "Disabled");
  } else if (wifi.state.sta_connected) {
    stat_pkt.wifi_status = 1;
    fill_status_ip(stat_pkt.ip, wifi.ip);
    copy_status_string(stat_pkt.ssid, sizeof(stat_pkt.ssid), wifi.ssid.c_str());
    stat_pkt.rssi = static_cast<int8_t>(wifi.state.rssi);
  } else {
    stat_pkt.wifi_status = wifi.state.sta_connecting ? 2U : 1U;
    stat_pkt.rssi = 0;
    if (wifi.ssid.length() != 0U) {
      copy_status_string(stat_pkt.ssid, sizeof(stat_pkt.ssid), wifi.ssid.c_str());
    } else {
      copy_status_string(stat_pkt.ssid, sizeof(stat_pkt.ssid), "Enabled");
    }
  }

  char client_status[20] = {};
  std::snprintf(client_status,
                sizeof(client_status),
                "clients:%lu",
                static_cast<unsigned long>(web_server_total_ws_client_count()));
  copy_status_string(stat_pkt.user, sizeof(stat_pkt.user), client_status);

  stat_pkt.crc8 =
      calc_crc8(reinterpret_cast<const uint8_t*>(&stat_pkt), sizeof(UART_Wifi_Status_t) - 1U);

  (void)seal_and_write(2U, reinterpret_cast<const uint8_t*>(&stat_pkt), sizeof(stat_pkt));
}

void appendSystemLog(const char* source, String line) {
  const bool from_t41 = (source != nullptr && strcmp(source, "t41") == 0);
  String clean = from_t41 ? sanitizeT41LogLine(line) : sanitizeLogLine(line);
  const UartLogNoiseMode mode = g_uart_log_noise_mode;
  if (mode == UartLogNoiseMode::Quiet && !isImportantLogLine(clean)) return;
  if (mode == UartLogNoiseMode::Normal && !from_t41 && isLikelyNoiseLine(clean)) return;

  if (!ensureSystemLogsStorageReady()) {
    return;
  }
  if (!lockSystemLogs(pdMS_TO_TICKS(20))) {
    increment_counter(&g_system_logs_lock_fail_count);
    return;
  }

  if (source != nullptr && source[0] != '\0') {
    writeSystemLogBytes(source, strlen(source));
    writeSystemLogBytes(": ", 2);
  }
  writeSystemLogBytes(clean.c_str(), clean.length());
  increment_counter(&systemLogsVersion);
  increment_counter(&g_system_logs_append_count);
  unlockSystemLogs();
}

bool uart1_cobs_send_text_line(const char* text) {
  if (text == nullptr) {
    return false;
  }
  return seal_and_write(1U, reinterpret_cast<const uint8_t*>(text), std::strlen(text));
}

bool uart1_cobs_send_binary_frame(const uint8_t* data, const size_t len) {
  if (data == nullptr || len == 0U || len > 1200U) {
    return false;
  }
  return seal_and_write(2U, data, len);
}

void uart1_cobs_set_log_noise_mode(UartLogNoiseMode mode) {
  if (mode != UartLogNoiseMode::Quiet && mode != UartLogNoiseMode::Normal &&
      mode != UartLogNoiseMode::Verbose) {
    mode = UartLogNoiseMode::Normal;
  }
  g_uart_log_noise_mode = mode;
}

UartLogNoiseMode uart1_cobs_get_log_noise_mode() {
  return g_uart_log_noise_mode;
}

const char* uart1_cobs_log_noise_mode_name(UartLogNoiseMode mode) {
  switch (mode) {
    case UartLogNoiseMode::Quiet:
      return "quiet";
    case UartLogNoiseMode::Verbose:
      return "verbose";
    case UartLogNoiseMode::Normal:
    default:
      return "normal";
  }
}

static void process_decoded_payload(const uint8_t* decoded, const size_t decoded_len) {
  std::vector<uint8_t> plaintext;
  uint8_t frame_type = 0U;
  std::string error;
  if (!mros::security::uart_secure::open(decoded, decoded_len, &plaintext, &frame_type, &error)) {
    ESP_LOGW(kUartTag, "secure UART open failed: %s frame_len=%lu",
             error.c_str(), static_cast<unsigned long>(decoded_len));
    return;
  }
  const bool is_msh1 = plaintext.size() >= 4U &&
                       std::memcmp(plaintext.data(), "MSH1", 4U) == 0;
  if (frame_type == 2U || is_msh1) {
    if (is_msh1) {
      (void)mros::shell::remote::handle_uart_binary_frame(plaintext.data(), plaintext.size());
    } else {
      mros::shell::remote::note_plain_uart_activity();
    }
    return;
  }
  if (plaintext.empty()) return;
  std::string line(plaintext.begin(), plaintext.end());
  if (mros::shell::remote::handle_uart_line(line.c_str())) return;

  String line_with_nl(line.c_str());
  line_with_nl += '\n';
  appendSystemLog("t41", line_with_nl);
  mros::shell::remote::note_plain_uart_activity();
  if (line == "CMD:WIFI_ON") {
    wifi_manager_set_enabled(true);
    wifi_manager_request_reconnect();
  } else if (line == "CMD:WIFI_OFF") {
    wifi_manager_set_enabled(false);
  }
}

static void process_uart1_input() {
  uint8_t b = 0U;
  while (mros::platform::mros_uart_available(UART_NUM_1) > 0 &&
         mros::platform::mros_uart_read_byte(UART_NUM_1, &b, 0)) {
    if (b == 0U) {
      if (g_cobs_frame_len_t41 > 0U && !g_cobs_frame_truncated_t41) {
        uint8_t decoded[kT41CobsFrameBufferSize] = {};
        size_t decoded_len = 0U;
        const bool decoded_ok = COBS_Decode(g_cobs_frame_buf_t41,
                                            g_cobs_frame_len_t41,
                                            decoded,
                                            sizeof(decoded),
                                            &decoded_len);
        if (decoded_ok) {
          process_decoded_payload(decoded, decoded_len);
        } else {
          mros::shell::remote::note_uart_binary_decode_error();
        }
      } else if (g_cobs_frame_truncated_t41) {
        mros::shell::remote::note_uart_binary_decode_error();
      }
      g_cobs_frame_len_t41 = 0U;
      g_cobs_frame_truncated_t41 = false;
      continue;
    }

    if (g_cobs_frame_len_t41 < kT41CobsFrameBufferSize) {
      g_cobs_frame_buf_t41[g_cobs_frame_len_t41++] = b;
    } else {
      g_cobs_frame_truncated_t41 = true;
    }

  }
}

void uart1_cobs_handle_uart_event(const uart_event_t& event, unsigned long now_ms) {
  (void)event;
  process_uart1_input();
  uart1_cobs_periodic(now_ms);
}

void uart1_cobs_periodic(unsigned long now) {
  process_uart1_input();

  if (now - uart_timer > 1000) {
    uart_timer = now;
    sendUARTStatus();
  }

  // 3. Process any local S3 logs generated via mros_console
  mros_console.process();
}

void uart1_cobs_loop(unsigned long now) {
  uart1_cobs_periodic(now);
}

uint32_t uart1_cobs_get_log_version() { return systemLogsVersion; }

size_t uart1_cobs_get_system_logs_size() {
  if (!ensureSystemLogsStorageReady()) {
    return 0U;
  }
  if (!lockSystemLogs(pdMS_TO_TICKS(100))) {
    increment_counter(&g_system_logs_lock_fail_count);
    return 0U;
  }
  const size_t len = g_system_logs_size;
  unlockSystemLogs();
  return len;
}

size_t uart1_cobs_get_system_logs_base_offset() {
  if (!ensureSystemLogsStorageReady()) {
    return 0U;
  }
  if (!lockSystemLogs(pdMS_TO_TICKS(100))) {
    increment_counter(&g_system_logs_lock_fail_count);
    return 0U;
  }
  const size_t base_offset =
      (g_system_logs_total_written >= g_system_logs_size)
          ? (g_system_logs_total_written - g_system_logs_size)
          : 0U;
  unlockSystemLogs();
  return base_offset;
}

bool uart1_cobs_copy_system_logs(char* dst, size_t capacity, size_t* out_len) {
  if (out_len != nullptr) {
    *out_len = 0U;
  }
  if (dst == nullptr || capacity == 0U) {
    return false;
  }
  dst[0] = '\0';
  if (!ensureSystemLogsStorageReady()) {
    return false;
  }
  if (!lockSystemLogs(pdMS_TO_TICKS(100))) {
    increment_counter(&g_system_logs_lock_fail_count);
    return false;
  }
  increment_counter(&g_system_logs_full_copy_count);

  const size_t len = g_system_logs_size;
  if (len == 0U) {
    unlockSystemLogs();
    return true;
  }

  const size_t copy_len = (len < (capacity - 1U)) ? len : (capacity - 1U);
  const size_t start =
      (g_system_logs_head + kSystemLogsCapacityBytes - copy_len) %
      kSystemLogsCapacityBytes;
  const size_t first_len =
      (start + copy_len <= kSystemLogsCapacityBytes)
          ? copy_len
          : (kSystemLogsCapacityBytes - start);
  const size_t second_len = copy_len - first_len;
  if (first_len > 0U) {
    memcpy(dst, g_system_logs + start, first_len);
  }
  if (second_len > 0U) {
    memcpy(dst + first_len, g_system_logs, second_len);
  }
  dst[copy_len] = '\0';
  if (out_len != nullptr) {
    *out_len = copy_len;
  }
  unlockSystemLogs();
  return true;
}

bool uart1_cobs_copy_system_logs_since(char* dst,
                                       size_t capacity,
                                       size_t offset,
                                       size_t max_bytes,
                                       size_t* out_len,
                                       size_t* out_next_offset,
                                       size_t* out_base_offset,
                                       bool* out_truncated) {
  if (out_len != nullptr) {
    *out_len = 0U;
  }
  if (out_next_offset != nullptr) {
    *out_next_offset = offset;
  }
  if (out_base_offset != nullptr) {
    *out_base_offset = 0U;
  }
  if (out_truncated != nullptr) {
    *out_truncated = false;
  }
  if (dst == nullptr || capacity == 0U) {
    return false;
  }
  dst[0] = '\0';
  if (!ensureSystemLogsStorageReady()) {
    return false;
  }
  if (!lockSystemLogs(pdMS_TO_TICKS(100))) {
    increment_counter(&g_system_logs_lock_fail_count);
    return false;
  }
  increment_counter(&g_system_logs_since_copy_count);

  const size_t newest_offset = g_system_logs_total_written;
  const size_t base_offset =
      (newest_offset >= g_system_logs_size)
          ? (newest_offset - g_system_logs_size)
          : 0U;
  if (out_base_offset != nullptr) {
    *out_base_offset = base_offset;
  }

  if (offset < base_offset) {
    offset = base_offset;
    if (out_truncated != nullptr) {
      *out_truncated = true;
    }
  } else if (offset > newest_offset) {
    offset = newest_offset;
  }

  const size_t safe_capacity = capacity - 1U;
  const size_t requested_bytes =
      max_bytes > 0U ? max_bytes : safe_capacity;
  const size_t available = newest_offset - offset;
  const size_t copy_len =
      (available < requested_bytes ? available : requested_bytes) < safe_capacity
          ? (available < requested_bytes ? available : requested_bytes)
          : safe_capacity;

  if (copy_len > 0U) {
    const size_t first_offset =
        (g_system_logs_head + kSystemLogsCapacityBytes - g_system_logs_size) %
        kSystemLogsCapacityBytes;
    const size_t logical_index = offset - base_offset;
    const size_t start = (first_offset + logical_index) % kSystemLogsCapacityBytes;
    const size_t first_len =
        (start + copy_len <= kSystemLogsCapacityBytes)
            ? copy_len
            : (kSystemLogsCapacityBytes - start);
    const size_t second_len = copy_len - first_len;
    if (first_len > 0U) {
      memcpy(dst, g_system_logs + start, first_len);
    }
    if (second_len > 0U) {
      memcpy(dst + first_len, g_system_logs, second_len);
    }
  }

  dst[copy_len] = '\0';
  if (out_len != nullptr) {
    *out_len = copy_len;
  }
  if (out_next_offset != nullptr) {
    *out_next_offset = offset + copy_len;
  }
  unlockSystemLogs();
  return true;
}

String uart1_cobs_get_system_logs_snapshot() {
  String out;
  if (!ensureSystemLogsStorageReady()) {
    return out;
  }
  if (!lockSystemLogs(pdMS_TO_TICKS(100))) {
    increment_counter(&g_system_logs_lock_fail_count);
    return out;
  }
  increment_counter(&g_system_logs_snapshot_string_count);

  const size_t len = g_system_logs_size;
  if (len == 0) {
    unlockSystemLogs();
    return out;
  }

  out.reserve(len + 1);
  const size_t start = (g_system_logs_head + kSystemLogsCapacityBytes - len) %
                       kSystemLogsCapacityBytes;
  const size_t first_len =
      (start + len <= kSystemLogsCapacityBytes)
          ? len
          : (kSystemLogsCapacityBytes - start);
  const size_t second_len = len - first_len;
  if (first_len > 0) {
    out.concat(g_system_logs + start, static_cast<unsigned int>(first_len));
  }
  if (second_len > 0) {
    out.concat(g_system_logs, static_cast<unsigned int>(second_len));
  }

  unlockSystemLogs();
  return out;
}

void uart1_cobs_clear_system_logs() {
  if (!ensureSystemLogsStorageReady()) {
    return;
  }
  if (!lockSystemLogs(pdMS_TO_TICKS(100))) {
    increment_counter(&g_system_logs_lock_fail_count);
    return;
  }

  g_system_logs_head = 0;
  g_system_logs_size = 0;
  g_system_logs_total_written = 0;
  increment_counter(&systemLogsVersion);

  unlockSystemLogs();
}

void uart1_cobs_get_diag_snapshot(UartLogDiagSnapshot* snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->log_capacity = kSystemLogsCapacityBytes;
  snapshot->revision = systemLogsVersion;
  snapshot->append_count = g_system_logs_append_count;
  snapshot->full_copy_count = g_system_logs_full_copy_count;
  snapshot->since_copy_count = g_system_logs_since_copy_count;
  snapshot->snapshot_string_count = g_system_logs_snapshot_string_count;
  snapshot->lock_fail_count = g_system_logs_lock_fail_count;
  snapshot->storage_allocated = (g_system_logs != nullptr);
  snapshot->storage_static =
#if MROS_USE_STATIC_PSRAM_SYSTEM_LOGS && \
    defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
      true;
#else
      false;
#endif
  snapshot->storage_init_failed = g_system_logs_init_failed;

  if (!ensureSystemLogsStorageReady()) {
    return;
  }
  snapshot->storage_allocated = (g_system_logs != nullptr);
  snapshot->storage_init_failed = g_system_logs_init_failed;
  if (!lockSystemLogs(pdMS_TO_TICKS(20))) {
    increment_counter(&g_system_logs_lock_fail_count);
    return;
  }
  snapshot->log_size = g_system_logs_size;
  snapshot->total_written = g_system_logs_total_written;
  snapshot->base_offset =
      (g_system_logs_total_written >= g_system_logs_size)
          ? (g_system_logs_total_written - g_system_logs_size)
          : 0U;
  unlockSystemLogs();
}
