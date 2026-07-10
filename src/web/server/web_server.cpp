#include "web_server.h"
#include "trajectory_handler.h"
#include "wifi_manager.h"
#include "web_async_stream.h"
#include "src/app/fw_kinematics.h"
#include "src/drivers/i2c/pca9685_driver.h"
#include "src/config/pin_config.h"
#include "src/core/memory/memory_monitor.h"
#include "src/core/debug/mros_debug_tools.h"
#include "src/core/rtos/device_process_manager.h"
#include "src/core/power/power_manager.h"
#include "src/core/rtos/task_manager.h"
#include "src/shell/mros_shell.h"
#include "src/shell/mshell_remote.h"
#include "src/shell/mshell_runtime.h"
#include "src/shell/shell_service.h"
#include "src/drivers/spi/spi_slave_driver_s3.h"
#include "src/experimental/experimental_worker.h"
#include "src/kinematics/robot_model.h"
#include "src/drivers/storage/logger_driver.h"
#include "src/drivers/uart/uart1_cobs_driver.h"
#include "src/drivers/spi/spi_c3_master.h"
#include "src/platform/mros_file.h"
#include "src/platform/mros_fs.h"
#include "src/platform/mros_http_client.h"
#include "src/platform/mros_nvs.h"
#include "src/platform/mros_system.h"
#include "src/platform/mros_time.h"
#include "src/utils/mros_json_writer.h"
#include "src/net/mcp_service.h"
#include "src/net/ssh_service.h"
#include "src/security/ssh_identity.h"
#include "src/security/user_profile.h"
#include "ESPAsyncWebServer.h"
#include "WString.h"
#include <esp_attr.h>
#include <esp_chip_info.h>
#include <esp_cpu.h>
#include <esp_flash.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_image_format.h>
#include <esp_private/esp_clk.h>
#include <esp_private/esp_clk_utils.h>
#include <cJSON.h>
#include <cstdio>
#if defined(__has_include)
#if __has_include(<esp_pm.h>)
#include <esp_pm.h>
#define HAS_ESP_PM 1
#else
#define HAS_ESP_PM 0
#endif
#if __has_include(<soc/rtc.h>)
#include <soc/rtc.h>
#define HAS_IDF_CPU_CLOCK_CTRL 1
#else
#define HAS_IDF_CPU_CLOCK_CTRL 0
#endif
#else
#define HAS_ESP_PM 0
#define HAS_IDF_CPU_CLOCK_CTRL 0
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <memory>
#include <new>
#include <mbedtls/md.h>
#include <esp_random.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <ctime>
#include <dirent.h>
#include <map>
#include <cmath>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "src/drivers/utils/mros_console.h"

static constexpr bool kNativeHttpPrimaryEnabled =
#if defined(MROS_NATIVE_HTTP_PRIMARY) && MROS_NATIVE_HTTP_PRIMARY
    true;
#else
    false;
#endif
static constexpr uint16_t kNativeHttpPrimaryPort = 80U;
static constexpr const char *kNativeHttpPrimaryEngine = "esp_http_server_compat";
static constexpr bool kNativeHttpDirectLabEnabled =
#if defined(MROS_NATIVE_HTTP_DIRECT_LAB) && MROS_NATIVE_HTTP_DIRECT_LAB
    true;
#else
    false;
#endif
static constexpr uint16_t kNativeHttpDirectLabPort = 8081U;

static AsyncWebServer server(kNativeHttpPrimaryPort);
static AsyncWebSocket ws("/ws");
static AsyncWebSocket ws_telemetry("/ws/telemetry");
static AsyncWebSocket ws_shell("/ws-shell");
static AsyncWebSocket ws_shell_v2("/ws/shell");
static AsyncWebSocket ws_debug("/ws/debug");
static AsyncWebSocket ws_mcp("/ws/mcp");
static bool web_server_started = false;

static String jsonEscape(const String &in);
static String shellCaptureJson(const char *command);
static void start_native_http_direct_lab_once();

#ifndef CONFIG_ESPTOOLPY_FLASHFREQ
#define CONFIG_ESPTOOLPY_FLASHFREQ "unknown"
#endif
#ifndef CONFIG_SPIRAM_SPEED
#define CONFIG_SPIRAM_SPEED 0
#endif
static uint32_t config_freq_mhz_from_text(const char* text) {
  if (text == nullptr || text[0] == '\0') return 0U;
  return static_cast<uint32_t>(strtoul(text, nullptr, 10));
}

static uint32_t config_flash_freq_mhz() {
#if defined(CONFIG_ESPTOOLPY_FLASHFREQ_120M) && CONFIG_ESPTOOLPY_FLASHFREQ_120M
  return 120U;
#elif defined(CONFIG_ESPTOOLPY_FLASHFREQ_80M) && CONFIG_ESPTOOLPY_FLASHFREQ_80M
  return 80U;
#elif defined(CONFIG_ESPTOOLPY_FLASHFREQ_40M) && CONFIG_ESPTOOLPY_FLASHFREQ_40M
  return 40U;
#elif defined(CONFIG_ESPTOOLPY_FLASHFREQ_20M) && CONFIG_ESPTOOLPY_FLASHFREQ_20M
  return 20U;
#endif
  return config_freq_mhz_from_text(CONFIG_ESPTOOLPY_FLASHFREQ);
}

static uint32_t config_psram_freq_mhz() {
  return static_cast<uint32_t>(CONFIG_SPIRAM_SPEED);
}

static void start_web_server_once(const char *stage) {
  if (web_server_started) {
    return;
  }
  server.begin();
  start_native_http_direct_lab_once();
  web_server_started = true;
  ESP_LOGI("WEB", "Native HTTP primary server started on port %u via %s (%s)",
           static_cast<unsigned>(kNativeHttpPrimaryPort),
           kNativeHttpPrimaryEngine,
           stage != nullptr ? stage : "unknown");
}

static String current_token = "";
static String current_username = "";
static constexpr size_t kAuthUserMaxLen = 32U;
static constexpr size_t kAuthPassMaxLen = 96U;
static constexpr size_t kAuthSessionTokenLen = 48U;
static constexpr size_t kAuthCsrfTokenBytes = 16U;
static constexpr size_t kAuthCsrfTokenLen = kAuthCsrfTokenBytes * 2U;
static constexpr size_t kAuthSessionSlots = 4U;
static constexpr unsigned long kAuthSessionTtlMs = 30UL * 60UL * 1000UL;
static String generateSecureToken(size_t bytes);
struct WebAuthSession {
  String token;
  String csrf_token;
  String username;
  unsigned long issued_at_ms = 0;
  unsigned long last_seen_ms = 0;
  unsigned long expires_at_ms = 0;
};
static WebAuthSession g_auth_sessions[kAuthSessionSlots];
static uint32_t g_auth_session_issued = 0;
static uint32_t g_auth_session_expired = 0;
static uint32_t g_auth_session_evicted = 0;
static uint32_t g_auth_session_revoked = 0;
static SemaphoreHandle_t g_auth_state_lock = nullptr;
static SemaphoreHandle_t g_ws_auth_lock = nullptr;
static unsigned long last_ws_update = 0;
static unsigned long last_ws_medium_update = 0;
static unsigned long last_ws_slow_update = 0;
static unsigned long last_ws_debug_update = 0;
static uint32_t g_json_overflow_count = 0;
static uint32_t g_file_list_paged_count = 0;
static uint32_t g_ws_last_fast_bytes = 0;
static uint32_t g_ws_last_medium_bytes = 0;
static uint32_t g_ws_last_slow_bytes = 0;
static uint32_t g_ws_bin_frame_count = 0;
static uint32_t g_ws_bin_byte_count = 0;
static uint32_t g_ws_json_fallback_count = 0;
static uint32_t g_ws_format_error_count = 0;
static uint32_t g_ws_last_bin_bytes = 0;
static uint32_t g_ws_bin_seq = 0;
static uint32_t g_ws_scene_client_count = 0;
static uint32_t g_ws_debug_client_count = 0;
static uint32_t g_ws_background_client_count = 0;
static uint32_t g_ws_gated_scene_frames = 0;
static uint32_t g_ws_gated_medium_frames = 0;
static uint32_t g_ws_gated_slow_frames = 0;
static uint32_t g_ws_memory_budget_degrade_frames = 0;
static uint32_t g_ws_memory_budget_critical_frames = 0;
static uint32_t g_ws_memory_budget_scene_suppressed = 0;
static uint32_t g_ws_memory_budget_fast_suppressed = 0;
static uint32_t g_ws_memory_budget_medium_suppressed = 0;
static uint32_t g_ws_telemetry_budget_dropped = 0;
static uint32_t g_ws_telemetry_budget_deferred = 0;
static uint32_t g_ws_full_required_count = 0;
static uint32_t g_ws_memory_budget_last_free = 0;
static uint32_t g_ws_memory_budget_last_largest = 0;
static const char *g_ws_memory_budget_mode = "normal";
static uint32_t g_psram_lazy_alloc_count = 0;
static uint32_t g_psram_lazy_alloc_bytes = 0;
static uint32_t g_psram_internal_fallback_count = 0;
static uint32_t g_psram_internal_fallback_bytes = 0;
static uint32_t g_psram_alloc_fail_count = 0;
static bool g_shell_forward_lazy_allocated = false;
static bool g_trajectory_buffers_lazy_allocated = false;
static uint32_t g_trajectory_buffers_bytes = 0;
static uint32_t g_native_http_route_hits = 0;
static uint32_t g_native_http_fallback_hits = 0;
static uint32_t g_native_http_errors = 0;
static uint32_t g_native_http_send_bytes = 0;
static uint32_t g_native_http_ws_sends = 0;
static uint32_t g_native_http_async_queue_errors = 0;
static uint32_t g_native_http_active_sockets = 0;
static uint32_t g_native_http_lru_purge_count = 0;
static uint32_t g_shell_cbor_control_frames = 0;
static uint32_t g_shell_cbor_control_bytes = 0;
static uint32_t g_shell_cbor_control_errors = 0;
static uint32_t g_worker_decode_frames = 0;
static uint32_t g_worker_decode_errors = 0;
static uint32_t g_worker_decode_fallbacks = 0;
static uint32_t g_worker_decode_latency_ms = 0;
static uint32_t g_worker_decode_dropped = 0;
static httpd_handle_t g_native_httpd = nullptr;
static char *g_shell_json_forward_buf = nullptr;
static constexpr size_t kShellJsonForwardCapacity = 16384U;
static constexpr uint32_t kShellWsClientMask = 0x80000000UL;
enum class WsTicketScope : uint8_t;
struct WsTicketClaim;
static String fm_list_json(const std::string& path,
                           size_t offset,
                           size_t limit,
                           const char* sort_key,
                           const char* sort_dir);
static bool consumeWsTicket(const String& ticket,
                            WsTicketScope expected_scope,
                            WsTicketClaim* claim = nullptr);
#ifndef MROS_USE_STATIC_PSRAM_WEB_BUFFERS
#define MROS_USE_STATIC_PSRAM_WEB_BUFFERS 0
#endif
#if MROS_USE_STATIC_PSRAM_WEB_BUFFERS && \
    defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
static EXT_RAM_BSS_ATTR char g_shell_json_forward_storage[kShellJsonForwardCapacity];
#endif

#if defined(MROS_NATIVE_HTTP_DIRECT_LAB) && MROS_NATIVE_HTTP_DIRECT_LAB
static esp_err_t native_status_handler(httpd_req_t *req) {
  g_native_http_route_hits++;
  char body[384] = {};
  std::snprintf(body, sizeof(body),
                "{\"success\":true,\"native_direct_lab\":true,\"native_primary\":false,"
                "\"route\":\"/api/status\",\"t41_connected\":%s,\"loop_ms\":%lu,"
                "\"turret\":%.1f,\"j1\":%.1f,\"motor\":%ld,"
                "\"uptime_ms\":%lu,\"heap_free\":%lu,\"port\":%u}",
                spi_s3_is_connected() ? "true" : "false",
                static_cast<unsigned long>(spi_s3_get_loop_ms()),
                static_cast<double>(spi_s3_get_turret_deg()),
                static_cast<double>(spi_s3_get_joint_deg(0)),
                static_cast<long>(spi_s3_get_motor_state()),
                static_cast<unsigned long>(mros::platform::mros_millis()),
                static_cast<unsigned long>(mros::platform::mros_system_heap_free()),
                static_cast<unsigned>(kNativeHttpDirectLabPort));
  httpd_resp_set_type(req, "application/json");
  g_native_http_send_bytes += std::strlen(body);
  return httpd_resp_sendstr(req, body);
}

static esp_err_t native_health_handler(httpd_req_t *req) {
  g_native_http_route_hits++;
  WifiManagerSnapshot wifi_snapshot {};
  wifi_manager_get_snapshot(&wifi_snapshot);
  char body[768] = {};
  std::snprintf(body, sizeof(body),
                "{\"success\":true,\"native\":true,\"native_direct_lab\":true,"
                "\"native_http_enabled\":true,\"native_http_port\":%u,"
                "\"native_http_mode\":\"direct-lab\",\"native_http_engine\":\"esp_http_server\","
                "\"native_primary_routes\":[\"/api/status\","
                "\"/api/health\",\"/api/files/list\",\"/api/logs/tail\",\"/ws/telemetry\"],"
                "\"native_route_hits\":%lu,\"native_fallback_hits\":%lu,"
                "\"native_http_errors\":%lu,\"native_http_send_bytes\":%lu,"
                "\"native_http_ws_sends\":%lu,"
                "\"worker_decode_enabled\":true,\"worker_decode_errors\":%lu,"
                "\"worker_decode_frames\":%lu,\"worker_decode_fallbacks\":%lu,"
                "\"wifi_snapshot_source\":\"%s\",\"wifi_dns_backend\":\"%s\","
                "\"wifi_event_rev\":%lu,"
                "\"uptime_ms\":%lu,\"heap_free\":%lu}",
                static_cast<unsigned>(kNativeHttpDirectLabPort),
                static_cast<unsigned long>(g_native_http_route_hits),
                static_cast<unsigned long>(g_native_http_fallback_hits),
                static_cast<unsigned long>(g_native_http_errors),
                static_cast<unsigned long>(g_native_http_send_bytes),
                static_cast<unsigned long>(g_native_http_ws_sends),
                static_cast<unsigned long>(g_worker_decode_errors),
                static_cast<unsigned long>(g_worker_decode_frames),
                static_cast<unsigned long>(g_worker_decode_fallbacks),
                wifi_snapshot.snapshot_source,
                wifi_snapshot.dns_backend,
                static_cast<unsigned long>(wifi_snapshot.event_revision),
                static_cast<unsigned long>(mros::platform::mros_millis()),
                static_cast<unsigned long>(mros::platform::mros_system_heap_free()));
  httpd_resp_set_type(req, "application/json");
  g_native_http_send_bytes += std::strlen(body);
  return httpd_resp_sendstr(req, body);
}

static esp_err_t native_logs_tail_handler(httpd_req_t *req) {
  g_native_http_route_hits++;
  size_t max_bytes = 8192U;
  char query[96] = {};
  char bytes_value[16] = {};
  char format_value[16] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (httpd_query_key_value(query, "bytes", bytes_value, sizeof(bytes_value)) == ESP_OK) {
      const long requested = std::strtol(bytes_value, nullptr, 10);
      if (requested > 0L) {
        max_bytes = static_cast<size_t>(std::min<long>(requested, 32768L));
      }
    }
    (void)httpd_query_key_value(query, "format", format_value, sizeof(format_value));
  }
  const String tail = logger_read_csv_tail(max_bytes);
  const bool wants_text =
      std::strcmp(format_value, "text") == 0 ||
      std::strcmp(format_value, "raw") == 0 ||
      std::strcmp(format_value, "plain") == 0;
  httpd_resp_set_type(req, wants_text ? "text/plain; charset=utf-8" : "text/csv");
  g_native_http_send_bytes += tail.length();
  return httpd_resp_send(req, tail.c_str(), tail.length());
}

static esp_err_t native_static_smoke_handler(httpd_req_t *req) {
  g_native_http_route_hits++;
  static constexpr const char *kBody = "mros native http smoke ok\n";
  httpd_resp_set_type(req, "text/plain");
  g_native_http_send_bytes += std::strlen(kBody);
  return httpd_resp_sendstr(req, kBody);
}

static esp_err_t native_files_list_handler(httpd_req_t *req) {
  g_native_http_route_hits++;
  char query[192] = {};
  char path[96] = "/ESPUSER";
  char offset_value[16] = "0";
  char limit_value[16] = "64";
  char sort_value[16] = "name";
  char dir_value[8] = "asc";
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    (void)httpd_query_key_value(query, "path", path, sizeof(path));
    (void)httpd_query_key_value(query, "offset", offset_value, sizeof(offset_value));
    (void)httpd_query_key_value(query, "limit", limit_value, sizeof(limit_value));
    (void)httpd_query_key_value(query, "sort", sort_value, sizeof(sort_value));
    (void)httpd_query_key_value(query, "dir", dir_value, sizeof(dir_value));
  }
  const size_t offset = static_cast<size_t>(std::max<long>(0L, std::strtol(offset_value, nullptr, 10)));
  long limit_long = std::strtol(limit_value, nullptr, 10);
  if (limit_long <= 0L) limit_long = 64L;
  if (limit_long > 500L) limit_long = 500L;
  const String body = fm_list_json(path, offset, static_cast<size_t>(limit_long), sort_value, dir_value);
  httpd_resp_set_type(req, "application/json");
  g_native_http_send_bytes += body.length();
  return httpd_resp_send(req, body.c_str(), body.length());
}

static esp_err_t native_telemetry_ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    g_native_http_active_sockets++;
    return ESP_OK;
  }
  httpd_ws_frame_t frame = {};
  const esp_err_t recv_status = httpd_ws_recv_frame(req, &frame, 0);
  if (recv_status != ESP_OK) {
    g_native_http_async_queue_errors++;
    return recv_status;
  }
  if (frame.len > 0U && frame.len < 192U) {
    uint8_t control_buf[192] = {};
    frame.payload = control_buf;
    const esp_err_t payload_status = httpd_ws_recv_frame(req, &frame, frame.len);
    if (payload_status != ESP_OK) {
      g_native_http_async_queue_errors++;
      return payload_status;
    }
    if (frame.type == HTTPD_WS_TYPE_TEXT) {
      control_buf[std::min<size_t>(frame.len, sizeof(control_buf) - 1U)] = '\0';
      const char* msg = reinterpret_cast<const char*>(control_buf);
      if (std::strncmp(msg, "AUTH:", 5) == 0) {
        const String ticket(msg + 5);
        if (!consumeWsTicket(ticket, WsTicketScope::Telemetry)) {
          static constexpr const char* kAuthFail = "{\"auth\":\"fail\",\"native\":true}";
          httpd_ws_frame_t reply = {};
          reply.type = HTTPD_WS_TYPE_TEXT;
          reply.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(kAuthFail));
          reply.len = std::strlen(kAuthFail);
          g_native_http_errors++;
          g_native_http_send_bytes += reply.len;
          return httpd_ws_send_frame(req, &reply);
        }
        static constexpr const char* kAuthReply =
            "{\"auth\":\"ok\",\"channel\":\"telemetry\",\"protocol\":\"mros-web-v2\","
            "\"native\":true,\"formats\":[\"bin-v1\",\"json-v1\"],"
            "\"default_format\":\"bin-v1\"}";
        httpd_ws_frame_t reply = {};
        reply.type = HTTPD_WS_TYPE_TEXT;
        reply.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(kAuthReply));
        reply.len = std::strlen(kAuthReply);
        g_native_http_ws_sends++;
        g_native_http_send_bytes += reply.len;
        return httpd_ws_send_frame(req, &reply);
      }
      if (std::strcmp(msg, "FORMAT:json-v1") == 0 ||
          std::strcmp(msg, "FORMAT:bin-v1") == 0) {
        char reply_text[96] = {};
        std::snprintf(reply_text, sizeof(reply_text),
                      "{\"telemetry\":{\"type\":\"protocol\",\"format\":\"%s\",\"native\":true}}",
                      std::strstr(msg, "json") != nullptr ? "json-v1" : "bin-v1");
        httpd_ws_frame_t reply = {};
        reply.type = HTTPD_WS_TYPE_TEXT;
        reply.payload = reinterpret_cast<uint8_t*>(reply_text);
        reply.len = std::strlen(reply_text);
        g_native_http_ws_sends++;
        g_native_http_send_bytes += reply.len;
        return httpd_ws_send_frame(req, &reply);
      }
    }
  }
  uint8_t payload[12] = {'M', 'R', 'B', '1', 1, 12, 2, 0, 0, 0, 0, 0};
  frame.payload = payload;
  frame.len = sizeof(payload);
  frame.type = HTTPD_WS_TYPE_BINARY;
  g_native_http_ws_sends++;
  g_native_http_send_bytes += sizeof(payload);
  return httpd_ws_send_frame(req, &frame);
}
#endif

static void start_native_http_direct_lab_once() {
  if (!kNativeHttpDirectLabEnabled || g_native_httpd != nullptr) {
    return;
  }
#if defined(MROS_NATIVE_HTTP_DIRECT_LAB) && MROS_NATIVE_HTTP_DIRECT_LAB
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = kNativeHttpDirectLabPort;
  config.ctrl_port = static_cast<uint16_t>(kNativeHttpDirectLabPort + 1U);
  config.lru_purge_enable = true;
  config.max_uri_handlers = 8;
  if (httpd_start(&g_native_httpd, &config) != ESP_OK) {
    g_native_http_async_queue_errors++;
    g_native_httpd = nullptr;
    ESP_LOGW("WEB", "Native HTTP direct lab failed to start");
    return;
  }
  httpd_uri_t status_uri = {};
  status_uri.uri = "/api/status";
  status_uri.method = HTTP_GET;
  status_uri.handler = native_status_handler;
  httpd_register_uri_handler(g_native_httpd, &status_uri);

  httpd_uri_t files_uri = {};
  files_uri.uri = "/api/files/list";
  files_uri.method = HTTP_GET;
  files_uri.handler = native_files_list_handler;
  httpd_register_uri_handler(g_native_httpd, &files_uri);

  httpd_uri_t health_uri = {};
  health_uri.uri = "/api/health";
  health_uri.method = HTTP_GET;
  health_uri.handler = native_health_handler;
  httpd_register_uri_handler(g_native_httpd, &health_uri);

  httpd_uri_t logs_tail_uri = {};
  logs_tail_uri.uri = "/api/logs/tail";
  logs_tail_uri.method = HTTP_GET;
  logs_tail_uri.handler = native_logs_tail_handler;
  httpd_register_uri_handler(g_native_httpd, &logs_tail_uri);

  httpd_uri_t smoke_uri = {};
  smoke_uri.uri = "/native-smoke";
  smoke_uri.method = HTTP_GET;
  smoke_uri.handler = native_static_smoke_handler;
  httpd_register_uri_handler(g_native_httpd, &smoke_uri);

  httpd_uri_t telemetry_uri = {};
  telemetry_uri.uri = "/ws/telemetry";
  telemetry_uri.method = HTTP_GET;
  telemetry_uri.handler = native_telemetry_ws_handler;
  telemetry_uri.is_websocket = true;
  httpd_register_uri_handler(g_native_httpd, &telemetry_uri);
  ESP_LOGI("WEB", "Native HTTP direct lab started on port %u",
           static_cast<unsigned>(kNativeHttpDirectLabPort));
#endif
}

static void ensure_web_state_locks() {
  if (g_auth_state_lock == nullptr) {
    g_auth_state_lock = xSemaphoreCreateMutex();
  }
  if (g_ws_auth_lock == nullptr) {
    g_ws_auth_lock = xSemaphoreCreateMutex();
  }
}

static void auth_state_lock() {
  ensure_web_state_locks();
  if (g_auth_state_lock != nullptr) {
    xSemaphoreTake(g_auth_state_lock, portMAX_DELAY);
  }
}

static void auth_state_unlock() {
  if (g_auth_state_lock != nullptr) {
    xSemaphoreGive(g_auth_state_lock);
  }
}

static void ws_auth_lock() {
  ensure_web_state_locks();
  if (g_ws_auth_lock != nullptr) {
    xSemaphoreTake(g_ws_auth_lock, portMAX_DELAY);
  }
}

static void ws_auth_unlock() {
  if (g_ws_auth_lock != nullptr) {
    xSemaphoreGive(g_ws_auth_lock);
  }
}

static bool auth_token_format_ok(const String& token) {
  if (token.length() != kAuthSessionTokenLen) {
    return false;
  }
  for (size_t i = 0; i < token.length(); ++i) {
    const char ch = token[i];
    const bool hex = (ch >= '0' && ch <= '9') ||
                     (ch >= 'a' && ch <= 'f') ||
                     (ch >= 'A' && ch <= 'F');
    if (!hex) {
      return false;
    }
  }
  return true;
}

static void auth_clear_session_slot_locked(const size_t slot) {
  if (slot >= kAuthSessionSlots) {
    return;
  }
  g_auth_sessions[slot].token = "";
  g_auth_sessions[slot].csrf_token = "";
  g_auth_sessions[slot].username = "";
  g_auth_sessions[slot].issued_at_ms = 0;
  g_auth_sessions[slot].last_seen_ms = 0;
  g_auth_sessions[slot].expires_at_ms = 0;
}

static void auth_select_current_session_locked() {
  size_t selected = kAuthSessionSlots;
  unsigned long selected_seen = 0;
  for (size_t i = 0; i < kAuthSessionSlots; ++i) {
    if (g_auth_sessions[i].token.length() == 0U) {
      continue;
    }
    if (selected == kAuthSessionSlots ||
        static_cast<long>(g_auth_sessions[i].last_seen_ms - selected_seen) > 0) {
      selected = i;
      selected_seen = g_auth_sessions[i].last_seen_ms;
    }
  }
  if (selected == kAuthSessionSlots) {
    current_token = "";
    return;
  }
  current_token = g_auth_sessions[selected].token;
  current_username = g_auth_sessions[selected].username;
}

static void auth_prune_sessions_locked(const unsigned long now_ms) {
  bool changed = false;
  for (size_t i = 0; i < kAuthSessionSlots; ++i) {
    if (g_auth_sessions[i].token.length() > 0U &&
        static_cast<long>(now_ms - g_auth_sessions[i].expires_at_ms) >= 0) {
      auth_clear_session_slot_locked(i);
      g_auth_session_expired++;
      changed = true;
    }
  }
  if (changed) {
    auth_select_current_session_locked();
  }
}

static String auth_current_token_copy() {
  auth_state_lock();
  auth_prune_sessions_locked(mros::platform::mros_millis());
  const String token = current_token;
  auth_state_unlock();
  return token;
}

static String auth_current_username_copy() {
  auth_state_lock();
  auth_prune_sessions_locked(mros::platform::mros_millis());
  const String username = current_username;
  auth_state_unlock();
  return username;
}

static size_t auth_active_session_count() {
  auth_state_lock();
  auth_prune_sessions_locked(mros::platform::mros_millis());
  size_t count = 0;
  for (size_t i = 0; i < kAuthSessionSlots; ++i) {
    if (g_auth_sessions[i].token.length() > 0U) {
      count++;
    }
  }
  auth_state_unlock();
  return count;
}

static void auth_set_current_username(const String &username) {
  auth_state_lock();
  current_username = username;
  auth_state_unlock();
}

static void auth_set_session_state(const String &username, const String &token) {
  auth_state_lock();
  const unsigned long now = mros::platform::mros_millis();
  auth_prune_sessions_locked(now);
  size_t slot = kAuthSessionSlots;
  for (size_t i = 0; i < kAuthSessionSlots; ++i) {
    if (g_auth_sessions[i].token.length() == 0U) {
      slot = i;
      break;
    }
  }
  if (slot == kAuthSessionSlots) {
    slot = 0U;
    for (size_t i = 1; i < kAuthSessionSlots; ++i) {
      if (static_cast<long>(g_auth_sessions[slot].last_seen_ms -
                            g_auth_sessions[i].last_seen_ms) > 0) {
        slot = i;
      }
    }
    auth_clear_session_slot_locked(slot);
    g_auth_session_evicted++;
  }
  g_auth_sessions[slot].username = username;
  g_auth_sessions[slot].token = token;
  g_auth_sessions[slot].csrf_token = generateSecureToken(kAuthCsrfTokenBytes);
  g_auth_sessions[slot].issued_at_ms = now;
  g_auth_sessions[slot].last_seen_ms = now;
  g_auth_sessions[slot].expires_at_ms = now + kAuthSessionTtlMs;
  current_username = username;
  current_token = token;
  g_auth_session_issued++;
  auth_state_unlock();
}

static bool auth_validate_session_token(const String& token,
                                        String* out_username = nullptr,
                                        String* out_csrf_token = nullptr) {
  if (!auth_token_format_ok(token)) {
    return false;
  }
  auth_state_lock();
  const unsigned long now = mros::platform::mros_millis();
  auth_prune_sessions_locked(now);
  for (size_t i = 0; i < kAuthSessionSlots; ++i) {
    if (g_auth_sessions[i].token == token) {
      g_auth_sessions[i].last_seen_ms = now;
      current_token = g_auth_sessions[i].token;
      current_username = g_auth_sessions[i].username;
      if (out_username != nullptr) {
        *out_username = g_auth_sessions[i].username;
      }
      if (out_csrf_token != nullptr) {
        *out_csrf_token = g_auth_sessions[i].csrf_token;
      }
      auth_state_unlock();
      return true;
    }
  }
  auth_state_unlock();
  return false;
}

static String auth_csrf_token_for_session(const String& token) {
  if (!auth_token_format_ok(token)) {
    return "";
  }
  auth_state_lock();
  auth_prune_sessions_locked(mros::platform::mros_millis());
  for (size_t i = 0; i < kAuthSessionSlots; ++i) {
    if (g_auth_sessions[i].token == token) {
      const String csrf = g_auth_sessions[i].csrf_token;
      auth_state_unlock();
      return csrf;
    }
  }
  auth_state_unlock();
  return "";
}

static bool auth_revoke_session_token(const String& token) {
  if (!auth_token_format_ok(token)) {
    return false;
  }
  auth_state_lock();
  auth_prune_sessions_locked(mros::platform::mros_millis());
  bool revoked = false;
  for (size_t i = 0; i < kAuthSessionSlots; ++i) {
    if (g_auth_sessions[i].token == token) {
      auth_clear_session_slot_locked(i);
      g_auth_session_revoked++;
      revoked = true;
      break;
    }
  }
  if (revoked) {
    auth_select_current_session_locked();
  }
  auth_state_unlock();
  return revoked;
}

static void auth_clear_session_token() {
  auth_state_lock();
  for (size_t i = 0; i < kAuthSessionSlots; ++i) {
    if (g_auth_sessions[i].token.length() > 0U) {
      auth_clear_session_slot_locked(i);
      g_auth_session_revoked++;
    }
  }
  current_token = "";
  auth_state_unlock();
}

// Variables ready for Frontend (3D viewer integration later)
static float fw_var_turret = 0.0;
static float fw_var_joints[6] = {0};
static float fw_var_gripper = 0.0;

// Live Preview FK state
static bool live_preview_enabled = true;
static FK_Result_t live_fk_result {};
static bool live_fk_dirty = true;  // Recalculate when angles change
static float fk_target_angles[7] = {0}; // Slider target angles for FK (NOT SPI feedback)
static float dbg_fk_last_ms = 0.0f;
static float dbg_fk_avg_ms = 0.0f;
static float dbg_fk_max_ms = 0.0f;
static uint32_t dbg_fk_samples = 0;
static portMUX_TYPE live_fk_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE robot_ui_mux = portMUX_INITIALIZER_UNLOCKED;
static char g_ik_compute_preference[16] = "auto";
static WebRobotUiCommand g_robot_ui_command {};
static WebRobotMathState g_robot_math_state {
    0U,
    "dls",
    "numerical",
    "joint_centering",
    "quintic",
    "current",
    "default",
    "mros-deuscara-s3v",
    "base",
    "mm",
    0.5f,
    0.5f,
    5.0f,
    0.5f,
    0.1f,
    0.5f,
    10.0f,
    500U,
    "ground",
    0.0f,
    "auto_shortest",
    8.0f,
    4.0f,
    18.0f,
    false};
static TaskHandle_t g_runtime_task_handle = nullptr;
static TaskHandle_t g_fk_task_handle = nullptr;
void web_server_notify_runtime_task();
void web_server_notify_fk_task();
static String cad_asset_version_tag = "";
static uint32_t g_cad_stream_requests = 0;
static uint32_t g_cad_stream_misses = 0;
static uint32_t g_asset_stream_requests = 0;
static uint32_t g_asset_stream_misses = 0;
static const char *k_system_name = "MROS DEUSCARA KINEMATICS";
static const char *k_system_version = "MROS_DEUSCARA_S3V";
static const char *k_system_github_url = "https://github.com/MCC45TR/MROS-DEUSCARA";
static const char *k_system_developer = "M\xC3\xBCr\xC5\x9Fit Can Cihan";

static inline void live_fk_set_target(size_t index, float value) {
  if (index >= 7) return;
  portENTER_CRITICAL(&live_fk_mux);
  fk_target_angles[index] = value;
  live_fk_dirty = true;
  portEXIT_CRITICAL(&live_fk_mux);
  web_server_notify_fk_task();
}

static inline void live_fk_set_enabled(bool enabled) {
  portENTER_CRITICAL(&live_fk_mux);
  live_preview_enabled = enabled;
  live_fk_dirty = true;
  portEXIT_CRITICAL(&live_fk_mux);
  web_server_notify_fk_task();
}

static const char* normalize_ik_compute_preference(const char *mode) {
  if (mode == nullptr) return "auto";
  if (strcasecmp(mode, "web") == 0) return "web";
  if (strcasecmp(mode, "onboard") == 0 || strcasecmp(mode, "onboard-s3") == 0 ||
      strcasecmp(mode, "s3") == 0 || strcasecmp(mode, "device") == 0) {
    return "onboard-s3";
  }
  if (strcasecmp(mode, "auto") == 0) return "auto";
  if (strcasecmp(mode, "t41") == 0 || strcasecmp(mode, "t41-qspi") == 0 ||
      strcasecmp(mode, "spi") == 0) {
    return "t41-qspi";
  }
  if (strcasecmp(mode, "espnow") == 0 || strcasecmp(mode, "t41-espnow") == 0 ||
      strcasecmp(mode, "T41-ESP-NOW") == 0) {
    return "T41-ESP-NOW";
  }
  return nullptr;
}

bool web_server_set_ik_compute_preference(const char *mode) {
  const char *normalized = normalize_ik_compute_preference(mode);
  if (normalized == nullptr) return false;
  portENTER_CRITICAL(&robot_ui_mux);
  strncpy(g_ik_compute_preference, normalized, sizeof(g_ik_compute_preference) - 1U);
  g_ik_compute_preference[sizeof(g_ik_compute_preference) - 1U] = '\0';
  portEXIT_CRITICAL(&robot_ui_mux);
  return true;
}

const char* web_server_get_ik_compute_preference() {
  return g_ik_compute_preference;
}

uint32_t web_server_publish_robot_ui_target(const WebRobotUiCommand *command) {
  if (command == nullptr) return 0U;
  portENTER_CRITICAL(&robot_ui_mux);
  const uint32_t next_revision = g_robot_ui_command.revision + 1U;
  g_robot_ui_command = *command;
  g_robot_ui_command.revision = next_revision;
  if (g_robot_ui_command.revision == 0U) g_robot_ui_command.revision = 1U;
  if (g_robot_ui_command.op[0] == '\0') {
    strncpy(g_robot_ui_command.op, "point", sizeof(g_robot_ui_command.op) - 1U);
    g_robot_ui_command.op[sizeof(g_robot_ui_command.op) - 1U] = '\0';
  }
  if (g_robot_ui_command.calc_mode[0] == '\0') {
    strncpy(g_robot_ui_command.calc_mode, g_ik_compute_preference, sizeof(g_robot_ui_command.calc_mode) - 1U);
    g_robot_ui_command.calc_mode[sizeof(g_robot_ui_command.calc_mode) - 1U] = '\0';
  }
  if (g_robot_ui_command.source[0] == '\0') {
    strncpy(g_robot_ui_command.source, "shell", sizeof(g_robot_ui_command.source) - 1U);
    g_robot_ui_command.source[sizeof(g_robot_ui_command.source) - 1U] = '\0';
  }
  if (!g_robot_ui_command.ee_auto) {
    if (g_robot_ui_command.pitch_deg == 0.0f && g_robot_ui_command.ee_pitch != 0.0f) {
      g_robot_ui_command.pitch_deg = g_robot_ui_command.ee_pitch;
    }
    g_robot_ui_command.ee_pitch = g_robot_ui_command.pitch_deg;
  } else {
    g_robot_ui_command.roll_deg = 0.0f;
    g_robot_ui_command.pitch_deg = 0.0f;
    g_robot_ui_command.yaw_deg = 0.0f;
    g_robot_ui_command.ee_pitch = 0.0f;
  }
  const uint32_t rev = g_robot_ui_command.revision;
  portEXIT_CRITICAL(&robot_ui_mux);
  return rev;
}

static String robot_ui_command_to_json(const WebRobotUiCommand& command) {
  char buffer[768] = {};
  char joints_json[128] = {};
  size_t joints_len = 0U;
  auto append_joint_text = [&](const char *text) {
    if (text == nullptr || joints_len >= sizeof(joints_json) - 1U) return;
    const int written = std::snprintf(joints_json + joints_len,
                                      sizeof(joints_json) - joints_len,
                                      "%s",
                                      text);
    if (written > 0) {
      joints_len += static_cast<size_t>(written);
      if (joints_len >= sizeof(joints_json)) joints_len = sizeof(joints_json) - 1U;
    }
  };
  append_joint_text("[");
  const uint8_t count = command.joint_count > 7U ? 7U : command.joint_count;
  for (uint8_t i = 0; i < count; ++i) {
    char item[24] = {};
    std::snprintf(item,
                  sizeof(item),
                  "%s%.1f",
                  i > 0U ? "," : "",
                  static_cast<double>(command.joints[i]));
    append_joint_text(item);
  }
  append_joint_text("]");

  mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
  writer.begin();
  writer.u32_field("rev", command.revision);
  writer.string_field("op", command.op[0] != '\0' ? command.op : "point");
  writer.float_field("x", command.x, 1U);
  writer.float_field("y", command.y, 1U);
  writer.float_field("z", command.z, 1U);
  writer.bool_field("has_from", command.has_from);
  writer.float_field("from_x", command.from_x, 1U);
  writer.float_field("from_y", command.from_y, 1U);
  writer.float_field("from_z", command.from_z, 1U);
  writer.float_field("t", command.t_ms, 1U);
  writer.bool_field("ee_auto", command.ee_auto);
  writer.float_field("roll_deg", command.roll_deg, 1U);
  writer.float_field("pitch_deg", command.pitch_deg, 1U);
  writer.float_field("yaw_deg", command.yaw_deg, 1U);
  writer.float_field("ee_pitch", command.ee_pitch, 1U);
  writer.bool_field("apply", command.apply);
  writer.string_field("calc", command.calc_mode);
  writer.raw_field("joints", joints_json);
  writer.string_field("source", command.source);
  writer.end();
  return writer.overflow() ? String("{\"error\":\"ROBOT_CMD_JSON_OVERFLOW\"}")
                           : String(writer.c_str());
}

uint32_t web_server_publish_robot_math_state(const WebRobotMathState *state) {
  if (state == nullptr) return 0U;
  portENTER_CRITICAL(&robot_ui_mux);
  const uint32_t next_revision = g_robot_math_state.revision + 1U;
  g_robot_math_state = *state;
  g_robot_math_state.revision = next_revision;
  if (g_robot_math_state.revision == 0U) g_robot_math_state.revision = 1U;
  const uint32_t rev = g_robot_math_state.revision;
  portEXIT_CRITICAL(&robot_ui_mux);
  return rev;
}

void web_server_get_robot_math_state(WebRobotMathState *state) {
  if (state == nullptr) return;
  portENTER_CRITICAL(&robot_ui_mux);
  *state = g_robot_math_state;
  portEXIT_CRITICAL(&robot_ui_mux);
}

static String robot_math_state_to_json(const WebRobotMathState& state) {
  char buffer[1280] = {};
  mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
  writer.begin();
  writer.u32_field("rev", state.revision);
  writer.string_field("model_revision", mros::kinematics::kRobotModelRevision);
  writer.string_field("solver", state.solver);
  writer.string_field("jacobian", state.jacobian);
  writer.string_field("nullspace", state.nullspace);
  writer.string_field("trajectory", state.trajectory);
  writer.string_field("seed_policy", state.seed_policy);
  writer.string_field("limits_profile", state.limits_profile);
  writer.string_field("model", state.model);
  writer.string_field("frame", state.frame);
  writer.string_field("units", state.units);
  writer.float_field("pos_tol_mm", state.pos_tol_mm, 3U);
  writer.float_field("ori_tol_deg", state.ori_tol_deg, 3U);
  writer.float_field("singularity_threshold", state.singularity_threshold, 3U);
  writer.float_field("alpha_step", state.alpha_step, 3U);
  writer.float_field("null_gain", state.null_gain, 3U);
  writer.float_field("lambda_max", state.lambda_max, 3U);
  writer.float_field("max_step_deg", state.max_step_deg, 3U);
  writer.u32_field("max_iter", static_cast<uint32_t>(state.max_iter));
  writer.string_field("path_height_mode", state.path_height_mode);
  writer.float_field("ground_z_mm", state.ground_z_mm, 3U);
  writer.string_field("turret_mode", state.turret_mode);
  writer.float_field("cart_step_mm", state.cart_step_mm, 3U);
  writer.float_field("yaw_step_deg", state.yaw_step_deg, 3U);
  writer.float_field("jump_revolute_deg", state.jump_revolute_deg, 3U);
  writer.bool_field("allow_negative_z_input", state.allow_negative_z_input);
  writer.end();
  return writer.overflow() ? String("{\"error\":\"ROBOT_MATH_JSON_OVERFLOW\"}")
                           : String(writer.c_str());
}
static uint16_t dbg_prev_loop_ms = 0;
static float dbg_loop_jitter_ms = 0.0f;
static unsigned long dbg_last_motion_cmd_ms = 0;
static String dbg_last_motion_cmd = "Beklemede";

static const uint32_t PM_FREQ_TURBO_MHZ = 240;
static const uint32_t PM_FREQ_HIGH_MHZ = 160;
static const uint32_t PM_FREQ_BASE_MHZ = 80;
static const uint32_t PM_BOOST_HOLD_MS = 1800;
static const uint32_t PM_WEB_FEEDBACK_TIMEOUT_MS = 60000;
static const uint32_t PM_WEB_FEEDBACK_HOT_MS = 4000;
static const uint32_t PM_GOVERNOR_PERIOD_MS = 250;

static bool pm_ready = false;
static bool pm_probe_init = false;
static float pm_prev_coord_x = 0.0f;
static float pm_prev_coord_y = 0.0f;
static float pm_prev_coord_z = 0.0f;
static float pm_prev_coord_a = 0.0f;
static float pm_prev_turret = 0.0f;
static float pm_prev_j0 = 0.0f;
static float pm_prev_j1 = 0.0f;
static float pm_prev_j2 = 0.0f;
static float pm_prev_j3 = 0.0f;
static float pm_prev_j4 = 0.0f;
static float pm_prev_j5 = 0.0f;

static portMUX_TYPE pm_scale_mux = portMUX_INITIALIZER_UNLOCKED;
static float pm_pid_cycle_avg_ms = 0.0f;
static uint32_t pm_pid_cycle_last_ms = 0;
static uint32_t pm_pid_exec_last_ms = 0;
static uint32_t pm_pid_cycle_peak_ms = 0;
static uint32_t pm_pid_cycle_samples = 0;
static unsigned long pm_last_web_feedback_ms = 0;
static unsigned long pm_perf_until_ms = 0;
static unsigned long pm_last_governor_eval_ms = 0;
static uint32_t pm_core0_target_mhz = PM_FREQ_BASE_MHZ;
static uint32_t pm_core1_target_mhz = PM_FREQ_BASE_MHZ;
static uint32_t pm_system_target_mhz = PM_FREQ_BASE_MHZ;
static uint32_t pm_system_applied_mhz = PM_FREQ_BASE_MHZ;

static inline uint32_t pm_get_current_cpu_mhz() {
  return static_cast<uint32_t>(esp_clk_cpu_freq() / 1000000);
}

static inline void pm_mark_web_feedback(unsigned long now_ms = 0) {
  if (now_ms == 0) {
    now_ms = mros::platform::mros_millis();
  }
  mros::power::mark_web_feedback(static_cast<uint32_t>(now_ms));
  (void)mros::power::hold_lock(mros::power::LockOwner::WebTelemetry,
                               "web-feedback", PM_WEB_FEEDBACK_HOT_MS);
  portENTER_CRITICAL(&pm_scale_mux);
  pm_last_web_feedback_ms = now_ms;
  portEXIT_CRITICAL(&pm_scale_mux);
}

static uint32_t pm_pick_pid_core_target(bool pca_ready, float cycle_avg_ms,
                                        uint32_t cycle_last_ms,
                                        uint32_t prev_target_mhz) {
  if (pca_ready) {
    if (cycle_last_ms >= 10 || cycle_avg_ms >= 9.0f) {
      return PM_FREQ_TURBO_MHZ;
    }
    if (prev_target_mhz >= PM_FREQ_TURBO_MHZ) {
      if (cycle_avg_ms >= 6.0f || cycle_last_ms >= 8) {
        return PM_FREQ_TURBO_MHZ;
      }
      return PM_FREQ_HIGH_MHZ;
    }
    if (prev_target_mhz >= PM_FREQ_HIGH_MHZ) {
      if (cycle_avg_ms >= 8.5f || cycle_last_ms >= 10) {
        return PM_FREQ_TURBO_MHZ;
      }
      if (cycle_avg_ms <= 4.5f && cycle_last_ms <= 6) {
        return PM_FREQ_BASE_MHZ;
      }
      return PM_FREQ_HIGH_MHZ;
    }
    if (cycle_avg_ms >= 6.0f || cycle_last_ms >= 8) {
      return PM_FREQ_HIGH_MHZ;
    }
    return PM_FREQ_BASE_MHZ;
  }

  if (cycle_last_ms >= 10 || cycle_avg_ms >= 9.0f) {
    return PM_FREQ_BASE_MHZ;
  }
  return PM_FREQ_BASE_MHZ;
}

static uint32_t pm_pick_wifi_core_target(bool wifi_connected,
                                         unsigned long web_feedback_age_ms) {
  if (!wifi_connected) {
    return PM_FREQ_BASE_MHZ;
  }
  if (web_feedback_age_ms >= PM_WEB_FEEDBACK_TIMEOUT_MS) {
    return PM_FREQ_BASE_MHZ;
  }
  if (web_feedback_age_ms <= PM_WEB_FEEDBACK_HOT_MS) {
    return PM_FREQ_TURBO_MHZ;
  }
  return PM_FREQ_HIGH_MHZ;
}

static void pm_configure_dynamic_scaling() {
  mros::power::init();
  pm_ready = true;
  pm_last_governor_eval_ms = mros::platform::mros_millis();
  pm_mark_web_feedback(pm_last_governor_eval_ms);
  pm_system_applied_mhz = pm_get_current_cpu_mhz();
  pm_system_target_mhz = pm_system_applied_mhz;
  pm_core0_target_mhz = PM_FREQ_BASE_MHZ;
  pm_core1_target_mhz = PM_FREQ_BASE_MHZ;
  mros_console.printf(
      "[PM] ESP-IDF power manager aktif. DFS/PM lock telemetry baslangic=%uMHz\n",
      (unsigned)pm_system_applied_mhz);
}

uint32_t web_server_total_ws_client_count() {
  return static_cast<uint32_t>(ws.count() + ws_telemetry.count() +
                               ws_shell.count() + ws_shell_v2.count() +
                               ws_debug.count() + ws_mcp.count());
}

static void pm_request_perf_boost(uint32_t hold_ms = PM_BOOST_HOLD_MS) {
  if (!pm_ready) {
    return;
  }
  mros::power::request_boost("web-runtime", hold_ms);
  const unsigned long now_ms = mros::platform::mros_millis();
  const unsigned long until = now_ms + hold_ms;
  portENTER_CRITICAL(&pm_scale_mux);
  if ((long)(until - pm_perf_until_ms) > 0) {
    pm_perf_until_ms = until;
  }
  portEXIT_CRITICAL(&pm_scale_mux);
}

static void pm_probe_external_motion() {
  if (!pm_ready) return;
  const float x = spi_s3_get_coord_x();
  const float y = spi_s3_get_coord_y();
  const float z = spi_s3_get_coord_z();
  const float a = spi_s3_get_alpha();
  const float t = spi_s3_get_turret_deg();
  const float j0 = spi_s3_get_joint_deg(0);
  const float j1 = spi_s3_get_joint_deg(1);
  const float j2 = spi_s3_get_joint_deg(2);
  const float j3 = spi_s3_get_joint_deg(3);
  const float j4 = spi_s3_get_joint_deg(4);
  const float j5 = spi_s3_get_joint_deg(5);

  if (!pm_probe_init) {
    pm_probe_init = true;
    pm_prev_coord_x = x;
    pm_prev_coord_y = y;
    pm_prev_coord_z = z;
    pm_prev_coord_a = a;
    pm_prev_turret = t;
    pm_prev_j0 = j0;
    pm_prev_j1 = j1;
    pm_prev_j2 = j2;
    pm_prev_j3 = j3;
    pm_prev_j4 = j4;
    pm_prev_j5 = j5;
    return;
  }

  const bool changed = fabsf(x - pm_prev_coord_x) > 0.35f ||
                       fabsf(y - pm_prev_coord_y) > 0.35f ||
                       fabsf(z - pm_prev_coord_z) > 0.35f ||
                       fabsf(a - pm_prev_coord_a) > 0.20f ||
                       fabsf(t - pm_prev_turret) > 0.20f ||
                       fabsf(j0 - pm_prev_j0) > 0.20f ||
                       fabsf(j1 - pm_prev_j1) > 0.20f ||
                       fabsf(j2 - pm_prev_j2) > 0.20f ||
                       fabsf(j3 - pm_prev_j3) > 0.20f ||
                       fabsf(j4 - pm_prev_j4) > 0.20f ||
                       fabsf(j5 - pm_prev_j5) > 0.20f;

  pm_prev_coord_x = x;
  pm_prev_coord_y = y;
  pm_prev_coord_z = z;
  pm_prev_coord_a = a;
  pm_prev_turret = t;
  pm_prev_j0 = j0;
  pm_prev_j1 = j1;
  pm_prev_j2 = j2;
  pm_prev_j3 = j3;
  pm_prev_j4 = j4;
  pm_prev_j5 = j5;

  if (changed) {
    pm_request_perf_boost();
  }
}

static void pm_service(unsigned long now_ms) {
  if (!pm_ready) {
    return;
  }
  mros::power::service(static_cast<uint32_t>(now_ms));
  if ((long)(now_ms - pm_last_governor_eval_ms) <
      (long)PM_GOVERNOR_PERIOD_MS) {
    return;
  }
  pm_last_governor_eval_ms = now_ms;

  float pid_cycle_avg_ms = 0.0f;
  uint32_t pid_cycle_last_ms = 0;
  unsigned long last_feedback_ms = 0;
  unsigned long perf_until_ms = 0;
  uint32_t prev_core1_target = PM_FREQ_BASE_MHZ;
  portENTER_CRITICAL(&pm_scale_mux);
  pid_cycle_avg_ms = pm_pid_cycle_avg_ms;
  pid_cycle_last_ms = pm_pid_cycle_last_ms;
  last_feedback_ms = pm_last_web_feedback_ms;
  perf_until_ms = pm_perf_until_ms;
  prev_core1_target = pm_core1_target_mhz;
  portEXIT_CRITICAL(&pm_scale_mux);

  const bool pca_ready = pca9685_is_ready();
  const bool wifi_connected = wifi_manager_is_connected();
  const unsigned long web_feedback_age_ms =
      (last_feedback_ms == 0) ? 0xFFFFFFFFUL : (now_ms - last_feedback_ms);

  uint32_t core1_target =
      pm_pick_pid_core_target(pca_ready, pid_cycle_avg_ms, pid_cycle_last_ms,
                              prev_core1_target);
  if ((long)(perf_until_ms - now_ms) > 0) {
    core1_target = PM_FREQ_TURBO_MHZ;
  }
  const uint32_t core0_target =
      pm_pick_wifi_core_target(wifi_connected, web_feedback_age_ms);
  const uint32_t system_target =
      (core0_target > core1_target) ? core0_target : core1_target;
  mros::power::Status power {};
  mros::power::get_status(&power);
  const uint32_t applied_freq = power.actual_cpu_mhz;

  portENTER_CRITICAL(&pm_scale_mux);
  pm_core0_target_mhz = power.net_demand_mhz;
  pm_core1_target_mhz = power.rt_demand_mhz;
  pm_system_target_mhz = power.target_mhz != 0U ? power.target_mhz : system_target;
  pm_system_applied_mhz = applied_freq;
  portEXIT_CRITICAL(&pm_scale_mux);
}

void web_server_report_pid_cycle_ms(uint32_t cycle_ms, uint32_t exec_ms) {
  if (cycle_ms == 0) {
    return;
  }
  mros::power::report_pid_cycle(cycle_ms, exec_ms);
  portENTER_CRITICAL(&pm_scale_mux);
  pm_pid_cycle_last_ms = cycle_ms;
  pm_pid_exec_last_ms = exec_ms;
  if (cycle_ms > pm_pid_cycle_peak_ms) {
    pm_pid_cycle_peak_ms = cycle_ms;
  }
  pm_pid_cycle_samples++;
  if (pm_pid_cycle_samples == 1) {
    pm_pid_cycle_avg_ms = static_cast<float>(cycle_ms);
  } else {
    pm_pid_cycle_avg_ms =
        (pm_pid_cycle_avg_ms * 0.85f) + (static_cast<float>(cycle_ms) * 0.15f);
  }
  portEXIT_CRITICAL(&pm_scale_mux);
}

static inline void cad_hash_mix(uint32_t &hash, const uint8_t *data, size_t len) {
  if (!data || len == 0) return;
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<uint32_t>(data[i]);
    hash *= 16777619u;
  }
}

static inline void cad_hash_mix_file(uint32_t &hash, const char *path) {
  if (!path) return;
  cad_hash_mix(hash, reinterpret_cast<const uint8_t *>(path), strlen(path));
  FILE* file = mros::platform::mros_fs_open(path, "rb");
  if (file == nullptr) return;
  uint8_t buf[256];
  while (true) {
    const size_t n = std::fread(buf, 1U, sizeof(buf), file);
    if (n == 0) break;
    cad_hash_mix(hash, buf, n);
  }
  std::fclose(file);
}

static size_t cad_file_size(const char *path) {
  if (path == nullptr) return 0U;
  FILE* file = mros::platform::mros_fs_open(path, "rb");
  if (file == nullptr) return 0U;
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return 0U;
  }
  const long size = std::ftell(file);
  std::fclose(file);
  return size > 0 ? static_cast<size_t>(size) : 0U;
}

static String cad_public_path(const char *fs_path) {
  if (fs_path == nullptr) return "";
  const char prefix[] = "/assets/cad";
  if (strncmp(fs_path, prefix, sizeof(prefix) - 1U) == 0) {
    return String("/cad") + String(fs_path + sizeof(prefix) - 1U);
  }
  return String(fs_path);
}

static const char *k_cad_asset_files[] = {
    "/assets/cad/ARM1/ARM1.gltf",
    "/assets/cad/ARM1/data.bin",
    "/assets/cad/ARM2/ARM2.gltf",
    "/assets/cad/ARM2/data.bin",
    "/assets/cad/ARM3/ARM3.gltf",
    "/assets/cad/ARM3/data.bin",
    "/assets/cad/ARM4/ARM4.gltf",
    "/assets/cad/ARM4/data.bin",
    "/assets/cad/ARM5/ARM5.gltf",
    "/assets/cad/ARM5/data.bin",
    "/assets/cad/ARM6/ARM6.gltf",
    "/assets/cad/ARM6/data.bin",
    "/assets/cad/MROS_BASE_TURRET/MROS_BASE_TURRET.gltf",
    "/assets/cad/MROS_BASE_TURRET/data.bin",
    "/assets/cad/MROS_TOP_TURRET/MROS_TOP_TURRET.gltf",
    "/assets/cad/MROS_TOP_TURRET/data.bin",
    "/assets/cad/MROS_TURRET_CARRIER/MROS_TURRET_CARRIER.gltf",
    "/assets/cad/MROS_TURRET_CARRIER/data.bin",
    "/assets/cad/MROS_TURRET_COVER/MROS_TURRET_COVER.gltf",
    "/assets/cad/MROS_TURRET_COVER/data.bin",
    "/assets/cad/MROS_PLANET_GEAR_18T/MROS_PLANET_GEAR_18T.gltf",
    "/assets/cad/MROS_PLANET_GEAR_18T/data.bin"};

static constexpr const char *kImmutableAssetCacheControl =
    "public, max-age=31536000, immutable";

struct AssetCatalogEntry {
  const char* public_prefix;
  const char* fs_prefix;
  const char* cache_control;
  bool gzip_candidate;
};

static constexpr AssetCatalogEntry kAssetCatalog[] = {
    {"/css", "/css", kImmutableAssetCacheControl, true},
    {"/js", "/js", kImmutableAssetCacheControl, true},
    {"/assets", "/assets", kImmutableAssetCacheControl, true},
    {"/cad", "/assets/cad", kImmutableAssetCacheControl, true},
    {"/materials", "/materials", "no-store", false},
    {"/robot-data", "/robot", "no-store", false},
};

static String asset_etag_for_path(const char* path, const size_t size) {
  uint32_t hash = 2166136261u;
  cad_hash_mix(hash, reinterpret_cast<const uint8_t*>(path),
               path != nullptr ? strlen(path) : 0U);
  cad_hash_mix(hash, reinterpret_cast<const uint8_t*>(&size), sizeof(size));
  char tag[24] = {};
  snprintf(tag, sizeof(tag), "\"%08lX-%lu\"",
           static_cast<unsigned long>(hash), static_cast<unsigned long>(size));
  return String(tag);
}

static const String &get_cad_asset_version_tag() {
  if (cad_asset_version_tag.length() > 0) return cad_asset_version_tag;

  uint32_t hash = 2166136261u;  // FNV-1a 32-bit
  for (size_t i = 0; i < (sizeof(k_cad_asset_files) / sizeof(k_cad_asset_files[0]));
       i++) {
    cad_hash_mix_file(hash, k_cad_asset_files[i]);
  }

  char tag[16];
  snprintf(tag, sizeof(tag), "%08lX", static_cast<unsigned long>(hash));
  cad_asset_version_tag = String(tag);
  return cad_asset_version_tag;
}

struct ManifestStreamState {
  size_t index = 0U;
  uint8_t stage = 0U;
  std::string pending;
};

static size_t manifest_copy_pending(ManifestStreamState* state,
                                    uint8_t* buffer,
                                    const size_t max_len) {
  if (state == nullptr || buffer == nullptr || max_len == 0U || state->pending.empty()) {
    return 0U;
  }
  const size_t n = std::min(max_len, state->pending.size());
  std::memcpy(buffer, state->pending.data(), n);
  state->pending.erase(0U, n);
  return n;
}

static size_t stream_asset_catalog_manifest_chunk(ManifestStreamState* state,
                                                  uint8_t* buffer,
                                                  const size_t max_len) {
  if (state == nullptr || buffer == nullptr || max_len == 0U) return 0U;
  if (!state->pending.empty()) return manifest_copy_pending(state, buffer, max_len);

  constexpr size_t kAssetCount = sizeof(kAssetCatalog) / sizeof(kAssetCatalog[0]);
  if (state->stage == 0U) {
    state->stage = 1U;
    state->pending = "{\"success\":true,\"streamed\":true,\"assets\":[";
  } else if (state->stage == 1U && state->index < kAssetCount) {
    const AssetCatalogEntry& entry = kAssetCatalog[state->index];
    String item = state->index > 0U ? "," : "";
    item += "{\"public_prefix\":\"";
    item += entry.public_prefix;
    item += "\",\"fs_prefix\":\"";
    item += entry.fs_prefix;
    item += "\",\"cache_control\":\"";
    item += entry.cache_control;
    item += "\",\"gzip_candidate\":";
    item += entry.gzip_candidate ? "true" : "false";
    item += "}";
    state->pending = item.c_str();
    state->index++;
  } else if (state->stage == 1U) {
    state->stage = 2U;
    state->pending = "]}";
  } else {
    return 0U;
  }
  return manifest_copy_pending(state, buffer, max_len);
}

static size_t stream_cad_manifest_chunk(ManifestStreamState* state,
                                        uint8_t* buffer,
                                        const size_t max_len) {
  if (state == nullptr || buffer == nullptr || max_len == 0U) return 0U;
  if (!state->pending.empty()) return manifest_copy_pending(state, buffer, max_len);

  constexpr size_t kCadFileCount = sizeof(k_cad_asset_files) / sizeof(k_cad_asset_files[0]);
  if (state->stage == 0U) {
    state->stage = 1U;
    state->pending = std::string("{\"success\":true,\"streamed\":true,\"version\":\"") +
                     get_cad_asset_version_tag().c_str() + "\",\"base\":\"/cad\",\"parts\":[";
  } else if (state->stage == 1U && state->index < kCadFileCount) {
    const size_t i = state->index;
    const char *gltf_path = k_cad_asset_files[i];
    const char *bin_path =
        ((i + 1U) < kCadFileCount)
            ? k_cad_asset_files[i + 1U]
            : "";
    const bool gltf_ok = mros::platform::mros_fs_exists(gltf_path);
    const bool bin_ok = (bin_path != nullptr && bin_path[0] != '\0') &&
                        mros::platform::mros_fs_exists(bin_path);

    String id = cad_public_path(gltf_path);
    int last_slash = id.lastIndexOf('/');
    if (last_slash >= 0) id = id.substring(0, last_slash);
    last_slash = id.lastIndexOf('/');
    id = (last_slash >= 0) ? id.substring(last_slash + 1) : id;

    uint32_t part_hash = 2166136261u;
    cad_hash_mix_file(part_hash, gltf_path);
    cad_hash_mix_file(part_hash, bin_path);
    char hash_buf[16];
    snprintf(hash_buf, sizeof(hash_buf), "%08lX",
             static_cast<unsigned long>(part_hash));

    String item = i > 0U ? "," : "";
    item += "{";
    item += "\"id\":\"" + id + "\",";
    item += "\"gltf\":\"" + cad_public_path(gltf_path) + "\",";
    item += "\"bin\":\"" + cad_public_path(bin_path) + "\",";
    item += "\"gltf_exists\":" + String(gltf_ok ? "true" : "false") + ",";
    item += "\"bin_exists\":" + String(bin_ok ? "true" : "false") + ",";
    item += "\"gltf_size\":" + String(static_cast<uint32_t>(cad_file_size(gltf_path))) + ",";
    item += "\"bin_size\":" + String(static_cast<uint32_t>(cad_file_size(bin_path))) + ",";
    item += "\"hash\":\"" + String(hash_buf) + "\"";
    item += "}";
    state->pending = item.c_str();
    state->index += 2U;
  } else if (state->stage == 1U) {
    state->stage = 2U;
    state->pending = "]}";
  } else {
    return 0U;
  }
  return manifest_copy_pending(state, buffer, max_len);
}

static void add_no_cache_headers(AsyncWebServerResponse *response) {
  if (!response) return;
  response->addHeader("Cache-Control",
                      "no-store, no-cache, must-revalidate, max-age=0");
  response->addHeader("Pragma", "no-cache");
  response->addHeader("Expires", "0");
}

static void send_dpm_json(AsyncWebServerRequest *request,
                          bool (*builder)(char*, size_t)) {
  if (request == nullptr || builder == nullptr) return;
  constexpr size_t kDpmApiJsonBytes = 12288U;
  char* buffer = static_cast<char*>(
      heap_caps_malloc(kDpmApiJsonBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    buffer = static_cast<char*>(
        heap_caps_malloc(kDpmApiJsonBytes, MALLOC_CAP_8BIT));
  }
  if (buffer == nullptr) {
    request->send(500, "application/json",
                  "{\"ok\":false,\"error\":\"no_buffer\"}");
    return;
  }
  const bool ok = builder(buffer, kDpmApiJsonBytes);
  AsyncWebServerResponse *response =
      request->beginResponse(ok ? 200 : 500, "application/json", buffer);
  add_no_cache_headers(response);
  request->send(response);
  heap_caps_free(buffer);
}

static void send_fs_asset(AsyncWebServerRequest *request, const char *path,
                          const char *mime, bool no_cache = true) {
  if (!request || !path || !mime) return;
  if (!mros::platform::mros_fs_exists(path)) {
    request->send(404, "text/plain", "Missing asset");
    return;
  }
  if (!web_async_send_littlefs_stream_psram(request, path, mime, no_cache, nullptr)) {
    request->send(500, "text/plain", "Asset stream failed");
  }
}

static const char *asset_mime_from_path(const String &path) {
  String lower(path);
  lower.toLowerCase();
  if (lower.endsWith(".gltf")) return "model/gltf+json";
  if (lower.endsWith(".glb")) return "model/gltf-binary";
  if (lower.endsWith(".bin")) return "application/octet-stream";
  if (lower.endsWith(".svg")) return "image/svg+xml";
  if (lower.endsWith(".png")) return "image/png";
  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
  if (lower.endsWith(".webp")) return "image/webp";
  if (lower.endsWith(".js")) return "application/javascript";
  if (lower.endsWith(".css")) return "text/css";
  if (lower.endsWith(".html") || lower.endsWith(".htm")) return "text/html";
  if (lower.endsWith(".json")) return "application/json";
  if (lower.endsWith(".csv")) return "text/csv";
  if (lower.endsWith(".c") || lower.endsWith(".cpp") || lower.endsWith(".cc") ||
      lower.endsWith(".cxx") || lower.endsWith(".h") || lower.endsWith(".hpp") ||
      lower.endsWith(".hh") || lower.endsWith(".ino")) return "text/x-csrc";
  if (lower.endsWith(".py")) return "text/x-python";
  if (lower.endsWith(".yml") || lower.endsWith(".yaml")) return "application/x-yaml";
  if (lower.endsWith(".cfg") || lower.endsWith(".conf") || lower.endsWith(".ini")) return "text/plain";
  if (lower.endsWith(".db") || lower.endsWith(".sql")) return "text/plain";
  if (lower.endsWith(".txt") || lower.endsWith(".log") || lower.endsWith(".md")) return "text/plain";
  if (lower.endsWith(".wasm")) return "application/wasm";
  return "application/octet-stream";
}

static bool fm_starts_with_path(const std::string& value, const char* prefix) {
  if (prefix == nullptr || prefix[0] == '\0') return false;
  const size_t len = strlen(prefix);
  return value == prefix || (value.size() > len && value.compare(0U, len, prefix) == 0 && value[len] == '/');
}

static std::string fm_normalize_path(const char* raw_path) {
  std::string raw = (raw_path != nullptr && raw_path[0] != '\0') ? raw_path : "/ESPUSER";
  for (char& ch : raw) {
    if (ch == '\\') ch = '/';
  }
  if (raw.empty() || raw[0] != '/') raw.insert(raw.begin(), '/');
  if (raw == "/" || raw == "/fs" || raw == "/littlefs") raw = "/ESPUSER";
  if (fm_starts_with_path(raw, "/fs/ESPUSER")) {
    raw = raw.substr(strlen("/fs"));
  } else if (fm_starts_with_path(raw, "/littlefs/ESPUSER")) {
    raw = raw.substr(strlen("/littlefs"));
  }

  std::vector<std::string> parts;
  size_t cursor = 0U;
  while (cursor < raw.size()) {
    while (cursor < raw.size() && raw[cursor] == '/') ++cursor;
    const size_t start = cursor;
    while (cursor < raw.size() && raw[cursor] != '/') ++cursor;
    if (cursor == start) continue;
    const std::string part = raw.substr(start, cursor - start);
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(part);
  }

  std::string normalized = "/";
  for (size_t i = 0U; i < parts.size(); ++i) {
    if (i > 0U) normalized.push_back('/');
    normalized += parts[i];
  }
  return normalized;
}

static bool fm_is_user_path(const std::string& path) {
  return path == "/ESPUSER" || fm_starts_with_path(path, "/ESPUSER");
}

static bool fm_is_protected_user_path(const std::string& path) {
  return path == "/ESPUSER/auth" || fm_starts_with_path(path, "/ESPUSER/auth") ||
         path == "/ESPUSER/auth_credentials.dat" ||
         path == "/ESPUSER/auth_credentials.tmp";
}

static bool fm_is_visible_user_path(const std::string& path) {
  return fm_is_user_path(path) && !fm_is_protected_user_path(path);
}

static bool fm_is_user_write_target(const std::string& path) {
  return fm_is_visible_user_path(path) && path != "/ESPUSER";
}

static bool fm_is_remote_path(const std::string& path) {
  return mros::shell::remote::fs_is_remote_path(path);
}

static String fm_remote_error_json(const std::string& path,
                                   const char* op,
                                   const char* code,
                                   const char* message) {
  return String(mros::shell::remote::fs_error_json(path, op, code, message).c_str());
}

static std::string fm_parent_path(const std::string& path) {
  const std::string normalized = fm_normalize_path(path.c_str());
  const size_t slash = normalized.find_last_of('/');
  if (slash == std::string::npos || slash == 0U) return "/ESPUSER";
  return normalized.substr(0U, slash);
}

static std::string fm_basename(const std::string& path) {
  const std::string normalized = fm_normalize_path(path.c_str());
  const size_t slash = normalized.find_last_of('/');
  return slash == std::string::npos ? normalized : normalized.substr(slash + 1U);
}

static bool fm_stat_logical(const std::string& path, struct stat* out_info) {
  if (!fm_is_visible_user_path(path) || out_info == nullptr) return false;
  const std::string vfs = mros::platform::mros_fs_vfs_path(path.c_str());
  return ::stat(vfs.c_str(), out_info) == 0;
}

static std::string fm_lower_copy(std::string text);

static const char* fm_kind_for_path(const std::string& path, const bool is_dir) {
  if (is_dir) return "folder";
  String lower(path.c_str());
  lower.toLowerCase();
  if (lower.endsWith(".bin")) return "firmware";
  if (lower.endsWith(".json")) return "json";
  if (lower.endsWith(".csv")) return "table";
  if (lower.endsWith(".txt") || lower.endsWith(".log") || lower.endsWith(".md") ||
      lower.endsWith(".cfg") || lower.endsWith(".conf") || lower.endsWith(".ini") ||
      lower.endsWith(".yml") || lower.endsWith(".yaml") ||
      lower.endsWith(".db") || lower.endsWith(".sql")) return "text";
  if (lower.endsWith(".c") || lower.endsWith(".cpp") || lower.endsWith(".cc") ||
      lower.endsWith(".cxx") || lower.endsWith(".h") || lower.endsWith(".hpp") ||
      lower.endsWith(".hh") || lower.endsWith(".ino") || lower.endsWith(".py") ||
      lower.endsWith(".js") || lower.endsWith(".css") || lower.endsWith(".html") ||
      lower.endsWith(".xml") || lower.endsWith(".sh") || lower.endsWith(".msh") ||
      lower.endsWith(".bash") || lower.endsWith(".zsh") || lower.endsWith("cmakelists.txt") ||
      lower.endsWith(".cmake")) return "code";
  if (lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg") ||
      lower.endsWith(".webp") || lower.endsWith(".svg")) return "image";
  if (lower.endsWith(".gz") || lower.endsWith(".zip")) return "archive";
  return "binary";
}

static String fm_public_path_json(const std::string& path) {
  return jsonEscape(String(path.c_str()));
}

static String fm_time_iso_utc(const time_t value) {
  if (value <= 0) return "";
  struct tm tm_value {};
  if (gmtime_r(&value, &tm_value) == nullptr) return "";
  char buffer[24] = {};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm_value) == 0U) {
    return "";
  }
  return String(buffer);
}

static String fm_entry_json(const std::string& path, const char* name, const struct stat& info) {
  const bool is_dir = S_ISDIR(info.st_mode);
  const String mime = is_dir ? String("inode/directory") : String(asset_mime_from_path(String(path.c_str())));
  String json = "{";
  json += "\"name\":\"" + jsonEscape(String(name != nullptr ? name : "")) + "\",";
  json += "\"path\":\"" + fm_public_path_json(path) + "\",";
  json += "\"parent\":\"" + fm_public_path_json(fm_parent_path(path)) + "\",";
  json += "\"is_dir\":" + String(is_dir ? "true" : "false") + ",";
  json += "\"size\":" + String(static_cast<uint32_t>(is_dir ? 0 : info.st_size)) + ",";
  json += "\"mtime\":" + String(static_cast<uint32_t>(info.st_mtime)) + ",";
  json += "\"modified\":\"" + jsonEscape(fm_time_iso_utc(info.st_mtime)) + "\",";
  json += "\"mime\":\"" + jsonEscape(mime) + "\",";
  json += "\"kind\":\"" + String(fm_kind_for_path(path, is_dir)) + "\",";
  json += "\"writable\":" + String(fm_is_visible_user_path(path) ? "true" : "false");
  json += "}";
  return json;
}

enum class FmExpectedMtimeStatus {
  kMissing,
  kValid,
  kInvalid,
};

static FmExpectedMtimeStatus fm_request_expected_mtime(AsyncWebServerRequest* request,
                                                       uint32_t* out_value) {
  if (out_value == nullptr || request == nullptr) return FmExpectedMtimeStatus::kInvalid;
  String raw;
  if (request->hasParam("expected_mtime", true)) {
    raw = request->getParam("expected_mtime", true)->value();
  } else if (request->hasParam("expected_mtime")) {
    raw = request->getParam("expected_mtime")->value();
  } else if (request->hasHeader("If-Unmodified-Since-MROS")) {
    raw = request->header("If-Unmodified-Since-MROS");
  } else {
    return FmExpectedMtimeStatus::kMissing;
  }
  raw.trim();
  if (raw.length() == 0U || raw.length() > 10U) {
    return FmExpectedMtimeStatus::kInvalid;
  }
  for (size_t i = 0U; i < raw.length(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(raw[i]))) {
      return FmExpectedMtimeStatus::kInvalid;
    }
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long parsed = strtoul(raw.c_str(), &end, 10);
  if (errno == ERANGE || end == raw.c_str() || (end != nullptr && *end != '\0') ||
      parsed > static_cast<unsigned long>(UINT32_MAX)) {
    return FmExpectedMtimeStatus::kInvalid;
  }
  *out_value = static_cast<uint32_t>(parsed);
  return FmExpectedMtimeStatus::kValid;
}

static String fm_stale_file_json(const std::string& path, const struct stat* info) {
  String json = "{\"success\":false,\"error\":\"STALE_FILE\",\"path\":\"";
  json += fm_public_path_json(path);
  json += "\"";
  if (info != nullptr) {
    json += ",\"current_mtime\":";
    json += String(static_cast<uint32_t>(info->st_mtime));
    json += ",\"current_modified\":\"";
    json += jsonEscape(fm_time_iso_utc(info->st_mtime));
    json += "\"";
  }
  json += "}";
  return json;
}

static String fm_invalid_expected_mtime_json(const std::string& path) {
  String json = "{\"success\":false,\"error\":\"INVALID_EXPECTED_MTIME\",\"path\":\"";
  json += fm_public_path_json(path);
  json += "\"}";
  return json;
}

static bool fm_expected_mtime_matches(const std::string& path,
                                      uint32_t expected,
                                      String* out_error_json);

static bool fm_save_precondition_ok(AsyncWebServerRequest* request,
                                     const std::string& path,
                                     String* out_error_json,
                                     int* out_status_code) {
  if (out_status_code != nullptr) *out_status_code = 409;
  uint32_t expected = 0U;
  const FmExpectedMtimeStatus status = fm_request_expected_mtime(request, &expected);
  if (status == FmExpectedMtimeStatus::kMissing) return true;
  if (status == FmExpectedMtimeStatus::kInvalid) {
    if (out_status_code != nullptr) *out_status_code = 400;
    if (out_error_json != nullptr) *out_error_json = fm_invalid_expected_mtime_json(path);
    return false;
  }
  return fm_expected_mtime_matches(path, expected, out_error_json);
}

static bool fm_expected_mtime_matches(const std::string& path,
                                      const uint32_t expected,
                                      String* out_error_json) {
  struct stat info {};
  const bool exists = fm_stat_logical(path, &info);
  if (!exists) {
    if (expected == 0U) return true;
    if (out_error_json != nullptr) *out_error_json = fm_stale_file_json(path, nullptr);
    return false;
  }
  if (static_cast<uint32_t>(info.st_mtime) == expected) return true;
  if (out_error_json != nullptr) *out_error_json = fm_stale_file_json(path, &info);
  return false;
}

static String fm_save_success_json(const std::string& path, const size_t bytes) {
  struct stat info {};
  const bool has_info = fm_stat_logical(path, &info);
  String json = "{\"success\":true,\"path\":\"";
  json += fm_public_path_json(path);
  json += "\",\"bytes\":";
  json += String(static_cast<uint32_t>(bytes));
  if (has_info) {
    json += ",\"mtime\":";
    json += String(static_cast<uint32_t>(info.st_mtime));
    json += ",\"modified\":\"";
    json += jsonEscape(fm_time_iso_utc(info.st_mtime));
    json += "\"";
  }
  json += "}";
  return json;
}

static bool fm_delete_recursive(const std::string& path) {
  if (!fm_is_visible_user_path(path) || path == "/ESPUSER") return false;
  struct stat info {};
  if (!fm_stat_logical(path, &info)) return false;
  if (!S_ISDIR(info.st_mode)) {
    return mros::platform::mros_fs_remove(path.c_str());
  }
  const std::string vfs = mros::platform::mros_fs_vfs_path(path.c_str());
  DIR* dir = opendir(vfs.c_str());
  if (dir == nullptr) return false;
  bool ok = true;
  dirent* entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    const std::string child = path + "/" + entry->d_name;
    if (!fm_delete_recursive(child)) ok = false;
  }
  closedir(dir);
  return ok && ::rmdir(vfs.c_str()) == 0;
}

static bool fm_copy_file(const std::string& from, const std::string& to) {
  if (!fm_is_visible_user_path(from) || !fm_is_visible_user_path(to) || from == to) return false;
  struct stat info {};
  if (!fm_stat_logical(from, &info) || S_ISDIR(info.st_mode)) return false;
  FILE* src = mros::platform::mros_fs_open(from.c_str(), "rb");
  FILE* dst = mros::platform::mros_fs_open(to.c_str(), "wb");
  if (src == nullptr || dst == nullptr) {
    if (src != nullptr) fclose(src);
    if (dst != nullptr) fclose(dst);
    return false;
  }
  uint8_t* buffer = static_cast<uint8_t*>(
      heap_caps_malloc(1024U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    buffer = static_cast<uint8_t*>(heap_caps_malloc(1024U, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  bool ok = buffer != nullptr;
  while (ok) {
    const size_t rd = fread(buffer, 1U, 1024U, src);
    if (rd > 0U && fwrite(buffer, 1U, rd, dst) != rd) ok = false;
    if (rd < 1024U) break;
  }
  if (buffer != nullptr) heap_caps_free(buffer);
  fclose(src);
  fclose(dst);
  if (!ok) (void)mros::platform::mros_fs_remove(to.c_str());
  return ok;
}

static String fm_list_json(const std::string& path,
                           const size_t offset,
                           const size_t limit,
                           const char* sort_key,
                           const char* sort_dir) {
  struct LocalEntry {
    std::string child;
    std::string name;
    struct stat info {};
  };
  constexpr size_t kFileListEntryCap = 2048U;

  const std::string sort = sort_key != nullptr ? fm_lower_copy(sort_key) : "name";
  const std::string sort_direction = sort_dir != nullptr ? fm_lower_copy(sort_dir) : "asc";
  const bool desc = sort_direction == "desc";
  const bool sort_type = sort == "type";
  const bool sort_size = sort == "size";
  const bool sort_mtime = sort == "mtime" || sort == "modified";

  if (fm_is_remote_path(path)) {
    return String(mros::shell::remote::fs_list_json(path, offset, limit).c_str());
  }
  if (!mros::platform::mros_fs_is_mounted()) {
    return "{\"success\":false,\"error\":\"FS_NOT_MOUNTED\"}";
  }
  if (!fm_is_visible_user_path(path)) {
    return "{\"success\":false,\"error\":\"PERMISSION_DENIED\"}";
  }
  struct stat root_info {};
  const bool root_exists = fm_stat_logical(path, &root_info);
  if (!root_exists || !S_ISDIR(root_info.st_mode)) {
    return "{\"success\":false,\"error\":\"NOT_A_DIRECTORY\"}";
  }
  const std::string vfs = mros::platform::mros_fs_vfs_path(path.c_str());
  DIR* dirp = opendir(vfs.c_str());
  if (dirp == nullptr) {
    return "{\"success\":false,\"error\":\"OPEN_DIR_FAILED\"}";
  }
  std::vector<LocalEntry> entries;
  entries.reserve(limit > 0U ? std::min(limit, static_cast<size_t>(64U)) : 64U);
  size_t scan_total = 0U;
  bool truncated = false;
  dirent* entry = nullptr;
  while ((entry = readdir(dirp)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    const std::string child = (path == "/ESPUSER") ? (path + "/" + entry->d_name)
                                                   : (path + "/" + entry->d_name);
    if (fm_is_protected_user_path(child)) continue;
    scan_total++;
    if (entries.size() >= kFileListEntryCap) {
      truncated = true;
      continue;
    }
    struct stat info {};
    if (!fm_stat_logical(child, &info)) continue;
    LocalEntry local {};
    local.child = child;
    local.name = entry->d_name;
    local.info = info;
    entries.push_back(std::move(local));
  }
  closedir(dirp);
  std::sort(entries.begin(), entries.end(), [&](const LocalEntry& a, const LocalEntry& b) {
    const bool a_dir = S_ISDIR(a.info.st_mode);
    const bool b_dir = S_ISDIR(b.info.st_mode);
    if (a_dir != b_dir) return a_dir;
    int cmp = 0;
    if (sort_size) {
      const int64_t av = a_dir ? -1 : static_cast<int64_t>(a.info.st_size);
      const int64_t bv = b_dir ? -1 : static_cast<int64_t>(b.info.st_size);
      cmp = (av < bv) ? -1 : ((av > bv) ? 1 : 0);
    } else if (sort_mtime) {
      const int64_t av = static_cast<int64_t>(a.info.st_mtime);
      const int64_t bv = static_cast<int64_t>(b.info.st_mtime);
      cmp = (av < bv) ? -1 : ((av > bv) ? 1 : 0);
    } else if (sort_type) {
      const int type_cmp = std::strcmp(fm_kind_for_path(a.child, a_dir), fm_kind_for_path(b.child, b_dir));
      cmp = type_cmp < 0 ? -1 : (type_cmp > 0 ? 1 : 0);
    } else {
      cmp = a.name.compare(b.name);
    }
    if (cmp == 0) cmp = a.name.compare(b.name);
    return desc ? (cmp > 0) : (cmp < 0);
  });
  const size_t total_items = entries.size();
  const bool paged = limit > 0U;
  const size_t safe_offset = offset < total_items ? offset : total_items;
  const size_t end_index = paged ? std::min(total_items, safe_offset + limit) : total_items;
  if (paged) g_file_list_paged_count++;
  uint64_t total = 0U;
  uint64_t used = 0U;
  (void)mros::platform::mros_fs_info(&total, &used);
  String json = "{\"success\":true,";
  json.reserve(paged ? 1536U : 2048U);
  json += "\"provider\":\"local\",\"remote\":false,\"mounted\":true,\"writable\":true,";
  json += "\"peer_status\":\"ready\",\"error_code\":\"OK\",";
  json += "\"root\":\"/ESPUSER\",";
  json += "\"path\":\"" + fm_public_path_json(path) + "\",";
  json += "\"parent\":\"" + fm_public_path_json(path == "/ESPUSER" ? "/ESPUSER" : fm_parent_path(path)) + "\",";
  json += "\"fs_total\":" + String(static_cast<uint32_t>(total)) + ",";
  json += "\"fs_used\":" + String(static_cast<uint32_t>(used)) + ",";
  json += "\"offset\":" + String(static_cast<uint32_t>(safe_offset)) + ",";
  json += "\"limit\":" + String(static_cast<uint32_t>(paged ? limit : total_items)) + ",";
  json += "\"sort\":\"" + jsonEscape(String(sort.c_str())) + "\",";
  json += "\"dir\":\"" + jsonEscape(String(desc ? "desc" : "asc")) + "\",";
  json += "\"total\":" + String(static_cast<uint32_t>(total_items)) + ",";
  json += "\"scan_total\":" + String(static_cast<uint32_t>(scan_total)) + ",";
  json += "\"entry_cap\":" + String(static_cast<uint32_t>(kFileListEntryCap)) + ",";
  json += "\"truncated\":" + String(truncated ? "true" : "false") + ",";
  json += "\"next_offset\":" + String(static_cast<uint32_t>(end_index)) + ",";
  json += "\"has_more\":" + String((end_index < total_items) ? "true" : "false") + ",";
  json += "\"items\":[";
  for (size_t i = safe_offset; i < end_index; ++i) {
    if (i > safe_offset) json += ",";
    json += fm_entry_json(entries[i].child, entries[i].name.c_str(), entries[i].info);
  }
  json += "]}";
  return json;
}

struct FileUploadContext {
  AsyncWebServerRequest* request = nullptr;
  FILE* file = nullptr;
  std::string path;
  std::string temp_path;
  size_t written = 0U;
  uint32_t expected_mtime = 0U;
  bool has_expected_mtime = false;
  bool failed = false;
  bool in_use = false;
  char operation[8] = {};

  void close() {
    if (file != nullptr) {
      fclose(file);
      file = nullptr;
    }
  }

  void reset() {
    close();
    if (!temp_path.empty()) {
      (void)mros::platform::mros_fs_remove(temp_path.c_str());
    }
    *this = {};
  }
};

static FileUploadContext g_file_upload_slots[2];

static std::string fm_temp_path_for(const std::string& path, const char* op, const size_t slot) {
  std::string out = path;
  out += ".tmp-";
  out += op != nullptr ? op : "write";
  out += "-";
  out += std::to_string(static_cast<unsigned long>(mros::platform::mros_millis()));
  out += "-";
  out += std::to_string(static_cast<unsigned long>(slot));
  return out;
}

static FileUploadContext* fm_find_upload_context(AsyncWebServerRequest* request) {
  if (request == nullptr) return nullptr;
  for (FileUploadContext& slot : g_file_upload_slots) {
    if (slot.in_use && slot.request == request) return &slot;
  }
  return nullptr;
}

static bool fm_target_busy(const std::string& path) {
  for (const FileUploadContext& slot : g_file_upload_slots) {
    if (slot.in_use && slot.path == path) return true;
  }
  return false;
}

static FileUploadContext* fm_acquire_upload_context(AsyncWebServerRequest* request,
                                                    const std::string& path,
                                                    const char* op,
                                                    String* error_json,
                                                    int* status_code) {
  if (error_json != nullptr) *error_json = "";
  if (status_code != nullptr) *status_code = 409;
  if (fm_target_busy(path)) {
    if (error_json != nullptr) *error_json = "{\"success\":false,\"error\":\"TARGET_BUSY\"}";
    return nullptr;
  }
  for (size_t i = 0U; i < (sizeof(g_file_upload_slots) / sizeof(g_file_upload_slots[0])); ++i) {
    FileUploadContext& slot = g_file_upload_slots[i];
    if (slot.in_use) continue;
    slot.reset();
    slot.in_use = true;
    slot.request = request;
    slot.path = path;
    slot.temp_path = fm_temp_path_for(path, op, i);
    std::snprintf(slot.operation, sizeof(slot.operation), "%s", op != nullptr ? op : "write");
    return &slot;
  }
  if (error_json != nullptr) *error_json = "{\"success\":false,\"error\":\"UPLOAD_BUSY\"}";
  return nullptr;
}

static bool fm_finalize_upload_context(FileUploadContext* context) {
  if (context == nullptr) return false;
  context->close();
  if (context->temp_path.empty() || context->path.empty()) return false;
  return mros::platform::mros_fs_rename(context->temp_path.c_str(), context->path.c_str());
}

static void fm_release_upload_context(FileUploadContext* context, const bool keep_temp = false) {
  if (context == nullptr) return;
  context->close();
  if (!keep_temp && !context->temp_path.empty()) {
    (void)mros::platform::mros_fs_remove(context->temp_path.c_str());
  }
  *context = {};
}

static size_t fm_active_upload_count() {
  size_t count = 0U;
  for (const FileUploadContext& slot : g_file_upload_slots) {
    if (slot.in_use) count++;
  }
  return count;
}

struct FileDownloadStatus {
  bool active = false;
  bool done = false;
  bool success = false;
  bool cancel_requested = false;
  bool temp_active = false;
  uint32_t started_ms = 0U;
  uint32_t updated_ms = 0U;
  uint32_t bytes_per_sec = 0U;
  int status_code = 0;
  int progress_pct = 0;
  int64_t total = -1;
  int64_t written = 0;
  char url[384] = {};
  char target[192] = {};
  char temp_path[224] = {};
  char sha256[65] = {};
  char guard[32] = "idle";
  char phase[32] = "idle";
  char error[64] = {};
};

static FileDownloadStatus g_file_download_status;
static portMUX_TYPE g_file_download_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t fm_free_bytes();

#ifndef MROS_FILE_FETCH_ALLOW_HTTP
#define MROS_FILE_FETCH_ALLOW_HTTP 0
#endif
#ifndef MROS_FILE_FETCH_ALLOW_PRIVATE_HOSTS
#define MROS_FILE_FETCH_ALLOW_PRIVATE_HOSTS 0
#endif
#ifndef MROS_FILE_FETCH_ALLOW_REDIRECTS
#define MROS_FILE_FETCH_ALLOW_REDIRECTS 0
#endif

#if defined(MROS_PRODUCTION_BUILD) && MROS_FILE_FETCH_ALLOW_HTTP
#error "MROS_PRODUCTION_BUILD must not allow HTTP file fetch"
#endif
#if defined(MROS_PRODUCTION_BUILD) && MROS_FILE_FETCH_ALLOW_PRIVATE_HOSTS
#error "MROS_PRODUCTION_BUILD must not allow private-host file fetch"
#endif
#if defined(MROS_PRODUCTION_BUILD) && MROS_FILE_FETCH_ALLOW_REDIRECTS
#error "MROS_PRODUCTION_BUILD must not allow file fetch redirects"
#endif

#if MROS_FILE_FETCH_ALLOW_HTTP
static constexpr bool kFileFetchHttpAllowed = true;
#else
static constexpr bool kFileFetchHttpAllowed = false;
#endif
#if MROS_FILE_FETCH_ALLOW_PRIVATE_HOSTS
static constexpr bool kFileFetchPrivateHostsAllowed = true;
#else
static constexpr bool kFileFetchPrivateHostsAllowed = false;
#endif
#if MROS_FILE_FETCH_ALLOW_REDIRECTS
static constexpr bool kFileFetchRedirectsAllowed = true;
#else
static constexpr bool kFileFetchRedirectsAllowed = false;
#endif
static constexpr int kFileFetchMaxRedirects =
    kFileFetchRedirectsAllowed ? 3 : 0;

static std::string fm_lower_copy(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

static std::string fm_basename_from_url(const std::string& url) {
  size_t end = url.find('?');
  if (end == std::string::npos) end = url.size();
  while (end > 0U && url[end - 1U] == '/') --end;
  const size_t slash = url.rfind('/', end == 0U ? 0U : end - 1U);
  std::string name = slash == std::string::npos ? url.substr(0U, end)
                                                : url.substr(slash + 1U, end - slash - 1U);
  if (name.empty()) name = "download.bin";
  for (char& ch : name) {
    if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
        ch == '"' || ch == '<' || ch == '>' || ch == '|') {
      ch = '_';
    }
  }
  return name;
}

static bool fm_has_rejected_text_char(const std::string& text) {
  for (const unsigned char ch : text) {
    if (ch < 0x20U || ch == 0x7fU || ch == '"' || ch == '\'' || ch == '`' ||
        ch == '<' || ch == '>' || ch == '|') {
      return true;
    }
  }
  return false;
}

static bool fm_has_suffix(const std::string& text, const char* suffix) {
  if (suffix == nullptr) {
    return false;
  }
  const size_t suffix_len = strlen(suffix);
  return text.size() >= suffix_len &&
         text.compare(text.size() - suffix_len, suffix_len, suffix) == 0;
}

static std::string fm_strip_trailing_dots(std::string host) {
  while (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  return host;
}

static bool fm_is_all_digits(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (const unsigned char ch : text) {
    if (!std::isdigit(ch)) {
      return false;
    }
  }
  return true;
}

static bool fm_host_dns_syntax_ok(const std::string& host) {
  if (host.empty() || host.size() > 253U || host.find("..") != std::string::npos) {
    return false;
  }
  for (const unsigned char ch : host) {
    if (!std::isalnum(ch) && ch != '-' && ch != '.') {
      return false;
    }
  }
  return true;
}

static bool fm_parse_ipv4_literal(const std::string& host, uint8_t octets[4]) {
  size_t start = 0U;
  for (int part = 0; part < 4; ++part) {
    const size_t dot = host.find('.', start);
    const size_t end = dot == std::string::npos ? host.size() : dot;
    if ((part < 3 && dot == std::string::npos) ||
        (part == 3 && dot != std::string::npos) || end <= start ||
        (end - start) > 3U) {
      return false;
    }
    int value = 0;
    for (size_t i = start; i < end; ++i) {
      const unsigned char ch = static_cast<unsigned char>(host[i]);
      if (!std::isdigit(ch)) {
        return false;
      }
      value = (value * 10) + static_cast<int>(ch - '0');
      if (value > 255) {
        return false;
      }
    }
    octets[part] = static_cast<uint8_t>(value);
    start = end + 1U;
  }
  return start == host.size() + 1U;
}

static bool fm_is_blocked_ipv4_literal(const uint8_t octets[4]) {
  const uint8_t a = octets[0];
  const uint8_t b = octets[1];
  if (a == 0U || a == 10U || a == 127U) return true;
  if (a == 100U && b >= 64U && b <= 127U) return true;
  if (a == 169U && b == 254U) return true;
  if (a == 172U && b >= 16U && b <= 31U) return true;
  if (a == 192U && b == 168U) return true;
  if (a == 198U && (b == 18U || b == 19U)) return true;
  if (a >= 224U) return true;
  return false;
}

static bool fm_split_download_authority(const std::string& authority,
                                        std::string* host,
                                        std::string* port,
                                        std::string* error) {
  if (authority.empty() || authority.find('@') != std::string::npos ||
      authority.find_first_of(" \t\r\n") != std::string::npos) {
    if (error != nullptr) *error = "BAD_HOST";
    return false;
  }

  std::string parsed_host;
  std::string parsed_port;
  if (authority[0] == '[') {
    const size_t close = authority.find(']');
    if (close == std::string::npos || close <= 1U) {
      if (error != nullptr) *error = "BAD_HOST";
      return false;
    }
    parsed_host = authority.substr(1U, close - 1U);
    if (close + 1U < authority.size()) {
      if (authority[close + 1U] != ':') {
        if (error != nullptr) *error = "BAD_HOST";
        return false;
      }
      parsed_port = authority.substr(close + 2U);
    }
  } else {
    if (authority.find('[') != std::string::npos ||
        authority.find(']') != std::string::npos) {
      if (error != nullptr) *error = "BAD_HOST";
      return false;
    }
    const size_t first_colon = authority.find(':');
    const size_t last_colon = authority.rfind(':');
    if (first_colon != std::string::npos && first_colon != last_colon) {
      if (error != nullptr) *error = "BAD_HOST";
      return false;
    }
    if (last_colon != std::string::npos) {
      parsed_host = authority.substr(0U, last_colon);
      parsed_port = authority.substr(last_colon + 1U);
    } else {
      parsed_host = authority;
    }
  }

  if (parsed_host.empty() || parsed_port.size() > 5U) {
    if (error != nullptr) *error = parsed_host.empty() ? "EMPTY_HOST" : "BAD_PORT";
    return false;
  }
  if (!parsed_port.empty()) {
    uint32_t value = 0U;
    for (const unsigned char ch : parsed_port) {
      if (!std::isdigit(ch)) {
        if (error != nullptr) *error = "BAD_PORT";
        return false;
      }
      value = (value * 10U) + static_cast<uint32_t>(ch - '0');
      if (value > 65535U) {
        if (error != nullptr) *error = "BAD_PORT";
        return false;
      }
    }
    if (value == 0U) {
      if (error != nullptr) *error = "BAD_PORT";
      return false;
    }
  }

  if (host != nullptr) *host = parsed_host;
  if (port != nullptr) *port = parsed_port;
  return true;
}

static bool fm_is_blocked_download_host(std::string host) {
  host = fm_strip_trailing_dots(fm_lower_copy(host));
  if (host.empty()) return true;
  if (host == "localhost" || fm_has_suffix(host, ".localhost") ||
      fm_has_suffix(host, ".local") || fm_has_suffix(host, ".lan") ||
      fm_has_suffix(host, ".home.arpa")) {
    return true;
  }
  if (host.find(':') != std::string::npos || host.find('%') != std::string::npos) {
    return true;
  }
  if (fm_is_all_digits(host) || host.rfind("0x", 0U) == 0U) {
    return true;
  }
  uint8_t octets[4] = {};
  if (fm_parse_ipv4_literal(host, octets)) {
    return fm_is_blocked_ipv4_literal(octets);
  }
  return false;
}

static bool fm_validate_download_url(const std::string& url, std::string* error) {
  constexpr size_t kMaxDownloadUrlLen = 383U;
  if (url.empty()) {
    if (error != nullptr) *error = "EMPTY_URL";
    return false;
  }
  if (url.size() > kMaxDownloadUrlLen) {
    if (error != nullptr) *error = "URL_TOO_LONG";
    return false;
  }
  if (fm_has_rejected_text_char(url) || url.find('\\') != std::string::npos) {
    if (error != nullptr) *error = "BAD_URL_CHARS";
    return false;
  }
  const std::string lower = fm_lower_copy(url);
  const bool http = lower.rfind("http://", 0U) == 0U;
  const bool https = lower.rfind("https://", 0U) == 0U;
  if (!http && !https) {
    if (error != nullptr) *error = "BAD_SCHEME";
    return false;
  }
  if (http && !kFileFetchHttpAllowed) {
    if (error != nullptr) *error = "HTTPS_REQUIRED";
    return false;
  }
  const size_t host_start = http ? strlen("http://") : strlen("https://");
  const size_t host_end = url.find_first_of("/?#", host_start);
  if (host_end == host_start) {
    if (error != nullptr) *error = "EMPTY_HOST";
    return false;
  }
  const std::string authority =
      url.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
  std::string host;
  std::string port;
  if (!fm_split_download_authority(authority, &host, &port, error)) {
    return false;
  }
  const std::string normalized_host = fm_strip_trailing_dots(fm_lower_copy(host));
  uint8_t octets[4] = {};
  const bool ipv4_literal = fm_parse_ipv4_literal(normalized_host, octets);
  const bool ipv6_literal = normalized_host.find(':') != std::string::npos;
  if (!ipv4_literal && !ipv6_literal && !fm_host_dns_syntax_ok(normalized_host)) {
    if (error != nullptr) *error = "BAD_HOST";
    return false;
  }
  if (!kFileFetchPrivateHostsAllowed && fm_is_blocked_download_host(normalized_host)) {
    if (error != nullptr) *error = "PRIVATE_HOST_BLOCKED";
    return false;
  }
  return true;
}

static std::string fm_resolve_download_target(const std::string& url,
                                              const std::string& raw_target,
                                              const std::string& current_path) {
  const std::string name = fm_basename_from_url(url);
  const std::string lower_name = fm_lower_copy(name);
  std::string target = raw_target.empty() ? "auto" : raw_target;
  if (target == "auto") {
    const bool update_like =
        lower_name.find(".bin") != std::string::npos ||
        lower_name.find("firmware") != std::string::npos ||
        lower_name.find("update") != std::string::npos;
    return std::string("/ESPUSER/") + (update_like ? "firmware/" : "downloads/") + name;
  }
  target = fm_normalize_path(target.c_str());
  if (target == "/ESPUSER" || target.back() == '/') {
    return target + (target.back() == '/' ? "" : "/") + name;
  }
  struct stat info {};
  if (fm_stat_logical(target, &info) && S_ISDIR(info.st_mode)) {
    return target + "/" + name;
  }
  if (!fm_starts_with_path(target, "/ESPUSER")) {
    const std::string base = fm_normalize_path(current_path.c_str());
    while (!target.empty() && target[0] == '/') target.erase(target.begin());
    return base + "/" + target;
  }
  return target;
}

static bool fm_validate_download_target(const std::string& target, std::string* error) {
  constexpr size_t kMaxDownloadTargetLen = sizeof(g_file_download_status.target) - 1U;
  if (target.empty() || target.size() > kMaxDownloadTargetLen) {
    if (error != nullptr) *error = "TARGET_TOO_LONG";
    return false;
  }
  if (fm_has_rejected_text_char(target)) {
    if (error != nullptr) *error = "BAD_TARGET_CHARS";
    return false;
  }
  if (!fm_is_user_write_target(target)) {
    if (error != nullptr) *error = "PERMISSION_DENIED";
    return false;
  }
  return true;
}

static bool fm_validate_fetch_request(const std::string& url,
                                      const std::string& raw_target,
                                      const std::string& current_path,
                                      std::string* target,
                                      std::string* error) {
  if (!fm_validate_download_url(url, error)) {
    return false;
  }
  const std::string resolved = fm_resolve_download_target(url, raw_target, current_path);
  if (!fm_validate_download_target(resolved, error)) {
    return false;
  }
  if (target != nullptr) *target = resolved;
  return true;
}

static bool fm_is_firmware_like_target(const std::string& target) {
  const std::string lower = fm_lower_copy(target);
  return lower.size() >= 4U && lower.rfind(".bin") == (lower.size() - 4U);
}

static uint32_t fm_app0_partition_size() {
  const esp_partition_t* app0 = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "app0");
  return app0 != nullptr ? app0->size : 0U;
}

static bool fm_download_guard_content_length(const std::string& target,
                                             const int64_t total,
                                             std::string* guard) {
  if (total < 0) {
    if (guard != nullptr) *guard = "SIZE_UNKNOWN";
    return false;
  }
  if (fm_is_firmware_like_target(target)) {
    const uint32_t app0_size = fm_app0_partition_size();
    if (app0_size > 0U && static_cast<uint64_t>(total) > app0_size) {
      if (guard != nullptr) *guard = "APP_TOO_LARGE";
      return false;
    }
  }
  if (static_cast<uint64_t>(total) > fm_free_bytes()) {
    if (guard != nullptr) *guard = "NO_SPACE";
    return false;
  }
  if (guard != nullptr) *guard = "OK";
  return true;
}

static bool fm_ensure_dir_recursive(const std::string& dir_path) {
  if (!fm_is_visible_user_path(dir_path)) return false;
  if (dir_path == "/ESPUSER") return true;
  std::string current = "/ESPUSER";
  size_t pos = strlen("/ESPUSER/");
  while (pos <= dir_path.size()) {
    const size_t slash = dir_path.find('/', pos);
    const std::string next = dir_path.substr(0U, slash == std::string::npos ? dir_path.size() : slash);
    if (!next.empty()) {
      struct stat info {};
      if (!fm_stat_logical(next, &info)) {
        (void)mros::platform::mros_fs_mkdir(next.c_str());
      } else if (!S_ISDIR(info.st_mode)) {
        return false;
      }
    }
    if (slash == std::string::npos) break;
    pos = slash + 1U;
  }
  struct stat info {};
  return fm_stat_logical(dir_path, &info) && S_ISDIR(info.st_mode);
}

static uint64_t fm_free_bytes() {
  uint64_t total = 0U;
  uint64_t used = 0U;
  (void)mros::platform::mros_fs_info(&total, &used);
  return total > used ? (total - used) : 0U;
}

static String fm_i64_json(const int64_t value) {
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
  return String(buffer);
}

static void fm_sha256_hex(const uint8_t digest[32], char out[65]) {
  static constexpr char kHex[] = "0123456789abcdef";
  for (size_t i = 0U; i < 32U; ++i) {
    out[i * 2U] = kHex[(digest[i] >> 4U) & 0x0fU];
    out[(i * 2U) + 1U] = kHex[digest[i] & 0x0fU];
  }
  out[64] = '\0';
}

static void fm_download_set_status(const char* phase, bool active, bool done,
                                   bool success, int status_code,
                                   int64_t total, int64_t written,
                                   const char* error = nullptr) {
  portENTER_CRITICAL(&g_file_download_mux);
  g_file_download_status.active = active;
  g_file_download_status.done = done;
  g_file_download_status.success = success;
  g_file_download_status.updated_ms = mros::platform::mros_millis();
  g_file_download_status.status_code = status_code;
  g_file_download_status.total = total;
  g_file_download_status.written = written;
  const uint32_t elapsed_ms =
      g_file_download_status.started_ms > 0U
          ? (g_file_download_status.updated_ms - g_file_download_status.started_ms)
          : 0U;
  g_file_download_status.bytes_per_sec =
      elapsed_ms > 0U ? static_cast<uint32_t>((written * 1000LL) / elapsed_ms) : 0U;
  g_file_download_status.progress_pct =
      total > 0 ? static_cast<int>((written * 100LL) / total) : 0;
  if (g_file_download_status.progress_pct > 100) g_file_download_status.progress_pct = 100;
  if (phase != nullptr) {
    strncpy(g_file_download_status.phase, phase, sizeof(g_file_download_status.phase) - 1U);
    g_file_download_status.phase[sizeof(g_file_download_status.phase) - 1U] = '\0';
  }
  if (error != nullptr) {
    strncpy(g_file_download_status.error, error, sizeof(g_file_download_status.error) - 1U);
    g_file_download_status.error[sizeof(g_file_download_status.error) - 1U] = '\0';
  } else if (!done || success) {
    g_file_download_status.error[0] = '\0';
  }
  portEXIT_CRITICAL(&g_file_download_mux);
}

static void fm_download_set_guard(const char* guard) {
  portENTER_CRITICAL(&g_file_download_mux);
  std::snprintf(g_file_download_status.guard, sizeof(g_file_download_status.guard),
                "%s", guard != nullptr ? guard : "UNKNOWN");
  portEXIT_CRITICAL(&g_file_download_mux);
}

static void fm_download_set_temp_active(const bool active) {
  portENTER_CRITICAL(&g_file_download_mux);
  g_file_download_status.temp_active = active;
  portEXIT_CRITICAL(&g_file_download_mux);
}

static void fm_download_set_sha256(const char* sha256) {
  portENTER_CRITICAL(&g_file_download_mux);
  std::snprintf(g_file_download_status.sha256, sizeof(g_file_download_status.sha256),
                "%s", sha256 != nullptr ? sha256 : "");
  portEXIT_CRITICAL(&g_file_download_mux);
}

static bool fm_download_cancel_requested() {
  bool requested = false;
  portENTER_CRITICAL(&g_file_download_mux);
  requested = g_file_download_status.cancel_requested;
  portEXIT_CRITICAL(&g_file_download_mux);
  return requested;
}

struct FileDownloadTaskArgs {
  std::string url;
  std::string target;
  std::string temp_path;
};

static void fm_download_task(void* arg) {
  std::unique_ptr<FileDownloadTaskArgs> task(static_cast<FileDownloadTaskArgs*>(arg));
  if (!task) {
    vTaskDelete(nullptr);
    return;
  }
  fm_download_set_status("connect", true, false, false, 0, -1, 0);
  fm_download_set_guard("pending");

  mros::platform::HttpClientStream stream {};
  mros::platform::HttpClientConfig http_config {};
  http_config.allow_insecure_tls = false;
  http_config.allow_private_hosts = kFileFetchPrivateHostsAllowed;
  http_config.max_redirects = kFileFetchMaxRedirects;
  http_config.timeout_ms = 20000;
  http_config.buffer_size = 1024U;
  if (!mros::platform::mros_http_client_begin_get(task->url.c_str(), http_config, &stream)) {
    fm_download_set_status("failed", false, true, false, 0, -1, 0, "CONNECT_FAILED");
    vTaskDelete(nullptr);
    return;
  }
  if (stream.status_code < 200 || stream.status_code >= 300) {
    fm_download_set_status("failed", false, true, false, stream.status_code,
                           stream.content_length, 0, "HTTP_ERROR");
    mros::platform::mros_http_client_close(&stream);
    vTaskDelete(nullptr);
    return;
  }
  if (stream.content_length < 0) {
    fm_download_set_guard("SIZE_UNKNOWN");
    fm_download_set_status("failed", false, true, false, stream.status_code,
                           stream.content_length, 0, "SIZE_UNKNOWN");
    mros::platform::mros_http_client_close(&stream);
    vTaskDelete(nullptr);
    return;
  }
  std::string guard;
  if (!fm_download_guard_content_length(task->target, stream.content_length, &guard)) {
    fm_download_set_guard(guard.c_str());
    fm_download_set_status("failed", false, true, false, stream.status_code,
                           stream.content_length, 0, guard.c_str());
    mros::platform::mros_http_client_close(&stream);
    vTaskDelete(nullptr);
    return;
  }
  fm_download_set_guard("OK");
  if (!fm_ensure_dir_recursive(fm_parent_path(task->target))) {
    fm_download_set_status("failed", false, true, false, stream.status_code,
                           stream.content_length, 0, "PARENT_FAILED");
    mros::platform::mros_http_client_close(&stream);
    vTaskDelete(nullptr);
    return;
  }
  FILE* file = mros::platform::mros_fs_open(task->temp_path.c_str(), "wb");
  if (file == nullptr) {
    fm_download_set_status("failed", false, true, false, stream.status_code,
                           stream.content_length, 0, "OPEN_FAILED");
    mros::platform::mros_http_client_close(&stream);
    vTaskDelete(nullptr);
    return;
  }
  fm_download_set_temp_active(true);

  uint8_t buffer[1024] = {};
  uint8_t digest[32] = {};
  char digest_hex[65] = {};
  mbedtls_md_context_t sha_ctx {};
  mbedtls_md_init(&sha_ctx);
  const mbedtls_md_info_t* sha_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  bool sha_ok = sha_info != nullptr && mbedtls_md_setup(&sha_ctx, sha_info, 0) == 0 &&
                mbedtls_md_starts(&sha_ctx) == 0;
  int64_t written = 0;
  bool ok = true;
  fm_download_set_status("download", true, false, false, stream.status_code,
                         stream.content_length, written);
  while (true) {
    if (fm_download_cancel_requested()) {
      ok = false;
      fm_download_set_status("cancelled", false, true, false, stream.status_code,
                             stream.content_length, written, "CANCELLED");
      break;
    }
    const int read_len = mros::platform::mros_http_client_read(&stream, buffer, sizeof(buffer));
    if (read_len < 0) {
      ok = false;
      fm_download_set_status("failed", false, true, false, stream.status_code,
                             stream.content_length, written, "READ_FAILED");
      break;
    }
    if (read_len == 0) break;
    if (fm_download_cancel_requested()) {
      ok = false;
      fm_download_set_status("cancelled", false, true, false, stream.status_code,
                             stream.content_length, written, "CANCELLED");
      break;
    }
    if (std::fwrite(buffer, 1U, static_cast<size_t>(read_len), file) !=
        static_cast<size_t>(read_len)) {
      ok = false;
      fm_download_set_status("failed", false, true, false, stream.status_code,
                             stream.content_length, written, "WRITE_FAILED");
      break;
    }
    if (sha_ok && mbedtls_md_update(&sha_ctx, buffer, static_cast<size_t>(read_len)) != 0) {
      sha_ok = false;
    }
    written += read_len;
    fm_download_set_status("download", true, false, false, stream.status_code,
                           stream.content_length, written);
  }
  std::fclose(file);
  mros::platform::mros_http_client_close(&stream);
  if (!ok) {
    (void)mros::platform::mros_fs_remove(task->temp_path.c_str());
    fm_download_set_temp_active(false);
    mbedtls_md_free(&sha_ctx);
    vTaskDelete(nullptr);
    return;
  }
  if (stream.content_length > 0 && written != stream.content_length) {
    (void)mros::platform::mros_fs_remove(task->temp_path.c_str());
    fm_download_set_temp_active(false);
    mbedtls_md_free(&sha_ctx);
    fm_download_set_status("failed", false, true, false, stream.status_code,
                           stream.content_length, written, "SIZE_MISMATCH");
    vTaskDelete(nullptr);
    return;
  }
  if (sha_ok && mbedtls_md_finish(&sha_ctx, digest) == 0) {
    fm_sha256_hex(digest, digest_hex);
    fm_download_set_sha256(digest_hex);
  }
  mbedtls_md_free(&sha_ctx);
  if (!mros::platform::mros_fs_rename(task->temp_path.c_str(), task->target.c_str())) {
    (void)mros::platform::mros_fs_remove(task->temp_path.c_str());
    fm_download_set_temp_active(false);
    fm_download_set_status("failed", false, true, false, stream.status_code,
                           stream.content_length, written, "RENAME_FAILED");
    vTaskDelete(nullptr);
    return;
  }
  fm_download_set_temp_active(false);
  fm_download_set_status("complete", false, true, true, stream.status_code,
                         stream.content_length, written);
  vTaskDelete(nullptr);
}

static String fm_download_status_json() {
  FileDownloadStatus copy {};
  portENTER_CRITICAL(&g_file_download_mux);
  copy = g_file_download_status;
  portEXIT_CRITICAL(&g_file_download_mux);
  const uint32_t now = mros::platform::mros_millis();
  const uint32_t elapsed_ms = copy.started_ms > 0U ? now - copy.started_ms : 0U;
  int32_t eta_ms = -1;
  if (copy.active && copy.total > 0 && copy.written > 0 && elapsed_ms > 0U) {
    const int64_t remaining = copy.total - copy.written;
    const int64_t rate = (copy.written * 1000LL) / elapsed_ms;
    if (remaining >= 0 && rate > 0) eta_ms = static_cast<int32_t>((remaining * 1000LL) / rate);
  }
  String json = "{\"success\":true,";
  json += "\"active\":" + String(copy.active ? "true" : "false") + ",";
  json += "\"done\":" + String(copy.done ? "true" : "false") + ",";
  json += "\"ok\":" + String(copy.success ? "true" : "false") + ",";
  json += "\"phase\":\"" + jsonEscape(String(copy.phase)) + "\",";
  json += "\"error\":\"" + jsonEscape(String(copy.error)) + "\",";
  json += "\"url\":\"" + jsonEscape(String(copy.url)) + "\",";
  json += "\"target\":\"" + jsonEscape(String(copy.target)) + "\",";
  json += "\"temp_active\":" + String(copy.temp_active ? "true" : "false") + ",";
  json += "\"cancel_requested\":" + String(copy.cancel_requested ? "true" : "false") + ",";
  json += "\"guard\":\"" + jsonEscape(String(copy.guard)) + "\",";
  json += "\"sha256\":\"" + jsonEscape(String(copy.sha256)) + "\",";
  json += "\"bytes_per_sec\":" + String(copy.bytes_per_sec) + ",";
  json += "\"status_code\":" + String(copy.status_code) + ",";
  json += "\"progress\":" + String(copy.progress_pct) + ",";
  json += "\"total\":" + fm_i64_json(copy.total) + ",";
  json += "\"written\":" + fm_i64_json(copy.written) + ",";
  json += "\"elapsed_ms\":" + String(elapsed_ms) + ",";
  json += "\"eta_ms\":" + String(eta_ms);
  json += "}";
  return json;
}

static bool is_safe_asset_suffix(const String &suffix) {
  if (suffix.length() == 0 || suffix == "/") return false;
  if (suffix.indexOf("..") >= 0) return false;
  if (suffix.indexOf('\\') >= 0) return false;
  return true;
}

static void send_large_asset_psram_stream(AsyncWebServerRequest *request,
                                          const char *uri_prefix,
                                          const char *fs_prefix) {
  if (!request || !uri_prefix || !fs_prefix) {
    return;
  }
  String url = request->url();
  const int query_pos = url.indexOf('?');
  if (query_pos >= 0) {
    url = url.substring(0, query_pos);
  }
  const bool is_cad = (strcmp(uri_prefix, "/cad") == 0);
  if (is_cad) {
    g_cad_stream_requests++;
  } else {
    g_asset_stream_requests++;
  }
  if (!url.startsWith(uri_prefix)) {
    request->send(404, "text/plain", "Not Found");
    return;
  }

  String suffix = url.substring(strlen(uri_prefix));
  if (!is_safe_asset_suffix(suffix)) {
    if (is_cad) {
      g_cad_stream_misses++;
    } else {
      g_asset_stream_misses++;
    }
    request->send(404, "text/plain", "Not Found");
    return;
  }

  String fs_path = String(fs_prefix);
  if (!suffix.startsWith("/")) fs_path += "/";
  fs_path += suffix;

  if (!mros::platform::mros_fs_exists(fs_path.c_str())) {
    if (is_cad) {
      g_cad_stream_misses++;
    } else {
      g_asset_stream_misses++;
    }
    request->send(404, "text/plain", "Missing asset");
    return;
  }

  String served_path = fs_path;
  const char* content_encoding = nullptr;
  String gzip_path = fs_path + ".gz";
  if (!fs_path.endsWith(".gz") && mros::platform::mros_fs_exists(gzip_path.c_str())) {
    served_path = gzip_path;
    content_encoding = "gzip";
  }
  size_t served_size = 0U;
  (void)mros::platform::mros_fs_file_size(served_path.c_str(), &served_size);
  const String etag = asset_etag_for_path(served_path.c_str(), served_size);
  const char *mime = asset_mime_from_path(fs_path);
  if (!web_async_send_littlefs_stream_psram(
          request, served_path.c_str(), mime, false, kImmutableAssetCacheControl,
          content_encoding, etag.c_str())) {
    request->send(500, "text/plain", "Asset stream failed");
  }
}

struct TrajPointPSRAM {
  float x;
  float y;
  float z;
  float t_ms;
  float roll_deg;
  float ee_pitch_deg;
  float yaw_deg;
  uint8_t ee_auto;
};

struct PreviewPointPSRAM {
  float x;
  float y;
  float z;
};

struct TrajectoryPreviewStreamState {
  size_t src_count = 0U;
  size_t stride = 1U;
  size_t next_index = 0U;
  size_t prefix_pos = 0U;
  size_t record_pos = 0U;
  size_t record_len = 0U;
  size_t suffix_pos = 0U;
  bool first_record = true;
  char prefix[128] = {};
  char record[96] = {};
};

static TrajPointPSRAM *psram_traj_points = nullptr;
static PreviewPointPSRAM *psram_preview_points = nullptr;
static size_t psram_traj_capacity = 0;
static size_t psram_preview_capacity = 0;
static size_t psram_traj_count = 0;
static size_t psram_preview_count = 0;
static bool psram_buffers_ready = false;
static bool psram_traj_last_truncated = false;
static float psram_preview_step_mm = 2.0f;
static constexpr size_t kLargeTolerantPsramOnlyThreshold = 4096;
static constexpr size_t kStaticTrajCapacity = 32768;
static constexpr size_t kStaticPreviewCapacity = 131072;
#if MROS_USE_STATIC_PSRAM_WEB_BUFFERS && \
    defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
static EXT_RAM_BSS_ATTR TrajPointPSRAM g_psram_traj_points_storage[kStaticTrajCapacity];
static EXT_RAM_BSS_ATTR PreviewPointPSRAM g_psram_preview_points_storage[kStaticPreviewCapacity];
#endif

static void *alloc_tolerant_buffer(size_t bytes) {
  if (bytes == 0) return nullptr;
  const bool has_psram = mros::platform::mros_system_has_psram();
  if (has_psram) {
    void *ptr =
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) {
      g_psram_lazy_alloc_count++;
      g_psram_lazy_alloc_bytes += static_cast<uint32_t>(
          std::min<size_t>(bytes, UINT32_MAX));
      return ptr;
    }
    // Keep large tolerant allocations out of internal SRAM when PSRAM exists.
    if (bytes > kLargeTolerantPsramOnlyThreshold) {
      g_psram_alloc_fail_count++;
      return nullptr;
    }
  }
  void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (ptr != nullptr) {
    g_psram_internal_fallback_count++;
    g_psram_internal_fallback_bytes += static_cast<uint32_t>(
        std::min<size_t>(bytes, UINT32_MAX));
  } else {
    g_psram_alloc_fail_count++;
  }
  return ptr;
}

static char *ensure_shell_json_forward_buffer() {
  if (g_shell_json_forward_buf != nullptr) {
    return g_shell_json_forward_buf;
  }
#if MROS_USE_STATIC_PSRAM_WEB_BUFFERS && \
    defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
  g_shell_json_forward_buf = g_shell_json_forward_storage;
  return g_shell_json_forward_buf;
#endif
  g_shell_json_forward_buf = static_cast<char *>(
      alloc_tolerant_buffer(kShellJsonForwardCapacity));
  if (g_shell_json_forward_buf != nullptr) {
    g_shell_forward_lazy_allocated = true;
  }
  return g_shell_json_forward_buf;
}

struct ConsoleSnapshotContext {
  char* data = nullptr;
  size_t len = 0U;

  ~ConsoleSnapshotContext() {
    if (data != nullptr) {
      heap_caps_free(data);
      data = nullptr;
    }
  }
};

static bool send_console_snapshot_psram(AsyncWebServerRequest* request) {
  if (request == nullptr) {
    return false;
  }
  const size_t snapshot_size = uart1_cobs_get_system_logs_size();
  if (snapshot_size == 0U) {
    AsyncWebServerResponse* response =
        request->beginResponse(200, "text/plain; charset=utf-8", "");
    add_no_cache_headers(response);
    request->send(response);
    return true;
  }

  auto ctx = std::make_shared<ConsoleSnapshotContext>();
  ctx->data = static_cast<char*>(alloc_tolerant_buffer(snapshot_size + 1U));
  if (ctx->data == nullptr) {
    return false;
  }
  if (!uart1_cobs_copy_system_logs(ctx->data, snapshot_size + 1U, &ctx->len)) {
    return false;
  }

  AsyncWebServerResponse* response = request->beginChunkedResponse(
      "text/plain; charset=utf-8",
      [ctx](uint8_t* buffer, size_t max_len, size_t index) -> size_t {
        if (buffer == nullptr || max_len == 0U || index >= ctx->len) {
          return 0U;
        }
        const size_t remaining = ctx->len - index;
        const size_t copy_len = remaining < max_len ? remaining : max_len;
        memcpy(buffer, ctx->data + index, copy_len);
        return copy_len;
      });
  if (response == nullptr) {
    return false;
  }
  add_no_cache_headers(response);
  request->send(response);
  return true;
}

static String json_escape_text(const char* text, const size_t len) {
  String escaped;
  escaped.reserve((len * 2U) + 8U);
  for (size_t i = 0; i < len; ++i) {
    const char ch = text[i];
    switch (ch) {
      case '\"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          char encoded[8] = {};
          std::snprintf(encoded, sizeof(encoded), "\\u%04x",
                        static_cast<unsigned int>(static_cast<unsigned char>(ch)));
          escaped += encoded;
        } else {
          escaped += ch;
        }
        break;
    }
  }
  return escaped;
}

static bool send_console_delta_json(AsyncWebServerRequest* request) {
  if (request == nullptr) {
    return false;
  }

  size_t offset = 0U;
  size_t max_bytes = 4096U;
  if (request->hasParam("offset")) {
    offset = static_cast<size_t>(
        strtoull(request->getParam("offset")->value().c_str(), nullptr, 10));
  }
  if (request->hasParam("max")) {
    max_bytes = static_cast<size_t>(
        strtoull(request->getParam("max")->value().c_str(), nullptr, 10));
  }
  if (max_bytes == 0U) {
    max_bytes = 4096U;
  }
  if (max_bytes > 16384U) {
    max_bytes = 16384U;
  }

  auto buffer = std::make_unique<char[]>(max_bytes + 1U);
  if (!buffer) {
    return false;
  }

  size_t copied_len = 0U;
  size_t next_offset = offset;
  size_t base_offset = 0U;
  bool truncated = false;
  if (!uart1_cobs_copy_system_logs_since(buffer.get(), max_bytes + 1U, offset,
                                         max_bytes, &copied_len, &next_offset,
                                         &base_offset, &truncated)) {
    return false;
  }

  String payload = "{";
  payload += "\"success\":true,";
  payload += "\"offset\":" + String(static_cast<uint32_t>(offset)) + ",";
  payload += "\"base_offset\":" + String(static_cast<uint32_t>(base_offset)) + ",";
  payload += "\"next_offset\":" + String(static_cast<uint32_t>(next_offset)) + ",";
  payload += "\"rev\":" + String(uart1_cobs_get_log_version()) + ",";
  payload += "\"truncated\":";
  payload += truncated ? "true" : "false";
  payload += ",\"text\":\"";
  payload += json_escape_text(buffer.get(), copied_len);
  payload += "\"}";

  AsyncWebServerResponse* response =
      request->beginResponse(200, "application/json; charset=utf-8", payload);
  add_no_cache_headers(response);
  request->send(response);
  return true;
}

static bool initPsrTrajectoryBuffers() {
  if (psram_buffers_ready) return true;
  const uint32_t total_psram = mros::platform::mros_system_psram_total();
  if (total_psram == 0) {
    mros_console.println("[TRAJ] PSRAM yok. Uzun kuyruk tamponu devre disi.");
    return false;
  }

  // ~1.5 MB toplam PSRAM kullanimi: uzun kuyruk + yogun onizleme.
  psram_traj_capacity = kStaticTrajCapacity;
  psram_preview_capacity = kStaticPreviewCapacity;

#if MROS_USE_STATIC_PSRAM_WEB_BUFFERS && \
    defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
  psram_traj_points = g_psram_traj_points_storage;
  psram_preview_points = g_psram_preview_points_storage;
#else
  psram_traj_points = static_cast<TrajPointPSRAM *>(
      heap_caps_malloc(sizeof(TrajPointPSRAM) * psram_traj_capacity,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  psram_preview_points = static_cast<PreviewPointPSRAM *>(
      heap_caps_malloc(sizeof(PreviewPointPSRAM) * psram_preview_capacity,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (!psram_traj_points || !psram_preview_points) {
    if (psram_traj_points) {
      heap_caps_free(psram_traj_points);
      psram_traj_points = nullptr;
    }
    if (psram_preview_points) {
      heap_caps_free(psram_preview_points);
      psram_preview_points = nullptr;
    }
    psram_traj_capacity = 0;
    psram_preview_capacity = 0;
    mros_console.println("[TRAJ] PSRAM ayirma basarisiz.");
    return false;
  }
  g_trajectory_buffers_lazy_allocated = true;
#endif
  g_trajectory_buffers_bytes =
      static_cast<uint32_t>(std::min<size_t>(
          (sizeof(TrajPointPSRAM) * psram_traj_capacity) +
              (sizeof(PreviewPointPSRAM) * psram_preview_capacity),
          UINT32_MAX));

  psram_traj_count = 0;
  psram_preview_count = 0;
  psram_traj_last_truncated = false;
  psram_buffers_ready = true;
  mros_console.printf("[TRAJ] PSRAM hazir. Kuyruk=%u, Onizleme=%u nokta.\n",
                      (unsigned)psram_traj_capacity,
                      (unsigned)psram_preview_capacity);
  return true;
}

static bool copyParsedTrajectoryToPsrBuffer(size_t *parsed_count) {
  if (parsed_count) *parsed_count = 0;
  if (!psram_buffers_ready || !psram_traj_points) return false;

  psram_traj_count = 0;
  psram_traj_last_truncated = false;

  const size_t src_count = trajectory_handler_count();
  for (size_t i = 0; i < src_count; i++) {
    TrajectoryPoint src {};
    if (!trajectory_handler_get_point(i, &src)) {
      break;
    }
    if (psram_traj_count < psram_traj_capacity) {
      TrajPointPSRAM &dst = psram_traj_points[psram_traj_count++];
      dst.x = src.x;
      dst.y = src.y;
      dst.z = src.z;
      dst.t_ms = (std::isfinite(src.t_ms) && src.t_ms > 0.0f) ? src.t_ms : 80.0f;
      dst.ee_auto = src.ee_auto ? 1 : 0;
      dst.roll_deg =
          std::isfinite(src.roll_deg) ? src.roll_deg : 0.0f;
      dst.ee_pitch_deg =
          std::isfinite(src.ee_pitch_deg) ? src.ee_pitch_deg : 0.0f;
      dst.yaw_deg =
          std::isfinite(src.yaw_deg) ? src.yaw_deg : 0.0f;
    } else {
      psram_traj_last_truncated = true;
    }
  }

  if (parsed_count) *parsed_count = psram_traj_count;
  return src_count > 0;
}

static void rebuildDensePreviewFromPsrTrajectory(float step_mm) {
  if (!psram_buffers_ready || !psram_traj_points || !psram_preview_points) return;
  psram_preview_count = 0;
  if (psram_traj_count == 0) return;

  const float step = (std::isfinite(step_mm) && step_mm > 0.1f) ? step_mm : 2.0f;
  psram_preview_step_mm = step;

  // First point
  psram_preview_points[psram_preview_count].x = psram_traj_points[0].x;
  psram_preview_points[psram_preview_count].y = psram_traj_points[0].y;
  psram_preview_points[psram_preview_count].z = psram_traj_points[0].z;
  psram_preview_count++;

  for (size_t i = 1; i < psram_traj_count; i++) {
    const TrajPointPSRAM &a = psram_traj_points[i - 1];
    const TrajPointPSRAM &b = psram_traj_points[i];
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dz = b.z - a.z;
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    int seg_steps = 1;
    if (std::isfinite(dist) && dist > step) {
      seg_steps = (int)ceilf(dist / step);
    }
    if (seg_steps < 1) seg_steps = 1;

    for (int s = 1; s <= seg_steps; s++) {
      if (psram_preview_count >= psram_preview_capacity) return;
      const float u = (float)s / (float)seg_steps;
      PreviewPointPSRAM &dst = psram_preview_points[psram_preview_count++];
      dst.x = a.x + dx * u;
      dst.y = a.y + dy * u;
      dst.z = a.z + dz * u;
    }
  }
}

enum class WsTicketScope : uint8_t {
  Telemetry = 0U,
  Shell,
  Debug,
  Mcp,
};

struct WsTicketClaim {
  String username;
  uint32_t capability_mask = 0U;
  WsTicketScope scope = WsTicketScope::Telemetry;
};

// WebSocket per-client authentication state
static std::map<uint32_t, bool> ws_auth_clients;
static std::map<uint32_t, bool> ws_full_snapshot_pending;
static std::map<uint32_t, bool> ws_scene_subscriptions;
static std::map<uint32_t, uint8_t> ws_subscription_masks;
static std::map<uint32_t, uint8_t> ws_telemetry_formats;
static std::map<uint32_t, bool> ws_shell_auth_clients;
static std::map<uint32_t, bool> ws_debug_auth_clients;
static std::map<uint32_t, bool> ws_debug_subscriptions;
static std::map<uint32_t, bool> ws_mcp_auth_clients;
static std::map<uint32_t, WsTicketClaim> ws_mcp_contexts;
static std::map<uint32_t, unsigned long> ws_auth_deadlines;
static std::map<uint32_t, unsigned long> ws_shell_auth_deadlines;
static std::map<uint32_t, unsigned long> ws_debug_auth_deadlines;
static std::map<uint32_t, unsigned long> ws_mcp_auth_deadlines;
static bool ws_traj_upload_active = false;
static uint32_t ws_traj_upload_client_id = 0;
static size_t ws_traj_upload_prefix_len = 0;
static float ws_traj_upload_preview_step_mm = 2.0f;
static const size_t WS_TICKET_SLOTS = 16;
static const unsigned long WS_TICKET_TTL_MS = 15000UL;
static const unsigned long WS_AUTH_TIMEOUT_MS = 5000UL;
struct WsTicketEntry {
  String token;
  String username;
  uint32_t capability_mask = 0U;
  WsTicketScope scope = WsTicketScope::Telemetry;
  unsigned long expires_at_ms = 0;
};
static WsTicketEntry ws_tickets[WS_TICKET_SLOTS];
static uint32_t ws_ticket_issued = 0;
static uint32_t ws_ticket_consumed = 0;
static uint32_t ws_ticket_expired = 0;
static uint32_t ws_ticket_evicted = 0;
static uint32_t ws_ticket_failed = 0;

static void resetWsTrajectoryUploadState();

static constexpr uint8_t WS_SUB_SCENE = 1U << 0;
static constexpr uint8_t WS_SUB_DEBUG = 1U << 1;
static constexpr uint8_t WS_SUB_SHELL = 1U << 2;
static constexpr uint8_t WS_SUB_TRAJECTORY = 1U << 3;
static constexpr uint8_t WS_SUB_LOGS = 1U << 4;
static constexpr uint8_t WS_SUB_RATE_FAST = 1U << 5;
static constexpr uint8_t WS_SUB_RATE_MEDIUM = 1U << 6;
static constexpr uint8_t WS_SUB_RATE_SLOW = 1U << 7;
static constexpr uint8_t WS_SUB_RATE_DEFAULT =
    WS_SUB_RATE_FAST | WS_SUB_RATE_MEDIUM | WS_SUB_RATE_SLOW;

static constexpr uint8_t WS_TELEMETRY_FORMAT_JSON_V1 = 0;
static constexpr uint8_t WS_TELEMETRY_FORMAT_BIN_V1 = 1;

struct WsTelemetryClientSnapshot {
  uint32_t id = 0;
  uint8_t profile = WS_SUB_RATE_DEFAULT;
  uint8_t format = WS_TELEMETRY_FORMAT_JSON_V1;
  bool pending_full = false;
};

struct WsDebugClientSnapshot {
  uint32_t id = 0;
};

static const char* wsTelemetryFormatName(const uint8_t format) {
  return format == WS_TELEMETRY_FORMAT_BIN_V1 ? "bin-v1" : "json-v1";
}

enum TelemetryBinField : uint8_t {
  BIN_FIELD_TURRET = 0,
  BIN_FIELD_GRIPPER = 1,
  BIN_FIELD_JOINTS = 2,
  BIN_FIELD_COORD_X = 3,
  BIN_FIELD_COORD_Y = 4,
  BIN_FIELD_COORD_Z = 5,
  BIN_FIELD_COORD_ROLL = 6,
  BIN_FIELD_COORD_PITCH = 7,
  BIN_FIELD_COORD_YAW = 8,
  BIN_FIELD_ALPHA = 9,
  BIN_FIELD_LP = 10,
  BIN_FIELD_FK_X = 11,
  BIN_FIELD_FK_Y = 12,
  BIN_FIELD_FK_Z = 13,
  BIN_FIELD_FK_A = 14,
  BIN_FIELD_t41_loop_ms = 15,
  BIN_FIELD_MOTOR = 16,
  BIN_FIELD_SPI_TOTAL = 17,
  BIN_FIELD_SPI_CRC_ERR = 18,
  BIN_FIELD_SPI_MARKER_ERR = 19,
  BIN_FIELD_SPI_ERR_REV = 20,
  BIN_FIELD_SPI_LAST_MARKER = 21,
  BIN_FIELD_SPI_CONNECTED = 22,
  BIN_FIELD_S3_DEVSTAT = 23,
  BIN_FIELD_ESPNOW_CONNECTED = 24,
  BIN_FIELD_C3_POS = 25,
  BIN_FIELD_C3_SPD = 26,
  BIN_FIELD_C3_ACC = 27,
  BIN_FIELD_C3_CONNECTED = 28,
  BIN_FIELD_C3_ESPNOW_ACTIVE = 29,
  BIN_FIELD_C3_CRC_ERR = 30,
  BIN_FIELD_C3_MARKER_ERR = 31,
  BIN_FIELD_C3_TOTAL_RX = 32,
  BIN_FIELD_C3_QUALITY = 33,
  BIN_FIELD_C3_HZ = 34,
  BIN_FIELD_PID_OUT = 35,
  BIN_FIELD_TRAJ_SCALE = 36,
  BIN_FIELD_OE = 37,
  BIN_FIELD_UPTIME = 38,
  BIN_FIELD_CONSOLE_REV = 39,
  BIN_FIELD_PCA_READY = 40,
  BIN_FIELD_WIFI_AP = 41,
};

static constexpr uint8_t BIN_FRAME_FULL = 1;
static constexpr uint8_t BIN_FRAME_FAST = 2;
static constexpr uint8_t BIN_FRAME_MEDIUM = 3;
static constexpr uint8_t BIN_FRAME_SLOW = 4;
static constexpr size_t BIN_HEADER_LEN = 28U;

static void bin_put_u32_at(std::vector<uint8_t>& out,
                           const size_t offset,
                           const uint32_t value) {
  if ((offset + 4U) > out.size()) return;
  out[offset + 0U] = static_cast<uint8_t>(value & 0xFFU);
  out[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  out[offset + 2U] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  out[offset + 3U] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

struct TelemetryBinFrame {
  std::vector<uint8_t> bytes;
  uint32_t mask0 = 0U;
  uint32_t mask1 = 0U;

  TelemetryBinFrame(const uint8_t frame_type,
                    const uint32_t seq,
                    const uint32_t server_ms) {
    bytes.assign(BIN_HEADER_LEN, 0U);
    bytes[0] = 'M';
    bytes[1] = 'R';
    bytes[2] = 'B';
    bytes[3] = '1';
    bytes[4] = 1U;
    bytes[5] = static_cast<uint8_t>(BIN_HEADER_LEN);
    bytes[6] = frame_type;
    bytes[7] = 0U;
    bin_put_u32_at(bytes, 8U, seq);
    bin_put_u32_at(bytes, 12U, server_ms);
  }

  bool has_fields() const {
    return mask0 != 0U || mask1 != 0U;
  }

  void mark(const uint8_t field_id) {
    if (field_id < 32U) {
      mask0 |= (1UL << field_id);
    } else {
      mask1 |= (1UL << (field_id - 32U));
    }
  }

  void append_u8_raw(const uint8_t value) {
    bytes.push_back(value);
  }

  void append_u32_raw(const uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((value >> 24U) & 0xFFU));
  }

  void i32(const uint8_t field_id, const int32_t value) {
    mark(field_id);
    append_u32_raw(static_cast<uint32_t>(value));
  }

  void u32(const uint8_t field_id, const uint32_t value) {
    mark(field_id);
    append_u32_raw(value);
  }

  void boolean(const uint8_t field_id, const bool value) {
    mark(field_id);
    append_u8_raw(value ? 1U : 0U);
  }

  void i32_array6(const uint8_t field_id, const int32_t values[6]) {
    mark(field_id);
    for (int i = 0; i < 6; ++i) {
      append_u32_raw(static_cast<uint32_t>(values[i]));
    }
  }

  void finish() {
    const uint32_t payload_len =
        bytes.size() > BIN_HEADER_LEN
            ? static_cast<uint32_t>(bytes.size() - BIN_HEADER_LEN)
            : 0U;
    bin_put_u32_at(bytes, 16U, payload_len);
    bin_put_u32_at(bytes, 20U, mask0);
    bin_put_u32_at(bytes, 24U, mask1);
  }
};

static int32_t telemetry_q10(const float value) {
  return static_cast<int32_t>(lroundf(value * 10.0f));
}

static int32_t telemetry_q100(const float value) {
  return static_cast<int32_t>(lroundf(value * 100.0f));
}

static void ws_send_binary_telemetry(AsyncWebSocketClient* client,
                                     TelemetryBinFrame& frame) {
  if (client == nullptr || !frame.has_fields()) return;
  frame.finish();
  client->binary(frame.bytes.data(), frame.bytes.size());
  g_ws_bin_frame_count++;
  g_ws_bin_byte_count += static_cast<uint32_t>(frame.bytes.size());
  g_ws_last_bin_bytes = static_cast<uint32_t>(frame.bytes.size());
}

static bool ws_auth_map_is_authenticated(const std::map<uint32_t, bool> &auth_map,
                                         const uint32_t client_id) {
  auto it = auth_map.find(client_id);
  return it != auth_map.end() && it->second;
}

static bool ws_client_authenticated(std::map<uint32_t, bool> &auth_map,
                                    const uint32_t client_id) {
  ws_auth_lock();
  const bool ok = ws_auth_map_is_authenticated(auth_map, client_id);
  ws_auth_unlock();
  return ok;
}

static void ws_set_pending_auth(std::map<uint32_t, bool> &auth_map,
                                std::map<uint32_t, unsigned long> &deadline_map,
                                const uint32_t client_id) {
  ws_auth_lock();
  auth_map[client_id] = false;
  deadline_map[client_id] = mros::platform::mros_millis() + WS_AUTH_TIMEOUT_MS;
  ws_auth_unlock();
}

static void ws_mark_authenticated(std::map<uint32_t, bool> &auth_map,
                                  std::map<uint32_t, unsigned long> &deadline_map,
                                  const uint32_t client_id) {
  ws_auth_lock();
  auth_map[client_id] = true;
  deadline_map.erase(client_id);
  ws_auth_unlock();
}

static void ws_erase_auth(std::map<uint32_t, bool> &auth_map,
                          std::map<uint32_t, unsigned long> &deadline_map,
                          const uint32_t client_id) {
  ws_auth_lock();
  auth_map.erase(client_id);
  deadline_map.erase(client_id);
  ws_auth_unlock();
}

static void ws_set_telemetry_pending(const uint32_t client_id) {
  ws_auth_lock();
  ws_auth_clients[client_id] = false;
  ws_full_snapshot_pending[client_id] = false;
  ws_scene_subscriptions[client_id] = false;
  ws_subscription_masks[client_id] = WS_SUB_RATE_DEFAULT;
  ws_telemetry_formats[client_id] = WS_TELEMETRY_FORMAT_JSON_V1;
  ws_auth_deadlines[client_id] = mros::platform::mros_millis() + WS_AUTH_TIMEOUT_MS;
  ws_auth_unlock();
}

static void ws_mark_telemetry_authenticated(const uint32_t client_id) {
  ws_auth_lock();
  ws_auth_clients[client_id] = true;
  ws_full_snapshot_pending[client_id] = true;
  ws_scene_subscriptions[client_id] = false;
  ws_subscription_masks[client_id] = WS_SUB_RATE_DEFAULT;
  ws_telemetry_formats[client_id] = WS_TELEMETRY_FORMAT_JSON_V1;
  ws_auth_deadlines.erase(client_id);
  ws_auth_unlock();
}

static void ws_erase_telemetry_client(const uint32_t client_id) {
  bool reset_upload = false;
  ws_auth_lock();
  ws_auth_clients.erase(client_id);
  ws_full_snapshot_pending.erase(client_id);
  ws_scene_subscriptions.erase(client_id);
  ws_subscription_masks.erase(client_id);
  ws_telemetry_formats.erase(client_id);
  ws_auth_deadlines.erase(client_id);
  if (ws_traj_upload_active && ws_traj_upload_client_id == client_id) {
    reset_upload = true;
  }
  ws_auth_unlock();
  if (reset_upload) {
    resetWsTrajectoryUploadState();
  }
}

static void ws_set_debug_pending(const uint32_t client_id) {
  ws_auth_lock();
  ws_debug_auth_clients[client_id] = false;
  ws_debug_subscriptions[client_id] = false;
  ws_debug_auth_deadlines[client_id] = mros::platform::mros_millis() + WS_AUTH_TIMEOUT_MS;
  ws_auth_unlock();
}

static void ws_mark_debug_authenticated(const uint32_t client_id) {
  ws_auth_lock();
  ws_debug_auth_clients[client_id] = true;
  ws_debug_subscriptions[client_id] = false;
  ws_debug_auth_deadlines.erase(client_id);
  ws_auth_unlock();
}

static void ws_erase_debug_client(const uint32_t client_id) {
  ws_auth_lock();
  ws_debug_auth_clients.erase(client_id);
  ws_debug_subscriptions.erase(client_id);
  ws_debug_auth_deadlines.erase(client_id);
  ws_auth_unlock();
}

static void ws_set_debug_subscription(const uint32_t client_id, const bool subscribed) {
  ws_auth_lock();
  if (ws_auth_map_is_authenticated(ws_debug_auth_clients, client_id)) {
    ws_debug_subscriptions[client_id] = subscribed;
  }
  ws_auth_unlock();
}

static uint32_t ws_authenticated_count_locked(const std::map<uint32_t, bool> &auth_map) {
  uint32_t count = 0;
  for (const auto &item : auth_map) {
    if (item.second) ++count;
  }
  return count;
}

static uint32_t ws_debug_subscribed_count_locked() {
  uint32_t count = 0;
  for (const auto &item : ws_debug_subscriptions) {
    if (item.second && ws_auth_map_is_authenticated(ws_debug_auth_clients, item.first)) {
      ++count;
    }
  }
  return count;
}

// Login rate limiting (5 failures → 60 s block)
static uint8_t       login_fail_count     = 0;
static unsigned long login_block_until_ms = 0;

#ifndef MROS_INITIAL_SETUP_SECRET
#define MROS_INITIAL_SETUP_SECRET "CHANGE_ME_SETUP_SECRET"
#define MROS_INITIAL_SETUP_SECRET_PLACEHOLDER 1
#else
#define MROS_INITIAL_SETUP_SECRET_PLACEHOLDER 0
#endif

#if defined(MROS_PRODUCTION_BUILD) && MROS_INITIAL_SETUP_SECRET_PLACEHOLDER
#error "Production builds require a device-specific MROS_INITIAL_SETUP_SECRET"
#endif

static const char   *k_initial_setup_secret = MROS_INITIAL_SETUP_SECRET;
static constexpr size_t kAuthCookieHeaderMaxLen = 512U;
static constexpr size_t kAvatarDataMaxLen = 131072U;

static inline uint32_t shell_ws_encode_client_id(const uint32_t client_id) {
  return client_id | kShellWsClientMask;
}

static inline bool shell_ws_is_encoded_client_id(const uint32_t client_id) {
  return (client_id & kShellWsClientMask) != 0U;
}

static inline uint32_t shell_ws_decode_client_id(const uint32_t client_id) {
  return client_id & ~kShellWsClientMask;
}

// SHA-256 one-shot helper (used for login credential verification)
static String compute_sha256(const String &input) {
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md_info == nullptr) {
    return String();
  }
  uint8_t hash[32];
  if (mbedtls_md(md_info,
                 reinterpret_cast<const unsigned char *>(input.c_str()),
                 input.length(), hash) != 0) {
    return String();
  }
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
  hex[64] = '\0';
  return String(hex);
}

static constexpr uint32_t kAuthPasswordPbkdf2Iterations = 12000U;
static constexpr size_t kAuthPasswordSaltBytes = 16U;
static constexpr size_t kAuthPasswordMaxSaltBytes = 32U;
static constexpr size_t kAuthPasswordDigestBytes = 32U;
static constexpr const char* kAuthPasswordPbkdf2Prefix = "pbkdf2_sha256";

struct WebAuthPbkdf2Hash {
  uint32_t iterations = 0;
  uint8_t salt[kAuthPasswordMaxSaltBytes] = {};
  size_t salt_len = 0;
  uint8_t digest[kAuthPasswordDigestBytes] = {};
};

static bool web_auth_is_hex_len(const String& value, const size_t expected_len) {
  if (value.length() != expected_len) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

static bool web_auth_is_hex_even_range(const String& value,
                                       const size_t min_len,
                                       const size_t max_len) {
  if (value.length() < min_len || value.length() > max_len ||
      (value.length() % 2U) != 0U) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

static int web_auth_hex_nibble(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static String web_auth_hex_from_bytes(const uint8_t* bytes, const size_t len) {
  static const char kHex[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2U);
  for (size_t i = 0; i < len; ++i) {
    out += kHex[(bytes[i] >> 4) & 0x0F];
    out += kHex[bytes[i] & 0x0F];
  }
  return out;
}

static bool web_auth_hex_to_bytes(const String& hex,
                                  uint8_t* out,
                                  const size_t out_capacity,
                                  size_t* out_len) {
  if (out == nullptr || out_len == nullptr ||
      (hex.length() % 2U) != 0U ||
      (hex.length() / 2U) > out_capacity) {
    return false;
  }
  const size_t len = hex.length() / 2U;
  for (size_t i = 0; i < len; ++i) {
    const int high = web_auth_hex_nibble(hex[2U * i]);
    const int low = web_auth_hex_nibble(hex[(2U * i) + 1U]);
    if (high < 0 || low < 0) {
      return false;
    }
    out[i] = static_cast<uint8_t>((high << 4) | low);
  }
  *out_len = len;
  return true;
}

static bool web_auth_parse_uint32(const String& text, uint32_t* out) {
  if (out == nullptr || text.length() == 0U || text.length() > 8U) {
    return false;
  }
  uint32_t value = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return false;
    }
    value = (value * 10U) + static_cast<uint32_t>(c - '0');
  }
  if (value < 1000U) {
    return false;
  }
  *out = value;
  return true;
}

static bool web_auth_constant_time_equal(const uint8_t* a,
                                         const uint8_t* b,
                                         const size_t len) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0U;
}

static bool web_auth_hmac_sha256(const uint8_t* key,
                                 const size_t key_len,
                                 const uint8_t* input,
                                 const size_t input_len,
                                 uint8_t out[kAuthPasswordDigestBytes]) {
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md_info == nullptr || out == nullptr) {
    return false;
  }
  return mbedtls_md_hmac(md_info, key, key_len, input, input_len, out) == 0;
}

static bool web_auth_pbkdf2_sha256(const String& password,
                                   const uint8_t* salt,
                                   const size_t salt_len,
                                   const uint32_t iterations,
                                   uint8_t out[kAuthPasswordDigestBytes]) {
  if (password.length() == 0U || salt == nullptr || salt_len == 0U ||
      salt_len > kAuthPasswordMaxSaltBytes || iterations < 1000U ||
      out == nullptr) {
    return false;
  }
  uint8_t block_input[kAuthPasswordMaxSaltBytes + 4U] = {};
  memcpy(block_input, salt, salt_len);
  block_input[salt_len + 0U] = 0U;
  block_input[salt_len + 1U] = 0U;
  block_input[salt_len + 2U] = 0U;
  block_input[salt_len + 3U] = 1U;

  const uint8_t* key = reinterpret_cast<const uint8_t*>(password.c_str());
  const size_t key_len = password.length();
  uint8_t u[kAuthPasswordDigestBytes] = {};
  uint8_t t[kAuthPasswordDigestBytes] = {};
  if (!web_auth_hmac_sha256(key, key_len, block_input, salt_len + 4U, u)) {
    return false;
  }
  memcpy(t, u, sizeof(t));
  for (uint32_t round = 1U; round < iterations; ++round) {
    if (!web_auth_hmac_sha256(key, key_len, u, sizeof(u), u)) {
      return false;
    }
    for (size_t i = 0; i < sizeof(t); ++i) {
      t[i] ^= u[i];
    }
  }
  memcpy(out, t, kAuthPasswordDigestBytes);
  return true;
}

static bool web_auth_parse_pbkdf2_hash(const String& value,
                                       WebAuthPbkdf2Hash* parsed) {
  if (parsed == nullptr || !value.startsWith("pbkdf2_sha256$") ||
      value.length() > 160U) {
    return false;
  }
  const int p1 = value.indexOf('$');
  const int p2 = value.indexOf('$', p1 + 1);
  const int p3 = value.indexOf('$', p2 + 1);
  if (p1 <= 0 || p2 <= p1 + 1 || p3 <= p2 + 1 ||
      value.indexOf('$', p3 + 1) >= 0) {
    return false;
  }
  const String algorithm = value.substring(0, p1);
  const String iterations_text = value.substring(p1 + 1, p2);
  const String salt_hex = value.substring(p2 + 1, p3);
  const String digest_hex = value.substring(p3 + 1);
  if (algorithm != kAuthPasswordPbkdf2Prefix ||
      !web_auth_is_hex_even_range(salt_hex, 16U, kAuthPasswordMaxSaltBytes * 2U) ||
      !web_auth_is_hex_len(digest_hex, kAuthPasswordDigestBytes * 2U) ||
      !web_auth_parse_uint32(iterations_text, &parsed->iterations)) {
    return false;
  }
  if (!web_auth_hex_to_bytes(salt_hex, parsed->salt, sizeof(parsed->salt),
                             &parsed->salt_len)) {
    return false;
  }
  size_t digest_len = 0;
  if (!web_auth_hex_to_bytes(digest_hex, parsed->digest, sizeof(parsed->digest),
                             &digest_len) ||
      digest_len != kAuthPasswordDigestBytes) {
    return false;
  }
  return true;
}

static bool web_auth_stored_hash_format_ok(const String& value) {
  WebAuthPbkdf2Hash parsed;
  return web_auth_is_hex_len(value, 64U) ||
         web_auth_parse_pbkdf2_hash(value, &parsed);
}

static String web_auth_hash_password(const String& password) {
  if (password.length() == 0U || password.length() > kAuthPassMaxLen) {
    return String();
  }
  uint8_t salt[kAuthPasswordSaltBytes] = {};
  for (size_t i = 0; i < sizeof(salt); ++i) {
    salt[i] = static_cast<uint8_t>(esp_random() & 0xFFU);
  }
  uint8_t digest[kAuthPasswordDigestBytes] = {};
  if (!web_auth_pbkdf2_sha256(password, salt, sizeof(salt),
                              kAuthPasswordPbkdf2Iterations, digest)) {
    return String();
  }
  String encoded = kAuthPasswordPbkdf2Prefix;
  encoded += "$";
  encoded += String(static_cast<unsigned long>(kAuthPasswordPbkdf2Iterations));
  encoded += "$";
  encoded += web_auth_hex_from_bytes(salt, sizeof(salt));
  encoded += "$";
  encoded += web_auth_hex_from_bytes(digest, sizeof(digest));
  return encoded;
}

static bool web_auth_verify_hash(const String& password,
                                 const String& stored_hash,
                                 bool* needs_upgrade) {
  if (needs_upgrade != nullptr) {
    *needs_upgrade = false;
  }
  if (password.length() == 0U || stored_hash.length() == 0U) {
    return false;
  }
  if (web_auth_is_hex_len(stored_hash, 64U)) {
    const bool ok = compute_sha256(password).equalsIgnoreCase(stored_hash);
    if (ok && needs_upgrade != nullptr) {
      *needs_upgrade = true;
    }
    return ok;
  }
  WebAuthPbkdf2Hash parsed;
  if (!web_auth_parse_pbkdf2_hash(stored_hash, &parsed)) {
    return false;
  }
  uint8_t actual[kAuthPasswordDigestBytes] = {};
  if (!web_auth_pbkdf2_sha256(password, parsed.salt, parsed.salt_len,
                              parsed.iterations, actual)) {
    return false;
  }
  const bool ok = web_auth_constant_time_equal(actual, parsed.digest, sizeof(actual));
  if (ok && needs_upgrade != nullptr &&
      (parsed.iterations < kAuthPasswordPbkdf2Iterations ||
       parsed.salt_len < kAuthPasswordSaltBytes)) {
    *needs_upgrade = true;
  }
  return ok;
}

static String generateSecureToken(size_t bytes) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; i++) {
    uint8_t b = static_cast<uint8_t>(esp_random() & 0xFF);
    out += hex[(b >> 4) & 0x0F];
    out += hex[b & 0x0F];
  }
  return out;
}

static String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
    case '\"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<uint8_t>(c) < 0x20) {
        out += " ";
      } else {
        out += c;
      }
      break;
    }
  }
  return out;
}

static String shellCaptureJson(const char *command) {
  std::string output;
  const String username = auth_current_username_copy();
  const bool ok = mros::shell::execute_line_capture_as_user(
      command != nullptr ? command : "",
      username.c_str(),
      &output,
      false,
      mros::shell::ShellTransport::Web);
  if (output.size() > 8192U) output.resize(8192U);
  String body(output.c_str());
  body.replace("@@RAW_JSON@@", "");
  body.trim();
  if (body.length() == 0) {
    body = ok ? "{\"ok\":true}" : "{\"ok\":false,\"error_code\":\"COMMAND_FAILED\"}";
  } else if (body[0] != '{' && body[0] != '[') {
    String wrapped = "{\"ok\":";
    wrapped += ok ? "true" : "false";
    wrapped += ",\"output\":\"";
    wrapped += jsonEscape(body);
    wrapped += "\"}";
    body = wrapped;
  }
  return body;
}

static bool mshell_is_safe_atom_char(const char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '/' ||
         c == '_' || c == '-' || c == '.' || c == ':';
}

static bool mshell_validate_param(const String& value,
                                  const size_t max_len,
                                  const bool shell_atom) {
  if (value.length() == 0U || value.length() > max_len) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    const uint8_t uc = static_cast<uint8_t>(c);
    if (uc < 0x20U || uc == 0x7FU || c == '"' || c == '\\') {
      return false;
    }
    if (shell_atom && !mshell_is_safe_atom_char(c)) {
      return false;
    }
  }
  return true;
}

static void send_mshell_invalid_argument(AsyncWebServerRequest* request,
                                         const char* field) {
  String body = "{\"ok\":false,\"error_code\":\"INVALID_ARGUMENT\",\"field\":\"";
  body += jsonEscape(String(field != nullptr ? field : "unknown"));
  body += "\"}";
  request->send(400, "application/json", body);
}

static bool append_mshell_key_value(String& command,
                                    AsyncWebServerRequest* request,
                                    const char* field,
                                    const size_t max_len) {
  if (request == nullptr || field == nullptr || !request->hasParam(field, true)) {
    return true;
  }
  const String value = request->getParam(field, true)->value();
  if (!mshell_validate_param(value, max_len, false)) {
    send_mshell_invalid_argument(request, field);
    return false;
  }
  command += " ";
  command += field;
  command += "=\"";
  command += value;
  command += "\"";
  return true;
}

class PsramJsonWriter {
 public:
  explicit PsramJsonWriter(const size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0U) {
      return;
    }
    buffer_ = static_cast<char*>(
        heap_caps_malloc(capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer_ == nullptr) {
      buffer_ = static_cast<char*>(
          heap_caps_malloc(capacity_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (buffer_ != nullptr) {
      buffer_[0] = '\0';
    }
  }

  ~PsramJsonWriter() {
    if (buffer_ != nullptr) {
      heap_caps_free(buffer_);
      buffer_ = nullptr;
    }
  }

  PsramJsonWriter(const PsramJsonWriter&) = delete;
  PsramJsonWriter& operator=(const PsramJsonWriter&) = delete;

  void begin() {
    length_ = 0U;
    first_ = true;
    overflow_ = false;
    append_char('{');
  }

  void end() { append_char('}'); }

  void raw_field(const char* key, const char* value) {
    field_prefix(key);
    append(value != nullptr ? value : "null");
  }

  void raw_field(const char* key, const String& value) {
    raw_field(key, value.c_str());
  }

  void bool_field(const char* key, const bool value) {
    raw_field(key, value ? "true" : "false");
  }

  void i32_field(const char* key, const int32_t value) {
    char tmp[24] = {};
    std::snprintf(tmp, sizeof(tmp), "%ld", static_cast<long>(value));
    raw_field(key, tmp);
  }

  void u32_field(const char* key, const uint32_t value) {
    char tmp[24] = {};
    std::snprintf(tmp, sizeof(tmp), "%lu", static_cast<unsigned long>(value));
    raw_field(key, tmp);
  }

  void u64_field(const char* key, const uint64_t value) {
    char tmp[32] = {};
    std::snprintf(tmp, sizeof(tmp), "%llu",
                  static_cast<unsigned long long>(value));
    raw_field(key, tmp);
  }

  void float_field(const char* key, const float value, const unsigned decimals) {
    char tmp[32] = {};
    std::snprintf(tmp, sizeof(tmp), "%.*f", static_cast<int>(decimals),
                  static_cast<double>(value));
    raw_field(key, tmp);
  }

  void string_field(const char* key, const char* value) {
    field_prefix(key);
    append_json_string(value != nullptr ? value : "");
  }

  void string_field(const char* key, const String& value) {
    string_field(key, value.c_str());
  }

  const char* c_str() const {
    return buffer_ != nullptr ? buffer_ : "{}";
  }

  size_t length() const { return length_; }
  bool overflowed() const { return overflow_; }

 private:
  void field_prefix(const char* key) {
    if (!first_) {
      append_char(',');
    }
    first_ = false;
    append_char('"');
    append(key != nullptr ? key : "");
    append("\":");
  }

  void append_json_string(const char* text) {
    append_char('"');
    for (const char* p = text; p != nullptr && *p != '\0'; ++p) {
      switch (*p) {
        case '"': append("\\\""); break;
        case '\\': append("\\\\"); break;
        case '\n': append("\\n"); break;
        case '\r': append("\\r"); break;
        case '\t': append("\\t"); break;
        default:
          if (static_cast<unsigned char>(*p) < 0x20U) {
            char tmp[8] = {};
            std::snprintf(tmp, sizeof(tmp), "\\u%04x",
                          static_cast<unsigned int>(
                              static_cast<unsigned char>(*p)));
            append(tmp);
          } else {
            append_char(*p);
          }
          break;
      }
    }
    append_char('"');
  }

  void append(const char* text) {
    if (buffer_ == nullptr || text == nullptr) {
      overflow_ = true;
      return;
    }
    while (*text != '\0') {
      append_char(*text++);
    }
  }

  void append_char(const char ch) {
    if (buffer_ == nullptr || capacity_ == 0U || length_ + 1U >= capacity_) {
      overflow_ = true;
      if (buffer_ != nullptr && capacity_ > 0U) {
        buffer_[capacity_ - 1U] = '\0';
      }
      return;
    }
    buffer_[length_++] = ch;
    buffer_[length_] = '\0';
  }

  char* buffer_ = nullptr;
  size_t capacity_ = 0U;
  size_t length_ = 0U;
  bool first_ = true;
  bool overflow_ = false;
};

constexpr size_t kDeviceSettingsPayloadMax = 16U * 1024U;
constexpr const char* kDeviceSettingsNamespace = "web_cfg";
constexpr const char* kDeviceSettingsKey = "device_v1";

static const char* device_settings_default_json() {
  return "{\"schema_version\":1,"
         "\"net\":{\"autoScan\":true,\"rememberLastSsid\":true,"
         "\"showSignalDetails\":true,\"mdnsName\":\"mros-bridge\"},"
         "\"robot\":{\"mathBackend\":\"auto\",\"onboardMathEnabled\":false,"
         "\"mathProfile\":\"default\",\"trajectoryMode\":\"quintic\","
         "\"previewRequired\":true,\"singularityWarnings\":true,"
         "\"solver\":\"dls\",\"jacobian\":\"numerical\","
         "\"nullspace\":\"joint-center\",\"seedPolicy\":\"current\","
         "\"limitsProfile\":\"soft\",\"frame\":\"base\",\"units\":\"mm-deg\","
         "\"posTolMm\":1,\"oriTolDeg\":2,\"singularityThreshold\":0.05,"
         "\"alphaStep\":0.8,\"nullGain\":0.15,\"lambdaMax\":0.5,"
         "\"maxStepDeg\":10,\"maxIter\":120,\"pathHeightMode\":\"auto\","
         "\"groundZMm\":0,\"turretMode\":\"nearest\",\"cartStepMm\":12,"
         "\"yawStepDeg\":5,\"jumpRevoluteDeg\":35},"
         "\"services\":{\"sshEnabled\":false,\"mcpEnabled\":false,"
         "\"mcpAllowShell\":true},"
         "\"general\":{\"locale\":\"tr_TR.utf8\",\"startupPanel\":\"3d\","
         "\"deviceAlias\":\"mros-s3\",\"showBootSummary\":true},"
         "\"terminal\":{\"theme\":\"mros\",\"followUiTheme\":true,"
         "\"showRefreshButton\":true,\"showKillButton\":true,"
         "\"fontSize\":12,\"fullscreenFontSize\":13,"
         "\"fullscreenOpacity\":25,\"fullscreenBlurPx\":10,"
         "\"shellProfile\":\"operator\",\"historyLimit\":80,"
         "\"commandTimeoutMs\":5000,\"showManHints\":true,"
         "\"wrapLongOutput\":true,\"dynamicFetchLayout\":true},"
         "\"perf\":{\"telemetryProfile\":\"balanced\",\"logTailBytes\":8192,"
         "\"preferPsramBuffers\":true,\"reduceMotion\":false,"
         "\"dpmPolicy\":\"observe\",\"powerMode\":\"balanced\"},"
         "\"security\":{\"sessionTimeoutMin\":30,\"requireRootConfirm\":true,"
         "\"requireDangerConfirm\":true,\"allowRememberSession\":false},"
         "\"files\":{\"defaultView\":\"details\",\"showHidden\":false,"
         "\"multiSelect\":true,\"uploadLimitKb\":1024,"
         "\"openTextEditor\":true,\"defaultScale\":100,"
         "\"defaultSort\":\"name\",\"defaultSortDir\":\"asc\"},"
         "\"update\":{\"firmwarePath\":\"/ESPUSER/firmware\","
         "\"autoCheckRecovery\":true,\"requireBatterySafe\":true,"
         "\"rollbackGuard\":true,\"recoveryReadsDeviceSettings\":true,"
         "\"autoOtaScan\":false,\"otaScanHour\":\"03:00\","
         "\"scheduledReboot\":false,\"scheduledRebootHour\":\"04:00\","
         "\"updateWindow\":\"night\",\"lastErrorCode\":\"OK\"},"
         "\"storage\":{\"logTailBytes\":8192,\"warnFreeKb\":1024,\"keepLogDays\":7,"
         "\"cacheStaticAssets\":true},"
         "\"devtools\":{\"debugEndpoints\":true,\"websocketInspect\":false,"
         "\"rawJsonExport\":true,\"experimentalFlags\":false,"
         "\"binaryTelemetry\":true,\"shellBinary\":true,"
         "\"cborControl\":true,\"telemetryFieldGating\":true,"
         "\"nativeHttpPilotVisible\":false,\"verboseLogs\":false,"
         "\"perfOverlay\":false},"
         "\"devices\":{\"uartShellBridge\":\"off\",\"t41AutoReconnect\":true,"
         "\"c3StatusVisible\":true,\"espnowPreferBridge\":false,"
         "\"recoveryWarnings\":true,\"passiveDiagVisible\":true}}";
}

static const char* device_settings_schema_json() {
  return "{\"success\":true,\"schema_version\":1,\"max_bytes\":16384,"
         "\"storage\":{\"partition\":\"nvs_sys_usr\",\"namespace\":\"web_cfg\","
         "\"key\":\"device_v1\"},"
         "\"password_policy\":\"wifi credentials stay in /api/wifi/save\","
         "\"sections\":{"
         "\"net\":[\"autoScan\",\"rememberLastSsid\",\"showSignalDetails\","
         "\"mdnsName\"],"
         "\"robot\":[\"mathBackend\",\"onboardMathEnabled\",\"mathProfile\","
         "\"trajectoryMode\",\"previewRequired\",\"singularityWarnings\","
         "\"solver\",\"jacobian\",\"nullspace\",\"seedPolicy\","
         "\"limitsProfile\",\"frame\",\"units\",\"posTolMm\",\"oriTolDeg\","
         "\"singularityThreshold\",\"alphaStep\",\"nullGain\",\"lambdaMax\","
         "\"maxStepDeg\",\"maxIter\",\"pathHeightMode\",\"groundZMm\","
         "\"turretMode\",\"cartStepMm\",\"yawStepDeg\",\"jumpRevoluteDeg\"],"
         "\"services\":[\"sshEnabled\",\"mcpEnabled\",\"mcpAllowShell\"],"
         "\"general\":[\"locale\",\"startupPanel\",\"deviceAlias\","
         "\"showBootSummary\"],"
         "\"terminal\":[\"theme\",\"followUiTheme\",\"showRefreshButton\","
         "\"showKillButton\",\"fontSize\",\"fullscreenFontSize\","
         "\"fullscreenOpacity\",\"fullscreenBlurPx\","
         "\"shellProfile\",\"historyLimit\",\"commandTimeoutMs\","
         "\"showManHints\",\"wrapLongOutput\",\"dynamicFetchLayout\"],"
         "\"perf\":[\"telemetryProfile\",\"logTailBytes\","
         "\"preferPsramBuffers\",\"reduceMotion\",\"dpmPolicy\","
         "\"powerMode\"],"
         "\"security\":[\"sessionTimeoutMin\",\"requireRootConfirm\","
         "\"requireDangerConfirm\",\"allowRememberSession\"],"
         "\"files\":[\"defaultView\",\"showHidden\",\"multiSelect\","
         "\"uploadLimitKb\",\"openTextEditor\",\"defaultScale\","
         "\"defaultSort\",\"defaultSortDir\"],"
         "\"update\":[\"firmwarePath\",\"autoCheckRecovery\","
         "\"requireBatterySafe\",\"rollbackGuard\","
         "\"recoveryReadsDeviceSettings\",\"autoOtaScan\",\"otaScanHour\","
         "\"scheduledReboot\",\"scheduledRebootHour\",\"updateWindow\","
         "\"lastErrorCode\"],"
         "\"storage\":[\"logTailBytes\",\"warnFreeKb\",\"keepLogDays\","
         "\"cacheStaticAssets\"],"
         "\"devtools\":[\"debugEndpoints\",\"websocketInspect\","
         "\"rawJsonExport\",\"experimentalFlags\",\"binaryTelemetry\","
         "\"shellBinary\",\"cborControl\",\"telemetryFieldGating\","
         "\"nativeHttpPilotVisible\",\"verboseLogs\",\"perfOverlay\"],"
         "\"devices\":[\"uartShellBridge\",\"t41AutoReconnect\","
         "\"c3StatusVisible\",\"espnowPreferBridge\",\"recoveryWarnings\","
         "\"passiveDiagVisible\"]}}";
}

static bool str_in_table(const char* value, const char* const* table,
                         const size_t count) {
  if (value == nullptr) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (std::strcmp(value, table[i]) == 0) {
      return true;
    }
  }
  return false;
}

static bool device_settings_section_allowed(const char* section) {
  static constexpr const char* kSections[] = {
      "net", "robot", "services", "terminal", "perf",
      "general", "security", "files", "update", "storage", "devtools", "devices",
  };
  return str_in_table(section, kSections,
                      sizeof(kSections) / sizeof(kSections[0]));
}

static bool device_settings_key_allowed(const char* section, const char* key) {
  static constexpr const char* kNet[] = {
      "autoScan", "rememberLastSsid", "showSignalDetails", "mdnsName"};
  static constexpr const char* kRobot[] = {
      "mathBackend", "onboardMathEnabled", "mathProfile", "trajectoryMode",
      "previewRequired", "singularityWarnings", "solver", "jacobian",
      "nullspace", "seedPolicy", "limitsProfile", "frame", "units",
      "posTolMm", "oriTolDeg", "singularityThreshold", "alphaStep",
      "nullGain", "lambdaMax", "maxStepDeg", "maxIter", "pathHeightMode",
      "groundZMm", "turretMode", "cartStepMm", "yawStepDeg",
      "jumpRevoluteDeg"};
  static constexpr const char* kServices[] = {
      "sshEnabled", "mcpEnabled", "mcpAllowShell"};
  static constexpr const char* kGeneral[] = {
      "locale", "startupPanel", "deviceAlias", "showBootSummary"};
  static constexpr const char* kTerminal[] = {
      "theme", "followUiTheme", "showRefreshButton", "showKillButton",
      "fontSize", "fullscreenFontSize", "fullscreenOpacity", "fullscreenBlurPx",
      "shellProfile", "historyLimit",
      "commandTimeoutMs", "showManHints", "wrapLongOutput",
      "dynamicFetchLayout"};
  static constexpr const char* kPerf[] = {
      "telemetryProfile", "logTailBytes", "preferPsramBuffers",
      "reduceMotion", "dpmPolicy", "powerMode"};
  static constexpr const char* kSecurity[] = {
      "sessionTimeoutMin", "requireRootConfirm", "requireDangerConfirm",
      "allowRememberSession"};
  static constexpr const char* kFiles[] = {
      "defaultView", "showHidden", "multiSelect", "uploadLimitKb",
      "openTextEditor", "defaultScale", "defaultSort", "defaultSortDir"};
  static constexpr const char* kUpdate[] = {
      "firmwarePath", "autoCheckRecovery", "requireBatterySafe",
      "rollbackGuard", "recoveryReadsDeviceSettings", "autoOtaScan",
      "otaScanHour", "scheduledReboot", "scheduledRebootHour",
      "updateWindow", "lastErrorCode"};
  static constexpr const char* kStorage[] = {
      "logTailBytes", "warnFreeKb", "keepLogDays", "cacheStaticAssets"};
  static constexpr const char* kDevtools[] = {
      "debugEndpoints", "websocketInspect", "rawJsonExport",
      "experimentalFlags", "binaryTelemetry", "shellBinary", "cborControl",
      "telemetryFieldGating", "nativeHttpPilotVisible", "verboseLogs",
      "perfOverlay"};
  static constexpr const char* kDevices[] = {
      "uartShellBridge", "t41AutoReconnect", "c3StatusVisible",
      "espnowPreferBridge", "recoveryWarnings", "passiveDiagVisible"};
  if (section == nullptr || key == nullptr) {
    return false;
  }
  if (std::strcmp(section, "net") == 0) {
    return str_in_table(key, kNet, sizeof(kNet) / sizeof(kNet[0]));
  }
  if (std::strcmp(section, "robot") == 0) {
    return str_in_table(key, kRobot, sizeof(kRobot) / sizeof(kRobot[0]));
  }
  if (std::strcmp(section, "services") == 0) {
    return str_in_table(key, kServices, sizeof(kServices) / sizeof(kServices[0]));
  }
  if (std::strcmp(section, "general") == 0) {
    return str_in_table(key, kGeneral, sizeof(kGeneral) / sizeof(kGeneral[0]));
  }
  if (std::strcmp(section, "terminal") == 0) {
    return str_in_table(key, kTerminal, sizeof(kTerminal) / sizeof(kTerminal[0]));
  }
  if (std::strcmp(section, "perf") == 0) {
    return str_in_table(key, kPerf, sizeof(kPerf) / sizeof(kPerf[0]));
  }
  if (std::strcmp(section, "security") == 0) {
    return str_in_table(key, kSecurity, sizeof(kSecurity) / sizeof(kSecurity[0]));
  }
  if (std::strcmp(section, "files") == 0) {
    return str_in_table(key, kFiles, sizeof(kFiles) / sizeof(kFiles[0]));
  }
  if (std::strcmp(section, "update") == 0) {
    return str_in_table(key, kUpdate, sizeof(kUpdate) / sizeof(kUpdate[0]));
  }
  if (std::strcmp(section, "storage") == 0) {
    return str_in_table(key, kStorage, sizeof(kStorage) / sizeof(kStorage[0]));
  }
  if (std::strcmp(section, "devtools") == 0) {
    return str_in_table(key, kDevtools, sizeof(kDevtools) / sizeof(kDevtools[0]));
  }
  if (std::strcmp(section, "devices") == 0) {
    return str_in_table(key, kDevices, sizeof(kDevices) / sizeof(kDevices[0]));
  }
  return false;
}

static bool device_settings_string_safe(const char* value,
                                        const size_t max_len) {
  if (value == nullptr) {
    return true;
  }
  const size_t len = std::strlen(value);
  if (len > max_len) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    if (static_cast<unsigned char>(value[i]) < 0x20U) {
      return false;
    }
  }
  return true;
}

static bool validate_device_settings_json(const char* payload,
                                          const size_t payload_len,
                                          String* out_clean,
                                          String* out_error) {
  if (out_clean == nullptr || payload == nullptr || payload_len == 0U ||
      payload_len > kDeviceSettingsPayloadMax) {
    if (out_error != nullptr) *out_error = "invalid_size";
    return false;
  }
  const char* parse_end = nullptr;
  cJSON* root = cJSON_ParseWithLengthOpts(payload, payload_len, &parse_end, 1);
  if (root == nullptr || !cJSON_IsObject(root)) {
    if (root != nullptr) cJSON_Delete(root);
    if (out_error != nullptr) *out_error = "invalid_json";
    return false;
  }

  bool ok = true;
  String error = "";
  const cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
  if (!cJSON_IsNumber(schema) || schema->valueint != 1) {
    ok = false;
    error = "unsupported_schema";
  }

  for (cJSON* section = root->child; ok && section != nullptr; section = section->next) {
    const char* section_name = section->string;
    if (std::strcmp(section_name, "schema_version") == 0) {
      continue;
    }
    if (!device_settings_section_allowed(section_name) || !cJSON_IsObject(section)) {
      ok = false;
      error = "unknown_or_invalid_section";
      break;
    }
    for (cJSON* item = section->child; item != nullptr; item = item->next) {
      const char* key = item->string;
      if (!device_settings_key_allowed(section_name, key)) {
        ok = false;
        error = "unknown_key";
        break;
      }
      if (cJSON_IsString(item)) {
        if (!device_settings_string_safe(item->valuestring, 96U)) {
          ok = false;
          error = "unsafe_string";
          break;
        }
      } else if (cJSON_IsBool(item) || cJSON_IsNumber(item) || cJSON_IsNull(item)) {
        // Supported primitive setting value.
      } else {
        ok = false;
        error = "unsupported_value_type";
        break;
      }
    }
  }

  if (!ok) {
    cJSON_Delete(root);
    if (out_error != nullptr) *out_error = error;
    return false;
  }

  char* printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == nullptr || std::strlen(printed) > kDeviceSettingsPayloadMax) {
    if (printed != nullptr) cJSON_free(printed);
    if (out_error != nullptr) *out_error = "print_failed";
    return false;
  }
  *out_clean = String(printed);
  cJSON_free(printed);
  return true;
}

static String read_device_settings_json() {
  mros::platform::NvsNamespace ns;
  std::string stored;
  if (ns.open(kDeviceSettingsNamespace, true,
              mros::platform::NvsPartitionMode::UserPartitionsThenDefault) &&
      ns.get_string(kDeviceSettingsKey, &stored) && !stored.empty() &&
      stored.size() <= kDeviceSettingsPayloadMax) {
    return String(stored.c_str());
  }
  return String(device_settings_default_json());
}

static bool write_device_settings_json(const String& json,
                                       const char** out_partition,
                                       String* out_error) {
  String clean;
  String error;
  if (!validate_device_settings_json(json.c_str(), json.length(), &clean, &error)) {
    if (out_error != nullptr) *out_error = error;
    return false;
  }
  mros::platform::NvsNamespace ns;
  if (!ns.open(kDeviceSettingsNamespace, false,
               mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    if (out_error != nullptr) *out_error = "nvs_open_failed";
    return false;
  }
  if (out_partition != nullptr) {
    *out_partition = ns.partition_label();
  }
  if (!ns.set_string(kDeviceSettingsKey,
                     std::string(clean.c_str(), clean.length()))) {
    if (out_error != nullptr) *out_error = "nvs_write_failed";
    return false;
  }
  return true;
}

static String extractCookieValue(const String &cookie_header,
                                 const String &name) {
  if (cookie_header.length() == 0U ||
      cookie_header.length() > kAuthCookieHeaderMaxLen ||
      name.length() == 0U || name.length() > 32U) {
    return "";
  }
  int pos = 0;
  while (pos < cookie_header.length()) {
    while (pos < cookie_header.length() &&
           (cookie_header[pos] == ' ' || cookie_header[pos] == ';')) {
      pos++;
    }
    int eq = cookie_header.indexOf('=', pos);
    if (eq < 0) {
      break;
    }
    int end = cookie_header.indexOf(';', eq + 1);
    if (end < 0) {
      end = cookie_header.length();
    }
    String key = cookie_header.substring(pos, eq);
    key.trim();
    if (key == name) {
      String value = cookie_header.substring(eq + 1, end);
      value.trim();
      return value;
    }
    pos = end + 1;
  }
  return "";
}

static bool is_hex_string_fixed(const String &value, const size_t expected_len) {
  if (value.length() != expected_len) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value[i];
    const bool hex = (ch >= '0' && ch <= '9') ||
                     (ch >= 'a' && ch <= 'f') ||
                     (ch >= 'A' && ch <= 'F');
    if (!hex) {
      return false;
    }
  }
  return true;
}

static String request_session_cookie_token(AsyncWebServerRequest* request) {
  if (request == nullptr || !request->hasHeader("Cookie")) {
    return "";
  }
  String cookie = request->header("Cookie");
  if (cookie.length() > kAuthCookieHeaderMaxLen) {
    return "";
  }
  const String session_id = extractCookieValue(cookie, "ESPSESSIONID");
  return is_hex_string_fixed(session_id, kAuthSessionTokenLen) ? session_id : "";
}

static uint32_t login_lockout_remaining_ms() {
  const unsigned long now = mros::platform::mros_millis();
  return (login_block_until_ms > now)
             ? static_cast<uint32_t>(login_block_until_ms - now)
             : 0U;
}

static String auth_json_error(const char* error, const bool setup_required = false) {
  char buffer[256] = {};
  mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
  writer.begin();
  writer.bool_field("success", false);
  if (setup_required) writer.bool_field("setup_required", true);
  writer.string_field("error", error != nullptr ? error : "AUTH_ERROR");
  writer.end();
  return writer.overflow() ? String("{\"success\":false,\"error\":\"AUTH_ERROR\"}")
                           : String(writer.c_str());
}

static String session_cookie_header(const String& token, const bool expire) {
  String cookie = "ESPSESSIONID=";
  cookie += expire ? "" : token;
  cookie += "; Path=/; HttpOnly; SameSite=Strict";
  if (expire) {
    cookie += "; Max-Age=0";
  } else {
    cookie += "; Max-Age=";
    cookie += String(static_cast<unsigned long>(kAuthSessionTtlMs / 1000UL));
  }
  return cookie;
}

static bool load_setup_gate_secret(String* out_secret) {
  if (out_secret == nullptr) return false;
  mros::platform::NvsNamespace ns;
  if (!ns.open("security", true,
               mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    return false;
  }
  std::string stored;
  if (!ns.get_string("setup_gate", &stored) || stored.empty() || stored.size() > 96U) {
    return false;
  }
  *out_secret = String(stored.c_str());
  return true;
}

static bool save_setup_gate_secret(const String& secret) {
  if (secret.length() < 16U || secret.length() > 96U) return false;
  mros::platform::NvsNamespace ns;
  if (!ns.open("security", false,
               mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    return false;
  }
  return ns.set_string("setup_gate", std::string(secret.c_str(), secret.length()));
}

static bool setup_gate_secret_matches(const String& supplied) {
  String expected;
  if (load_setup_gate_secret(&expected)) {
    return supplied == expected;
  }
#if MROS_INITIAL_SETUP_SECRET_PLACEHOLDER
  return false;
#else
  return supplied == k_initial_setup_secret;
#endif
}

static void rotate_setup_gate_secret_after_registration() {
  (void)save_setup_gate_secret(generateSecureToken(24));
}

static void clear_setup_gate_secret() {
  mros::platform::NvsNamespace ns;
  if (ns.open("security", false,
              mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    (void)ns.erase_key("setup_gate");
  }
}

static String sanitize_username_for_filename(const String &user) {
  String out;
  out.reserve(user.length());
  for (size_t i = 0; i < user.length(); i++) {
    const char c = user[i];
    const bool ok_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool ok_num = (c >= '0' && c <= '9');
    const bool ok_sym = (c == '_' || c == '-' || c == '.');
    out += (ok_alpha || ok_num || ok_sym) ? c : '_';
  }
  if (out.length() == 0) out = "user";
  if (out.length() > 40) out = out.substring(0, 40);
  return out;
}

static bool load_stored_credentials(String &out_user, String &out_hash) {
  out_user = "";
  out_hash = "";
  if (!prefs_load_credentials(out_user, out_hash)) {
    return false;
  }
  out_user.trim();
  out_hash.trim();
  out_hash.toLowerCase();
  return (out_user.length() > 0 && web_auth_stored_hash_format_ok(out_hash));
}

static bool is_initial_setup_required() {
  String user, hash;
  return !load_stored_credentials(user, hash);
}

static void refresh_current_username_from_credentials();

static bool web_auth_verify_user_password(const String& username,
                                          const String& password,
                                          const String& stored_user,
                                          const String& stored_hash,
                                          bool* stored_hash_needs_upgrade = nullptr) {
  if (stored_hash_needs_upgrade != nullptr) {
    *stored_hash_needs_upgrade = false;
  }
  if (username.length() == 0U || password.length() == 0U) {
    return false;
  }
  if (username == stored_user && stored_hash.length() > 0U) {
    if (web_auth_verify_hash(password, stored_hash, stored_hash_needs_upgrade)) {
      return true;
    }
  }
  if (mros::ssh::verify_password(username, password)) {
    return true;
  }
  return false;
}

static bool web_auth_save_primary_credentials(const String& username,
                                              const String& password) {
  const String hash = web_auth_hash_password(password);
  if (!web_auth_stored_hash_format_ok(hash)) {
    return false;
  }
  prefs_save_credentials(username, hash);
  String verify_user;
  String verify_hash;
  return load_stored_credentials(verify_user, verify_hash) &&
         verify_user == username &&
         verify_hash == hash;
}

static void web_security_audit(const char* event, const String& detail);

static bool web_current_user_is_admin() {
  String username = auth_current_username_copy();
  if (username.length() == 0U) {
    refresh_current_username_from_credentials();
    username = auth_current_username_copy();
  }
  mros::ssh::UserAccount account;
  return username.length() > 0U &&
         mros::ssh::get_user(username, &account) &&
         (account.admin || account.root);
}

static String web_current_username_or_refresh() {
  String username = auth_current_username_copy();
  if (username.length() == 0U) {
    refresh_current_username_from_credentials();
    username = auth_current_username_copy();
  }
  return username;
}

static uint32_t web_capability_mask_for_username(const String& username) {
  return mros::shell::capability_mask_for_user(username.c_str());
}

static uint32_t web_current_user_capability_mask() {
  return web_capability_mask_for_username(web_current_username_or_refresh());
}

static bool web_current_user_has_capability(const uint32_t required) {
  const uint32_t capabilities = web_current_user_capability_mask();
  return required == 0U || ((capabilities & required) == required);
}

static bool request_require_capability(AsyncWebServerRequest* request,
                                       const uint32_t required,
                                       const char* error_code = "CAPABILITY_DENIED") {
  if (web_current_user_has_capability(required)) {
    return true;
  }
  String body = "{\"success\":false,\"error\":\"";
  body += error_code != nullptr ? error_code : "CAPABILITY_DENIED";
  body += "\",\"required\":\"";
  body += jsonEscape(String(mros::shell::capabilities_text(required)));
  body += "\"}";
  request->send(403, "application/json", body);
  web_security_audit("capability-denied", auth_current_username_copy());
  return false;
}

static bool request_param_value(AsyncWebServerRequest* request,
                                const char* name,
                                String* out,
                                const bool post = true) {
  if (out == nullptr || request == nullptr || name == nullptr) return false;
  if (request->hasParam(name, post)) {
    *out = request->getParam(name, post)->value();
    return true;
  }
  return false;
}

static bool request_param_value_any(AsyncWebServerRequest* request,
                                    const char* name,
                                    String* out) {
  if (request_param_value(request, name, out, true)) return true;
  if (out != nullptr && request != nullptr && name != nullptr && request->hasParam(name, false)) {
    *out = request->getParam(name, false)->value();
    return true;
  }
  return false;
}

static bool request_reauth_password_ok(AsyncWebServerRequest* request) {
  String password;
  if (!request_param_value(request, "current_password", &password) &&
      !request_param_value(request, "reauth_password", &password)) {
    if (request != nullptr && request->hasHeader("X-MROS-Reauth-Password")) {
      password = request->header("X-MROS-Reauth-Password");
    }
  }
  if (password.length() == 0U) {
    return false;
  }
  if (password.length() == 0U || password.length() > kAuthPassMaxLen) {
    return false;
  }
  String username = auth_current_username_copy();
  if (username.length() == 0U) {
    refresh_current_username_from_credentials();
    username = auth_current_username_copy();
  }
  String stored_user;
  String stored_hash;
  (void)load_stored_credentials(stored_user, stored_hash);
  return web_auth_verify_user_password(username, password, stored_user, stored_hash);
}

static String web_normalize_host(String value) {
  value.trim();
  value.toLowerCase();
  const int comma = value.indexOf(',');
  if (comma >= 0) {
    value = value.substring(0, comma);
    value.trim();
  }
  if (value.endsWith(":80")) {
    value = value.substring(0, value.length() - 3);
  } else if (value.endsWith(":443")) {
    value = value.substring(0, value.length() - 4);
  }
  return value;
}

static String web_host_from_origin_like(String value) {
  value.trim();
  value.toLowerCase();
  if (value.startsWith("http://")) {
    value = value.substring(7);
  } else if (value.startsWith("https://")) {
    value = value.substring(8);
  } else if (value.startsWith("//")) {
    value = value.substring(2);
  }
  const int at = value.indexOf('@');
  if (at >= 0) {
    value = value.substring(at + 1);
  }
  int cut = value.length();
  for (const char marker : {'/', '?', '#'}) {
    const int pos = value.indexOf(marker);
    if (pos >= 0 && pos < cut) {
      cut = pos;
    }
  }
  value = value.substring(0, cut);
  return web_normalize_host(value);
}

static bool request_same_origin_ok(AsyncWebServerRequest* request) {
  if (request == nullptr) {
    return false;
  }
  const String host = web_normalize_host(request->host());
  if (request->hasHeader("Origin")) {
    const String origin_host = web_host_from_origin_like(request->header("Origin"));
    return host.length() > 0U && origin_host == host;
  }
  if (request->hasHeader("Referer")) {
    const String referer_host = web_host_from_origin_like(request->header("Referer"));
    return host.length() > 0U && referer_host == host;
  }
  return true;
}

static String request_csrf_token_value(AsyncWebServerRequest* request) {
  if (request == nullptr) {
    return "";
  }
  String token;
  if (request->hasHeader("X-MROS-CSRF")) {
    token = request->header("X-MROS-CSRF");
  } else if (request->hasHeader("X-CSRF-Token")) {
    token = request->header("X-CSRF-Token");
  } else if (!request_param_value(request, "csrf", &token) &&
             !request_param_value(request, "csrf", &token, false)) {
    token = "";
  }
  token.trim();
  token.toLowerCase();
  return is_hex_string_fixed(token, kAuthCsrfTokenLen) ? token : "";
}

static bool request_requires_csrf(AsyncWebServerRequest* request) {
  return request != nullptr && request->method() != HTTP_GET;
}

static bool request_mutation_guard_ok(AsyncWebServerRequest* request,
                                      const String& expected_csrf) {
  if (!request_requires_csrf(request)) {
    return true;
  }
  if (!request_same_origin_ok(request)) {
    request->send(403, "application/json", auth_json_error("ORIGIN_DENIED"));
    web_security_audit("origin-denied", auth_current_username_copy());
    return false;
  }
  const String supplied = request_csrf_token_value(request);
  if (expected_csrf.length() != kAuthCsrfTokenLen || supplied != expected_csrf) {
    request->send(403, "application/json", auth_json_error("CSRF_REQUIRED"));
    web_security_audit("csrf-denied", auth_current_username_copy());
    return false;
  }
  return true;
}

static void web_security_audit(const char* event, const String& detail = String()) {
  String safe_detail = detail;
  safe_detail.replace("\r", " ");
  safe_detail.replace("\n", " ");
  if (safe_detail.length() > 64U) safe_detail = safe_detail.substring(0, 64);
  mros::shell::audit_record(event != nullptr ? event : "security", safe_detail.c_str());
}

static void append_security_user_json(mros::utils::FixedJsonWriter& writer,
                                      const mros::ssh::UserAccount& account,
                                      const String& current_user,
                                      const bool first) {
  if (!first) writer.append_raw(",");
  writer.append_raw("{\"username\":\"");
  writer.append_escaped(account.username.c_str());
  writer.append_raw("\",\"display_name\":\"");
  writer.append_escaped(account.display_name.c_str());
  writer.append_raw("\",\"admin\":");
  writer.append_raw(account.admin ? "true" : "false");
  writer.append_raw(",\"sudo\":");
  writer.append_raw(account.sudo ? "true" : "false");
  writer.append_raw(",\"primary\":");
  writer.append_raw(account.primary ? "true" : "false");
  writer.append_raw(",\"root\":");
  writer.append_raw(account.root ? "true" : "false");
  writer.append_raw(",\"capabilities\":\"");
  writer.append_escaped(mros::shell::capabilities_text(
      mros::shell::capability_mask_for_user(account.username.c_str())));
  writer.append_raw("\"");
  writer.append_raw(",\"current\":");
  writer.append_raw(account.username == current_user ? "true" : "false");
  writer.append_raw("}");
}

static String build_security_users_json() {
  char buffer[4096] = {};
  mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
  const std::vector<mros::ssh::UserAccount> users = mros::ssh::list_users();
  const String current_user = auth_current_username_copy();
  writer.begin();
  writer.bool_field("success", true);
  writer.string_field("current_user", current_user.c_str());
  writer.string_field("current_capabilities",
                      mros::shell::capabilities_text(
                          mros::shell::capability_mask_for_user(current_user.c_str())));
  const size_t active_sessions = auth_active_session_count();
  writer.bool_field("session_active", active_sessions > 0U);
  writer.u32_field("session_count", static_cast<uint32_t>(active_sessions));
  writer.u32_field("session_capacity", static_cast<uint32_t>(kAuthSessionSlots));
  writer.u32_field("session_issued", g_auth_session_issued);
  writer.u32_field("session_expired", g_auth_session_expired);
  writer.u32_field("session_evicted", g_auth_session_evicted);
  writer.u32_field("session_revoked", g_auth_session_revoked);
  writer.u32_field("login_lockout_ms", login_lockout_remaining_ms());
  writer.u32_field("shell_sessions", mros::shell::active_session_count());
  writer.u32_field("root_shell_sessions", mros::shell::active_root_session_count());
  writer.u32_field("shell_session_capacity", mros::shell::session_capacity());
  writer.u32_field("ws_shell_clients", ws_shell.count() + ws_shell_v2.count());
  writer.raw_field("users", "[");
  for (size_t i = 0U; i < users.size(); ++i) {
    append_security_user_json(writer, users[i], current_user, i == 0U);
  }
  writer.append_raw("]");
  writer.end();
  return writer.overflow() ? String("{\"success\":false,\"error\":\"SECURITY_USERS_OVERFLOW\"}")
                           : String(writer.c_str());
}

static void refresh_current_username_from_credentials() {
  String next_user = "";
  String stored_user, stored_hash;
  if (load_stored_credentials(stored_user, stored_hash)) {
    next_user = stored_user;
  } else {
    auth_state_lock();
    const bool empty = current_username.length() == 0;
    auth_state_unlock();
    if (empty) next_user = "admin";
  }
  if (next_user.length() > 0) {
    auth_set_current_username(next_user);
  }
}

static String user_popup_settings_path_for(const String &username) {
  const String relative_name =
      String("ui_settings_") + sanitize_username_for_filename(username) + ".json";
  return logger_user_path(relative_name.c_str());
}

static String user_popup_settings_path_current() {
  String username = auth_current_username_copy();
  if (username.length() == 0) {
    refresh_current_username_from_credentials();
    username = auth_current_username_copy();
  }
  return user_popup_settings_path_for(username);
}

static bool ensure_auth_user_dir() {
  if (!logger_storage_ready()) {
    logger_init();
  }
  const String root = logger_user_path("auth");
  if (mros::platform::mros_fs_exists(root.c_str())) {
    return true;
  }
  return mros::platform::mros_fs_mkdir(root.c_str());
}

static bool save_profile_avatar_selection(const String& avatar_choice,
                                          const String& avatar_data_url) {
  String choice = avatar_choice;
  choice.trim();
  if (choice.length() == 0) {
    choice = "avatar-1";
  }
  if (choice.length() > 32U) {
    return false;
  }
  for (size_t i = 0; i < choice.length(); ++i) {
    const char ch = choice[i];
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    if (!ok) {
      return false;
    }
  }
  if (!ensure_auth_user_dir()) {
    return false;
  }
  std::string meta = "choice=";
  meta += choice.c_str();
  meta += "\n";
  if (!mros::platform::mros_file_write_all(logger_user_path("auth/profile_avatar.meta").c_str(), meta)) {
    return false;
  }
  String data = avatar_data_url;
  data.trim();
  if (data.length() == 0) {
    return true;
  }
  if (data.length() > 131072U || !data.startsWith("data:image/")) {
    return false;
  }
  return mros::platform::mros_file_write_all(
      logger_user_path("auth/profile_avatar.dataurl").c_str(),
      std::string(data.c_str(), data.length()));
}

static uint32_t fnv1a32_string(const String &value) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < value.length(); ++i) {
    hash ^= static_cast<uint8_t>(value[i]);
    hash *= 16777619UL;
  }
  return hash;
}

static String user_popup_settings_nvs_key_current() {
  String username = auth_current_username_copy();
  if (username.length() == 0) {
    refresh_current_username_from_credentials();
    username = auth_current_username_copy();
  }
  char key[16] = {};
  std::snprintf(key, sizeof(key), "ui%08lx",
                static_cast<unsigned long>(fnv1a32_string(username)));
  return String(key);
}

static bool read_popup_settings_from_nvs(String *out_json) {
  if (out_json == nullptr) return false;
  mros::platform::NvsNamespace ns;
  if (!ns.open("web_ui", true,
               mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    return false;
  }
  const String key = user_popup_settings_nvs_key_current();
  const size_t blob_size = ns.get_blob_size(key.c_str());
  if (blob_size == 0U || blob_size > 24576U) return false;
  std::string raw(blob_size, '\0');
  if (!ns.get_blob(key.c_str(), raw.data(), blob_size)) return false;
  String json(raw.c_str());
  json.trim();
  if (json.length() == 0 || json[0] != '{') return false;
  *out_json = json;
  return true;
}

static bool write_popup_settings_to_nvs(const String &payload) {
  mros::platform::NvsNamespace ns;
  if (!ns.open("web_ui", false,
               mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    return false;
  }
  const String key = user_popup_settings_nvs_key_current();
  return ns.set_blob(key.c_str(), payload.c_str(), payload.length() + 1U);
}

static String read_user_popup_settings_json() {
  String nvs_json;
  if (read_popup_settings_from_nvs(&nvs_json)) return nvs_json;

  const String path = user_popup_settings_path_current();
  std::string json_raw;
  if (!mros::platform::mros_file_read_all(path.c_str(), &json_raw)) return "{}";
  String json(json_raw.c_str());
  json.trim();
  if (json.length() == 0) return "{}";
  if (json[0] != '{') return "{}";
  (void)write_popup_settings_to_nvs(json);
  return json;
}

static bool write_user_popup_settings_json(const String &json) {
  String payload = json;
  payload.trim();
  if (payload.length() == 0) payload = "{}";
  if (payload[0] != '{' || payload[payload.length() - 1] != '}') return false;
  if (payload.length() > 24576) return false;

  const bool nvs_ok = write_popup_settings_to_nvs(payload);
  const String path = user_popup_settings_path_current();
  // User-facing settings are small and users expect the next GET/reload to see
  // them immediately, so commit this path synchronously instead of waiting for
  // the background storage queue.
  const String root = logger_user_root_path();
  if (!mros::platform::mros_fs_exists(root.c_str())) {
    (void)mros::platform::mros_fs_mkdir(root.c_str());
  }
  if (logger_write_text_file_atomic(path, payload)) {
    return true;
  }
  const bool file_queue_ok = logger_enqueue_text_file_write(path, payload);
  return nvs_ok || file_queue_ok;
}

static void migrate_user_popup_settings_on_username_change(const String &old_user,
                                                           const String &new_user) {
  const String old_path = user_popup_settings_path_for(old_user);
  const String new_path = user_popup_settings_path_for(new_user);
  if (old_path == new_path) return;
  if (!mros::platform::mros_fs_exists(old_path.c_str()) ||
      mros::platform::mros_fs_exists(new_path.c_str())) return;
  (void)mros::platform::mros_fs_rename(old_path.c_str(), new_path.c_str());
}

static void resetWsTrajectoryUploadState() {
  ws_traj_upload_active = false;
  ws_traj_upload_client_id = 0;
  ws_traj_upload_prefix_len = 0;
  ws_traj_upload_preview_step_mm = 2.0f;
  trajectory_handler_stream_reset();
}

static void clearWsAuthState() {
  ws_auth_lock();
  ws_auth_clients.clear();
  ws_full_snapshot_pending.clear();
  ws_scene_subscriptions.clear();
  ws_subscription_masks.clear();
  ws_telemetry_formats.clear();
  ws_shell_auth_clients.clear();
  ws_debug_auth_clients.clear();
  ws_debug_subscriptions.clear();
  ws_mcp_auth_clients.clear();
  ws_mcp_contexts.clear();
  ws_auth_deadlines.clear();
  ws_shell_auth_deadlines.clear();
  ws_debug_auth_deadlines.clear();
  ws_mcp_auth_deadlines.clear();
  ws_auth_unlock();
  resetWsTrajectoryUploadState();
}

static void clearWsTickets() {
  ws_auth_lock();
  for (size_t i = 0; i < WS_TICKET_SLOTS; i++) {
    ws_tickets[i].token = "";
    ws_tickets[i].username = "";
    ws_tickets[i].capability_mask = 0U;
    ws_tickets[i].scope = WsTicketScope::Telemetry;
    ws_tickets[i].expires_at_ms = 0;
  }
  ws_auth_unlock();
}

static void invalidateSessionState() {
  auth_clear_session_token();
  clearWsAuthState();
  clearWsTickets();
}

void web_server_logout_all() {
  invalidateSessionState();
  prefs_save_token("");
}

static void pruneWsTicketsUnlocked() {
  unsigned long now = mros::platform::mros_millis();
  for (size_t i = 0; i < WS_TICKET_SLOTS; i++) {
    if (ws_tickets[i].token.length() > 0 &&
        static_cast<long>(now - ws_tickets[i].expires_at_ms) >= 0) {
      ws_tickets[i].token = "";
      ws_tickets[i].username = "";
      ws_tickets[i].capability_mask = 0U;
      ws_tickets[i].scope = WsTicketScope::Telemetry;
      ws_tickets[i].expires_at_ms = 0;
      ws_ticket_expired++;
    }
  }
}

static const char* ws_ticket_scope_text(const WsTicketScope scope) {
  switch (scope) {
    case WsTicketScope::Telemetry:
      return "telemetry";
    case WsTicketScope::Shell:
      return "shell";
    case WsTicketScope::Debug:
      return "debug";
    case WsTicketScope::Mcp:
      return "mcp";
    default:
      return "unknown";
  }
}

static String issueWsTicket(const WsTicketScope scope,
                            const String& username,
                            const uint32_t capability_mask) {
  if (username.length() == 0U || capability_mask == 0U) {
    return "";
  }
  ws_auth_lock();
  pruneWsTicketsUnlocked();
  int slot = -1;
  for (size_t i = 0; i < WS_TICKET_SLOTS; i++) {
    if (ws_tickets[i].token.length() == 0) {
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot < 0) {
    slot = 0;
    ws_ticket_evicted++;
  }
  ws_tickets[slot].token = generateSecureToken(16);
  ws_tickets[slot].username = username;
  ws_tickets[slot].capability_mask = capability_mask;
  ws_tickets[slot].scope = scope;
  ws_tickets[slot].expires_at_ms = mros::platform::mros_millis() + WS_TICKET_TTL_MS;
  ws_ticket_issued++;
  const String token = ws_tickets[slot].token;
  ws_auth_unlock();
  return token;
}

static bool consumeWsTicket(const String& ticket,
                            const WsTicketScope expected_scope,
                            WsTicketClaim* claim) {
  if (ticket.length() == 0) {
    return false;
  }
  ws_auth_lock();
  pruneWsTicketsUnlocked();
  for (size_t i = 0; i < WS_TICKET_SLOTS; i++) {
    if (ws_tickets[i].token == ticket &&
        ws_tickets[i].scope == expected_scope) {
      if (claim != nullptr) {
        claim->username = ws_tickets[i].username;
        claim->capability_mask = ws_tickets[i].capability_mask;
        claim->scope = ws_tickets[i].scope;
      }
      ws_tickets[i].token = "";
      ws_tickets[i].username = "";
      ws_tickets[i].capability_mask = 0U;
      ws_tickets[i].scope = WsTicketScope::Telemetry;
      ws_tickets[i].expires_at_ms = 0;
      ws_ticket_consumed++;
      ws_auth_unlock();
      return true;
    }
  }
  ws_ticket_failed++;
  ws_auth_unlock();
  return false;
}

static uint32_t activeWsTicketCount() {
  ws_auth_lock();
  pruneWsTicketsUnlocked();
  uint32_t count = 0;
  for (size_t i = 0; i < WS_TICKET_SLOTS; i++) {
    if (ws_tickets[i].token.length() > 0) count++;
  }
  ws_auth_unlock();
  return count;
}

static String mcp_extract_string_field(const String& json, const char* key) {
  if (key == nullptr) return "";
  const String needle = String("\"") + key + "\"";
  int p = json.indexOf(needle);
  if (p < 0) return "";
  p = json.indexOf(':', p + needle.length());
  if (p < 0) return "";
  p++;
  while (p < json.length() && isspace(static_cast<unsigned char>(json[p]))) p++;
  if (p >= json.length() || json[p] != '"') return "";
  p++;
  String out;
  bool escape = false;
  for (; p < json.length(); ++p) {
    const char c = json[p];
    if (escape) {
      switch (c) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        default: out += c; break;
      }
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') break;
    out += c;
  }
  return out;
}

static String normalize_theme_mode(String value) {
  value.trim();
  value.toLowerCase();
  if (value == "light" || value == "dark" || value == "system") {
    return value;
  }
  return "dark";
}

static String normalize_color_palette(String value) {
  value.trim();
  value.toLowerCase();
  if (value == "mros" || value == "graphite" || value == "ocean" ||
      value == "ember" || value == "violet" || value == "slate") {
    return value;
  }
  return "mros";
}

static void get_saved_ui_theme(String *theme_mode, String *color_palette) {
  if (theme_mode == nullptr || color_palette == nullptr) {
    return;
  }
  const String settings = read_user_popup_settings_json();
  *theme_mode = normalize_theme_mode(mcp_extract_string_field(settings, "themeMode"));
  *color_palette = normalize_color_palette(mcp_extract_string_field(settings, "colorPalette"));
}

static String mcp_extract_raw_id(const String& json) {
  int p = json.indexOf("\"id\"");
  if (p < 0) return "";
  p = json.indexOf(':', p + 4);
  if (p < 0) return "";
  p++;
  while (p < json.length() && isspace(static_cast<unsigned char>(json[p]))) p++;
  if (p >= json.length()) return "";
  const int start = p;
  if (json[p] == '"') {
    p++;
    bool escape = false;
    for (; p < json.length(); ++p) {
      if (escape) {
        escape = false;
        continue;
      }
      if (json[p] == '\\') {
        escape = true;
        continue;
      }
      if (json[p] == '"') return json.substring(start, p + 1);
    }
    return "";
  }
  while (p < json.length() && json[p] != ',' && json[p] != '}' &&
         json[p] != '\r' && json[p] != '\n') {
    p++;
  }
  String id = json.substring(start, p);
  id.trim();
  return id;
}

static String mcp_response(const String& id, const String& result) {
  return String("{\"jsonrpc\":\"2.0\",\"id\":") + id + ",\"result\":" + result + "}";
}

static String mcp_error(const String& id, const int code, const char* message) {
  return String("{\"jsonrpc\":\"2.0\",\"id\":") + (id.length() ? id : "null") +
         ",\"error\":{\"code\":" + String(code) + ",\"message\":\"" +
         jsonEscape(String(message != nullptr ? message : "MCP error")) + "\"}}";
}

static String mcp_text_result(const String& text) {
  return String("{\"content\":[{\"type\":\"text\",\"text\":\"") + jsonEscape(text) +
         "\"}],\"isError\":false}";
}

static String mcp_status_json() {
  uint64_t fs_total = 0U;
  uint64_t fs_used = 0U;
  (void)mros::platform::mros_fs_info(&fs_total, &fs_used);
  String json = "{";
  json.reserve(512U);
  json += "\"system\":\"" + String(k_system_name) + "\",";
  json += "\"version\":\"" + String(k_system_version) + "\",";
  json += "\"uptime_ms\":" + String(static_cast<uint32_t>(mros::platform::mros_millis())) + ",";
  json += "\"heap_free\":" + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) + ",";
  json += "\"psram_free\":" + String(mros::platform::mros_system_psram_free()) + ",";
  json += "\"fs_total\":" + String(static_cast<uint32_t>(fs_total)) + ",";
  json += "\"fs_used\":" + String(static_cast<uint32_t>(fs_used)) + ",";
  json += "\"t41_connected\":" + String(spi_s3_is_connected() ? "true" : "false") + ",";
  json += "\"loop_ms\":" + String(spi_s3_get_loop_ms()) + ",";
  json += "\"turret\":" + String(spi_s3_get_turret_deg(), 1);
  json += "}";
  return json;
}

static String mcp_tools_list_result() {
  return
      "{\"tools\":["
      "{\"name\":\"mros.status\",\"description\":\"Read MROS bridge status, heap, PSRAM, filesystem and robot telemetry.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
      "{\"name\":\"mros.shell.exec\",\"description\":\"Execute an authenticated MROS shell command and return captured output.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Command line to run\"}},\"required\":[\"command\"]}},"
      "{\"name\":\"mros.files.list\",\"description\":\"List files under the user writable /ESPUSER tree.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"default\":\"/ESPUSER\"}}}},"
      "{\"name\":\"mros.settings.read\",\"description\":\"Read persisted web popup settings from nvs_sys_usr-backed storage.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
      "]}";
}

static String mcp_resources_list_result() {
  return
      "{\"resources\":["
      "{\"uri\":\"mros://status\",\"name\":\"MROS status\",\"mimeType\":\"application/json\"},"
      "{\"uri\":\"mros://files/ESPUSER\",\"name\":\"User filesystem root\",\"mimeType\":\"application/json\"},"
      "{\"uri\":\"mros://settings/popup\",\"name\":\"Popup settings\",\"mimeType\":\"application/json\"}"
      "]}";
}

static String mcp_prompts_list_result() {
  return
      "{\"prompts\":["
      "{\"name\":\"mros-diagnose\",\"description\":\"Diagnose bridge health using status, logs, files and shell tools.\"},"
      "{\"name\":\"mros-pick-place\",\"description\":\"Plan a pick-and-place scene interaction against the 3D CAD simulator.\"},"
      "{\"name\":\"mros-files\",\"description\":\"Inspect and manage files under /ESPUSER.\"}"
      "]}";
}

static String mcp_capability_error(const uint32_t required) {
  String out = "{\"content\":[{\"type\":\"text\",\"text\":\"capability denied; required: ";
  out += jsonEscape(String(mros::shell::capabilities_text(required)));
  out += "\"}],\"isError\":true}";
  return out;
}

static bool mcp_has_capability(const uint32_t capabilities, const uint32_t required) {
  return required == 0U || ((capabilities & required) == required);
}

static String mcp_handle_tool_call(const String& body,
                                   const bool allow_shell,
                                   const String& username,
                                   const uint32_t capabilities) {
  const String tool = mcp_extract_string_field(body, "name");
  if (tool == "mros.status") {
    if (!mcp_has_capability(capabilities, mros::shell::ShellCapabilityRead)) {
      return mcp_capability_error(mros::shell::ShellCapabilityRead);
    }
    return mcp_text_result(mcp_status_json());
  }
  if (tool == "mros.settings.read") {
    if (!mcp_has_capability(capabilities, mros::shell::ShellCapabilityRead)) {
      return mcp_capability_error(mros::shell::ShellCapabilityRead);
    }
    return mcp_text_result(read_user_popup_settings_json());
  }
  if (tool == "mros.files.list") {
    if (!mcp_has_capability(capabilities, mros::shell::ShellCapabilityRead)) {
      return mcp_capability_error(mros::shell::ShellCapabilityRead);
    }
    String path = mcp_extract_string_field(body, "path");
    if (path.length() == 0) path = "/ESPUSER";
    return mcp_text_result(fm_list_json(fm_normalize_path(path.c_str()), 0U, 0U, "name", "asc"));
  }
  if (tool == "mros.shell.exec") {
    if (!allow_shell) {
      return "{\"content\":[{\"type\":\"text\",\"text\":\"shell execution requires authenticated MCP transport\"}],\"isError\":true}";
    }
    String command = mcp_extract_string_field(body, "command");
    command.trim();
    if (command.length() == 0 || command.length() > 256) {
      return "{\"content\":[{\"type\":\"text\",\"text\":\"missing or too long command\"}],\"isError\":true}";
    }
    std::string output;
    const bool ok = mros::shell::execute_line_capture_as_user(
        command.c_str(), username.c_str(), &output, false, mros::shell::ShellTransport::Web);
    if (output.size() > 4096U) output.resize(4096U);
    String result = "{\"content\":[{\"type\":\"text\",\"text\":\"";
    result += jsonEscape(String(output.c_str()));
    result += "\"}],\"isError\":";
    result += ok ? "false" : "true";
    result += "}";
    return result;
  }
  return "{\"content\":[{\"type\":\"text\",\"text\":\"unknown MCP tool\"}],\"isError\":true}";
}

static String mcp_handle_resource_read(const String& body) {
  const String uri = mcp_extract_string_field(body, "uri");
  String text;
  if (uri == "mros://status") {
    text = mcp_status_json();
  } else if (uri == "mros://files/ESPUSER") {
    text = fm_list_json("/ESPUSER", 0U, 0U, "name", "asc");
  } else if (uri == "mros://settings/popup") {
    text = read_user_popup_settings_json();
  } else {
    text = "{\"error\":\"unknown resource\"}";
  }
  return String("{\"contents\":[{\"uri\":\"") + jsonEscape(uri) +
         "\",\"mimeType\":\"application/json\",\"text\":\"" + jsonEscape(text) + "\"}]}";
}

static String mcp_handle_prompt_get(const String& body) {
  const String name = mcp_extract_string_field(body, "name");
  String text = "Use the available MROS MCP tools to inspect status first, then operate conservatively.";
  if (name == "mros-pick-place") {
    text = "Use mros.status, inspect the pick-and-place 3D scene, then add or move boxes without sending physical motion unless explicitly requested.";
  } else if (name == "mros-files") {
    text = "List /ESPUSER, prefer read-only inspection first, and keep file operations inside the writable user tree.";
  }
  return String("{\"description\":\"") + jsonEscape(name) +
         "\",\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":\"" +
         jsonEscape(text) + "\"}}]}";
}

static String mcp_handle_json_rpc(const String& body,
                                  const bool allow_shell,
                                  const String& username,
                                  const uint32_t capabilities) {
  const String method = mcp_extract_string_field(body, "method");
  const String id = mcp_extract_raw_id(body);
  if (method.length() == 0) {
    return mcp_error(id, -32600, "Invalid JSON-RPC request");
  }
  if (id.length() == 0) {
    return "";
  }
  if (method == "initialize") {
    return mcp_response(
        id,
        "{\"protocolVersion\":\"2025-06-18\",\"serverInfo\":{\"name\":\"mros-esp32s3-bridge\",\"version\":\"0.1.0\"},\"capabilities\":{\"tools\":{\"listChanged\":false},\"resources\":{\"subscribe\":false,\"listChanged\":false},\"prompts\":{\"listChanged\":false},\"logging\":{}}}");
  }
  if (method == "ping") return mcp_response(id, "{}");
  if (method == "tools/list") return mcp_response(id, mcp_tools_list_result());
  if (method == "tools/call") return mcp_response(id, mcp_handle_tool_call(body, allow_shell, username, capabilities));
  if (method == "resources/list") return mcp_response(id, mcp_resources_list_result());
  if (method == "resources/read") return mcp_response(id, mcp_handle_resource_read(body));
  if (method == "prompts/list") return mcp_response(id, mcp_prompts_list_result());
  if (method == "prompts/get") return mcp_response(id, mcp_handle_prompt_get(body));
  return mcp_error(id, -32601, "Method not found");
}

static bool isAuthenticated(AsyncWebServerRequest *request) {
  const String session_id = request_session_cookie_token(request);
  String csrf_token;
  if (!auth_validate_session_token(session_id, nullptr, &csrf_token)) {
    return false;
  }
  if (!request_mutation_guard_ok(request, csrf_token)) {
    return false;
  }
  pm_mark_web_feedback();
  return true;
}

static void sendWsTrajectoryStoreResponse(AsyncWebSocketClient *client,
                                          bool success, size_t parsed_count,
                                          const char *error_message) {
  if (!client) return;
  String json = "{";
  json += "\"traj_store\":true,";
  json += "\"success\":" + String(success ? "true" : "false");
  if (success) {
    json += ",\"stored\":" + String((uint32_t)psram_traj_count);
    json += ",\"capacity\":" + String((uint32_t)psram_traj_capacity);
    json += ",\"preview\":" + String((uint32_t)psram_preview_count);
    json += ",\"preview_capacity\":" + String((uint32_t)psram_preview_capacity);
    json += ",\"preview_step_mm\":" + String(psram_preview_step_mm, 2);
    json += ",\"parsed\":" + String((uint32_t)parsed_count);
    json +=
        ",\"truncated\":" + String(psram_traj_last_truncated ? "true" : "false");
  } else {
    String err = error_message ? String(error_message) : String("traj_store_failed");
    if (err.length() == 0) err = "traj_store_failed";
    json += ",\"error\":\"" + jsonEscape(err) + "\"";
  }
  json += "}";
  client->text(json);
}

static bool finalizeWsTrajectoryUpload(AsyncWebSocketClient *client) {
  if (!client) {
    resetWsTrajectoryUploadState();
    return false;
  }

  if (!psram_buffers_ready && !initPsrTrajectoryBuffers()) {
    sendWsTrajectoryStoreResponse(client, false, 0, "psram_not_ready");
    resetWsTrajectoryUploadState();
    return false;
  }

  if (!trajectory_handler_stream_parse()) {
    sendWsTrajectoryStoreResponse(client, false, 0,
                                  trajectory_handler_last_error());
    resetWsTrajectoryUploadState();
    return false;
  }

  size_t parsed = 0;
  if (!copyParsedTrajectoryToPsrBuffer(&parsed)) {
    sendWsTrajectoryStoreResponse(client, false, 0, "trajectory_copy_failed");
    resetWsTrajectoryUploadState();
    return false;
  }

  float step_mm = ws_traj_upload_preview_step_mm;
  if (!std::isfinite(step_mm) || step_mm < 0.2f) step_mm = 2.0f;
  if (step_mm > 20.0f) step_mm = 20.0f;
  rebuildDensePreviewFromPsrTrajectory(step_mm);
  pm_request_perf_boost(2200);
  sendWsTrajectoryStoreResponse(client, true, parsed, nullptr);
  resetWsTrajectoryUploadState();
  return true;
}

static void sendWsShellError(AsyncWebSocketClient *client, const String &message) {
  if (!client) return;
  client->text("{\"shell\":{\"type\":\"error\",\"message\":\"" + jsonEscape(message) + "\"}}");
}

static AsyncWebSocketClient *findWsClientById(AsyncWebSocket &primary,
                                              AsyncWebSocket &secondary,
                                              const uint32_t client_id) {
  AsyncWebSocketClient *client = primary.client(client_id);
  if (client != nullptr && client->status() == WS_CONNECTED) return client;
  client = secondary.client(client_id);
  if (client != nullptr && client->status() == WS_CONNECTED) return client;
  return nullptr;
}

static void closeExpiredWsAuthClients(std::map<uint32_t, bool> &auth_map,
                                      std::map<uint32_t, unsigned long> &deadline_map,
                                      AsyncWebSocket &primary,
                                      AsyncWebSocket *secondary,
                                      const unsigned long now) {
  uint32_t expired[8];
  size_t expired_count = 0U;
  ws_auth_lock();
  for (auto it = deadline_map.begin(); it != deadline_map.end() &&
                                  expired_count < (sizeof(expired) / sizeof(expired[0]));
       ++it) {
    const uint32_t id = it->first;
    auto auth_it = auth_map.find(id);
    if (auth_it != auth_map.end() && auth_it->second) {
      continue;
    }
    if (static_cast<long>(now - it->second) >= 0) {
      expired[expired_count++] = id;
    }
  }

  for (size_t i = 0; i < expired_count; ++i) {
    const uint32_t id = expired[i];
    auth_map.erase(id);
    deadline_map.erase(id);
    if (&auth_map == &ws_auth_clients) {
      ws_full_snapshot_pending.erase(id);
      ws_scene_subscriptions.erase(id);
      ws_subscription_masks.erase(id);
      ws_telemetry_formats.erase(id);
    } else if (&auth_map == &ws_debug_auth_clients) {
      ws_debug_subscriptions.erase(id);
    }
  }
  ws_auth_unlock();

  for (size_t i = 0; i < expired_count; ++i) {
    const uint32_t id = expired[i];
    AsyncWebSocketClient *client = primary.client(id);
    if ((client == nullptr || client->status() != WS_CONNECTED) && secondary != nullptr) {
      client = secondary->client(id);
    }
    if (client != nullptr && client->status() == WS_CONNECTED) {
      client->text("{\"auth\":\"timeout\"}");
      client->close();
    }
  }
}

static void setWsSubscription(const uint32_t client_id,
                              const uint8_t bit,
                              const bool enabled) {
  ws_auth_lock();
  uint8_t mask = 0;
  auto it = ws_subscription_masks.find(client_id);
  if (it != ws_subscription_masks.end()) mask = it->second;
  if (enabled) {
    mask |= bit;
  } else {
    mask &= static_cast<uint8_t>(~bit);
  }
  ws_subscription_masks[client_id] = mask;
  if (bit == WS_SUB_SCENE) {
    ws_scene_subscriptions[client_id] = enabled;
    if (enabled) ws_full_snapshot_pending[client_id] = true;
  }
  ws_auth_unlock();
}

static bool parseSubscriptionEnabled(const String &msg, bool *enabled) {
  if (enabled == nullptr) return false;
  const int last_colon = msg.lastIndexOf(':');
  if (last_colon < 0 || last_colon >= (msg.length() - 1)) return false;
  const String value = msg.substring(last_colon + 1);
  *enabled = (value == "1" || value.equalsIgnoreCase("true") ||
              value.equalsIgnoreCase("on"));
  return true;
}

static bool handleWsSubscriptionMessage(const uint32_t client_id,
                                        const String &msg) {
  bool enabled = false;
  if (!parseSubscriptionEnabled(msg, &enabled)) return false;
  String topic;
  if (msg.startsWith("SUB:SCENE:")) {
    topic = "scene";
  } else if (msg.startsWith("SUB:")) {
    const int end = msg.indexOf(':', 4);
    if (end < 0) return false;
    topic = msg.substring(4, end);
    topic.toLowerCase();
  } else {
    return false;
  }

  if (topic == "scene") {
    setWsSubscription(client_id, WS_SUB_SCENE, enabled);
    return true;
  }
  if (topic == "debug") {
    setWsSubscription(client_id, WS_SUB_DEBUG, enabled);
    return true;
  }
  if (topic == "shell") {
    setWsSubscription(client_id, WS_SUB_SHELL, enabled);
    return true;
  }
  if (topic == "trajectory") {
    setWsSubscription(client_id, WS_SUB_TRAJECTORY, enabled);
    return true;
  }
  if (topic == "logs") {
    setWsSubscription(client_id, WS_SUB_LOGS, enabled);
    return true;
  }
  if (topic == "fast") {
    setWsSubscription(client_id, WS_SUB_RATE_FAST, enabled);
    return true;
  }
  if (topic == "medium") {
    setWsSubscription(client_id, WS_SUB_RATE_MEDIUM, enabled);
    return true;
  }
  if (topic == "slow") {
    setWsSubscription(client_id, WS_SUB_RATE_SLOW, enabled);
    return true;
  }
  return false;
}

static bool handleWsTelemetryFormatMessage(const uint32_t client_id,
                                           const String &msg,
                                           AsyncWebSocketClient *client) {
  if (!msg.startsWith("FORMAT:")) return false;
  String requested = msg.substring(7);
  requested.trim();
  requested.toLowerCase();

  uint8_t format = WS_TELEMETRY_FORMAT_JSON_V1;
  if (requested == "bin-v1" || requested == "mros-bin-v1") {
    format = WS_TELEMETRY_FORMAT_BIN_V1;
  } else if (requested == "json-v1" || requested == "json") {
    format = WS_TELEMETRY_FORMAT_JSON_V1;
    g_ws_json_fallback_count++;
  } else {
    g_ws_format_error_count++;
    if (client != nullptr) {
      client->text("{\"format\":\"error\",\"active\":\"json-v1\",\"message\":\"unsupported telemetry format\"}");
    }
    return true;
  }

  ws_auth_lock();
  ws_telemetry_formats[client_id] = format;
  ws_full_snapshot_pending[client_id] = true;
  ws_auth_unlock();

  if (client != nullptr) {
    String reply = "{\"format\":\"ok\",\"active\":\"";
    reply += wsTelemetryFormatName(format);
    reply += "\",\"protocol\":\"mros-bin-v1\",\"fallback\":\"json-v1\"}";
    client->text(reply);
  }
  return true;
}

static bool handleWsTelemetryFullRequiredMessage(const uint32_t client_id,
                                                 const String &msg,
                                                 AsyncWebSocketClient *client) {
  if (!(msg == "FULL_REQUIRED" || msg == "FULL:1" || msg == "TELEMETRY:FULL")) {
    return false;
  }
  ws_auth_lock();
  ws_full_snapshot_pending[client_id] = true;
  ws_auth_unlock();
  g_ws_full_required_count++;
  if (client != nullptr) {
    client->text("{\"telemetry\":{\"type\":\"full_required\",\"accepted\":true}}");
  }
  return true;
}

static bool handleWorkerDecodeMetricMessage(const String &msg) {
  if (!msg.startsWith("WORKER_DECODE_METRIC:")) {
    return false;
  }
  const String payload = msg.substring(21);
  unsigned long frames = 0;
  unsigned long errors = 0;
  unsigned long fallbacks = 0;
  unsigned long latency = 0;
  unsigned long dropped = 0;
  if (std::sscanf(payload.c_str(), "%lu:%lu:%lu:%lu:%lu",
                  &frames, &errors, &fallbacks, &latency, &dropped) >= 4) {
    g_worker_decode_frames = static_cast<uint32_t>(frames);
    g_worker_decode_errors = static_cast<uint32_t>(errors);
    g_worker_decode_fallbacks = static_cast<uint32_t>(fallbacks);
    g_worker_decode_latency_ms = static_cast<uint32_t>(latency);
    g_worker_decode_dropped = static_cast<uint32_t>(dropped);
  }
  return true;
}

static bool wsJsonExtractFloat(const String &json, const char *key, float *out) {
  if (key == nullptr || out == nullptr) return false;
  const String pattern = String("\"") + key + "\":";
  const int start = json.indexOf(pattern);
  if (start < 0) return false;
  int pos = start + pattern.length();
  while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
  int end = pos;
  while (end < json.length()) {
    const char c = json[end];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
      end++;
      continue;
    }
    break;
  }
  if (end <= pos) return false;
  *out = json.substring(pos, end).toFloat();
  return true;
}

static bool wsJsonExtractBool(const String &json, const char *key, bool *out) {
  if (key == nullptr || out == nullptr) return false;
  const String pattern = String("\"") + key + "\":";
  const int start = json.indexOf(pattern);
  if (start < 0) return false;
  int pos = start + pattern.length();
  while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
  if (json.startsWith("true", pos)) {
    *out = true;
    return true;
  }
  if (json.startsWith("false", pos)) {
    *out = false;
    return true;
  }
  float num = 0.0f;
  if (wsJsonExtractFloat(json, key, &num)) {
    *out = num >= 0.5f;
    return true;
  }
  return false;
}

static bool wsJsonExtractString(const String &json, const char *key, char *out, size_t out_size) {
  if (key == nullptr || out == nullptr || out_size == 0U) return false;
  const String pattern = String("\"") + key + "\":\"";
  const int start = json.indexOf(pattern);
  if (start < 0) return false;
  const int value_start = start + pattern.length();
  const int value_end = json.indexOf('\"', value_start);
  if (value_end < 0) return false;
  const String value = json.substring(value_start, value_end);
  std::snprintf(out, out_size, "%s", value.c_str());
  return true;
}

static bool parseWsRobotPoseJson(const String &json, bool apply, WebRobotUiCommand *command) {
  if (command == nullptr) return false;
  WebRobotUiCommand cmd {};
  std::snprintf(cmd.op, sizeof(cmd.op), "%s", "point");
  if (!wsJsonExtractFloat(json, "x", &cmd.x)) return false;
  if (!wsJsonExtractFloat(json, "y", &cmd.y)) return false;
  if (!wsJsonExtractFloat(json, "z", &cmd.z)) return false;
  if (!wsJsonExtractFloat(json, "t_ms", &cmd.t_ms)) {
    wsJsonExtractFloat(json, "t", &cmd.t_ms);
  }
  wsJsonExtractBool(json, "ee_auto", &cmd.ee_auto);
  wsJsonExtractFloat(json, "roll_deg", &cmd.roll_deg);
  if (!wsJsonExtractFloat(json, "pitch_deg", &cmd.pitch_deg)) {
    wsJsonExtractFloat(json, "ee_pitch", &cmd.pitch_deg);
  }
  wsJsonExtractFloat(json, "yaw_deg", &cmd.yaw_deg);
  cmd.ee_pitch = cmd.pitch_deg;
  cmd.apply = apply;
  if (!wsJsonExtractString(json, "calc", cmd.calc_mode, sizeof(cmd.calc_mode))) {
    std::snprintf(cmd.calc_mode, sizeof(cmd.calc_mode), "%s", web_server_get_ik_compute_preference());
  }
  std::snprintf(cmd.source, sizeof(cmd.source), "%s", "ws");
  *command = cmd;
  return true;
}

struct ShellCborControlMessage {
  char op[20] = {};
  char payload[1024] = {};
  uint8_t pane_id = 0;
  uint16_t command_id = 0;
};

static bool shell_cbor_read_len(const uint8_t *data,
                                const size_t len,
                                size_t *offset,
                                uint8_t *major,
                                uint32_t *value) {
  if (data == nullptr || offset == nullptr || major == nullptr || value == nullptr || *offset >= len) {
    return false;
  }
  const uint8_t head = data[(*offset)++];
  *major = static_cast<uint8_t>(head >> 5);
  const uint8_t add = static_cast<uint8_t>(head & 0x1FU);
  if (add < 24U) {
    *value = add;
    return true;
  }
  if (add == 24U && *offset < len) {
    *value = data[(*offset)++];
    return true;
  }
  if (add == 25U && (*offset + 1U) < len) {
    *value = (static_cast<uint32_t>(data[*offset]) << 8) |
             static_cast<uint32_t>(data[*offset + 1U]);
    *offset += 2U;
    return true;
  }
  if (add == 26U && (*offset + 3U) < len) {
    *value = (static_cast<uint32_t>(data[*offset]) << 24) |
             (static_cast<uint32_t>(data[*offset + 1U]) << 16) |
             (static_cast<uint32_t>(data[*offset + 2U]) << 8) |
             static_cast<uint32_t>(data[*offset + 3U]);
    *offset += 4U;
    return true;
  }
  return false;
}

static bool shell_cbor_read_text(const uint8_t *data,
                                 const size_t len,
                                 size_t *offset,
                                 char *out,
                                 const size_t out_size) {
  uint8_t major = 0;
  uint32_t text_len = 0;
  if (!shell_cbor_read_len(data, len, offset, &major, &text_len) || major != 3U ||
      out == nullptr || out_size == 0U || text_len >= out_size ||
      (*offset + text_len) > len) {
    return false;
  }
  std::memcpy(out, data + *offset, text_len);
  out[text_len] = '\0';
  *offset += text_len;
  return true;
}

static bool shell_cbor_skip_value(const uint8_t *data, const size_t len, size_t *offset) {
  uint8_t major = 0;
  uint32_t value = 0;
  if (!shell_cbor_read_len(data, len, offset, &major, &value)) {
    return false;
  }
  if (major == 0U) {
    return true;
  }
  if (major == 3U && (*offset + value) <= len) {
    *offset += value;
    return true;
  }
  return false;
}

static bool parseShellCborControlFrame(const uint8_t *data,
                                       const size_t len,
                                       ShellCborControlMessage *out,
                                       char *error,
                                       const size_t error_size) {
  auto fail = [&](const char *message) {
    if (error != nullptr && error_size > 0U) {
      std::snprintf(error, error_size, "%s", message != nullptr ? message : "bad shell cbor");
    }
    return false;
  };
  if (data == nullptr || out == nullptr || len < 9U) {
    return fail("short SCB1 frame");
  }
  if (std::memcmp(data, "SCB1", 4U) != 0 || data[4] != 1U || data[5] != 8U) {
    return fail("unsupported SCB1 frame");
  }
  size_t offset = 8U;
  uint8_t major = 0;
  uint32_t pair_count = 0;
  if (!shell_cbor_read_len(data, len, &offset, &major, &pair_count) || major != 5U || pair_count > 16U) {
    return fail("invalid SCB1 map");
  }
  for (uint32_t i = 0; i < pair_count; ++i) {
    char key[24] = {};
    if (!shell_cbor_read_text(data, len, &offset, key, sizeof(key))) {
      return fail("invalid SCB1 key");
    }
    if (std::strcmp(key, "op") == 0) {
      if (!shell_cbor_read_text(data, len, &offset, out->op, sizeof(out->op))) {
        return fail("invalid SCB1 op");
      }
    } else if (std::strcmp(key, "payload") == 0) {
      if (!shell_cbor_read_text(data, len, &offset, out->payload, sizeof(out->payload))) {
        return fail("invalid SCB1 payload");
      }
    } else if (std::strcmp(key, "pane") == 0 || std::strcmp(key, "command_id") == 0) {
      uint8_t value_major = 0;
      uint32_t value = 0;
      if (!shell_cbor_read_len(data, len, &offset, &value_major, &value) || value_major != 0U) {
        return fail("invalid SCB1 integer");
      }
      if (std::strcmp(key, "pane") == 0) {
        out->pane_id = static_cast<uint8_t>(value > 3U ? 3U : value);
      } else {
        out->command_id = static_cast<uint16_t>(value & 0xFFFFU);
      }
    } else if (!shell_cbor_skip_value(data, len, &offset)) {
      return fail("unsupported SCB1 value");
    }
  }
  if (out->op[0] == '\0') {
    return fail("SCB1 missing op");
  }
  return true;
}

static bool handleShellWsBinaryControl(AsyncWebSocketClient *client,
                                       const uint32_t shell_client_id,
                                       const uint8_t *data,
                                       const size_t len) {
  ShellCborControlMessage control = {};
  char error[160] = {};
  if (!parseShellCborControlFrame(data, len, &control, error, sizeof(error))) {
    g_shell_cbor_control_errors++;
    mros::shell::service::note_client_binary_decode_error(shell_client_id);
    sendWsShellError(client, error);
    return true;
  }
  g_shell_cbor_control_frames++;
  g_shell_cbor_control_bytes += static_cast<uint32_t>(len);

  mros::shell::service::WebRequestType request_type = mros::shell::service::WebRequestType::State;
  if (std::strcmp(control.op, "state") == 0) {
    request_type = mros::shell::service::WebRequestType::State;
  } else if (std::strcmp(control.op, "exec") == 0) {
    request_type = mros::shell::service::WebRequestType::Execute;
  } else if (std::strcmp(control.op, "complete") == 0) {
    request_type = mros::shell::service::WebRequestType::Complete;
  } else if (std::strcmp(control.op, "suggest") == 0) {
    request_type = mros::shell::service::WebRequestType::Suggest;
  } else if (std::strcmp(control.op, "resize") == 0) {
    request_type = mros::shell::service::WebRequestType::Resize;
  } else {
    g_shell_cbor_control_errors++;
    sendWsShellError(client, "unsupported SCB1 op");
    return true;
  }

  char enqueue_error[160] = {};
  if (!mros::shell::service::enqueue_web_request(
          request_type, shell_client_id, control.payload, enqueue_error,
          sizeof(enqueue_error), control.pane_id, control.command_id)) {
    sendWsShellError(client, enqueue_error);
  }
  return true;
}

static bool handleShellWsMessage(AsyncWebSocketClient *client,
                                 const uint32_t shell_client_id,
                                 const String &msg) {
  if (msg.startsWith("SHELL2:")) {
    const int type_end = msg.indexOf(':', 7);
    if (type_end <= 7) {
      sendWsShellError(client, "invalid SHELL2 message");
      return true;
    }
    const String type = msg.substring(7, type_end);
    const int pane_end = msg.indexOf(':', type_end + 1);
    const String pane_text = pane_end >= 0 ? msg.substring(type_end + 1, pane_end)
                                          : msg.substring(type_end + 1);
    const uint8_t pane_id = static_cast<uint8_t>(pane_text.toInt());
    String payload = pane_end >= 0 ? msg.substring(pane_end + 1) : "";
    uint16_t command_id = 0U;
    mros::shell::service::WebRequestType request_type = mros::shell::service::WebRequestType::State;
    if (type == "STATE") {
      request_type = mros::shell::service::WebRequestType::State;
    } else if (type == "EXEC") {
      request_type = mros::shell::service::WebRequestType::Execute;
    } else if (type == "COMPLETE") {
      request_type = mros::shell::service::WebRequestType::Complete;
    } else if (type == "SUGGEST") {
      request_type = mros::shell::service::WebRequestType::Suggest;
    } else if (type == "RESIZE") {
      request_type = mros::shell::service::WebRequestType::Resize;
    } else {
      sendWsShellError(client, "unsupported SHELL2 message");
      return true;
    }
    if (type == "EXEC" && payload.startsWith("@")) {
      const int command_end = payload.indexOf(':', 1);
      if (command_end > 1) {
        bool digits = true;
        for (int i = 1; i < command_end; ++i) {
          if (!std::isdigit(static_cast<unsigned char>(payload[i]))) {
            digits = false;
            break;
          }
        }
        if (digits) {
          command_id = static_cast<uint16_t>(payload.substring(1, command_end).toInt());
          payload = payload.substring(command_end + 1);
        }
      }
    }
    char error[160] = {};
    if (!mros::shell::service::enqueue_web_request(
            request_type, shell_client_id, payload.c_str(), error, sizeof(error), pane_id, command_id)) {
      sendWsShellError(client, error);
    }
    return true;
  }
  if (msg == "SHELL:STATE") {
    char error[160] = {};
    if (!mros::shell::service::enqueue_web_request(
            mros::shell::service::WebRequestType::State, shell_client_id, "",
            error, sizeof(error))) {
      sendWsShellError(client, error);
    }
    return true;
  }
  if (msg.startsWith("SHELL:EXEC:")) {
    char error[160] = {};
    const String line = msg.substring(11);
    if (!mros::shell::service::enqueue_web_request(
            mros::shell::service::WebRequestType::Execute, shell_client_id,
            line.c_str(), error, sizeof(error))) {
      sendWsShellError(client, error);
    }
    return true;
  }
  if (msg.startsWith("SHELL:COMPLETE:")) {
    char error[160] = {};
    const String partial = msg.substring(15);
    if (!mros::shell::service::enqueue_web_request(
            mros::shell::service::WebRequestType::Complete, shell_client_id,
            partial.c_str(), error, sizeof(error))) {
      sendWsShellError(client, error);
    }
    return true;
  }
  if (msg.startsWith("SHELL:SUGGEST:")) {
    char error[160] = {};
    const String partial = msg.substring(14);
    if (!mros::shell::service::enqueue_web_request(
            mros::shell::service::WebRequestType::Suggest, shell_client_id,
            partial.c_str(), error, sizeof(error))) {
      sendWsShellError(client, error);
    }
    return true;
  }
  if (msg.startsWith("SHELL:RESIZE:")) {
    char error[160] = {};
    const String size_payload = msg.substring(13);
    if (!mros::shell::service::enqueue_web_request(
            mros::shell::service::WebRequestType::Resize, shell_client_id,
            size_payload.c_str(), error, sizeof(error))) {
      sendWsShellError(client, error);
    }
    return true;
  }
  return false;
}

static void onWsShellEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                           AwsEventType type, void *arg, uint8_t *data,
                           size_t len) {
  web_server_notify_runtime_task();
  const bool legacy_shell_socket = (server == &ws_shell);
  const uint32_t shell_client_id = shell_ws_encode_client_id(client->id());
  if (type == WS_EVT_CONNECT) {
    ws_set_pending_auth(ws_shell_auth_clients, ws_shell_auth_deadlines, client->id());
    mros::shell::service::close_client_sessions(shell_client_id);
    return;
  }
  if (type == WS_EVT_DISCONNECT) {
    ws_erase_auth(ws_shell_auth_clients, ws_shell_auth_deadlines, client->id());
    mros::shell::service::close_client_sessions(shell_client_id);
    return;
  }
  if (type != WS_EVT_DATA) {
    return;
  }
  AwsFrameInfo *info = reinterpret_cast<AwsFrameInfo *>(arg);
  if (!info || !info->final || info->index != 0 || info->len != len) {
    return;
  }
  if (info->opcode == WS_BINARY) {
    if (!ws_client_authenticated(ws_shell_auth_clients, client->id()) || legacy_shell_socket) {
      mros::shell::service::note_client_binary_decode_error(shell_client_id);
      sendWsShellError(client, "binary shell control requires /ws/shell auth");
      return;
    }
    pm_mark_web_feedback();
    handleShellWsBinaryControl(client, shell_client_id, data, len);
    return;
  }
  if (info->opcode != WS_TEXT) {
    return;
  }
  String msg;
  msg.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    msg += static_cast<char>(data[i]);
  }

  if (msg.startsWith("AUTH:")) {
    const String ticket = msg.substring(5);
    WsTicketClaim claim;
    if (consumeWsTicket(ticket, WsTicketScope::Shell, &claim)) {
      mros::shell::service::set_client_user(shell_client_id, claim.username.c_str());
      ws_mark_authenticated(ws_shell_auth_clients, ws_shell_auth_deadlines, client->id());
      if (legacy_shell_socket) {
        mros::shell::service::set_client_binary_stream(shell_client_id, false);
        client->text("{\"auth\":\"ok\",\"channel\":\"shell\",\"protocol\":\"mros-web-v1\"}");
      } else {
        String reply = "{\"auth\":\"ok\",\"channel\":\"shell\",\"protocol\":\"";
        reply += mros::shell::kShellProtocolName;
        reply += "\",\"shell_version\":\"";
        reply += mros::shell::kShellVersion;
        reply += "\",\"release_line\":\"";
        reply += mros::shell::kShellReleaseLine;
        reply += "\",\"release_status\":\"";
        reply += mros::shell::kShellReleaseStatus;
        reply += "\",\"formats\":[\"";
        reply += mros::shell::kShellBinaryFormat;
        reply += "\",\"";
        reply += mros::shell::kShellCborFormat;
        reply += "\",\"";
        reply += mros::shell::kShellJsonFormat;
        reply += "\"],\"default_format\":\"";
        reply += mros::shell::kShellBinaryFormat;
        reply += "\",\"user\":\"";
        reply += jsonEscape(claim.username);
        reply += "\",\"capabilities\":\"";
        reply += jsonEscape(String(mros::shell::capabilities_text(claim.capability_mask)));
        reply += "\"}";
        client->text(reply);
      }
    } else {
      client->text("{\"auth\":\"fail\"}");
      client->close();
    }
    return;
  }

  if (!ws_client_authenticated(ws_shell_auth_clients, client->id())) {
    client->text("{\"auth\":\"required\"}");
    client->close();
    return;
  }
  pm_mark_web_feedback();
  if (msg == "FORMAT:shell-bin-v1") {
    if (!legacy_shell_socket) {
      mros::shell::service::set_client_binary_stream(shell_client_id, true);
      client->text("{\"shell\":{\"type\":\"protocol\",\"format\":\"shell-bin-v1\"}}");
    } else {
      mros::shell::service::set_client_binary_stream(shell_client_id, false);
      client->text("{\"shell\":{\"type\":\"protocol\",\"format\":\"shell-json-v1\"}}");
    }
    return;
  }
  if (msg == "FORMAT:shell-json-v1") {
    mros::shell::service::note_client_json_fallback(shell_client_id);
    client->text("{\"shell\":{\"type\":\"protocol\",\"format\":\"shell-json-v1\"}}");
    return;
  }
  if (msg == "FORMAT:shell-cbor-v1") {
    if (!legacy_shell_socket) {
      client->text("{\"shell\":{\"type\":\"protocol\",\"format\":\"shell-cbor-v1\"}}");
    } else {
      client->text("{\"shell\":{\"type\":\"protocol\",\"format\":\"shell-json-v1\"}}");
    }
    return;
  }
  if (msg.startsWith("SHELL_BIN_DECODE_ERROR")) {
    mros::shell::service::note_client_binary_decode_error(shell_client_id);
    return;
  }
  if (msg.startsWith("ACK:")) {
    const int sep = msg.indexOf(':', 4);
    const uint32_t ack = static_cast<uint32_t>(msg.substring(4, sep >= 0 ? sep : msg.length()).toInt());
    const uint32_t credit = sep >= 0 ? static_cast<uint32_t>(msg.substring(sep + 1).toInt()) : 0U;
    mros::shell::service::note_client_ack_credit(shell_client_id, ack, credit);
    return;
  }
  if (!handleShellWsMessage(client, shell_client_id, msg)) {
    sendWsShellError(client, "unsupported shell message");
  }
}

static void onWsDebugEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                           AwsEventType type, void *arg, uint8_t *data,
                           size_t len) {
  (void)server;
  web_server_notify_runtime_task();
  if (type == WS_EVT_CONNECT) {
    ws_set_debug_pending(client->id());
    return;
  }
  if (type == WS_EVT_DISCONNECT) {
    ws_erase_debug_client(client->id());
    return;
  }
  if (type != WS_EVT_DATA) return;

  AwsFrameInfo *info = reinterpret_cast<AwsFrameInfo *>(arg);
  if (!info || info->opcode != WS_TEXT || !info->final || info->index != 0 ||
      info->len != len) {
    return;
  }
  String msg;
  msg.reserve(len);
  for (size_t i = 0; i < len; ++i) msg += static_cast<char>(data[i]);

  if (msg.startsWith("AUTH:")) {
    const String ticket = msg.substring(5);
    WsTicketClaim claim;
    if (consumeWsTicket(ticket, WsTicketScope::Debug, &claim) &&
        ((claim.capability_mask & mros::shell::ShellCapabilityDebug) == mros::shell::ShellCapabilityDebug)) {
      ws_mark_debug_authenticated(client->id());
      client->text("{\"auth\":\"ok\",\"channel\":\"debug\",\"protocol\":\"mros-web-v1\"}");
    } else {
      client->text("{\"auth\":\"fail\"}");
      client->close();
    }
    return;
  }

  if (!ws_client_authenticated(ws_debug_auth_clients, client->id())) {
    client->text("{\"auth\":\"required\"}");
    client->close();
    return;
  }
  pm_mark_web_feedback();
  if (msg == "SUB:debug:1") {
    ws_set_debug_subscription(client->id(), true);
    return;
  }
  if (msg == "SUB:debug:0") {
    ws_set_debug_subscription(client->id(), false);
    return;
  }
}

static void onWsMcpEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                         AwsEventType type, void *arg, uint8_t *data,
                         size_t len) {
  (void)server;
  web_server_notify_runtime_task();
  if (type == WS_EVT_CONNECT) {
    if (!mros::mcp::service_is_enabled()) {
      client->text("{\"auth\":\"disabled\",\"channel\":\"mcp\",\"hint\":\"enable MCP in Settings > Sistem Servisleri\"}");
      client->close();
      return;
    }
    ws_set_pending_auth(ws_mcp_auth_clients, ws_mcp_auth_deadlines, client->id());
    ws_auth_lock();
    ws_mcp_contexts.erase(client->id());
    ws_auth_unlock();
    return;
  }
  if (type == WS_EVT_DISCONNECT) {
    ws_erase_auth(ws_mcp_auth_clients, ws_mcp_auth_deadlines, client->id());
    ws_auth_lock();
    ws_mcp_contexts.erase(client->id());
    ws_auth_unlock();
    return;
  }
  if (type != WS_EVT_DATA) return;
  AwsFrameInfo *info = reinterpret_cast<AwsFrameInfo *>(arg);
  if (!info || info->opcode != WS_TEXT || !info->final || info->index != 0 ||
      info->len != len) {
    return;
  }
  String msg;
  msg.reserve(len);
  for (size_t i = 0; i < len; ++i) msg += static_cast<char>(data[i]);

  if (msg.startsWith("AUTH:")) {
    const String ticket = msg.substring(5);
    WsTicketClaim claim;
    if (consumeWsTicket(ticket, WsTicketScope::Mcp, &claim) &&
        ((claim.capability_mask & mros::shell::ShellCapabilityDebug) == mros::shell::ShellCapabilityDebug)) {
      ws_auth_lock();
      ws_mcp_contexts[client->id()] = claim;
      ws_auth_unlock();
      ws_mark_authenticated(ws_mcp_auth_clients, ws_mcp_auth_deadlines, client->id());
      String reply = "{\"auth\":\"ok\",\"channel\":\"mcp\",\"protocol\":\"mcp-jsonrpc-2025-06-18\",\"user\":\"";
      reply += jsonEscape(claim.username);
      reply += "\",\"capabilities\":\"";
      reply += jsonEscape(String(mros::shell::capabilities_text(claim.capability_mask)));
      reply += "\"}";
      client->text(reply);
    } else {
      client->text("{\"auth\":\"failed\"}");
      client->close();
    }
    return;
  }

  if (!ws_client_authenticated(ws_mcp_auth_clients, client->id())) {
    client->text("{\"auth\":\"required\",\"hint\":\"send AUTH:<ticket> from /api/ws-ticket first\"}");
    client->close();
    return;
  }

  pm_mark_web_feedback();
  mros::mcp::service_mark_activity();
  WsTicketClaim claim;
  ws_auth_lock();
  const auto context = ws_mcp_contexts.find(client->id());
  if (context != ws_mcp_contexts.end()) {
    claim = context->second;
  }
  ws_auth_unlock();
  if (claim.username.length() == 0U) {
    client->text("{\"auth\":\"required\",\"hint\":\"MCP auth context missing\"}");
    client->close();
    return;
  }
  const String reply = mcp_handle_json_rpc(msg, mros::mcp::service_allow_shell(), claim.username, claim.capability_mask);
  if (reply.length() > 0) client->text(reply);
}

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  (void)server;
  web_server_notify_runtime_task();
  if (type == WS_EVT_CONNECT) {
    ws_set_telemetry_pending(client->id());
  } else if (type == WS_EVT_DISCONNECT) {
    ws_erase_telemetry_client(client->id());
    mros::shell::service::close_client_sessions(client->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (!info || info->opcode != WS_TEXT) {
      return;
    }

    // Large trajectory upload over WS (fragment-aware):
    // TRJCSV:<payload> or TRJJSON:<payload>
    static const char kTrajCsvPrefix[] = "TRJCSV:";
    static const char kTrajJsonPrefix[] = "TRJJSON:";

    if (info->index == 0) {
      size_t prefix_len = 0;
      if (len >= (sizeof(kTrajCsvPrefix) - 1) &&
          memcmp(data, kTrajCsvPrefix, sizeof(kTrajCsvPrefix) - 1) == 0) {
        prefix_len = sizeof(kTrajCsvPrefix) - 1;
      } else if (len >= (sizeof(kTrajJsonPrefix) - 1) &&
                 memcmp(data, kTrajJsonPrefix, sizeof(kTrajJsonPrefix) - 1) ==
                     0) {
        prefix_len = sizeof(kTrajJsonPrefix) - 1;
      }

      if (prefix_len > 0) {
        if (!ws_client_authenticated(ws_auth_clients, client->id())) {
          client->text("{\"auth\":\"required\"}");
          client->close();
          return;
        }
        pm_mark_web_feedback();

        if (ws_traj_upload_active && ws_traj_upload_client_id != client->id()) {
          sendWsTrajectoryStoreResponse(client, false, 0, "traj_upload_busy");
          return;
        }

        const size_t total_payload =
            (info->len > prefix_len) ? (info->len - prefix_len) : 0;
        if (total_payload == 0) {
          sendWsTrajectoryStoreResponse(client, false, 0, "traj_payload_empty");
          return;
        }

        ws_traj_upload_preview_step_mm = 2.0f;
        if (!trajectory_handler_stream_begin(total_payload)) {
          sendWsTrajectoryStoreResponse(client, false, 0,
                                        trajectory_handler_last_error());
          resetWsTrajectoryUploadState();
          return;
        }

        ws_traj_upload_active = true;
        ws_traj_upload_client_id = client->id();
        ws_traj_upload_prefix_len = prefix_len;

        const size_t payload_len = (len > prefix_len) ? (len - prefix_len) : 0;
        if (payload_len > 0) {
          if (!trajectory_handler_stream_write(0, data + prefix_len,
                                              payload_len)) {
            sendWsTrajectoryStoreResponse(client, false, 0,
                                          trajectory_handler_last_error());
            resetWsTrajectoryUploadState();
            return;
          }
        }
        if (info->final) {
          finalizeWsTrajectoryUpload(client);
        }
        return;
      }
    }

    if (ws_traj_upload_active && ws_traj_upload_client_id == client->id()) {
      if (info->index < ws_traj_upload_prefix_len) {
        sendWsTrajectoryStoreResponse(client, false, 0, "traj_chunk_index");
        resetWsTrajectoryUploadState();
        return;
      }
      const size_t payload_index = info->index - ws_traj_upload_prefix_len;
      if (!trajectory_handler_stream_write(payload_index, data, len)) {
        sendWsTrajectoryStoreResponse(client, false, 0,
                                      trajectory_handler_last_error());
        resetWsTrajectoryUploadState();
        return;
      }
      if (info->final) {
        finalizeWsTrajectoryUpload(client);
      }
      return;
    }

    if (info->final && info->index == 0 && info->len == len &&
        info->opcode == WS_TEXT) {
      // Do not write data[len] = 0; buffer is not guaranteed to have spare byte.
      String msg;
      msg.reserve(len);
      for (size_t i = 0; i < len; i++) {
        msg += (char)data[i];
      }

      // ---- AUTH handshake (must be the first message from any client) ----
      if (msg.startsWith("AUTH:")) {
        String ticket = msg.substring(5);
        WsTicketClaim claim;
        if (consumeWsTicket(ticket, WsTicketScope::Telemetry, &claim)) {
          ws_mark_telemetry_authenticated(client->id());
          mros::shell::service::set_client_user(client->id(), claim.username.c_str());
          pm_mark_web_feedback();
          String reply = "{\"auth\":\"ok\",\"channel\":\"telemetry\",\"protocol\":\"mros-web-v2\",\"formats\":[\"bin-v1\",\"json-v1\"],\"default_format\":\"bin-v1\",\"user\":\"";
          reply += jsonEscape(claim.username);
          reply += "\",\"capabilities\":\"";
          reply += jsonEscape(String(mros::shell::capabilities_text(claim.capability_mask)));
          reply += "\"}";
          client->text(reply);
        } else {
          client->text("{\"auth\":\"fail\"}");
          client->close();
        }
        return;
      }

      // ---- Reject any command from unauthenticated clients ----
      if (!ws_client_authenticated(ws_auth_clients, client->id())) {
        client->text("{\"auth\":\"required\"}");
        client->close();
        return;
      }
      pm_mark_web_feedback();

      if (handleWorkerDecodeMetricMessage(msg)) {
        return;
      }
      if (handleWsSubscriptionMessage(client->id(), msg)) {
        return;
      }
      if (handleWsTelemetryFormatMessage(client->id(), msg, client)) {
        return;
      }
      if (handleWsTelemetryFullRequiredMessage(client->id(), msg, client)) {
        return;
      }
      if (handleShellWsMessage(client, client->id(), msg)) {
        return;
      }

      // ---- Authenticated: process control commands ----
      if (msg.startsWith("IK6:") || msg.startsWith("CALC6:")) {
        WebRobotUiCommand cmd {};
        const bool apply = msg.startsWith("IK6:");
        const String payload = msg.substring(apply ? 4 : 6);
        if (parseWsRobotPoseJson(payload, apply, &cmd)) {
          web_server_publish_robot_ui_target(&cmd);
          spi_s3_set_target_cartesian(
              cmd.x, cmd.y, cmd.z, cmd.roll_deg, cmd.pitch_deg, cmd.yaw_deg,
              !cmd.ee_auto);
          pm_request_perf_boost();
        }
      } else if (msg.startsWith("IK:") || msg.startsWith("CALC:")) {
        WebRobotUiCommand cmd {};
        const bool apply = msg.startsWith("IK:");
        const String csv = msg.substring(apply ? 3 : 5);
        int p0 = 0;
        int p1 = csv.indexOf(',', p0);
        int p2 = p1 >= 0 ? csv.indexOf(',', p1 + 1) : -1;
        int p3 = p2 >= 0 ? csv.indexOf(',', p2 + 1) : -1;
        int t41 = p3 >= 0 ? csv.indexOf(',', p3 + 1) : -1;
        if (p1 > 0 && p2 > p1 && p3 > p2 && t41 > p3) {
          cmd.x = csv.substring(p0, p1).toFloat();
          cmd.y = csv.substring(p1 + 1, p2).toFloat();
          cmd.z = csv.substring(p2 + 1, p3).toFloat();
          cmd.t_ms = csv.substring(p3 + 1, t41).toFloat();
          const String alpha = csv.substring(t41 + 1);
          cmd.ee_auto = !(alpha.length() && alpha.charAt(0) != 'A');
          cmd.pitch_deg = cmd.ee_auto ? 0.0f : alpha.toFloat();
          cmd.ee_pitch = cmd.pitch_deg;
          cmd.apply = apply;
          std::snprintf(cmd.op, sizeof(cmd.op), "%s", "point");
          std::snprintf(cmd.calc_mode, sizeof(cmd.calc_mode), "%s", web_server_get_ik_compute_preference());
          std::snprintf(cmd.source, sizeof(cmd.source), "%s", "ws");
          web_server_publish_robot_ui_target(&cmd);
          spi_s3_set_target_cartesian(cmd.x, cmd.y, cmd.z, 0.0f, cmd.pitch_deg,
                                      0.0f, !cmd.ee_auto);
          pm_request_perf_boost();
        }
      } else if (msg.startsWith("T:")) {
        float v = msg.substring(2).toFloat();
        spi_s3_set_target_turret(v);
        live_fk_set_target(0, v);
        dbg_last_motion_cmd_ms = mros::platform::mros_millis();
        dbg_last_motion_cmd = "Turret hedef";
        pm_request_perf_boost();
      } else if (msg.startsWith("J0:")) {
        float v = msg.substring(3).toFloat();
        spi_s3_set_target_joint(0, v);
        live_fk_set_target(1, v);
        dbg_last_motion_cmd_ms = mros::platform::mros_millis();
        dbg_last_motion_cmd = "Joint 1 hedef";
        pm_request_perf_boost();
      } else if (msg.startsWith("J1:")) {
        float v = msg.substring(3).toFloat();
        spi_s3_set_target_joint(1, v);
        live_fk_set_target(2, v);
        dbg_last_motion_cmd_ms = mros::platform::mros_millis();
        dbg_last_motion_cmd = "Joint 2 hedef";
        pm_request_perf_boost();
      } else if (msg.startsWith("J2:")) {
        float v = msg.substring(3).toFloat();
        spi_s3_set_target_joint(2, v);
        live_fk_set_target(3, v);
        dbg_last_motion_cmd_ms = mros::platform::mros_millis();
        dbg_last_motion_cmd = "Joint 3 hedef";
        pm_request_perf_boost();
      } else if (msg.startsWith("J3:")) {
        float v = msg.substring(3).toFloat();
        spi_s3_set_target_joint(3, v);
        live_fk_set_target(4, v);
        dbg_last_motion_cmd_ms = mros::platform::mros_millis();
        dbg_last_motion_cmd = "Joint 4 hedef";
        pm_request_perf_boost();
      } else if (msg.startsWith("J4:")) {
        float v = msg.substring(3).toFloat();
        spi_s3_set_target_joint(4, v);
        live_fk_set_target(5, v);
        dbg_last_motion_cmd_ms = mros::platform::mros_millis();
        dbg_last_motion_cmd = "Joint 5 hedef";
        pm_request_perf_boost();
      } else if (msg.startsWith("J5:")) {
        float v = msg.substring(3).toFloat();
        spi_s3_set_target_joint(5, v);
        live_fk_set_target(6, v);
        dbg_last_motion_cmd_ms = mros::platform::mros_millis();
        dbg_last_motion_cmd = "Joint 6 hedef";
        pm_request_perf_boost();
      } else if (msg.startsWith("G:")) {
        spi_s3_set_target_gripper(msg.substring(2).toFloat());
        dbg_last_motion_cmd_ms = mros::platform::mros_millis();
        dbg_last_motion_cmd = "Gripper hedef";
        pm_request_perf_boost();
      } else if (msg.startsWith("TSCL:")) {
        float v = msg.substring(5).toFloat();
        spi_s3_set_joint_traj_time_scale(v);
      } else if (msg.startsWith("LP:")) {
        // Live Preview toggle: LP:1 = enabled, LP:0 = disabled
        bool lp_enabled = (msg.substring(3).toInt() == 1);
        live_fk_set_enabled(lp_enabled);
        mros_console.printf("Live Preview: %s\n", lp_enabled ? "ON" : "OFF");
      }
    }
  }
}

static bool should_bypass_captive_redirect(const String &url) {
  return url.startsWith("/api") || url.startsWith("/ws") || url == "/mcp" ||
         url.startsWith("/css") || url.startsWith("/js") ||
         url.startsWith("/assets") || url.startsWith("/cad") ||
         url.startsWith("/materials") || url.startsWith("/robot-data") ||
         url == "/favicon.ico";
}

void web_server_init() {
  logger_init();
  pm_configure_dynamic_scaling();
  initPsrTrajectoryBuffers();
  ensure_web_state_locks();
  web_server_logout_all();

  // Load PID Controller settings from internal storage
  float loaded_kp, loaded_ki, loaded_kd, loaded_imax, loaded_dspc;
  if (prefs_load_pid(loaded_kp, loaded_ki, loaded_kd, loaded_imax, loaded_dspc)) {
    spi_s3_set_turret_pid(loaded_kp, loaded_ki, loaded_kd, loaded_imax);
    spi_s3_set_turret_dspc(loaded_dspc);
    mros_console.println("[WEB] Applied PID gains from LittleFS");
  }

  // Initial setup mode: do not auto-create a user.
  {
    String _u, _h;
    if (!load_stored_credentials(_u, _h)) {
      login_fail_count = 0;
      login_block_until_ms = 0;
      mros_console.println("[SEC] No credentials found. Initial setup required.");
    }
  }
  refresh_current_username_from_credentials();
  wifi_manager_init();

  // Captive Portal Redirect Handler
  server.onNotFound([](AsyncWebServerRequest *request) {
    const String url = request->url();
    if (should_bypass_captive_redirect(url)) {
      if (url.startsWith("/api")) {
        request->send(404, "application/json",
                      "{\"success\":false,\"error\":\"NOT_FOUND\",\"message\":\"Route not found\"}");
      } else {
        request->send(404, "text/plain", "Not Found");
      }
      return;
    }
    String host = request->host();
    if (host.indexOf("192.168.") == -1 && host.indexOf("10.0.") == -1) {
      const char *redirect_ip = wifi_manager_state().ap_active
                                    ? wifi_manager_ap_ip()
                                    : wifi_manager_ip();
      if (redirect_ip != nullptr && strlen(redirect_ip) > 0 &&
          strcmp(redirect_ip, "0.0.0.0") != 0) {
        request->redirect(String("http://") + redirect_ip + "/");
      } else {
        request->send(404, "text/plain", "Not Found");
      }
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  ws.onEvent(onWsEvent);
  ws_telemetry.onEvent(onWsEvent);
  ws_shell.onEvent(onWsShellEvent);
  ws_shell_v2.onEvent(onWsShellEvent);
  ws_debug.onEvent(onWsDebugEvent);
  ws_mcp.onEvent(onWsMcpEvent);
  server.addHandler(&ws);
  server.addHandler(&ws_telemetry);
  server.addHandler(&ws_shell);
  server.addHandler(&ws_shell_v2);
  server.addHandler(&ws_debug);
  server.addHandler(&ws_mcp);

  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    send_fs_asset(request, "/css/style.css", "text/css");
  });

  server.on("/kinematics3d.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    send_fs_asset(request, "/js/scene/robot-scene.js", "application/javascript");
  });

  server.on("/main.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    send_fs_asset(request, "/js/bootstrap/app-entry.js", "application/javascript");
  });

  server.on(AsyncURIMatcher::prefix("/css"), HTTP_GET,
            [](AsyncWebServerRequest *request) {
              send_large_asset_psram_stream(request, "/css", "/css");
            });
  server.on(AsyncURIMatcher::prefix("/js"), HTTP_GET,
            [](AsyncWebServerRequest *request) {
              send_large_asset_psram_stream(request, "/js", "/js");
            });
  server.on(AsyncURIMatcher::prefix("/assets"), HTTP_GET,
            [](AsyncWebServerRequest *request) {
              send_large_asset_psram_stream(request, "/assets", "/assets");
            });
  server.on(AsyncURIMatcher::prefix("/cad"), HTTP_GET,
            [](AsyncWebServerRequest *request) {
              send_large_asset_psram_stream(request, "/cad", "/assets/cad");
            });
  server.on(AsyncURIMatcher::prefix("/materials"), HTTP_GET,
            [](AsyncWebServerRequest *request) {
              send_large_asset_psram_stream(request, "/materials", "/materials");
            });
  server.on(AsyncURIMatcher::prefix("/robot-data"), HTTP_GET,
            [](AsyncWebServerRequest *request) {
              send_large_asset_psram_stream(request, "/robot-data", "/robot");
            });

  server.on("/api/cad/version", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401);
      return;
    }
    String json = "{\"success\":true,\"version\":\"" + get_cad_asset_version_tag() + "\"}";
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    request->send(response);
  });

  server.on("/api/cad/manifest", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json",
                    "{\"success\":false,\"error\":\"UNAUTHORIZED\",\"message\":\"Authentication required\"}");
      return;
    }
    auto state = std::make_shared<ManifestStreamState>();
    AsyncWebServerResponse *response =
        request->beginChunkedResponse(
            "application/json",
            [state](uint8_t* buffer, size_t max_len, size_t) -> size_t {
              return stream_cad_manifest_chunk(state.get(), buffer, max_len);
            });
    if (response == nullptr) {
      return request->send(500, "application/json",
                           "{\"success\":false,\"error\":\"STREAM_ALLOC_FAILED\"}");
    }
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    request->send(response);
  });

  server.on("/api/assets/manifest", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json",
                    "{\"success\":false,\"error\":\"UNAUTHORIZED\",\"message\":\"Authentication required\"}");
      return;
    }
    auto state = std::make_shared<ManifestStreamState>();
    AsyncWebServerResponse *response =
        request->beginChunkedResponse(
            "application/json",
            [state](uint8_t* buffer, size_t max_len, size_t) -> size_t {
              return stream_asset_catalog_manifest_chunk(state.get(), buffer, max_len);
            });
    if (response == nullptr) {
      return request->send(500, "application/json",
                           "{\"success\":false,\"error\":\"STREAM_ALLOC_FAILED\"}");
    }
    add_no_cache_headers(response);
    request->send(response);
  });

  server.on("/api/materials/manifest", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json",
                    "{\"success\":false,\"error\":\"UNAUTHORIZED\",\"message\":\"Authentication required\"}");
      return;
    }
    send_fs_asset(request, "/materials/materials_manifest.json", "application/json");
  });

  server.on("/api/robot/mechanics/manifest", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json",
                    "{\"success\":false,\"error\":\"UNAUTHORIZED\",\"message\":\"Authentication required\"}");
      return;
    }
    send_fs_asset(request, "/robot/mechanics_manifest.json", "application/json");
  });

  server.on("/api/about", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401);
      return;
    }
    ws_auth_lock();
    const uint32_t ws_auth_count = ws_authenticated_count_locked(ws_auth_clients);
    ws_auth_unlock();
    const uint32_t ws_open_count = ws.count();
    esp_chip_info_t chip_info {};
    esp_chip_info(&chip_info);
    uint32_t flash_size = 0U;
    (void)esp_flash_get_size(nullptr, &flash_size);
    const esp_partition_t *running_part = esp_ota_get_running_partition();
    const esp_partition_t *app0_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "app0");
    const esp_partition_t *recovery_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "recovery");
    const esp_partition_t *littlefs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "littlefs");
    String chip_model = "ESP32";
    switch (chip_info.model) {
#if defined(CHIP_ESP32S3)
      case CHIP_ESP32S3:
        chip_model = "ESP32-S3";
        break;
#endif
#if defined(CHIP_ESP32C3)
      case CHIP_ESP32C3:
        chip_model = "ESP32-C3";
        break;
#endif
#if defined(CHIP_ESP32C6)
      case CHIP_ESP32C6:
        chip_model = "ESP32-C6";
        break;
#endif
#if defined(CHIP_ESP32P4)
      case CHIP_ESP32P4:
        chip_model = "ESP32-t41";
        break;
#endif
      default:
        chip_model = "ESP32";
        break;
    }
    String json = "{";
    json += "\"build_date\":\"" + String(__DATE__) + "\",";
    json += "\"build_time\":\"" + String(__TIME__) + "\",";
    json += "\"version\":\"" + String(k_system_version) + "\",";
    json += "\"system_name\":\"" + String(k_system_name) + "\",";
    json += "\"github_url\":\"" + String(k_system_github_url) + "\",";
    json += "\"developer\":\"" + String(k_system_developer) + "\",";
    json += "\"idf_version\":\"" + jsonEscape(String(esp_get_idf_version())) + "\",";
    json += "\"target\":\"" + jsonEscape(String(CONFIG_IDF_TARGET)) + "\",";
    json += "\"chip_model\":\"" + jsonEscape(chip_model) + "\",";
    json += "\"chip_revision\":" + String(chip_info.revision) + ",";
    json += "\"chip_cores\":" + String(chip_info.cores) + ",";
    json += "\"flash_size\":" + String(flash_size) + ",";
    json += "\"flash_mode\":\"" + String(CONFIG_ESPTOOLPY_FLASHMODE) + "\",";
    json += "\"flash_freq\":\"" + String(config_flash_freq_mhz()) + "m\",";
    json += "\"flash_freq_raw\":\"" + String(CONFIG_ESPTOOLPY_FLASHFREQ) + "\",";
    json += "\"flash_freq_mhz\":" + String(config_flash_freq_mhz()) + ",";
    json += "\"psram_freq_mhz\":" + String(config_psram_freq_mhz()) + ",";
    json += "\"flash_config_size\":\"" + String(CONFIG_ESPTOOLPY_FLASHSIZE) + "\",";
    json += "\"partition_table\":\"" + String(CONFIG_PARTITION_TABLE_FILENAME) + "\",";
    json += "\"partition_offset\":" + String(CONFIG_PARTITION_TABLE_OFFSET) + ",";
    json += "\"freertos_cores\":" + String(CONFIG_FREERTOS_NUMBER_OF_CORES) + ",";
    json += "\"freertos_hz\":" + String(CONFIG_FREERTOS_HZ) + ",";
    json += "\"compiler_opt\":\""
#if defined(CONFIG_COMPILER_OPTIMIZATION_SIZE) && CONFIG_COMPILER_OPTIMIZATION_SIZE
            "size"
#elif defined(CONFIG_COMPILER_OPTIMIZATION_PERF) && CONFIG_COMPILER_OPTIMIZATION_PERF
            "performance"
#elif defined(CONFIG_COMPILER_OPTIMIZATION_DEBUG) && CONFIG_COMPILER_OPTIMIZATION_DEBUG
            "debug"
#else
            "default"
#endif
            "\",";
    json += "\"panic_mode\":\""
#if defined(CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT) && CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT
            "print_reboot"
#elif defined(CONFIG_ESP_SYSTEM_PANIC_GDBSTUB) && CONFIG_ESP_SYSTEM_PANIC_GDBSTUB
            "gdbstub"
#elif defined(CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT) && CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT
            "silent_reboot"
#else
            "default"
#endif
            "\",";
    json += "\"running_partition\":\"" + jsonEscape(String(running_part ? running_part->label : "--")) + "\",";
    json += "\"running_offset\":" + String(running_part ? running_part->address : 0U) + ",";
    json += "\"app0_offset\":" + String(app0_part ? app0_part->address : 0U) + ",";
    json += "\"app0_size\":" + String(app0_part ? app0_part->size : 0U) + ",";
    json += "\"recovery_offset\":" + String(recovery_part ? recovery_part->address : 0U) + ",";
    json += "\"recovery_size\":" + String(recovery_part ? recovery_part->size : 0U) + ",";
    json += "\"littlefs_offset\":" + String(littlefs_part ? littlefs_part->address : 0U) + ",";
    json += "\"littlefs_size\":" + String(littlefs_part ? littlefs_part->size : 0U) + ",";
    json += "\"reset_reason\":" + String(static_cast<int>(esp_reset_reason())) + ",";
    json += "\"ws_open_count\":" + String(ws_open_count) + ",";
    json += "\"ws_auth_count\":" + String(ws_auth_count);
    json += "}";
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    request->send(response);
  });

  server.on("/api/system/reboot-recovery", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json",
                    "{\"success\":false,\"error\":\"UNAUTHORIZED\",\"message\":\"Authentication required\"}");
      return;
    }

    const esp_partition_t *recovery_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "recovery");
    if (recovery_part == nullptr) {
      request->send(404, "application/json",
                    "{\"success\":false,\"error\":\"RECOVERY_MISSING\",\"message\":\"Recovery partition not found\"}");
      return;
    }

    esp_app_desc_t recovery_desc {};
    if (esp_ota_get_partition_description(recovery_part, &recovery_desc) != ESP_OK) {
      request->send(409, "application/json",
                    "{\"success\":false,\"error\":\"RECOVERY_INVALID\",\"message\":\"Recovery image is missing or invalid\"}");
      return;
    }

    const esp_err_t err = esp_ota_set_boot_partition(recovery_part);
    if (err != ESP_OK) {
      String json = "{\"success\":false,\"error\":\"BOOT_SELECT_FAILED\",\"message\":\"";
      json += jsonEscape(String(esp_err_to_name(err)));
      json += "\"}";
      request->send(500, "application/json", json);
      return;
    }

    AsyncWebServerResponse *response =
        request->beginResponse(202, "application/json",
                               "{\"success\":true,\"status\":\"rebooting\",\"target\":\"recovery\"}");
    add_no_cache_headers(response);
    request->send(response);

    xTaskCreate(
        [](void *) {
          vTaskDelay(pdMS_TO_TICKS(450));
          mros::platform::mros_system_restart();
          vTaskDelete(nullptr);
        },
        "web_recovery_reboot", 2048, nullptr, 3, nullptr);
  });

  server.on("/api/settings/popup", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401);
      return;
    }
    refresh_current_username_from_credentials();
    String json = read_user_popup_settings_json();
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    request->send(response);
  });

  server.on("/api/settings/schema", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401);
      return;
    }
    AsyncWebServerResponse *response =
        request->beginResponse(200, "application/json", device_settings_schema_json());
    add_no_cache_headers(response);
    request->send(response);
  });

  server.on("/api/settings/device", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401);
      return;
    }
    const String json = read_device_settings_json();
    AsyncWebServerResponse *response =
        request->beginResponse(200, "application/json", json);
    add_no_cache_headers(response);
    request->send(response);
  });

  server.on("/api/profile", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401);
      return;
    }
    refresh_current_username_from_credentials();
    const String locale = mros::profile::locale();
    char buffer[256] = {};
    mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
    writer.begin();
    writer.bool_field("success", true);
    writer.string_field("user", auth_current_username_copy().c_str());
    writer.string_field("locale", locale.c_str());
    writer.string_field("locale_name", mros::profile::display_locale_name(locale).c_str());
    writer.end();
    AsyncWebServerResponse *response =
        request->beginResponse(200, "application/json",
                               writer.overflow() ? "{\"success\":false,\"error\":\"json_overflow\"}" : writer.c_str());
    add_no_cache_headers(response);
    request->send(response);
  });

  server.on("/api/profile", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401);
      return;
    }
    if (!request->hasParam("locale", true)) {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"locale gerekli\"}");
      return;
    }
    const String locale = request->getParam("locale", true)->value();
    if (!mros::profile::set_locale(locale)) {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"unsupported locale\"}");
      return;
    }
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on(
      "/api/settings/popup", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        // Body callback handles payload.
      },
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!isAuthenticated(request)) {
          request->send(401);
          return;
        }
        static char *json_buffer = nullptr;
        static size_t json_len = 0;
        
        if (index == 0) {
          if (json_buffer) {
             heap_caps_free(json_buffer);
             json_buffer = nullptr;
          }
          json_buffer = static_cast<char *>(alloc_tolerant_buffer(24577));
          json_len = 0;
        }
        
        if ((index + len) > 24576 || !json_buffer) {
          request->send(413, "application/json",
                        "{\"success\":false,\"error\":\"settings payload too large or OOM\"}");
          if (json_buffer) { heap_caps_free(json_buffer); json_buffer = nullptr; }
          return;
        }
        
        memcpy(json_buffer + json_len, data, len);
        json_len += len;
        
        if (index + len != total) return;
        
        json_buffer[json_len] = '\0';
        String payload_str = String(json_buffer);
        heap_caps_free(json_buffer);
        json_buffer = nullptr;
        
        refresh_current_username_from_credentials();
        const bool ok = write_user_popup_settings_json(payload_str);
        if (!ok) {
          request->send(400, "application/json",
                        "{\"success\":false,\"error\":\"invalid or failed settings write\"}");
          return;
        }
        request->send(200, "application/json", "{\"success\":true}");
      });

  server.on(
      "/api/settings/device", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        // Body callback handles payload.
      },
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!isAuthenticated(request)) {
          request->send(401);
          return;
        }
        static char *json_buffer = nullptr;
        static size_t json_len = 0U;
        if (index == 0U) {
          if (json_buffer != nullptr) {
            heap_caps_free(json_buffer);
            json_buffer = nullptr;
          }
          json_buffer = static_cast<char *>(alloc_tolerant_buffer(kDeviceSettingsPayloadMax + 1U));
          json_len = 0U;
        }
        if ((index + len) > kDeviceSettingsPayloadMax || json_buffer == nullptr) {
          request->send(413, "application/json",
                        "{\"success\":false,\"error\":\"device_settings_too_large\"}");
          if (json_buffer != nullptr) {
            heap_caps_free(json_buffer);
            json_buffer = nullptr;
          }
          return;
        }
        std::memcpy(json_buffer + json_len, data, len);
        json_len += len;
        if (index + len != total) return;

        json_buffer[json_len] = '\0';
        const String payload(json_buffer);
        heap_caps_free(json_buffer);
        json_buffer = nullptr;

        const char* partition = nullptr;
        String error;
        if (!write_device_settings_json(payload, &partition, &error)) {
          PsramJsonWriter out(256U);
          out.begin();
          out.bool_field("success", false);
          out.string_field("error", error.length() > 0 ? error.c_str() : "device_settings_write_failed");
          out.end();
          request->send(400, "application/json", out.c_str());
          return;
        }
        PsramJsonWriter out(256U);
        out.begin();
        out.bool_field("success", true);
        out.string_field("partition", partition != nullptr ? partition : "unknown");
        out.string_field("namespace", kDeviceSettingsNamespace);
        out.string_field("key", kDeviceSettingsKey);
        out.end();
        request->send(200, "application/json", out.c_str());
      });

  server.on("/api/services/state", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401);
      return;
    }
    const mros::ssh::IdentityConfig ssh_cfg = mros::ssh::identity_get();
    String json = "{\"success\":true,\"ssh\":{";
    json += "\"enabled\":";
    json += ssh_cfg.enabled ? "true" : "false";
    json += ",\"port\":";
    json += String(static_cast<unsigned>(ssh_cfg.port));
    json += ",\"backend\":\"";
    json += jsonEscape(String(mros::ssh::backend_name()));
    json += "\",\"status\":\"";
    json += jsonEscape(mros::ssh::service_status_text());
    json += "\"},\"mcp\":";
    json += mros::mcp::service_status_json();
    json += "}";
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    add_no_cache_headers(response);
    request->send(response);
  });

  server.on("/api/services/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401);
      return;
    }
    if (!request_require_capability(request, mros::shell::ShellCapabilityDebug, "SERVICE_CAPABILITY_REQUIRED")) {
      return;
    }
    if (!request->hasParam("service", true)) {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"service gerekli\"}");
      return;
    }
    String service = request->getParam("service", true)->value();
    service.trim();
    service.toLowerCase();
    bool enabled = false;
    if (request->hasParam("enabled", true)) {
      String value = request->getParam("enabled", true)->value();
      value.trim();
      value.toLowerCase();
      enabled = value == "1" || value == "true" || value == "on" || value == "yes";
    }
    bool ok = false;
    if (service == "ssh") {
      ok = enabled ? mros::ssh::service_enable() : mros::ssh::service_disable();
    } else if (service == "mcp") {
      ok = mros::mcp::service_set_enabled(enabled);
      if (request->hasParam("allow_shell", true)) {
        String value = request->getParam("allow_shell", true)->value();
        value.trim();
        value.toLowerCase();
        ok = mros::mcp::service_set_allow_shell(
                 value == "1" || value == "true" || value == "on" || value == "yes") &&
             ok;
      }
    } else {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"bilinmeyen servis\"}");
      return;
    }
    request->send(ok ? 200 : 500, "application/json",
                  ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"servis kaydedilemedi\"}");
  });

  server.on("/mcp", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json",
                    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32001,\"message\":\"Authentication required\"}}");
      return;
    }
    if (!web_current_user_has_capability(mros::shell::ShellCapabilityDebug)) {
      request->send(403, "application/json",
                    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32003,\"message\":\"MCP requires debug capability\"}}");
      web_security_audit("mcp-denied", auth_current_username_copy());
      return;
    }
    if (!mros::mcp::service_is_enabled()) {
      request->send(503, "application/json",
                    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32002,\"message\":\"MCP service is disabled\"}}");
      return;
    }
    request->send(405, "application/json",
                  "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32000,\"message\":\"SSE stream is not enabled on-device; use POST /mcp or WebSocket /ws/mcp\"}}");
  });

  server.on(
      "/mcp", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        // Body callback handles the JSON-RPC payload.
      },
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!isAuthenticated(request)) {
          request->send(401, "application/json",
                        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32001,\"message\":\"Authentication required\"}}");
          return;
        }
        const String username = web_current_username_or_refresh();
        const uint32_t capabilities = web_capability_mask_for_username(username);
        if ((capabilities & mros::shell::ShellCapabilityDebug) != mros::shell::ShellCapabilityDebug) {
          request->send(403, "application/json",
                        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32003,\"message\":\"MCP requires debug capability\"}}");
          web_security_audit("mcp-denied", username);
          return;
        }
        if (!mros::mcp::service_is_enabled()) {
          request->send(503, "application/json",
                        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32002,\"message\":\"MCP service is disabled\"}}");
          return;
        }
        static char *json_buffer = nullptr;
        static size_t json_len = 0U;
        if (index == 0U) {
          if (json_buffer != nullptr) {
            heap_caps_free(json_buffer);
            json_buffer = nullptr;
          }
          json_buffer = static_cast<char *>(alloc_tolerant_buffer(8193U));
          json_len = 0U;
        }
        if ((index + len) > 8192U || json_buffer == nullptr) {
          request->send(413, "application/json",
                        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32000,\"message\":\"MCP payload too large or OOM\"}}");
          if (json_buffer != nullptr) {
            heap_caps_free(json_buffer);
            json_buffer = nullptr;
          }
          return;
        }
        memcpy(json_buffer + json_len, data, len);
        json_len += len;
        if (index + len != total) return;

        json_buffer[json_len] = '\0';
        const String payload(json_buffer);
        heap_caps_free(json_buffer);
        json_buffer = nullptr;
        pm_mark_web_feedback();
        mros::mcp::service_mark_activity();
        const String reply = mcp_handle_json_rpc(payload, mros::mcp::service_allow_shell(), username, capabilities);
        if (reply.length() == 0) {
          request->send(202, "application/json", "");
          return;
        }
        AsyncWebServerResponse *response =
            request->beginResponse(200, "application/json", reply);
        response->addHeader("MCP-Protocol-Version", "2025-06-18");
        add_no_cache_headers(response);
        request->send(response);
      });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->redirect("/login");
      return;
    }
    send_fs_asset(request, "/index.html", "text/html");
  });

  server.on("/login", HTTP_GET, [](AsyncWebServerRequest *request) {
    send_fs_asset(request, "/login.html", "text/html");
  });

  server.on("/setup", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->redirect("/login");
      return;
    }
    send_fs_asset(request, "/setup.html", "text/html");
  });

  server.on("/debug", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->redirect("/login");
      return;
    }
    send_fs_asset(request, "/debug.html", "text/html");
  });

  ESP_LOGI("WEB", "Routes registered: core pages");

  server.on("/api/debug/sysinfo", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401);
      return;
    }
    const uint32_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t psram_total = mros::platform::mros_system_psram_total();
    const uint32_t psram_free = mros::platform::mros_system_psram_free();
    const esp_partition_t *app_part = esp_ota_get_running_partition();
    if (app_part == nullptr || strcmp(app_part->label, "recovery") == 0) {
      app_part = esp_partition_find_first(
          ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "app0");
    }
    const uint32_t app_used = mros::platform::mros_system_app_image_size();
    const uint32_t app_total = app_part ? app_part->size : 0;
    const uint32_t app_free = (app_total > app_used) ? (app_total - app_used) : 0;

    const esp_partition_t *recovery_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "recovery");
    uint32_t recovery_used = 0;
    uint32_t recovery_total = recovery_part ? recovery_part->size : 0;
    if (recovery_part != nullptr) {
      esp_partition_pos_t position = {};
      position.offset = recovery_part->address;
      position.size = recovery_part->size;
      esp_image_metadata_t metadata = {};
      if (esp_image_get_metadata(&position, &metadata) == ESP_OK) {
        recovery_used = metadata.image_len;
      }
    }

    uint64_t fs_total_64 = 0U;
    uint64_t fs_used_64 = 0U;
    (void)logger_storage_info(&fs_total_64, &fs_used_64);
    const uint32_t fs_total = static_cast<uint32_t>(fs_total_64);
    const uint32_t fs_used = static_cast<uint32_t>(fs_used_64);
    const uint16_t t41_loop_ms_ms = spi_s3_get_loop_ms();
    const float ctrl_actual_hz = t41_loop_ms_ms > 0 ? (1000.0f / (float)t41_loop_ms_ms) : 0.0f;
    const float ctrl_target_hz = 100.0f;
    const float ctrl_diff_hz = ctrl_actual_hz - ctrl_target_hz;
    const uint32_t c3_loop_hz = (uint32_t)spi_c3_get_loop_hz();
    const int32_t s3_devstat = (int32_t)spi_s3_get_device_status_code();

    if (t41_loop_ms_ms > 0) {
      if (dbg_prev_loop_ms == 0) {
        dbg_prev_loop_ms = t41_loop_ms_ms;
      }
      uint16_t diff_ms = t41_loop_ms_ms > dbg_prev_loop_ms
                             ? (uint16_t)(t41_loop_ms_ms - dbg_prev_loop_ms)
                             : (uint16_t)(dbg_prev_loop_ms - t41_loop_ms_ms);
      dbg_loop_jitter_ms = (dbg_loop_jitter_ms * 0.8f) + ((float)diff_ms * 0.2f);
      dbg_prev_loop_ms = t41_loop_ms_ms;
    }

    const unsigned long now_ms = mros::platform::mros_millis();
    const bool task_pending =
        (dbg_last_motion_cmd_ms > 0) && ((now_ms - dbg_last_motion_cmd_ms) < 1500UL);
    uint32_t task_eta_ms = 0;
    if (task_pending) {
      task_eta_ms = t41_loop_ms_ms > 0 ? ((uint32_t)t41_loop_ms_ms * 2U) : 120U;
      if (task_eta_ms < 80U)
        task_eta_ms = 80U;
    }

    const uint8_t safety_estop = (spi_s3_get_motor_state() == 0) ? 1 : 0;
    const int8_t safety_limit = -1;
    const int8_t safety_singularity = -1;
    const uint8_t safety_collision = (spi_s3_get_error_code() != 0) ? 1 : 0;

    const uint64_t fs_read_t0 = mros::platform::mros_micros();
    const String config_probe_path = logger_user_path("config.json");
    (void)mros::platform::mros_fs_exists(config_probe_path.c_str());
    const uint32_t fs_read_us =
        static_cast<uint32_t>(mros::platform::mros_micros() - fs_read_t0);
    const int32_t fs_write_us = -1;
    const float storage_wear_pct = -1.0f;
    const String storage_last_error =
        logger_storage_ready() ? "Yok" : "LittleFS hazir degil";

    float pid_cycle_avg_ms = 0.0f;
    uint32_t pid_cycle_last_ms = 0;
    uint32_t pid_exec_last_ms = 0;
    uint32_t pid_cycle_peak_ms = 0;
    uint32_t cpu_freq_core0_mhz = PM_FREQ_BASE_MHZ;
    uint32_t cpu_freq_core1_mhz = PM_FREQ_BASE_MHZ;
    uint32_t cpu_freq_target_mhz = PM_FREQ_BASE_MHZ;
    uint32_t cpu_freq_applied_mhz = 0;
    unsigned long last_web_feedback_ms = 0;
    portENTER_CRITICAL(&pm_scale_mux);
    pid_cycle_avg_ms = pm_pid_cycle_avg_ms;
    pid_cycle_last_ms = pm_pid_cycle_last_ms;
    pid_exec_last_ms = pm_pid_exec_last_ms;
    pid_cycle_peak_ms = pm_pid_cycle_peak_ms;
    cpu_freq_core0_mhz = pm_core0_target_mhz;
    cpu_freq_core1_mhz = pm_core1_target_mhz;
    cpu_freq_target_mhz = pm_system_target_mhz;
    cpu_freq_applied_mhz = pm_system_applied_mhz;
    last_web_feedback_ms = pm_last_web_feedback_ms;
    portEXIT_CRITICAL(&pm_scale_mux);
    if (cpu_freq_applied_mhz == 0) {
      cpu_freq_applied_mhz = mros::platform::mros_system_cpu_freq_mhz();
    }
    mros::power::Status power_status {};
    mros::power::get_status(&power_status);
    mros::rtos::dpm::PolicyDecision dpm_decision {};
    mros::rtos::dpm::get_policy_decision(&dpm_decision);
    cpu_freq_core0_mhz = power_status.net_demand_mhz;
    cpu_freq_core1_mhz = power_status.rt_demand_mhz;
    cpu_freq_target_mhz = power_status.target_mhz;
    cpu_freq_applied_mhz = power_status.actual_cpu_mhz;
    const unsigned long web_feedback_age_ms =
        (last_web_feedback_ms == 0) ? 0xFFFFFFFFUL
                                    : (now_ms - last_web_feedback_ms);

    const float cpu_load_pct = -1.0f;
    const uint32_t cpu_freq_mhz = cpu_freq_applied_mhz;
    const float cpu_temp_c =
        power_status.temperature_valid ? power_status.temperature_c : -1.0f;

    const float power_vin_v = -1.0f;
    const float power_current_ma = -1.0f;
    const uint8_t power_brownout_boot =
        (esp_reset_reason() == ESP_RST_BROWNOUT) ? 1 : 0;

    WifiManagerSnapshot wifi_snapshot = {};
    wifi_manager_get_snapshot(&wifi_snapshot);
    MrosRtosAggregateSnapshot rtos_snapshot {};
    app_rtos_get_aggregate_diag(&rtos_snapshot);
    mros::shell::service::ShellServiceMetrics shell_metrics {};
    mros::shell::service::get_metrics(&shell_metrics);
    LoggerDiagSnapshot storage_diag {};
    logger_get_diag_snapshot(&storage_diag);
    const uint32_t json_overflow_total =
        g_json_overflow_count + mros::utils::json_overflow_count();

    String json = "{";
    json += "\"rssi\":" + String(wifi_snapshot.state.rssi) + ",";
    json += "\"free_heap\":" + String(mros::platform::mros_system_heap_free()) + ",";
    json += "\"min_free_heap\":" + String(mros::platform::mros_system_heap_min_free()) + ",";
    json += "\"internal_total\":" + String(internal_total) + ",";
    json += "\"internal_free\":" + String(internal_free) + ",";
    json += "\"psram_total\":" + String(psram_total) + ",";
    json += "\"psram_free\":" + String(psram_free) + ",";
    json += "\"app_used\":" + String(app_used) + ",";
    json += "\"app_total\":" + String(app_total) + ",";
    json += "\"recovery_used\":" + String(recovery_used) + ",";
    json += "\"recovery_total\":" + String(recovery_total) + ",";
    json += "\"app_free\":" + String(app_free) + ",";
    json += "\"fs_used\":" + String(fs_used) + ",";
    json += "\"fs_total\":" + String(fs_total) + ",";
    json += "\"cpu_load_pct\":" + String(cpu_load_pct, 1) + ",";
    json += "\"cpu_freq_mhz\":" + String(cpu_freq_mhz) + ",";
    json += "\"cpu_freq_target_mhz\":" + String(cpu_freq_target_mhz) + ",";
    json += "\"cpu_freq_applied_mhz\":" + String(cpu_freq_applied_mhz) + ",";
    json += "\"cpu_freq_core0_mhz\":" + String(cpu_freq_core0_mhz) + ",";
    json += "\"cpu_freq_core1_mhz\":" + String(cpu_freq_core1_mhz) + ",";
    json += "\"actual_cpu_mhz\":" + String(power_status.actual_cpu_mhz) + ",";
    json += "\"net_demand_mhz\":" + String(power_status.net_demand_mhz) + ",";
    json += "\"rt_demand_mhz\":" + String(power_status.rt_demand_mhz) + ",";
    json += "\"pm_mode\":\"" + jsonEscape(String(power_status.mode)) + "\",";
    json += "\"wifi_ps_mode\":\"" + jsonEscape(String(power_status.wifi_ps_mode)) + "\",";
    json += "\"light_sleep_enabled\":" + String(power_status.light_sleep_enabled ? "true" : "false") + ",";
    json += "\"light_sleep_blockers\":\"" + jsonEscape(String(power_status.active_locks)) + "\",";
    json += "\"active_pm_locks\":\"" + jsonEscape(String(power_status.active_locks)) + "\",";
    json += "\"boost_reason\":\"" + jsonEscape(String(power_status.boost_reason)) + "\",";
    json += "\"temperature_c\":" + String(cpu_temp_c, 1) + ",";
    json += "\"internal_largest_block\":" + String(power_status.internal_largest_block) + ",";
    json += "\"lazy_tasks_saved_bytes\":" + String(power_status.lazy_tasks_saved_bytes) + ",";
    json += "\"web_feedback_age_ms\":" + String(web_feedback_age_ms) + ",";
    json += "\"cpu_temp_c\":" + String(cpu_temp_c, 1) + ",";
    json += "\"pid_cycle_last_ms\":" + String(pid_cycle_last_ms) + ",";
    json += "\"pid_cycle_avg_ms\":" + String(pid_cycle_avg_ms, 2) + ",";
    json += "\"pid_exec_last_ms\":" + String(pid_exec_last_ms) + ",";
    json += "\"pid_cycle_peak_ms\":" + String(pid_cycle_peak_ms) + ",";
    json += "\"fk_last_ms\":" + String(dbg_fk_last_ms, 3) + ",";
    json += "\"fk_avg_ms\":" + String(dbg_fk_avg_ms, 3) + ",";
    json += "\"fk_max_ms\":" + String(dbg_fk_max_ms, 3) + ",";
    json += "\"ik_last_ms\":" + String(t41_loop_ms_ms) + ",";
    json += "\"ctrl_actual_hz\":" + String(ctrl_actual_hz, 1) + ",";
    json += "\"ctrl_target_hz\":" + String(ctrl_target_hz, 1) + ",";
    json += "\"ctrl_diff_hz\":" + String(ctrl_diff_hz, 1) + ",";
    json += "\"c3_loop_hz\":" + String(c3_loop_hz) + ",";
    json += "\"s3_devstat\":" + String(s3_devstat) + ",";
    json += "\"ctrl_jitter_ms\":" + String(dbg_loop_jitter_ms, 2) + ",";
    json += "\"json_overflow\":" + String(json_overflow_total) + ",";
    json += "\"shell_pool_miss\":" + String(shell_metrics.response_pool_miss) + ",";
    json += "\"shell_drop\":" + String(shell_metrics.response_drop) + ",";
    json += "\"shell_cbor_frames\":" + String(g_shell_cbor_control_frames) + ",";
    json += "\"shell_cbor_bytes\":" + String(g_shell_cbor_control_bytes) + ",";
    json += "\"shell_cbor_errors\":" + String(g_shell_cbor_control_errors) + ",";
    json += "\"native_http_port\":" + String(kNativeHttpPrimaryPort) + ",";
    json += "\"native_http_mode\":\"primary\",";
    json += "\"native_http_engine\":\"" + String(kNativeHttpPrimaryEngine) + "\",";
    json += "\"rtos_deadline_miss\":" + String(rtos_snapshot.total_slip_count) + ",";
    json += "\"rtos_max_slip_ms\":" + String(rtos_snapshot.max_slip_ms) + ",";
    json += "\"wifi_reconnect_ms\":" + String(wifi_snapshot.last_connect_duration_ms) + ",";
    json += "\"wifi_fast_path_attempts\":" + String(wifi_snapshot.fast_path_attempts) + ",";
    json += "\"wifi_fast_path_successes\":" + String(wifi_snapshot.fast_path_successes) + ",";
    json += "\"task_queue_pending\":" + String(task_pending ? 1 : 0) + ",";
    json += "\"task_active\":\"" + jsonEscape(task_pending ? dbg_last_motion_cmd : String("Beklemede")) + "\",";
    json += "\"task_eta_ms\":" + String(task_eta_ms) + ",";
    json += "\"safety_estop\":" + String(safety_estop) + ",";
    json += "\"safety_limit\":" + String(safety_limit) + ",";
    json += "\"safety_singularity\":" + String(safety_singularity) + ",";
    json += "\"safety_collision\":" + String(safety_collision) + ",";
    json += "\"safety_error_code\":" + String(spi_s3_get_error_code()) + ",";
    json += "\"storage_wear_pct\":" + String(storage_wear_pct, 1) + ",";
    json += "\"storage_last_error\":\"" + jsonEscape(storage_last_error) + "\",";
    json += "\"storage_migration_attempted\":" +
            String(storage_diag.migration_attempted ? "true" : "false") + ",";
    json += "\"storage_migration_source\":\"" +
            jsonEscape(String(storage_diag.migration_source)) + "\",";
    json += "\"storage_migration_migrated\":" +
            String(storage_diag.migration_migrated ? "true" : "false") + ",";
    json += "\"storage_migration_error\":\"" +
            jsonEscape(String(storage_diag.migration_error)) + "\",";
    json += "\"storage_fs_read_us\":" + String(fs_read_us) + ",";
    json += "\"storage_fs_write_us\":" + String(fs_write_us) + ",";
    json += "\"power_vin_v\":" + String(power_vin_v, 2) + ",";
    json += "\"power_current_ma\":" + String(power_current_ma, 1) + ",";
    json += "\"power_brownout_boot\":" + String(power_brownout_boot) + ",";
    json += "\"traj_psram_count\":" + String((uint32_t)psram_traj_count) + ",";
    json += "\"traj_psram_capacity\":" + String((uint32_t)psram_traj_capacity) + ",";
    json += "\"preview_psram_count\":" + String((uint32_t)psram_preview_count) + ",";
    json += "\"preview_psram_capacity\":" + String((uint32_t)psram_preview_capacity) + ",";
    json += "\"uptime_s\":" + String(mros::platform::mros_millis() / 1000U);
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/health", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json",
                    "{\"success\":false,\"error\":\"UNAUTHORIZED\",\"message\":\"Authentication required\"}");
      return;
    }

    LoggerDiagSnapshot storage_diag {};
    logger_get_diag_snapshot(&storage_diag);
    mros::shell::service::ShellServiceMetrics shell_metrics {};
    mros::shell::service::get_metrics(&shell_metrics);
    mros::shell::remote::RemoteTunnelMetrics remote_metrics {};
    mros::shell::remote::get_tunnel_metrics(&remote_metrics);
    WebServerDiagSnapshot web_diag {};
    web_server_get_diag_snapshot(&web_diag);
    PCA9685_DiagSnapshot_t pca_diag {};
    pca9685_get_diag_snapshot(&pca_diag);
    WifiManagerSnapshot wifi_snapshot {};
    wifi_manager_get_snapshot(&wifi_snapshot);
    const AsyncWebDiagnostics async_diag = server.diagnostics();
    mros::power::Status power_status {};
    mros::power::get_status(&power_status);
    mros::rtos::dpm::PolicyDecision dpm_decision {};
    mros::rtos::dpm::get_policy_decision(&dpm_decision);

    uint64_t fs_total = 0U;
    uint64_t fs_used = 0U;
    (void)logger_storage_info(&fs_total, &fs_used);
    const esp_partition_t *app_part = esp_ota_get_running_partition();
    if (app_part == nullptr || strcmp(app_part->label, "recovery") == 0) {
      app_part = esp_partition_find_first(
          ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "app0");
    }
    const uint32_t app_image_size = mros::platform::mros_system_app_image_size();
    const uint32_t app_partition_size = app_part ? app_part->size : 0U;
    const uint32_t app_partition_free =
        (app_partition_size > app_image_size) ? (app_partition_size - app_image_size) : 0U;

    FileDownloadStatus download_status {};
    portENTER_CRITICAL(&g_file_download_mux);
    download_status = g_file_download_status;
    portEXIT_CRITICAL(&g_file_download_mux);

    PsramJsonWriter build_json(256U);
    build_json.begin();
    build_json.u32_field("image_size", app_image_size);
    build_json.u32_field("app_partition_size", app_partition_size);
    build_json.u32_field("app_partition_free", app_partition_free);
    build_json.u32_field("flash_freq_mhz", config_flash_freq_mhz());
    build_json.u32_field("psram_freq_mhz", config_psram_freq_mhz());
    build_json.u32_field("iram_used", 0U);
    build_json.u32_field("diram_used", 0U);
    build_json.string_field("size_budget_source", "tools/extract_size_budget.py");
    build_json.end();

    PsramJsonWriter heap_json(384U);
    heap_json.begin();
    heap_json.u32_field("internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    heap_json.u32_field("internal_total", heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    heap_json.u32_field("internal_min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    heap_json.u32_field("internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    heap_json.u32_field("psram_free", mros::platform::mros_system_psram_free());
    heap_json.u32_field("psram_total", mros::platform::mros_system_psram_total());
    heap_json.u32_field("psram_min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    heap_json.u32_field("psram_largest", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    heap_json.end();

    char coredump_buffer[768] = {};
    char heap_trace_buffer[768] = {};
    (void)mros::debug::coredump_json(coredump_buffer, sizeof(coredump_buffer));
    (void)mros::debug::heap_trace_json(heap_trace_buffer, sizeof(heap_trace_buffer));

    PsramJsonWriter storage_json(1024U);
    storage_json.begin();
    storage_json.bool_field("ready", logger_storage_ready());
    storage_json.u64_field("used", fs_used);
    storage_json.u64_field("total", fs_total);
    storage_json.u32_field("queue_depth", storage_diag.queue_depth);
    storage_json.u32_field("queue_capacity", storage_diag.queue_capacity);
    storage_json.u32_field("queue_high_watermark", storage_diag.queue_high_watermark);
    storage_json.u32_field("drops", storage_diag.drop_count);
    storage_json.u32_field("processed", storage_diag.processed_count);
    storage_json.u32_field("atomic_writes", storage_diag.atomic_write_count);
    storage_json.u32_field("atomic_write_fails", storage_diag.atomic_write_fail_count);
    storage_json.bool_field("last_atomic_ok", storage_diag.last_atomic_write_ok);
    storage_json.string_field("last_atomic_error", storage_diag.last_atomic_write_error);
    storage_json.bool_field("migration_attempted", storage_diag.migration_attempted);
    storage_json.string_field("migration_source", storage_diag.migration_source);
    storage_json.bool_field("migration_migrated", storage_diag.migration_migrated);
    storage_json.string_field("migration_error", storage_diag.migration_error);
    storage_json.end();

    PsramJsonWriter shell_json(1536U);
    shell_json.begin();
    shell_json.u32_field("request_queue_depth", shell_metrics.request_queue_depth);
    shell_json.u32_field("request_queue_capacity", shell_metrics.request_queue_capacity);
    shell_json.u32_field("response_queue_depth", shell_metrics.response_queue_depth);
    shell_json.u32_field("response_queue_capacity", shell_metrics.response_queue_capacity);
    shell_json.u32_field("response_pool_active", shell_metrics.response_pool_active);
    shell_json.u32_field("response_pool_capacity", shell_metrics.response_pool_capacity);
    shell_json.u32_field("response_pool_allocated", shell_metrics.response_pool_allocated);
    shell_json.u32_field("response_pool_miss", shell_metrics.response_pool_miss);
    shell_json.u32_field("response_fallback_alloc", shell_metrics.response_fallback_alloc);
    shell_json.u32_field("response_drop", shell_metrics.response_drop);
    shell_json.u32_field("mshell_job_storage_bytes",
                         mros::shell::runtime::job_storage_bytes());
    shell_json.bool_field("mshell_job_storage_allocated",
                          mros::shell::runtime::job_storage_allocated());
    shell_json.bool_field("mshell_job_storage_psram",
                          mros::shell::runtime::job_storage_uses_psram());
    shell_json.u32_field("stream_chunks", shell_metrics.stream_chunk_count);
    shell_json.u32_field("stream_bytes", shell_metrics.stream_byte_count);
    shell_json.u32_field("stream_drops", shell_metrics.stream_drop_count);
    shell_json.u32_field("final_truncations", shell_metrics.final_truncation_count);
    shell_json.u32_field("shell_stream_final_suppressed", shell_metrics.stream_final_suppressed);
    shell_json.u32_field("max_response_bytes", shell_metrics.max_response_bytes);
    shell_json.u32_field("shell_bin_frames", shell_metrics.shell_bin_frames);
    shell_json.u32_field("shell_bin_bytes", shell_metrics.shell_bin_bytes);
    shell_json.u32_field("shell_json_frames", shell_metrics.shell_json_frames);
    shell_json.u32_field("shell_json_bytes", shell_metrics.shell_json_bytes);
    shell_json.u32_field("shell_bin_decode_errors", shell_metrics.shell_bin_decode_errors);
    shell_json.u32_field("shell_json_fallbacks", shell_metrics.shell_json_fallbacks);
    shell_json.u32_field("shell_ack_rx", shell_metrics.shell_ack_rx);
    shell_json.u32_field("shell_credit_rx", shell_metrics.shell_credit_rx);
    shell_json.u32_field("shell_credit_exhausted", shell_metrics.shell_credit_exhausted);
    shell_json.u32_field("shell_cbor_frames", g_shell_cbor_control_frames);
    shell_json.u32_field("shell_cbor_bytes", g_shell_cbor_control_bytes);
    shell_json.u32_field("shell_cbor_errors", g_shell_cbor_control_errors);
    shell_json.u32_field("remote_bin_frames", remote_metrics.remote_bin_frames);
    shell_json.u32_field("remote_bin_bytes", remote_metrics.remote_bin_bytes);
    shell_json.u32_field("remote_text_frames", remote_metrics.remote_text_frames);
    shell_json.u32_field("remote_text_bytes", remote_metrics.remote_text_bytes);
    shell_json.u32_field("remote_fallbacks", remote_metrics.remote_fallbacks);
    shell_json.u32_field("remote_cobs_decode_errors", remote_metrics.remote_cobs_decode_errors);
    shell_json.u32_field("remote_rx_drops", remote_metrics.remote_rx_drops);
    shell_json.bool_field("remote_cap_msh1", remote_metrics.remote_cap_msh1);
    shell_json.end();

    PsramJsonWriter psram_json(512U);
    psram_json.begin();
    psram_json.u32_field("count", g_psram_lazy_alloc_count);
    psram_json.u32_field("bytes", g_psram_lazy_alloc_bytes);
    psram_json.u32_field("internal_fallback_count", g_psram_internal_fallback_count);
    psram_json.u32_field("internal_fallback_bytes", g_psram_internal_fallback_bytes);
    psram_json.u32_field("fail_count", g_psram_alloc_fail_count);
    psram_json.bool_field("shell_forward", g_shell_forward_lazy_allocated);
    psram_json.bool_field("trajectory_buffers", g_trajectory_buffers_lazy_allocated);
    psram_json.u32_field("trajectory_bytes", g_trajectory_buffers_bytes);
    psram_json.end();

    PsramJsonWriter web_json(2304U);
    web_json.begin();
    web_json.u32_field("ws_total", web_diag.ws_clients_total);
    web_json.u32_field("ws_auth", web_diag.ws_clients_auth);
    web_json.u32_field("ws_shell", web_diag.ws_clients_shell);
    web_json.u32_field("ws_shell_auth", web_diag.ws_clients_shell_auth);
    web_json.u32_field("ws_debug", web_diag.ws_clients_debug);
    web_json.u32_field("active_http_sockets", async_diag.active_http_sockets);
    web_json.u32_field("total_http_errors", async_diag.total_http_errors);
    web_json.u32_field("total_ws_errors", async_diag.total_ws_errors);
    web_json.u32_field("dropped_ws_tx_frames", async_diag.dropped_ws_tx_frames);
    web_json.string_field("telemetry_format_default", "bin-v1");
    web_json.u32_field("telemetry_bin_frames", g_ws_bin_frame_count);
    web_json.u32_field("telemetry_bin_bytes", g_ws_bin_byte_count);
    web_json.u32_field("telemetry_json_fallbacks", g_ws_json_fallback_count);
    web_json.u32_field("telemetry_format_errors", g_ws_format_error_count);
    web_json.u32_field("telemetry_last_bin_bytes", g_ws_last_bin_bytes);
    web_json.string_field("telemetry_gated_fields",
                          g_ws_scene_client_count == 0U ? "scene,fk,trajectory" : "");
    web_json.u32_field("telemetry_scene_clients", g_ws_scene_client_count);
    web_json.u32_field("telemetry_debug_clients", g_ws_debug_client_count);
    web_json.u32_field("telemetry_background_clients", g_ws_background_client_count);
    web_json.u32_field("telemetry_gated_scene_frames", g_ws_gated_scene_frames);
    web_json.u32_field("telemetry_gated_medium_frames", g_ws_gated_medium_frames);
    web_json.u32_field("telemetry_gated_slow_frames", g_ws_gated_slow_frames);
    web_json.string_field("telemetry_memory_budget_mode", g_ws_memory_budget_mode);
    web_json.u32_field("telemetry_budget_free", g_ws_memory_budget_last_free);
    web_json.u32_field("telemetry_budget_largest", g_ws_memory_budget_last_largest);
    web_json.u32_field("telemetry_budget_degrade_frames", g_ws_memory_budget_degrade_frames);
    web_json.u32_field("telemetry_budget_critical_frames", g_ws_memory_budget_critical_frames);
    web_json.u32_field("telemetry_budget_scene_suppressed",
                       g_ws_memory_budget_scene_suppressed);
    web_json.u32_field("telemetry_budget_fast_suppressed",
                       g_ws_memory_budget_fast_suppressed);
    web_json.u32_field("telemetry_budget_medium_suppressed",
                       g_ws_memory_budget_medium_suppressed);
    web_json.u32_field("telemetry_budget_dropped", g_ws_telemetry_budget_dropped);
    web_json.u32_field("telemetry_budget_deferred", g_ws_telemetry_budget_deferred);
    web_json.u32_field("full_required_count", g_ws_full_required_count);
    web_json.u32_field("telemetry_policy_fast_period_ms",
                       dpm_decision.telemetry_fast_period_ms);
    web_json.u32_field("telemetry_policy_medium_period_ms",
                       dpm_decision.telemetry_medium_period_ms);
    web_json.u32_field("telemetry_policy_slow_period_ms",
                       dpm_decision.telemetry_slow_period_ms);
    web_json.raw_field("psram_lazy_alloc", psram_json.c_str());
    web_json.bool_field("native_http_enabled", kNativeHttpPrimaryEnabled);
    web_json.bool_field("native_http_primary", kNativeHttpPrimaryEnabled);
    web_json.string_field("native_http_mode", "primary");
    web_json.string_field("native_http_engine", kNativeHttpPrimaryEngine);
    web_json.u32_field("native_http_port", kNativeHttpPrimaryPort);
    web_json.bool_field("native_http_direct_lab_enabled", kNativeHttpDirectLabEnabled);
    web_json.u32_field("native_http_direct_lab_port", kNativeHttpDirectLabPort);
    web_json.u32_field("native_route_hits", g_native_http_route_hits);
    web_json.u32_field("native_fallback_hits", g_native_http_fallback_hits);
    web_json.u32_field("native_http_errors", g_native_http_errors);
    web_json.u32_field("native_http_send_bytes", g_native_http_send_bytes);
    web_json.u32_field("native_ws_sends", g_native_http_ws_sends);
    web_json.u32_field("native_async_queue_errors", g_native_http_async_queue_errors);
    web_json.u32_field("native_active_sockets", g_native_http_active_sockets);
    web_json.u32_field("native_lru_purge_count", g_native_http_lru_purge_count);
    web_json.bool_field("worker_decode_enabled", true);
    web_json.u32_field("worker_decode_frames", g_worker_decode_frames);
    web_json.u32_field("worker_decode_errors", g_worker_decode_errors);
    web_json.u32_field("worker_decode_fallbacks", g_worker_decode_fallbacks);
    web_json.u32_field("worker_decode_latency_ms", g_worker_decode_latency_ms);
    web_json.u32_field("worker_decode_dropped", g_worker_decode_dropped);
    web_json.u32_field("shell_cbor_frames", g_shell_cbor_control_frames);
    web_json.u32_field("shell_cbor_bytes", g_shell_cbor_control_bytes);
    web_json.u32_field("shell_cbor_errors", g_shell_cbor_control_errors);
    web_json.end();

    PsramJsonWriter pca_json(1024U);
    pca_json.begin();
    pca_json.u32_field("queue_depth", pca_diag.queue_depth);
    pca_json.u32_field("queue_capacity", pca_diag.queue_capacity);
    pca_json.u32_field("queue_high_watermark", pca_diag.queue_high_watermark);
    pca_json.u32_field("enqueue_count", pca_diag.enqueue_count);
    pca_json.u32_field("process_count", pca_diag.process_count);
    pca_json.u32_field("coalesced_count", pca_diag.coalesced_count);
    pca_json.u32_field("drop_oldest_count", pca_diag.drop_oldest_count);
    pca_json.u32_field("drop_count", pca_diag.drop_count);
    pca_json.u32_field("pca_shadow_flushes", pca_diag.shadow_flush_count);
    pca_json.u32_field("pca_shadow_updates", pca_diag.shadow_update_count);
    pca_json.u32_field("pca_shadow_coalesces", pca_diag.shadow_coalesce_count);
    pca_json.bool_field("pca_shadow_valid", pca_diag.shadow_valid);
    pca_json.u32_field("pca_shadow_type", pca_diag.shadow_type);
    pca_json.u32_field("pca_shadow_start_channel", pca_diag.shadow_start_channel);
    pca_json.u32_field("pca_shadow_count", pca_diag.shadow_count);
    pca_json.end();

    PsramJsonWriter wifi_json(512U);
    wifi_json.begin();
    wifi_json.u32_field("wifi_event_rev", wifi_snapshot.event_revision);
    wifi_json.string_field("wifi_snapshot_source", wifi_snapshot.snapshot_source);
    wifi_json.string_field("wifi_dns_backend", wifi_snapshot.dns_backend);
    wifi_json.bool_field("native_event_active", wifi_snapshot.native_event_active);
    wifi_json.bool_field("dns_lwip_active", wifi_snapshot.dns_lwip_active);
    wifi_json.u32_field("dns_queries", wifi_snapshot.dns_queries);
    wifi_json.u32_field("dns_replies", wifi_snapshot.dns_replies);
    wifi_json.u32_field("dns_errors", wifi_snapshot.dns_errors);
    wifi_json.u32_field("state_json_builds", wifi_snapshot.state_json_builds);
    wifi_json.u32_field("state_json_cache_hits", wifi_snapshot.state_json_cache_hits);
    wifi_json.u32_field("runtime_publishes", wifi_snapshot.runtime_publishes);
    wifi_json.end();

    PsramJsonWriter files_json(1024U);
    files_json.begin();
    files_json.u32_field("upload_active", static_cast<uint32_t>(fm_active_upload_count()));
    files_json.u32_field("upload_slots", 2U);
    files_json.bool_field("download_active", download_status.active);
    files_json.bool_field("download_done", download_status.done);
    files_json.bool_field("download_ok", download_status.success);
    files_json.string_field("download_phase", download_status.phase);
    files_json.u32_field("download_progress", download_status.progress_pct);
    files_json.bool_field("download_cancel_requested", download_status.cancel_requested);
    files_json.bool_field("download_temp_active", download_status.temp_active);
    files_json.string_field("download_guard", download_status.guard);
    files_json.string_field("download_sha256", download_status.sha256);
    files_json.u32_field("download_bytes_per_sec", download_status.bytes_per_sec);
    files_json.end();

    PsramJsonWriter runtime_json(512U);
    runtime_json.begin();
    runtime_json.string_field("version", web_server_system_version());
    runtime_json.string_field("idf_version", esp_get_idf_version());
    runtime_json.i32_field("reset_reason", static_cast<int32_t>(esp_reset_reason()));
    runtime_json.u32_field("cpu_mhz", power_status.actual_cpu_mhz);
    runtime_json.u32_field("flash_freq_mhz", config_flash_freq_mhz());
    runtime_json.u32_field("psram_freq_mhz", config_psram_freq_mhz());
    runtime_json.string_field("pm_mode", power_status.mode);
    runtime_json.string_field("power_runtime_state", power_status.runtime_state);
    runtime_json.string_field("dpm_runtime_state", dpm_decision.runtime_state);
    runtime_json.string_field("dpm_policy", dpm_decision.policy);
    runtime_json.u32_field("runtime_state_age_ms", power_status.runtime_state_age_ms);
    runtime_json.u32_field("telemetry_fast_period_ms",
                           dpm_decision.telemetry_fast_period_ms);
    runtime_json.u32_field("telemetry_medium_period_ms",
                           dpm_decision.telemetry_medium_period_ms);
    runtime_json.u32_field("telemetry_slow_period_ms",
                           dpm_decision.telemetry_slow_period_ms);
    runtime_json.bool_field("wifi_power_save_allowed",
                            dpm_decision.wifi_power_save_allowed);
    runtime_json.bool_field("light_sleep_enabled", power_status.light_sleep_enabled);
    runtime_json.float_field("temperature_c",
                             power_status.temperature_valid ? power_status.temperature_c : -1.0f,
                             1U);
    runtime_json.end();

    PsramJsonWriter json(8192U);
    json.begin();
    json.bool_field("success", true);
    json.bool_field("ok", true);
    json.u32_field("uptime_ms", mros::platform::mros_millis());
    json.raw_field("build", build_json.c_str());
    json.raw_field("heap", heap_json.c_str());
    json.raw_field("storage", storage_json.c_str());
    json.raw_field("shell", shell_json.c_str());
    json.raw_field("web", web_json.c_str());
    json.raw_field("pca", pca_json.c_str());
    json.raw_field("wifi", wifi_json.c_str());
    json.raw_field("files", files_json.c_str());
    json.raw_field("runtime", runtime_json.c_str());
    json.raw_field("coredump", coredump_buffer);
    json.raw_field("heap_trace", heap_trace_buffer);
    json.bool_field("coredump_present", mros::debug::coredump_present());
    json.bool_field("heap_trace_enabled", false);
    json.bool_field("terminal_input_local_first", true);
    json.u32_field("terminal_input_jank", 0U);
    json.string_field("power_runtime_state", power_status.runtime_state);
    json.string_field("dpm_owner_locks", power_status.active_locks);
    json.string_field("dpm_runtime_state", dpm_decision.runtime_state);
    json.string_field("dpm_policy", dpm_decision.policy);
    json.u32_field("telemetry_fast_period_ms",
                   dpm_decision.telemetry_fast_period_ms);
    json.u32_field("telemetry_medium_period_ms",
                   dpm_decision.telemetry_medium_period_ms);
    json.u32_field("telemetry_slow_period_ms",
                   dpm_decision.telemetry_slow_period_ms);
    json.u32_field("dpm_web_wait_floor_ms", dpm_decision.web_wait_floor_ms);
    json.u32_field("dpm_wifi_wait_floor_ms", dpm_decision.wifi_wait_floor_ms);
    json.u32_field("dpm_storage_wait_floor_ms", dpm_decision.storage_wait_floor_ms);
    json.bool_field("wifi_power_save_allowed",
                    dpm_decision.wifi_power_save_allowed);
    json.u32_field("flash_freq_mhz", config_flash_freq_mhz());
    json.u32_field("psram_freq_mhz", config_psram_freq_mhz());
    json.u32_field("json_overflow",
                   g_json_overflow_count + mros::utils::json_overflow_count());
    json.end();
    if (build_json.overflowed() || heap_json.overflowed() ||
        storage_json.overflowed() || shell_json.overflowed() ||
        psram_json.overflowed() || web_json.overflowed() ||
        pca_json.overflowed() || wifi_json.overflowed() ||
        files_json.overflowed() || runtime_json.overflowed() ||
        json.overflowed()) {
      g_json_overflow_count++;
    }
    request->send(200, "application/json", json.c_str());
  });

  server.on("/api/debug/web", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json",
                    "{\"success\":false,\"error\":\"UNAUTHORIZED\",\"message\":\"Authentication required\"}");
      return;
    }

    const AsyncWebDiagnostics diag = server.diagnostics();
    PCA9685_DiagSnapshot_t pca_diag {};
    pca9685_get_diag_snapshot(&pca_diag);
    WifiManagerSnapshot wifi_snapshot {};
    wifi_manager_get_snapshot(&wifi_snapshot);
    mros::shell::service::ShellServiceMetrics shell_metrics {};
    mros::shell::service::get_metrics(&shell_metrics);
    mros::shell::remote::RemoteTunnelMetrics remote_metrics {};
    mros::shell::remote::get_tunnel_metrics(&remote_metrics);
    const uint32_t app_image_size = mros::platform::mros_system_app_image_size();
    mros::power::Status power_status {};
    mros::power::get_status(&power_status);
    mros::rtos::dpm::PolicyDecision dpm_decision {};
    mros::rtos::dpm::get_policy_decision(&dpm_decision);
    char coredump_buffer[768] = {};
    char heap_trace_buffer[768] = {};
    (void)mros::debug::coredump_json(coredump_buffer, sizeof(coredump_buffer));
    (void)mros::debug::heap_trace_json(heap_trace_buffer, sizeof(heap_trace_buffer));
    PsramJsonWriter json(6144U);
    json.begin();
    json.bool_field("success", true);
    json.u32_field("image_size", app_image_size);
    json.u32_field("flash_freq_mhz", config_flash_freq_mhz());
    json.u32_field("psram_freq_mhz", config_psram_freq_mhz());
    json.u32_field("iram_used", 0U);
    json.u32_field("diram_used", 0U);
    json.u32_field("route_count", diag.route_count);
    json.u32_field("ws_handler_count", diag.ws_handler_count);
    json.u32_field("ws_clients_total", web_server_total_ws_client_count());
    json.u32_field("ws_active_sessions", diag.active_ws_clients);
    json.string_field("dpm_policy", dpm_decision.policy);
    json.string_field("dpm_runtime_state", dpm_decision.runtime_state);
    json.u32_field("telemetry_fast_period_ms",
                   dpm_decision.telemetry_fast_period_ms);
    json.u32_field("telemetry_medium_period_ms",
                   dpm_decision.telemetry_medium_period_ms);
    json.u32_field("telemetry_slow_period_ms",
                   dpm_decision.telemetry_slow_period_ms);
    json.u32_field("dpm_web_wait_floor_ms", dpm_decision.web_wait_floor_ms);
    json.u32_field("dpm_wifi_wait_floor_ms", dpm_decision.wifi_wait_floor_ms);
    json.u32_field("dpm_storage_wait_floor_ms", dpm_decision.storage_wait_floor_ms);
    json.bool_field("wifi_power_save_allowed",
                    dpm_decision.wifi_power_save_allowed);
    json.u32_field("ws_legacy_clients", ws.count());
    json.u32_field("ws_telemetry_clients", ws_telemetry.count());
    json.string_field("telemetry_protocol", "mros-bin-v1");
    json.u32_field("telemetry_bin_frames", g_ws_bin_frame_count);
    json.u32_field("telemetry_bin_bytes", g_ws_bin_byte_count);
    json.u32_field("telemetry_json_fallbacks", g_ws_json_fallback_count);
    json.u32_field("telemetry_format_errors", g_ws_format_error_count);
    json.u32_field("telemetry_last_bin_bytes", g_ws_last_bin_bytes);
    json.string_field("telemetry_gated_fields",
                      g_ws_scene_client_count == 0U ? "scene,fk,trajectory" : "");
    json.u32_field("telemetry_scene_clients", g_ws_scene_client_count);
    json.u32_field("telemetry_debug_clients", g_ws_debug_client_count);
    json.u32_field("telemetry_background_clients", g_ws_background_client_count);
    json.u32_field("telemetry_gated_scene_frames", g_ws_gated_scene_frames);
    json.u32_field("telemetry_gated_medium_frames", g_ws_gated_medium_frames);
    json.u32_field("telemetry_gated_slow_frames", g_ws_gated_slow_frames);
    json.string_field("telemetry_memory_budget_mode", g_ws_memory_budget_mode);
    json.u32_field("telemetry_budget_free", g_ws_memory_budget_last_free);
    json.u32_field("telemetry_budget_largest", g_ws_memory_budget_last_largest);
    json.u32_field("telemetry_budget_degrade_frames", g_ws_memory_budget_degrade_frames);
    json.u32_field("telemetry_budget_critical_frames", g_ws_memory_budget_critical_frames);
    json.u32_field("telemetry_budget_scene_suppressed",
                   g_ws_memory_budget_scene_suppressed);
    json.u32_field("telemetry_budget_fast_suppressed",
                   g_ws_memory_budget_fast_suppressed);
    json.u32_field("telemetry_budget_medium_suppressed",
                   g_ws_memory_budget_medium_suppressed);
    json.u32_field("telemetry_budget_dropped", g_ws_telemetry_budget_dropped);
    json.u32_field("telemetry_budget_deferred", g_ws_telemetry_budget_deferred);
    json.u32_field("full_required_count", g_ws_full_required_count);
    json.u32_field("psram_lazy_alloc_count", g_psram_lazy_alloc_count);
    json.u32_field("psram_lazy_alloc_bytes", g_psram_lazy_alloc_bytes);
    json.u32_field("psram_internal_fallback_count", g_psram_internal_fallback_count);
    json.u32_field("psram_alloc_fail_count", g_psram_alloc_fail_count);
    json.bool_field("psram_lazy_alloc", true);
    json.bool_field("shell_forward_lazy_allocated", g_shell_forward_lazy_allocated);
    json.bool_field("trajectory_lazy_allocated", g_trajectory_buffers_lazy_allocated);
    json.u32_field("shell_pool_active", shell_metrics.response_pool_active);
    json.u32_field("shell_pool_capacity", shell_metrics.response_pool_capacity);
    json.u32_field("shell_pool_allocated", shell_metrics.response_pool_allocated);
    json.u32_field("shell_pool_miss", shell_metrics.response_pool_miss);
    json.u32_field("shell_pool_fallback_alloc", shell_metrics.response_fallback_alloc);
    json.u32_field("shell_pool_drop", shell_metrics.response_drop);
    json.u32_field("mshell_job_storage_bytes",
                   mros::shell::runtime::job_storage_bytes());
    json.bool_field("mshell_job_storage_allocated",
                    mros::shell::runtime::job_storage_allocated());
    json.bool_field("mshell_job_storage_psram",
                    mros::shell::runtime::job_storage_uses_psram());
    json.u32_field("shell_bin_frames", shell_metrics.shell_bin_frames);
    json.u32_field("shell_bin_bytes", shell_metrics.shell_bin_bytes);
    json.u32_field("shell_json_frames", shell_metrics.shell_json_frames);
    json.u32_field("shell_json_bytes", shell_metrics.shell_json_bytes);
    json.u32_field("shell_bin_decode_errors", shell_metrics.shell_bin_decode_errors);
    json.u32_field("shell_json_fallbacks", shell_metrics.shell_json_fallbacks);
    json.u32_field("shell_ack_rx", shell_metrics.shell_ack_rx);
    json.u32_field("shell_credit_rx", shell_metrics.shell_credit_rx);
    json.u32_field("shell_credit_exhausted", shell_metrics.shell_credit_exhausted);
    json.u32_field("shell_cbor_frames", g_shell_cbor_control_frames);
    json.u32_field("shell_cbor_bytes", g_shell_cbor_control_bytes);
    json.u32_field("shell_cbor_errors", g_shell_cbor_control_errors);
    json.u32_field("remote_bin_frames", remote_metrics.remote_bin_frames);
    json.u32_field("remote_bin_bytes", remote_metrics.remote_bin_bytes);
    json.u32_field("remote_text_frames", remote_metrics.remote_text_frames);
    json.u32_field("remote_text_bytes", remote_metrics.remote_text_bytes);
    json.u32_field("remote_fallbacks", remote_metrics.remote_fallbacks);
    json.u32_field("remote_cobs_decode_errors", remote_metrics.remote_cobs_decode_errors);
    json.u32_field("remote_rx_drops", remote_metrics.remote_rx_drops);
    json.bool_field("remote_cap_msh1", remote_metrics.remote_cap_msh1);
    json.u32_field("shell_stream_final_suppressed", shell_metrics.stream_final_suppressed);
    json.u32_field("pca_shadow_flushes", pca_diag.shadow_flush_count);
    json.u32_field("pca_shadow_updates", pca_diag.shadow_update_count);
    json.u32_field("pca_shadow_coalesces", pca_diag.shadow_coalesce_count);
    json.u32_field("wifi_event_rev", wifi_snapshot.event_revision);
    json.string_field("wifi_snapshot_source", wifi_snapshot.snapshot_source);
    json.string_field("wifi_dns_backend", wifi_snapshot.dns_backend);
    json.bool_field("wifi_native_event_active", wifi_snapshot.native_event_active);
    json.bool_field("wifi_dns_lwip_active", wifi_snapshot.dns_lwip_active);
    json.u32_field("wifi_dns_queries", wifi_snapshot.dns_queries);
    json.u32_field("wifi_dns_replies", wifi_snapshot.dns_replies);
    json.u32_field("wifi_dns_errors", wifi_snapshot.dns_errors);
    json.u32_field("wifi_state_json_builds", wifi_snapshot.state_json_builds);
    json.u32_field("wifi_state_json_cache_hits", wifi_snapshot.state_json_cache_hits);
    json.bool_field("native_http_enabled", kNativeHttpPrimaryEnabled);
    json.bool_field("native_http_primary", kNativeHttpPrimaryEnabled);
    json.string_field("native_http_mode", "primary");
    json.string_field("native_http_engine", kNativeHttpPrimaryEngine);
    json.u32_field("native_http_port", kNativeHttpPrimaryPort);
    json.bool_field("native_http_direct_lab_enabled", kNativeHttpDirectLabEnabled);
    json.u32_field("native_http_direct_lab_port", kNativeHttpDirectLabPort);
    json.u32_field("native_route_hits", g_native_http_route_hits);
    json.u32_field("native_fallback_hits", g_native_http_fallback_hits);
    json.u32_field("native_http_errors", g_native_http_errors);
    json.u32_field("native_http_send_bytes", g_native_http_send_bytes);
    json.u32_field("native_ws_sends", g_native_http_ws_sends);
    json.u32_field("native_async_queue_errors", g_native_http_async_queue_errors);
    json.u32_field("native_active_sockets", g_native_http_active_sockets);
    json.u32_field("native_lru_purge_count", g_native_http_lru_purge_count);
    json.bool_field("worker_decode_enabled", true);
    json.u32_field("worker_decode_frames", g_worker_decode_frames);
    json.u32_field("worker_decode_errors", g_worker_decode_errors);
    json.u32_field("worker_decode_fallbacks", g_worker_decode_fallbacks);
    json.u32_field("worker_decode_latency_ms", g_worker_decode_latency_ms);
    json.u32_field("worker_decode_dropped", g_worker_decode_dropped);
    json.u32_field("ws_shell_legacy_clients", ws_shell.count());
    json.u32_field("ws_shell_clients", ws_shell_v2.count());
    json.u32_field("ws_debug_clients", ws_debug.count());
    json.u32_field("ws_mcp_clients", ws_mcp.count());
    json.u32_field("total_ws_connects", diag.total_ws_connects);
    json.u32_field("total_ws_disconnects", diag.total_ws_disconnects);
    json.u32_field("total_ws_rx_frames", diag.total_ws_rx_frames);
    json.u32_field("total_ws_tx_frames", diag.total_ws_tx_frames);
    json.u32_field("dropped_ws_tx_frames", diag.dropped_ws_tx_frames);
    json.u32_field("total_http_errors", diag.total_http_errors);
    json.u32_field("total_ws_errors", diag.total_ws_errors);
    json.u32_field("active_http_sockets", diag.active_http_sockets);
    json.u32_field("total_socket_closes", diag.total_socket_closes);
    json.i32_field("last_http_error", diag.last_http_error);
    json.i32_field("last_ws_error", diag.last_ws_error);
    json.u32_field("max_body_size", diag.max_body_size);
    json.u32_field("max_ws_frame_size", diag.max_ws_frame_size);
    json.i32_field("max_open_sockets", diag.max_open_sockets);
    json.i32_field("lwip_max_sockets", diag.lwip_max_sockets);
    json.i32_field("httpd_stack_size", diag.httpd_stack_size);
    json.bool_field("lru_purge_enabled", diag.lru_purge_enabled);
    json.u32_field("ws_ticket_slots", static_cast<uint32_t>(WS_TICKET_SLOTS));
    json.u32_field("ws_ticket_active", activeWsTicketCount());
    json.u32_field("ws_ticket_issued", ws_ticket_issued);
    json.u32_field("ws_ticket_consumed", ws_ticket_consumed);
    json.u32_field("ws_ticket_expired", ws_ticket_expired);
    json.u32_field("ws_ticket_evicted", ws_ticket_evicted);
    json.u32_field("ws_ticket_failed", ws_ticket_failed);
    json.u32_field("cad_stream_requests", g_cad_stream_requests);
    json.u32_field("cad_stream_misses", g_cad_stream_misses);
    json.u32_field("asset_stream_requests", g_asset_stream_requests);
    json.u32_field("asset_stream_misses", g_asset_stream_misses);
    json.string_field("cad_version", get_cad_asset_version_tag());
    json.u32_field("heap_internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    json.u32_field("heap_internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    json.u32_field("heap_internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    json.u32_field("psram_free", mros::platform::mros_system_psram_free());
    json.u32_field("psram_min", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    json.u32_field("psram_largest", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    json.u32_field("json_overflow",
                   g_json_overflow_count + mros::utils::json_overflow_count());
    json.raw_field("coredump", coredump_buffer);
    json.raw_field("heap_trace", heap_trace_buffer);
    json.bool_field("coredump_present", mros::debug::coredump_present());
    json.bool_field("heap_trace_enabled", false);
    json.bool_field("terminal_input_local_first", true);
    json.u32_field("terminal_input_jank", 0U);
    json.string_field("power_runtime_state", power_status.runtime_state);
    json.string_field("dpm_owner_locks", power_status.active_locks);
    json.end();
    if (json.overflowed()) g_json_overflow_count++;
    request->send(200, "application/json", json.c_str());
  });

  server.on("/api/debug/coredump", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    }
    char buffer[1024] = {};
    const bool ok = mros::debug::coredump_json(buffer, sizeof(buffer));
    request->send(ok ? 200 : 500, "application/json", buffer);
  });

  server.on("/api/debug/coredump", HTTP_DELETE, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    }
    if (!web_current_user_is_admin()) {
      return request->send(403, "application/json",
                           "{\"success\":false,\"error\":\"ADMIN_REQUIRED\"}");
    }
    if (!request_reauth_password_ok(request)) {
      return request->send(403, "application/json",
                           "{\"success\":false,\"error\":\"REAUTH_REQUIRED\"}");
    }
    String confirm;
    if (!request_param_value_any(request, "confirm", &confirm) ||
        confirm != "ERASE_COREDUMP") {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"CONFIRM_REQUIRED\"}");
    }
    const esp_err_t err = mros::debug::coredump_clear();
    if (err == ESP_OK) {
      web_security_audit("coredump-clear", auth_current_username_copy());
    }
    PsramJsonWriter json(256U);
    json.begin();
    json.bool_field("ok", err == ESP_OK);
    json.bool_field("success", err == ESP_OK);
    if (err == ESP_ERR_NOT_FOUND) {
      json.string_field("error", "COREDUMP_PARTITION_NOT_FOUND");
    }
    json.u32_field("error_code", static_cast<uint32_t>(err));
    json.end();
    request->send(err == ESP_OK ? 200 : 404, "application/json", json.c_str());
  });

  server.on("/api/debug/coredump/download", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) return request->send(401);
    if (!web_current_user_is_admin()) {
      return request->send(403, "application/json",
                           "{\"success\":false,\"error\":\"ADMIN_REQUIRED\"}");
    }
    if (!request_reauth_password_ok(request)) {
      return request->send(403, "application/json",
                           "{\"success\":false,\"error\":\"REAUTH_REQUIRED\"}");
    }
    String confirm;
    if (!request_param_value_any(request, "confirm", &confirm) ||
        confirm != "DOWNLOAD_COREDUMP") {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"CONFIRM_REQUIRED\"}");
    }
    const esp_partition_t* part = mros::debug::coredump_partition_handle();
    if (part == nullptr) {
      return request->send(404, "application/json",
                           "{\"success\":false,\"error\":\"COREDUMP_PARTITION_NOT_FOUND\"}");
    }
    web_security_audit("coredump-download", auth_current_username_copy());
    auto ctx = std::make_shared<const esp_partition_t*>(part);
    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "application/octet-stream",
        [ctx](uint8_t* buffer, size_t max_len, size_t index) -> size_t {
          const esp_partition_t* p = (ctx != nullptr) ? *ctx : nullptr;
          if (p == nullptr || buffer == nullptr || max_len == 0U || index >= p->size) {
            return 0U;
          }
          const size_t n = std::min<size_t>(max_len, p->size - index);
          if (esp_partition_read(p, index, buffer, n) != ESP_OK) {
            return 0U;
          }
          return n;
        });
    if (response == nullptr) {
      return request->send(500, "application/json",
                           "{\"success\":false,\"error\":\"STREAM_ALLOC_FAILED\"}");
    }
    response->addHeader("Content-Disposition", "attachment; filename=\"mros-coredump.bin\"");
    request->send(response);
  });

  server.on("/api/debug/heap-trace", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    }
    char buffer[1024] = {};
    const bool ok = mros::debug::heap_trace_json(buffer, sizeof(buffer));
    request->send(ok ? 200 : 500, "application/json", buffer);
  });

  server.on("/api/debug/heap-trace", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    }
    const String action =
        request->hasParam("action", true) ? request->getParam("action", true)->value() :
        (request->hasParam("action") ? request->getParam("action")->value() : "summary");
    esp_err_t err = ESP_OK;
    if (action == "start") err = mros::debug::heap_trace_start();
    else if (action == "stop") err = mros::debug::heap_trace_stop();
    else if (action == "clear") mros::debug::heap_trace_clear();
    else err = ESP_ERR_INVALID_ARG;
    PsramJsonWriter json(384U);
    json.begin();
    json.bool_field("ok", err == ESP_OK);
    json.bool_field("success", err == ESP_OK);
    json.string_field("action", action.c_str());
    json.string_field("error", err == ESP_ERR_NOT_SUPPORTED ? "FEATURE_DISABLED" :
                                (err == ESP_ERR_INVALID_ARG ? "INVALID_ARGUMENT" : ""));
    json.u32_field("code", static_cast<uint32_t>(err));
    json.end();
    request->send(err == ESP_OK ? 200 : 409, "application/json", json.c_str());
  });

  server.on(
      "/api/trajectory/store", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        // handled in body callback
      },
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
         size_t index, size_t total) {
        if (!isAuthenticated(request)) {
          request->send(401);
          return;
        }

        if (index == 0) {
          trajectory_handler_stream_reset();
          if (!trajectory_handler_stream_begin(total)) {
            String err = String("{\"success\":false,\"error\":\"") +
                         jsonEscape(trajectory_handler_last_error()) + "\"}";
            request->send(413, "application/json", err);
            return;
          }
        }
        if (!trajectory_handler_stream_write(index, data, len)) {
          String err = String("{\"success\":false,\"error\":\"") +
                       jsonEscape(trajectory_handler_last_error()) + "\"}";
          request->send(400, "application/json", err);
          return;
        }
        if (index + len != total) return;

        if (!psram_buffers_ready && !initPsrTrajectoryBuffers()) {
          request->send(503, "application/json",
                        "{\"success\":false,\"error\":\"PSRAM hazir degil\"}");
          return;
        }

        float step_mm = 2.0f;
        if (request->hasParam("preview_step")) {
          step_mm = request->getParam("preview_step")->value().toFloat();
        }
        if (!std::isfinite(step_mm) || step_mm < 0.2f) step_mm = 2.0f;
        if (step_mm > 20.0f) step_mm = 20.0f;

        if (!trajectory_handler_stream_parse()) {
          String err = String("{\"success\":false,\"error\":\"") +
                       jsonEscape(trajectory_handler_last_error()) + "\"}";
          request->send(400, "application/json", err);
          return;
        }

        size_t parsed = 0;
        bool ok = copyParsedTrajectoryToPsrBuffer(&parsed);
        if (!ok) {
          request->send(
              400, "application/json",
              "{\"success\":false,\"error\":\"Gecersiz trajectory payload. Beklenen: CSV veya JSON\"}");
          return;
        }

        rebuildDensePreviewFromPsrTrajectory(step_mm);
        pm_request_perf_boost(2200);
        String json = "{";
        json += "\"success\":true,";
        json += "\"stored\":" + String((uint32_t)psram_traj_count) + ",";
        json += "\"capacity\":" + String((uint32_t)psram_traj_capacity) + ",";
        json += "\"preview\":" + String((uint32_t)psram_preview_count) + ",";
        json += "\"preview_capacity\":" + String((uint32_t)psram_preview_capacity) + ",";
        json += "\"preview_step_mm\":" + String(psram_preview_step_mm, 2) + ",";
        json += "\"truncated\":" + String(psram_traj_last_truncated ? "true" : "false");
        json += "}";
        request->send(200, "application/json", json);
      });

  server.on("/api/trajectory/stats", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) return request->send(401);
    String json = "{";
    json += "\"ready\":" + String(psram_buffers_ready ? "true" : "false") + ",";
    json += "\"stored\":" + String((uint32_t)psram_traj_count) + ",";
    json += "\"capacity\":" + String((uint32_t)psram_traj_capacity) + ",";
    json += "\"preview\":" + String((uint32_t)psram_preview_count) + ",";
    json += "\"preview_capacity\":" + String((uint32_t)psram_preview_capacity) + ",";
    json += "\"preview_step_mm\":" + String(psram_preview_step_mm, 2) + ",";
    json += "\"truncated\":" + String(psram_traj_last_truncated ? "true" : "false");
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/trajectory/preview", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) return request->send(401);
    if (!psram_buffers_ready || !psram_preview_points) {
      return request->send(200, "application/json",
                           "{\"success\":true,\"count\":0,\"points\":[]}");
    }

    size_t max_points = 3000;
    if (request->hasParam("max")) {
      long mp = request->getParam("max")->value().toInt();
      if (mp > 0) max_points = (size_t)mp;
    }
    if (max_points < 16) max_points = 16;
    if (max_points > 12000) max_points = 12000;

    size_t src_count = psram_preview_count;
    size_t stride = 1;
    if (src_count > max_points) {
      stride = (src_count + max_points - 1) / max_points;
      if (stride < 1) stride = 1;
    }

    auto state = std::make_shared<TrajectoryPreviewStreamState>();
    state->src_count = src_count;
    state->stride = stride;
    state->next_index = 0U;
    std::snprintf(state->prefix,
                  sizeof(state->prefix),
                  "{\"success\":true,\"streamed\":true,\"count\":%lu,\"stride\":%lu,\"points\":[",
                  static_cast<unsigned long>(src_count),
                  static_cast<unsigned long>(stride));
    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "application/json",
        [state](uint8_t* buffer, size_t max_len, size_t) -> size_t {
          if (state == nullptr || buffer == nullptr || max_len == 0U) return 0U;
          size_t written = 0U;
          auto copy_chunk = [&](const char* text, const size_t len, size_t* pos) {
            if (text == nullptr || pos == nullptr) return;
            while (written < max_len && *pos < len) {
              const size_t n = std::min<size_t>(max_len - written, len - *pos);
              memcpy(buffer + written, text + *pos, n);
              written += n;
              *pos += n;
            }
          };

          const size_t prefix_len = strlen(state->prefix);
          copy_chunk(state->prefix, prefix_len, &state->prefix_pos);
          while (written < max_len) {
            if (state->record_pos < state->record_len) {
              copy_chunk(state->record, state->record_len, &state->record_pos);
              continue;
            }
            if (state->next_index >= state->src_count) break;
            const PreviewPointPSRAM& p = psram_preview_points[state->next_index];
            state->record_len = static_cast<size_t>(
                std::snprintf(state->record,
                              sizeof(state->record),
                              "%s{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}",
                              state->first_record ? "" : ",",
                              static_cast<double>(p.x),
                              static_cast<double>(p.y),
                              static_cast<double>(p.z)));
            if (state->record_len >= sizeof(state->record)) {
              state->record_len = sizeof(state->record) - 1U;
            }
            state->record_pos = 0U;
            state->first_record = false;
            state->next_index += state->stride;
            if (state->stride == 0U) state->stride = 1U;
          }
          static constexpr const char* kSuffix = "]}";
          copy_chunk(kSuffix, 2U, &state->suffix_pos);
          return written;
        });
    if (response == nullptr) {
      return request->send(500, "application/json",
                           "{\"success\":false,\"error\":\"STREAM_ALLOC_FAILED\"}");
    }
    request->send(response);
  });

  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest *request) {
    String stored_user, stored_hash;
    if (!load_stored_credentials(stored_user, stored_hash)) {
      return request->send(
          403, "application/json",
          auth_json_error("İlk kurulum gerekli. Lütfen kullanıcı oluşturun.", true));
    }

    // Rate limiting: block for 60 s after 5 consecutive failures
    const uint32_t lockout_ms = login_lockout_remaining_ms();
    if (lockout_ms > 0U) {
      return request->send(429, "application/json",
                           auth_json_error("Çok fazla hatalı deneme. Lütfen daha sonra tekrar deneyin."));
    }
    if (request->hasParam("user", true) && request->hasParam("pass", true)) {
      String u = request->getParam("user", true)->value();
      String p = request->getParam("pass", true)->value();
      u.trim();
      if (u.length() > kAuthUserMaxLen || p.length() > kAuthPassMaxLen) {
        return request->send(400, "application/json",
                             auth_json_error("Kullanıcı adı veya şifre uzunluğu geçersiz."));
      }
      bool stored_hash_needs_upgrade = false;
      if (web_auth_verify_user_password(u, p, stored_user, stored_hash,
                                        &stored_hash_needs_upgrade)) {
        login_fail_count = 0;
        login_block_until_ms = 0;
        if (stored_hash_needs_upgrade && u == stored_user) {
          if (!web_auth_save_primary_credentials(u, p)) {
            return request->send(500, "application/json",
                                 auth_json_error("Kimlik bilgisi guvenli bicime yukseltilemedi."));
          }
          web_security_audit("web-auth-hash-upgrade", u);
        }
        const String session_token = generateSecureToken(24);
        if (!is_hex_string_fixed(session_token, kAuthSessionTokenLen)) {
          return request->send(500, "application/json",
                               auth_json_error("Oturum anahtarı üretilemedi."));
        }
        auth_set_session_state(u, session_token);
        clearWsAuthState();
        clearWsTickets();
        const String csrf_token = auth_csrf_token_for_session(session_token);
        String response_body = "{\"success\":true,\"csrf_token\":\"";
        response_body += csrf_token;
        response_body += "\",\"csrf_header\":\"X-MROS-CSRF\"}";
        AsyncWebServerResponse *response = request->beginResponse(
            200, "application/json", response_body);
        response->addHeader("Cache-Control", "no-store");
        response->addHeader("Set-Cookie", session_cookie_header(session_token, false));
        request->send(response);
        return;
      }
    }
    // Failed login: increment counter and conditionally engage lockout
    if (login_fail_count < 255U) login_fail_count++;
    if (login_fail_count >= 5) {
      login_block_until_ms = mros::platform::mros_millis() + 60000UL;
      login_fail_count = 0;
      mros_console.println("[SEC] Login locked for 60 s after 5 failed attempts.");
    }
    request->send(401, "application/json", auth_json_error("Kullanıcı adı veya şifre hatalı."));
  });

  server.on("/api/auth/state", HTTP_GET, [](AsyncWebServerRequest *request) {
    const bool setup_required = is_initial_setup_required();
    const bool authenticated = isAuthenticated(request);
    const String csrf_token =
        authenticated ? auth_csrf_token_for_session(request_session_cookie_token(request)) : "";
    String theme_mode;
    String color_palette;
    get_saved_ui_theme(&theme_mode, &color_palette);
    char buffer[512] = {};
    mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
    writer.begin();
    writer.bool_field("success", true);
    writer.bool_field("setup_required", setup_required);
    writer.bool_field("authenticated", authenticated);
    if (authenticated) {
      writer.string_field("csrf_token", csrf_token.c_str());
      writer.string_field("csrf_header", "X-MROS-CSRF");
    }
    writer.u32_field("lockout_ms", login_lockout_remaining_ms());
    writer.bool_field("serial_auth_required", mros::shell::serial_auth_required());
    writer.string_field("themeMode", theme_mode.c_str());
    writer.string_field("colorPalette", color_palette.c_str());
    writer.end();
    AsyncWebServerResponse *response = request->beginResponse(
        200, "application/json", writer.overflow() ? "{\"success\":false,\"error\":\"AUTH_STATE_OVERFLOW\"}" : writer.c_str());
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  server.on("/api/security/users", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      return request->send(401, "application/json", auth_json_error("AUTH_REQUIRED"));
    }
    AsyncWebServerResponse *response =
        request->beginResponse(200, "application/json", build_security_users_json());
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  server.on("/api/security/users/add", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      return request->send(401, "application/json", auth_json_error("AUTH_REQUIRED"));
    }
    if (!web_current_user_is_admin()) {
      return request->send(403, "application/json", auth_json_error("ADMIN_REQUIRED"));
    }
    if (!request_reauth_password_ok(request)) {
      return request->send(403, "application/json", auth_json_error("REAUTH_REQUIRED"));
    }
    if (!request->hasParam("username", true) ||
        !request->hasParam("password", true)) {
      return request->send(400, "application/json",
                           auth_json_error("username and password required"));
    }
    String username = request->getParam("username", true)->value();
    String password = request->getParam("password", true)->value();
    String display = request->hasParam("display_name", true)
                         ? request->getParam("display_name", true)->value()
                         : username;
    username.trim();
    display.trim();
    const bool admin = request->hasParam("admin", true) &&
                       request->getParam("admin", true)->value() == "1";
    const bool sudo = request->hasParam("sudo", true) &&
                      request->getParam("sudo", true)->value() == "1";
    if (username.length() > kAuthUserMaxLen || password.length() > kAuthPassMaxLen) {
      return request->send(400, "application/json",
                           auth_json_error("Kullanıcı adı veya şifre uzunluğu geçersiz."));
    }
    if (!mros::ssh::add_user(display, username, password, admin, sudo)) {
      return request->send(400, "application/json",
                           auth_json_error("Kullanıcı eklenemedi."));
    }
    web_security_audit("user-add", username);
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/security/users/delete", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      return request->send(401, "application/json", auth_json_error("AUTH_REQUIRED"));
    }
    if (!web_current_user_is_admin()) {
      return request->send(403, "application/json", auth_json_error("ADMIN_REQUIRED"));
    }
    if (!request_reauth_password_ok(request)) {
      return request->send(403, "application/json", auth_json_error("REAUTH_REQUIRED"));
    }
    if (!request->hasParam("username", true)) {
      return request->send(400, "application/json", auth_json_error("username required"));
    }
    String username = request->getParam("username", true)->value();
    username.trim();
    if (username == auth_current_username_copy()) {
      return request->send(409, "application/json",
                           auth_json_error("Aktif kullanıcı silinemez."));
    }
    if (!mros::ssh::disable_user(username)) {
      return request->send(400, "application/json", auth_json_error("Kullanıcı silinemedi."));
    }
    web_security_audit("user-delete", username);
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/security/users/password", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      return request->send(401, "application/json", auth_json_error("AUTH_REQUIRED"));
    }
    if (!request->hasParam("username", true) ||
        !request->hasParam("password", true)) {
      return request->send(400, "application/json",
                           auth_json_error("username and password required"));
    }
    String username = request->getParam("username", true)->value();
    String password = request->getParam("password", true)->value();
    String password2 = request->hasParam("password2", true)
                           ? request->getParam("password2", true)->value()
                           : password;
    username.trim();
    const bool self_change = username == auth_current_username_copy();
    if (!self_change && !web_current_user_is_admin()) {
      return request->send(403, "application/json", auth_json_error("ADMIN_REQUIRED"));
    }
    if (!request_reauth_password_ok(request)) {
      return request->send(403, "application/json", auth_json_error("REAUTH_REQUIRED"));
    }
    if (password != password2) {
      return request->send(400, "application/json",
                           auth_json_error("Şifre tekrar alanı eşleşmiyor."));
    }
    if (password.length() < 8U || password.length() > kAuthPassMaxLen) {
      return request->send(400, "application/json",
                           auth_json_error("Şifre 8-96 karakter olmalı."));
    }
    if (!mros::ssh::set_password_for_user(username, password)) {
      return request->send(400, "application/json",
                           auth_json_error("Şifre değiştirilemedi."));
    }
    web_security_audit("user-password", username);
    const mros::ssh::IdentityConfig identity = mros::ssh::identity_get();
    if (username == identity.username) {
      if (!web_auth_save_primary_credentials(username, password)) {
        return request->send(500, "application/json",
                             auth_json_error("Kimlik bilgisi kaydedilemedi."));
      }
    }
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/security/sessions/revoke", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      return request->send(401, "application/json", auth_json_error("AUTH_REQUIRED"));
    }
    String action;
    if (!request_param_value(request, "action", &action)) {
      return request->send(400, "application/json", auth_json_error("INVALID_ACTION"));
    }
    action.trim();
    action.toLowerCase();
    if (action != "current" && action != "all") {
      return request->send(400, "application/json", auth_json_error("INVALID_ACTION"));
    }
    const bool revoke_current = action == "current";
    const bool revoke_all = action == "all";
    if (revoke_all && !web_current_user_is_admin()) {
      return request->send(403, "application/json", auth_json_error("ADMIN_REQUIRED"));
    }
    if (revoke_all && !request_reauth_password_ok(request)) {
      return request->send(403, "application/json", auth_json_error("REAUTH_REQUIRED"));
    }
    if (revoke_all) {
      web_security_audit("session-revoke-all", auth_current_username_copy());
      web_server_logout_all();
    } else if (revoke_current) {
      const String audit_user = auth_current_username_copy();
      const String session_token = request_session_cookie_token(request);
      (void)auth_revoke_session_token(session_token);
      clearWsAuthState();
      clearWsTickets();
      web_security_audit("session-revoke-current", audit_user);
    }
    String body = "{\"success\":true,\"revoked\":\"";
    body += action;
    body += "\"}";
    AsyncWebServerResponse *response =
        request->beginResponse(200, "application/json", body);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Set-Cookie", session_cookie_header("", true));
    request->send(response);
  });

  server.on("/api/security/auth-reset", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      return request->send(401, "application/json", auth_json_error("AUTH_REQUIRED"));
    }
    if (!web_current_user_is_admin()) {
      return request->send(403, "application/json", auth_json_error("ADMIN_REQUIRED"));
    }
    if (!request_reauth_password_ok(request)) {
      return request->send(403, "application/json", auth_json_error("REAUTH_REQUIRED"));
    }
    String confirm;
    if (!request_param_value(request, "confirm", &confirm) ||
        confirm != "RESET-SETUP") {
      return request->send(400, "application/json", auth_json_error("CONFIRM_REQUIRED"));
    }
    const bool credentials_cleared = prefs_clear_credentials();
    const bool identity_reset = credentials_cleared && mros::ssh::reset_identity_for_initial_setup();
    if (!credentials_cleared || !identity_reset) {
      web_security_audit("auth-reset-failed", auth_current_username_copy());
      return request->send(500, "application/json", auth_json_error("AUTH_RESET_FAILED"));
    }
    clear_setup_gate_secret();
    web_security_audit("auth-reset", auth_current_username_copy());
    web_server_logout_all();
    login_fail_count = 0;
    login_block_until_ms = 0;
    AsyncWebServerResponse *response = request->beginResponse(
        200,
        "application/json",
        "{\"success\":true,\"setup_required\":true,\"redirect\":\"/login\"}");
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Set-Cookie", session_cookie_header("", true));
    request->send(response);
  });

  server.on("/api/register", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!is_initial_setup_required()) {
      return request->send(
          409, "application/json",
          auth_json_error("Kullanıcı zaten oluşturulmuş."));
    }
    if (!request->hasParam("user", true) || !request->hasParam("pass", true) ||
        !request->hasParam("pass2", true) || !request->hasParam("setup_pass", true)) {
      return request->send(
          400, "application/json",
          auth_json_error("user, pass, pass2 ve setup_pass gerekli."));
    }

    String user = request->getParam("user", true)->value();
    String pass = request->getParam("pass", true)->value();
    String pass2 = request->getParam("pass2", true)->value();
    String setup_pass = request->getParam("setup_pass", true)->value();
    String root_same = request->hasParam("root_same", true)
                           ? request->getParam("root_same", true)->value()
                           : "1";
    String root_pass = request->hasParam("root_pass", true)
                           ? request->getParam("root_pass", true)->value()
                           : "";
    String root_pass2 = request->hasParam("root_pass2", true)
                            ? request->getParam("root_pass2", true)->value()
                            : "";
    String avatar_choice = request->hasParam("avatar", true)
                               ? request->getParam("avatar", true)->value()
                               : "avatar-1";
    String avatar_data = request->hasParam("avatar_data", true)
                             ? request->getParam("avatar_data", true)->value()
                             : "";
    user.trim();
    root_same.trim();
    root_same.toLowerCase();
    const bool use_same_root_password =
        root_same == "1" || root_same == "true" || root_same == "on" || root_same == "yes";
    if (use_same_root_password) {
      root_pass = pass;
      root_pass2 = pass2;
    }

    if (user.length() < 3 || user.length() > kAuthUserMaxLen) {
      return request->send(400, "application/json",
                           auth_json_error("Kullanıcı adı 3-32 karakter olmalı."));
    }
    if (pass.length() < 8 || pass.length() > 64 || pass2.length() > 64) {
      return request->send(400, "application/json",
                           auth_json_error("Şifre 8-64 karakter olmalı."));
    }
    if (pass != pass2) {
      return request->send(400, "application/json",
                           auth_json_error("Şifre tekrar alanı eşleşmiyor."));
    }
    if (root_pass.length() < 8U || root_pass.length() > kAuthPassMaxLen ||
        root_pass2.length() > kAuthPassMaxLen) {
      return request->send(400, "application/json",
                           auth_json_error("Kök kullanıcı şifresi 8-96 karakter olmalı."));
    }
    if (root_pass != root_pass2) {
      return request->send(400, "application/json",
                           auth_json_error("Kök kullanıcı şifre tekrarı eşleşmiyor."));
    }
    if (avatar_data.length() > kAvatarDataMaxLen) {
      return request->send(413, "application/json",
                           auth_json_error("Profil resmi 128KB sınırını aşıyor."));
    }
    if (!setup_gate_secret_matches(setup_pass)) {
      return request->send(403, "application/json",
                           auth_json_error("Kullanıcı oluşturma şifresi hatalı."));
    }

    if (!web_auth_save_primary_credentials(user, pass)) {
      return request->send(500, "application/json",
                           auth_json_error("Kimlik bilgisi kaydedilemedi."));
    }
    (void)mros::ssh::set_username(user);
    (void)mros::ssh::set_display_name(user);
    (void)mros::ssh::set_primary_user_role(true, true);
    (void)mros::ssh::set_password_for_user(user, pass);
    if (!mros::ssh::set_password_for_user(String(mros::ssh::root_username()), root_pass)) {
      return request->send(500, "application/json",
                           auth_json_error("Kök kullanıcı şifresi kaydedilemedi."));
    }
    if (!save_profile_avatar_selection(avatar_choice, avatar_data)) {
      return request->send(400, "application/json",
                           auth_json_error("Profil resmi seçimi kaydedilemedi."));
    }

    String verify_user, verify_hash;
    if (!load_stored_credentials(verify_user, verify_hash)) {
      return request->send(500, "application/json",
                           auth_json_error("Kullanıcı kaydedilemedi."));
    }

    const String session_token = generateSecureToken(24);
    if (!is_hex_string_fixed(session_token, kAuthSessionTokenLen)) {
      return request->send(500, "application/json",
                           auth_json_error("Oturum anahtarı üretilemedi."));
    }
    auth_set_session_state(verify_user, session_token);
    clearWsAuthState();
    clearWsTickets();
    login_fail_count = 0;
    login_block_until_ms = 0;
    rotate_setup_gate_secret_after_registration();

    const String csrf_token = auth_csrf_token_for_session(session_token);
    String response_body =
        "{\"success\":true,\"message\":\"Kullanıcı oluşturuldu.\",\"csrf_token\":\"";
    response_body += csrf_token;
    response_body += "\",\"csrf_header\":\"X-MROS-CSRF\"}";
    AsyncWebServerResponse *response = request->beginResponse(
        200, "application/json", response_body);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Set-Cookie", session_cookie_header(session_token, false));
    request->send(response);
  });

  server.on("/api/ws-ticket", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    const String username = web_current_username_or_refresh();
    const uint32_t capabilities = web_capability_mask_for_username(username);
    const bool debug_allowed =
        (capabilities & mros::shell::ShellCapabilityDebug) == mros::shell::ShellCapabilityDebug;
    String telemetry = issueWsTicket(WsTicketScope::Telemetry, username, capabilities);
    String shell = issueWsTicket(WsTicketScope::Shell, username, capabilities);
    String debug = debug_allowed ? issueWsTicket(WsTicketScope::Debug, username, capabilities) : "";
    String mcp = debug_allowed ? issueWsTicket(WsTicketScope::Mcp, username, capabilities) : "";
    if (telemetry.length() == 0 || shell.length() == 0 ||
        (debug_allowed && (debug.length() == 0 || mcp.length() == 0))) {
      return request->send(500, "application/json",
                           "{\"success\":false,\"error\":\"WS_TICKET_FAILED\",\"message\":\"ticket failed\"}");
    }
    String json = "{\"success\":true,\"ticket\":\"" + telemetry +
                  "\",\"expires_ms\":" + String(WS_TICKET_TTL_MS) +
                  ",\"user\":\"" + jsonEscape(username) +
                  "\",\"capabilities\":\"" + jsonEscape(String(mros::shell::capabilities_text(capabilities))) +
                  "\",\"scoped\":true" +
                  ",\"tickets\":{\"telemetry\":\"" + telemetry +
                  "\",\"shell\":\"" + shell +
                  "\",\"debug\":\"" + debug +
                  "\",\"mcp\":\"" + mcp + "\"},\"allowed\":{\"debug\":" +
                  String(debug_allowed ? "true" : "false") +
                  ",\"mcp\":" + String(debug_allowed ? "true" : "false") + "}}";
    request->send(200, "application/json", json);
  });

  server.on("/api/shell/sessions", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    char buffer[1536] = {};
    if (!mros::shell::service::sessions_json(buffer, sizeof(buffer))) {
      return request->send(500, "application/json", "{\"ok\":false,\"error\":\"sessions_overflow\"}");
    }
    request->send(200, "application/json", buffer);
  });

  server.on("/api/shell/sessions", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    request->send(200, "application/json",
                  "{\"ok\":true,\"note\":\"websocket SHELL2 STATE opens pane sessions lazily\"}");
  });

  server.on("/api/shell/sessions", HTTP_DELETE, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("id")) {
      return request->send(400, "application/json", "{\"ok\":false,\"error\":\"missing_id\"}");
    }
    const uint32_t id = static_cast<uint32_t>(request->getParam("id")->value().toInt());
    const bool ok = mros::shell::service::close_session_id(id);
    request->send(ok ? 200 : 404, "application/json",
                  ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"session_not_found\"}");
  });

  server.on("/api/mshell/devices", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    const std::string text = mros::shell::remote::devices_report(nullptr);
    String json = "{\"ok\":true,\"bridge_mode\":\"";
    json += mros::shell::remote::bridge_mode_name(mros::shell::remote::bridge_mode());
    json += "\",\"text\":\"";
    json += jsonEscape(String(text.c_str()));
    json += "\"}";
    request->send(200, "application/json", json);
  });

  server.on("/api/mshell/connect", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    const String target = request->hasParam("target", true)
                              ? request->getParam("target", true)->value()
                              : String("");
    mros::shell::remote::Target parsed = mros::shell::remote::Target::None;
    if (!mros::shell::remote::parse_target(std::string(target.c_str()), &parsed) ||
        parsed == mros::shell::remote::Target::None) {
      return request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_target\"}");
    }
    const mros::shell::remote::BridgeMode mode = mros::shell::remote::bridge_mode();
    String json = "{\"ok\":";
    json += (parsed == mros::shell::remote::Target::S3 ||
             mode == mros::shell::remote::BridgeMode::On)
                ? "true"
                : "false";
    json += ",\"target\":\"";
    json += mros::shell::remote::target_name(parsed);
    json += "\",\"bridge_mode\":\"";
    json += mros::shell::remote::bridge_mode_name(mode);
    json += "\"}";
    request->send(200, "application/json", json);
  });

  server.on("/api/mshell/schema", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    const String subject = request->hasParam("subject")
                               ? request->getParam("subject")->value()
                               : String("api");
    String command = "mshell schema ";
    command += subject;
    command += " --json";
    request->send(200, "application/json", shellCaptureJson(command.c_str()));
  });

  server.on("/api/mshell/call", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String command = "mshell call ";
    if (request->hasParam("method", true) && request->hasParam("path", true)) {
      const String method = request->getParam("method", true)->value();
      const String path = request->getParam("path", true)->value();
      if (!mshell_validate_param(method, 16U, true)) {
        return send_mshell_invalid_argument(request, "method");
      }
      if (!mshell_validate_param(path, 160U, true)) {
        return send_mshell_invalid_argument(request, "path");
      }
      command += method;
      command += " ";
      command += path;
    } else if (request->hasParam("api", true)) {
      const String api = request->getParam("api", true)->value();
      if (!mshell_validate_param(api, 160U, true)) {
        return send_mshell_invalid_argument(request, "api");
      }
      command += api;
    } else {
      return request->send(400, "application/json", "{\"ok\":false,\"error_code\":\"INVALID_ARGUMENT\"}");
    }
    const char* keys[] = {"path", "target", "x", "y", "z", "joints"};
    for (const char* key : keys) {
      const size_t max_len = strcmp(key, "joints") == 0 ? 256U : 160U;
      if (!append_mshell_key_value(command, request, key, max_len)) {
        return;
      }
    }
    if (request->hasParam("target_device", true)) {
      const String target = request->getParam("target_device", true)->value();
      if (!mshell_validate_param(target, 32U, true)) {
        return send_mshell_invalid_argument(request, "target_device");
      }
      command += " --target ";
      command += target;
    }
    command += " --json";
    request->send(200, "application/json", shellCaptureJson(command.c_str()));
  });

  server.on("/api/mshell/jobs", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (request->hasParam("id")) {
      const uint32_t id = static_cast<uint32_t>(request->getParam("id")->value().toInt());
      char json[4096] = {};
      if (!mros::shell::runtime::job_log_json(id, json, sizeof(json))) {
        return request->send(500, "application/json", "{\"ok\":false,\"error_code\":\"TRUNCATED\"}");
      }
      return request->send(200, "application/json", json);
    }
    char json[4096] = {};
    if (!mros::shell::runtime::jobs_json(json, sizeof(json))) {
      return request->send(500, "application/json", "{\"ok\":false,\"error_code\":\"TRUNCATED\"}");
    }
    request->send(200, "application/json", json);
  });

  server.on("/api/mshell/jobs", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("command", true)) {
      return request->send(400, "application/json", "{\"ok\":false,\"error_code\":\"INVALID_ARGUMENT\"}");
    }
    uint32_t id = 0U;
    char error[96] = {};
    const String command = request->getParam("command", true)->value();
    const bool ok = mros::shell::runtime::job_start(
        command.c_str(),
        mros::shell::kShellCapabilityAdmin,
        "web",
        &id,
        error,
        sizeof(error));
    String json = "{\"ok\":";
    json += ok ? "true" : "false";
    json += ",\"job_id\":";
    json += String(id);
    json += ",\"error_code\":\"";
    json += error;
    json += "\"}";
    request->send(ok ? 200 : 409, "application/json", json);
  });

  server.on("/api/mshell/jobs", HTTP_DELETE, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("id")) {
      return request->send(400, "application/json", "{\"ok\":false,\"error_code\":\"INVALID_ARGUMENT\"}");
    }
    const uint32_t id = static_cast<uint32_t>(request->getParam("id")->value().toInt());
    char error[96] = {};
    const bool ok = mros::shell::runtime::job_cancel(id, error, sizeof(error));
    String json = "{\"ok\":";
    json += ok ? "true" : "false";
    json += ",\"error_code\":\"";
    json += error;
    json += "\"}";
    request->send(ok ? 200 : 404, "application/json", json);
  });

  server.on("/api/mshell/tx", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    char json[512] = {};
    if (!mros::shell::runtime::tx_json(json, sizeof(json))) {
      return request->send(500, "application/json", "{\"ok\":false,\"error_code\":\"INTERNAL_ERROR\"}");
    }
    request->send(200, "application/json", json);
  });

  server.on("/api/mshell/tx", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    const String action = request->hasParam("action", true)
                              ? request->getParam("action", true)->value()
                              : String("status");
    char error[96] = {};
    uint32_t id = 0U;
    bool ok = false;
    if (action == "begin") ok = mros::shell::runtime::tx_begin("web", &id, error, sizeof(error));
    else if (action == "stage") ok = mros::shell::runtime::tx_stage(
        request->hasParam("note", true) ? request->getParam("note", true)->value().c_str() : "",
        error,
        sizeof(error));
    else if (action == "commit") ok = mros::shell::runtime::tx_commit(error, sizeof(error));
    else if (action == "rollback") ok = mros::shell::runtime::tx_rollback(error, sizeof(error));
    else return request->send(400, "application/json", "{\"ok\":false,\"error_code\":\"INVALID_ARGUMENT\"}");
    String json = "{\"ok\":";
    json += ok ? "true" : "false";
    json += ",\"tx_id\":";
    json += String(id);
    json += ",\"error_code\":\"";
    json += error;
    json += "\"}";
    request->send(ok ? 200 : 409, "application/json", json);
  });

  server.on("/api/mros/doctor", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String command = "mros doctor ";
    command += request->hasParam("target") ? request->getParam("target")->value() : String("quick");
    command += " --json";
    request->send(200, "application/json", shellCaptureJson(command.c_str()));
  });

  server.on("/api/mros/report", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (request->hasParam("path")) {
      String command = "mros report show ";
      command += request->getParam("path")->value();
      return request->send(200, "application/json", shellCaptureJson(command.c_str()));
    }
    request->send(200, "application/json", shellCaptureJson("mros report list"));
  });

  server.on("/api/mros/report", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    request->send(200, "application/json", shellCaptureJson("mros report create"));
  });

  server.on("/api/mros/report", HTTP_DELETE, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("path")) {
      return request->send(400, "application/json", "{\"ok\":false,\"error_code\":\"INVALID_ARGUMENT\"}");
    }
    String command = "mros report delete ";
    command += request->getParam("path")->value();
    request->send(200, "application/json", shellCaptureJson(command.c_str()));
  });

  server.on("/api/mros/audit", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    request->send(200, "application/json", shellCaptureJson("mros audit list"));
  });

  server.on("/api/settings/uart-shell", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    const String mode_text = request->hasParam("mode", true)
                                 ? request->getParam("mode", true)->value()
                                 : String("");
    mros::shell::remote::BridgeMode mode = mros::shell::remote::BridgeMode::Off;
    if (!mros::shell::remote::parse_bridge_mode(std::string(mode_text.c_str()), &mode)) {
      return request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_mode\"}");
    }
    if (!mros::shell::remote::set_bridge_mode(mode)) {
      return request->send(500, "application/json", "{\"ok\":false,\"error\":\"persist_failed\"}");
    }
    char buffer[128] = {};
    mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
    writer.begin();
    writer.bool_field("ok", true);
    writer.string_field("mode", mros::shell::remote::bridge_mode_name(mode));
    writer.end();
    request->send(200, "application/json",
                  writer.overflow() ? "{\"ok\":false,\"error\":\"json_overflow\"}" : writer.c_str());
  });

  server.on("/api/logout", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    const String audit_user = auth_current_username_copy();
    const String session_token = request_session_cookie_token(request);
    (void)auth_revoke_session_token(session_token);
    clearWsAuthState();
    clearWsTickets();
    web_security_audit("logout-current", audit_user);
    AsyncWebServerResponse *response =
        request->beginResponse(200, "application/json", "{\"success\":true,\"revoked\":\"current\"}");
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Set-Cookie", session_cookie_header("", true));
    request->send(response);
  });

  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String scan_json;
    wifi_manager_get_scan_results_json(&scan_json);
    if (!wifi_manager_is_scan_in_progress()) {
      wifi_manager_request_scan();
    }
    if (scan_json != "[]") {
      request->send(200, "application/json", scan_json);
      return;
    }
    request->send(202, "application/json", "{\"status\":\"scanning\"}");
  });

  server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (request->hasParam("ssid", true) && request->hasParam("pass", true)) {
      const String ssid = request->getParam("ssid", true)->value();
      const String pass = request->getParam("pass", true)->value();
      if (!wifi_manager_request_test_connect(ssid, pass)) {
        request->send(409, "application/json",
                      "{\"status\":\"busy\",\"success\":false}");
        return;
      }
      request->send(200, "application/json", "{\"status\":\"testing\"}");
    } else {
      request->send(400, "text/plain", "Missing args");
    }
  });

  server.on("/api/wifi/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("ssid", true) || !request->hasParam("pass", true)) {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"ssid/pass gerekli\"}");
      return;
    }
    const String ssid = request->getParam("ssid", true)->value();
    const String pass = request->getParam("pass", true)->value();
    const bool ok = wifi_manager_save_credentials(ssid, pass, false);
    request->send(ok ? 200 : 500, "application/json",
                  ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"wifi save failed\"}");
  });

  server.on("/api/wifi/action", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("action", true)) {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"action gerekli\"}");
      return;
    }
    String action = request->getParam("action", true)->value();
    action.trim();
    action.toLowerCase();
    if (action == "on") {
      wifi_manager_set_enabled(true);
      wifi_manager_request_reconnect();
    } else if (action == "off") {
      wifi_manager_set_enabled(false);
    } else if (action == "hotspot") {
      wifi_manager_force_hotspot();
    } else if (action == "reconnect") {
      wifi_manager_request_reconnect();
    } else if (action == "scan") {
      (void)wifi_manager_request_scan();
    } else {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"unknown wifi action\"}");
      return;
    }
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/wifi/state", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String wifi_state_json;
    wifi_manager_build_state_json(&wifi_state_json);
    request->send(200, "application/json", wifi_state_json);
  });

  server.on("/api/wifi/diag", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String wifi_diag_json;
    wifi_manager_build_diag_json(&wifi_diag_json);
    AsyncWebServerResponse *response =
        request->beginResponse(200, "application/json", wifi_diag_json);
    add_no_cache_headers(response);
    request->send(response);
  });

  server.on("/api/dpm/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    send_dpm_json(request, mros::rtos::dpm::status_json);
  });

  server.on("/api/dpm/decision", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    send_dpm_json(request, mros::rtos::dpm::policy_decision_json);
  });

  server.on("/api/dpm/tasks", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    send_dpm_json(request, mros::rtos::dpm::tasks_json);
  });

  server.on("/api/dpm/policy", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String mode = request->hasParam("mode", true)
                      ? request->getParam("mode", true)->value()
                      : (request->hasParam("mode") ? request->getParam("mode")->value() : "");
    mode.trim();
    mode.toLowerCase();
    mros::rtos::dpm::Policy policy {};
    if (!mros::rtos::dpm::parse_policy(mode.c_str(), &policy)) {
      return request->send(400, "application/json",
                           "{\"ok\":false,\"error\":\"invalid_policy\"}");
    }
    (void)mros::rtos::dpm::set_policy(policy, true);
    send_dpm_json(request, mros::rtos::dpm::status_json);
  });

  server.on("/api/dpm/wake", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String task = request->hasParam("task", true)
                      ? request->getParam("task", true)->value()
                      : (request->hasParam("task") ? request->getParam("task")->value() : "");
    String reason = request->hasParam("reason", true)
                        ? request->getParam("reason", true)->value()
                        : (request->hasParam("reason") ? request->getParam("reason")->value() : "web");
    task.trim();
    reason.trim();
    if (task.length() == 0U) {
      return request->send(400, "application/json",
                           "{\"ok\":false,\"error\":\"missing_task\"}");
    }
    bool ok = false;
    if (task == "peers") {
      ok |= mros::rtos::dpm::wake_task_by_name("comm_spi_t41", reason.c_str(), "web");
      ok |= mros::rtos::dpm::wake_task_by_name("comm_spi_c3", reason.c_str(), "web");
      ok |= mros::rtos::dpm::wake_task_by_name("comm_uart_t41", reason.c_str(), "web");
    } else {
      ok = mros::rtos::dpm::wake_task_by_name(task.c_str(), reason.c_str(), "web");
    }
    request->send(ok ? 200 : 404, "application/json",
                  ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"not_found\"}");
  });

  server.on("/api/power/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    send_dpm_json(request, mros::power::status_json);
  });

  server.on("/api/power/locks", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    send_dpm_json(request, mros::power::locks_json);
  });

  server.on("/api/power/mode", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String mode = request->hasParam("mode", true)
                      ? request->getParam("mode", true)->value()
                      : (request->hasParam("mode") ? request->getParam("mode")->value() : "");
    mode.trim();
    mode.toLowerCase();
    mros::power::Mode parsed {};
    if (!mros::power::parse_mode(mode.c_str(), &parsed)) {
      return request->send(400, "application/json",
                           "{\"ok\":false,\"error\":\"invalid_mode\"}");
    }
    (void)mros::power::set_mode(parsed, true);
    send_dpm_json(request, mros::power::status_json);
  });

  server.on("/api/memory/sram", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    send_dpm_json(request, mros::power::sram_json);
  });

  server.on("/api/memory/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    send_dpm_json(request, mros::memory::status_json);
  });

  server.on("/api/memory/leaks", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    send_dpm_json(request, mros::memory::leaks_json);
  });

  server.on("/api/dpm/frequency", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    send_dpm_json(request, mros::power::status_json);
  });

  server.on("/api/robot/math/onboard", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    AsyncWebServerResponse *response =
        request->beginResponse(200, "application/json",
                               mros::experimental::worker_status_json());
    add_no_cache_headers(response);
    request->send(response);
  });

  server.on("/api/robot/math/onboard", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("action", true)) {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"action gerekli\"}");
      return;
    }
    String action = request->getParam("action", true)->value();
    action.trim();
    action.toLowerCase();
    bool ok = true;
    if (action == "enable") {
      ok = mros::experimental::worker_set_enabled(true);
    } else if (action == "disable") {
      ok = mros::experimental::worker_set_enabled(false);
    } else if (action == "clear") {
      mros::experimental::worker_clear_stats();
    } else if (action == "cancel") {
      mros::experimental::worker_cancel();
    } else if (action == "backend") {
      const String mode = request->hasParam("mode", true) ? request->getParam("mode", true)->value() : "auto";
      ok = web_server_set_ik_compute_preference(mode.c_str());
    } else {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"unknown action\"}");
      return;
    }
    request->send(ok ? 200 : 500, "application/json",
                  mros::experimental::worker_status_json());
  });

  server.on("/api/robot/math/onboard/run", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("op", true)) {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"op gerekli\"}");
      return;
    }
    String op = request->getParam("op", true)->value();
    op.trim();
    op.toLowerCase();
    mros::experimental::WorkerRequest worker_req {};
    std::snprintf(worker_req.source, sizeof(worker_req.source), "web");
    if (op == "fk") {
      worker_req.type = mros::experimental::WorkerJobType::Fk;
      for (uint8_t i = 0; i < 7U; ++i) {
        const String key = String("j") + String(i);
        worker_req.joints_deg[i] =
            request->hasParam(key.c_str(), true) ? request->getParam(key.c_str(), true)->value().toFloat() : 0.0f;
      }
    } else if (op == "ik") {
      worker_req.type = mros::experimental::WorkerJobType::Ik;
      worker_req.x = request->hasParam("x", true) ? request->getParam("x", true)->value().toFloat() : 0.0f;
      worker_req.y = request->hasParam("y", true) ? request->getParam("y", true)->value().toFloat() : 0.0f;
      worker_req.z = request->hasParam("z", true) ? request->getParam("z", true)->value().toFloat() : 0.0f;
      worker_req.joints_deg[0] = spi_s3_get_turret_deg();
      for (uint8_t i = 1; i < 7U; ++i) worker_req.joints_deg[i] = spi_s3_get_joint_deg(i - 1U);
    } else if (op == "trajectory") {
      worker_req.type = mros::experimental::WorkerJobType::Trajectory;
      worker_req.x = request->hasParam("x", true) ? request->getParam("x", true)->value().toFloat() : 0.0f;
      worker_req.y = request->hasParam("y", true) ? request->getParam("y", true)->value().toFloat() : 0.0f;
      worker_req.z = request->hasParam("z", true) ? request->getParam("z", true)->value().toFloat() : 0.0f;
      worker_req.t_ms = request->hasParam("t_ms", true) ? request->getParam("t_ms", true)->value().toFloat() : 1000.0f;
    } else {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"op must be fk, ik or trajectory\"}");
      return;
    }
    mros::experimental::WorkerResult worker_result {};
    (void)mros::experimental::worker_submit_sync(worker_req, 7000U, &worker_result);
    request->send(worker_result.ok ? 200 : 409, "application/json",
                  mros::experimental::worker_result_json(worker_result));
  });

  server.on("/api/devices/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    WifiManagerState wifi {};
    wifi_manager_get_state(&wifi);
    String json = "{";
    json.reserve(720U);
    json += "\"t41_connected\":" + String(spi_s3_is_connected() ? "true" : "false") + ",";
    json += "\"t41_qspi_txn\":" + String(spi_s3_get_total_transactions()) + ",";
    json += "\"t41_qspi_crc_errors\":" + String(spi_s3_get_crc_errors()) + ",";
    json += "\"c3_disabled\":true,";
    json += "\"c3_total_rx\":" + String(spi_c3_get_total_rx()) + ",";
    json += "\"c3_crc_errors\":" + String(spi_c3_get_crc_errors()) + ",";
    json += "\"wifi_connected\":" + String(wifi.sta_connected ? "true" : "false") + ",";
    json += "\"wifi_ap_active\":" + String(wifi.ap_active ? "true" : "false") + ",";
    json += "\"wifi_rssi\":" + String(wifi.rssi) + ",";
    json += "\"pca_ready\":" + String(pca9685_is_ready() ? "true" : "false") + ",";
    json += "\"espnow_active\":" + String(spi_c3_is_espnow_active() ? "true" : "false") + ",";
    json += "\"espnow_connected\":" + String(spi_c3_is_espnow_connected() ? "true" : "false") + ",";
    json += "\"ws_clients\":" + String(web_server_total_ws_client_count()) + ",";
    json += "\"worker\":";
    json += mros::experimental::worker_status_json();
    json += "}";
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    add_no_cache_headers(response);
    request->send(response);
  });

  server.on("/api/devices/test", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String target = request->hasParam("target", true) ? request->getParam("target", true)->value() : "all";
    target.trim();
    target.toLowerCase();
    uint8_t mask = 0U;
    if (target == "c3") {
      request->send(409, "application/json",
                    "{\"success\":false,\"disabled\":true,\"error\":\"c3 topology disabled; use uart/s3-uart for link diagnostics\"}");
      return;
    }
    if (target == "t41" || target == "uart" || target == "s3-uart") mask = mros::experimental::kDeviceTestT41;
    else if (target == "wifi" || target == "net") mask = mros::experimental::kDeviceTestWifi;
    else if (target == "pca" || target == "pca9685") mask = mros::experimental::kDeviceTestPca;
    else if (target == "web" || target == "ws") mask = mros::experimental::kDeviceTestWeb;
    else if (target == "all") {
      mask = static_cast<uint8_t>(mros::experimental::kDeviceTestT41 |
                                  mros::experimental::kDeviceTestWifi |
                                  mros::experimental::kDeviceTestPca |
                                  mros::experimental::kDeviceTestWeb);
    }
    if (mask == 0U) {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"target must be t41,uart,s3-uart,wifi,pca,web,all\"}");
      return;
    }
    mros::experimental::WorkerRequest worker_req {};
    worker_req.type = mros::experimental::WorkerJobType::DeviceTest;
    worker_req.device_mask = mask;
    std::snprintf(worker_req.source, sizeof(worker_req.source), "web");
    mros::experimental::WorkerResult worker_result {};
    (void)mros::experimental::worker_submit_sync(worker_req, 5000U, &worker_result);
    request->send(worker_result.ok ? 200 : 409, "application/json",
                  mros::experimental::worker_result_json(worker_result));
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String json = "{";
    json += "\"t41_connected\":" +
            String(spi_s3_is_connected() ? "true" : "false") + ",";
    json += "\"loop_ms\":" + String(spi_s3_get_loop_ms()) + ",";
    json += "\"turret\":" + String(spi_s3_get_turret_deg(), 1) + ",";
    json += "\"j1\":" + String(spi_s3_get_joint_deg(0), 1) + ",";
    json += "\"motor\":" + String(spi_s3_get_motor_state()) + ",";
    json += "\"auth_session_active\":" +
            String(auth_active_session_count() > 0U ? "true" : "false") + ",";
    json += "\"login_lockout_ms\":" + String(login_lockout_remaining_ms()) + ",";
    json += "\"serial_auth_required\":" +
            String(mros::shell::serial_auth_required() ? "true" : "false");
    json += "}";
    request->send(200, "application/json", json);
  });

  ESP_LOGI("WEB", "Routes registered: auth wifi status");

  server.on("/set", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (request->hasParam("rpos")) {
      spi_s3_set_reset_encoder();
    }
    if (request->hasParam("power")) {
      uint8_t p = request->getParam("power")->value().toInt();
      spi_s3_set_motor_power(p);
    }
    if (request->hasParam("v8")) {
      spi_s3_set_target_turret(request->getParam("v8")->value().toFloat() -
                               270.0f);
    }
    if (request->hasParam("oe")) {
      uint8_t oe = request->getParam("oe")->value().toInt();
      pca9685_set_output_enable(oe == 1);
    }
    if (request->hasParam("fsopt")) {
      int fsopt = request->getParam("fsopt")->value().toInt();
      if (fsopt < C3_FAILSAFE_T41_QSPI) fsopt = C3_FAILSAFE_T41_QSPI;
      if (fsopt > C3_FAILSAFE_RESERVED) fsopt = C3_FAILSAFE_RESERVED;
      spi_c3_set_failsafe_option(static_cast<int8_t>(fsopt));
    }
    if (request->hasParam("c3flags")) {
      int flags = request->getParam("c3flags")->value().toInt();
      if (flags < 0) flags = 0;
      if (flags > 255) flags = 255;
      spi_c3_set_cmd_flags(static_cast<uint8_t>(flags));
    }
    if (request->hasParam("c3sp")) {
      int32_t sp = static_cast<int32_t>(request->getParam("c3sp")->value().toInt());
      spi_c3_set_pid_setpoint_x100(sp);
    }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/c3/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    // Keep both reset paths in sync:
    // - C3 encoder absolute position via C3 SPI command
    // - Existing ESP32P4 reset flag used by other UI paths
    spi_c3_request_encoder_reset();
    spi_s3_set_reset_encoder();
    request->send(200, "application/json",
                  "{\"success\":true,\"message\":\"C3 encoder reset queued\"}");
  });

  server.on("/api/c3/failsafe", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String json = "{\"failsafe_option\":" + String(spi_c3_get_failsafe_option()) + "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/c3/failsafe", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    const bool has_query_param = request->hasParam("option");
    const bool has_body_param = request->hasParam("option", true);
    if (!has_query_param && !has_body_param) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"option param required (0..3)\"}");
    }
    const AsyncWebParameter *param =
        has_query_param ? request->getParam("option") : request->getParam("option", true);
    int fsopt = param->value().toInt();
    if (fsopt < C3_FAILSAFE_T41_QSPI) fsopt = C3_FAILSAFE_T41_QSPI;
    if (fsopt > C3_FAILSAFE_RESERVED) fsopt = C3_FAILSAFE_RESERVED;
    spi_c3_set_failsafe_option(static_cast<int8_t>(fsopt));
    String json = "{\"success\":true,\"failsafe_option\":" + String(fsopt) + "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/pid", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    float kp = 0.0f, ki = 0.0f, kd = 0.0f, i_limit = 0.0f;
    spi_s3_get_turret_pid(&kp, &ki, &kd, &i_limit);
    String json = "{";
    json += "\"kp\":" + String(kp, 4) + ",";
    json += "\"ki\":" + String(ki, 4) + ",";
    json += "\"kd\":" + String(kd, 4) + ",";
    json += "\"imax\":" + String(i_limit, 4) + ",";
    json += "\"dspc\":" + String(spi_s3_get_turret_dspc(), 2) + ",";
    json += "\"out\":" + String(spi_s3_get_turret_pid_output(), 2) + ",";
    json += "\"err\":" + String(spi_s3_get_turret_pid_error(), 2) + ",";
    json += "\"actual\":" + String(spi_s3_get_turret_actual_deg(), 2);
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/turret/output_lock", HTTP_GET,
            [](AsyncWebServerRequest *request) {
              if (!isAuthenticated(request))
                return request->send(401);
              String json = "{\"locked\":" +
                            String(spi_s3_get_turret_output_lock() ? 1 : 0) +
                            "}";
              request->send(200, "application/json", json);
            });

  server.on("/api/turret/output_lock", HTTP_POST,
            [](AsyncWebServerRequest *request) {
              if (!isAuthenticated(request))
                return request->send(401);
              if (!request->hasParam("en")) {
                return request->send(
                    400, "application/json",
                    "{\"success\":false,\"error\":\"en param required (0/1)\"}");
              }
              bool lock_en = request->getParam("en")->value().toInt() != 0;
              spi_s3_set_turret_output_lock(lock_en);
              String json = "{\"success\":true,\"locked\":" +
                            String(lock_en ? 1 : 0) + "}";
              request->send(200, "application/json", json);
            });

  server.on("/api/pid", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("kp") || !request->hasParam("ki") ||
        !request->hasParam("kd") || !request->hasParam("imax")) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"kp,ki,kd,imax required\"}");
    }
    float kp = request->getParam("kp")->value().toFloat();
    float ki = request->getParam("ki")->value().toFloat();
    float kd = request->getParam("kd")->value().toFloat();
    float imax = request->getParam("imax")->value().toFloat();
    spi_s3_set_turret_pid(kp, ki, kd, imax);
    float dspc_val = spi_s3_get_turret_dspc();
    if (request->hasParam("dspc")) {
      dspc_val = request->getParam("dspc")->value().toFloat();
    }
    prefs_save_pid(kp, ki, kd, imax, dspc_val);
    if (request->hasParam("dspc")) {
      float dspc = request->getParam("dspc")->value().toFloat();
      spi_s3_set_turret_dspc(dspc);
    }
    request->send(200, "application/json", "{\"success\":true}");
  });

  // --- PCA9685 Calibration API ---

  server.on("/api/pca/cal", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String json = "{\"channels\":[";
    for (int i = 1; i <= PCA_TOTAL_CHANNELS; i++) {
      PCA9685_ChannelCal_t *c = pca9685_get_cal(i);
      if (i > 1)
        json += ",";
      json += "{\"ch\":" + String(i);
      json += ",\"a_min\":" + String(c->angle_min_us, 1);
      json += ",\"a_max\":" + String(c->angle_max_us, 1);
      json += ",\"s_min\":" + String(c->speed_min_us, 1);
      json += ",\"s_ctr\":" + String(c->speed_center_us, 1);
      json += ",\"s_max\":" + String(c->speed_max_us, 1);
      json += "}";
    }
    json += "],\"oe\":" + String(pca9685_get_output_enable() ? 1 : 0);
    json += ",\"ready\":" + String(pca9685_is_ready() ? 1 : 0);
    json += ",\"osc\":" + String(pca9685_get_osc_freq());
    json += ",\"freq\":" + String(pca9685_get_frequency(), 1);
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/pca/osc", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    request->send(200, "application/json",
                  "{\"osc\":" + String(pca9685_get_osc_freq()) + "}");
  });

  server.on("/api/pca/osc", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("val", true)) {
      return request->send(400, "application/json", "{\"error\":\"val param required\"}");
    }
    uint32_t val = request->getParam("val", true)->value().toInt();
    if (val < 10000000 || val > 40000000) {
      return request->send(400, "application/json", "{\"error\":\"osc freq out of range (10-40MHz)\"}");
    }
    pca9685_set_osc_freq(val);
    pca9685_save_cal();
    request->send(200, "application/json", "{\"success\":true,\"osc\":" + String(val) + "}");
  });

  server.on(
      "/api/pca/cal", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
         size_t index, size_t total) {
        if (!isAuthenticated(request)) {
          request->send(401);
          return;
        }
        static String json_buf = "";
        if (index == 0)
          json_buf = "";
        for (size_t i = 0; i < len; i++)
          json_buf += (char)data[i];

        if (index + len == total) {
          // Parse simple format: ch,a_min,a_max,s_min,s_ctr,s_max
          // Example: "3,544.0,2400.0,500.0,1500.0,2500.0"
          int ch = 0;
          float a_min = 0, a_max = 0, s_min = 0, s_ctr = 0, s_max = 0;
          int parsed = sscanf(json_buf.c_str(), "%d,%f,%f,%f,%f,%f", &ch,
                              &a_min, &a_max, &s_min, &s_ctr, &s_max);
          if (parsed == 6 && ch >= 1 && ch <= PCA_TOTAL_CHANNELS) {
            PCA9685_ChannelCal_t cal;
            cal.angle_min_us = a_min;
            cal.angle_max_us = a_max;
            cal.speed_min_us = s_min;
            cal.speed_center_us = s_ctr;
            cal.speed_max_us = s_max;
            pca9685_set_cal(ch, &cal);
            pca9685_save_cal();
            request->send(200, "application/json",
                          "{\"success\":true,\"ch\":" + String(ch) + "}");
          } else {
            request->send(400, "application/json",
                          "{\"success\":false,\"error\":\"format: "
                          "ch,a_min,a_max,s_min,s_ctr,s_max\"}");
          }
        }
      });

  server.on(
      "/api/pca/cal/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!isAuthenticated(request))
          return request->send(401);
        pca9685_reset_cal();
        pca9685_save_cal();
        request->send(
            200, "application/json",
            "{\"success\":true,\"message\":\"Calibration reset to defaults\"}");
      });

  // Instant test: send raw PWM µs to a channel
  server.on("/api/pca/test", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("ch") || !request->hasParam("us")) {
      request->send(400, "application/json",
                    "{\"error\":\"ch and us params required\"}");
      return;
    }
    int ch = request->getParam("ch")->value().toInt();
    float us = request->getParam("us")->value().toFloat();
    if (ch < 1 || ch > PCA_TOTAL_CHANNELS || us < 0 || us > 3000) {
      request->send(400, "application/json",
                    "{\"error\":\"ch:1-16, us:0-3000\"}");
      return;
    }
    // Convert µs to PCA9685 tick: tick = us * 4096 * freq / 1e6
    uint16_t tick = (uint16_t)(((us * 4096.0f * pca9685_get_frequency()) / 1000000.0f) + 0.5f);
    if (tick > 4095)
      tick = 4095;
    bool ok = pca9685_set_pwm((uint8_t)ch, 0, tick);
    String json = "{\"success\":" + String(ok ? "true" : "false") +
                  ",\"ch\":" + String(ch) + ",\"us\":" + String(us, 1) +
                  ",\"tick\":" + String(tick) + "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/console", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!send_console_snapshot_psram(request)) {
      request->send(500, "text/plain; charset=utf-8",
                    "console snapshot unavailable");
    }
  });
  server.on("/api/console/delta", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"unauthorized\"}");
    if (!send_console_delta_json(request)) {
      request->send(500, "application/json",
                    "{\"success\":false,\"error\":\"console_delta_unavailable\"}");
    }
  });

  server.on("/api/files/mounts", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    request->send(
        200,
        "application/json",
        String(mros::shell::remote::fs_mounts_json(true, mros::platform::mros_fs_is_mounted()).c_str()));
  });

  server.on("/api/files/mount", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    const String action =
        request->hasParam("action", true) ? request->getParam("action", true)->value()
                                          : (request->hasParam("action") ? request->getParam("action")->value() : "status");
    const String target =
        request->hasParam("target", true) ? request->getParam("target", true)->value()
                                          : (request->hasParam("target") ? request->getParam("target")->value() : "");
    if (action == "status") {
      return request->send(
          200,
          "application/json",
          String(mros::shell::remote::fs_mounts_json(true, mros::platform::mros_fs_is_mounted()).c_str()));
    }
    if (target == "all" && action == "umount") {
      mros::shell::remote::fs_umount_all();
      return request->send(
          200,
          "application/json",
          String(mros::shell::remote::fs_mounts_json(true, mros::platform::mros_fs_is_mounted()).c_str()));
    }
    mros::shell::remote::FsMount mount = mros::shell::remote::FsMount::T41;
    if (!mros::shell::remote::fs_parse_mount(target.c_str(), &mount)) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"INVALID_TARGET\",\"error_code\":\"INVALID_TARGET\"}");
    }
    std::string message;
    bool ok = false;
    if (action == "mount") {
      ok = mros::shell::remote::fs_mount(mount, &message);
    } else if (action == "umount") {
      ok = mros::shell::remote::fs_umount(mount, &message);
    } else {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"INVALID_ACTION\",\"error_code\":\"INVALID_ACTION\"}");
    }
    String json = "{\"success\":";
    json += ok ? "true" : "false";
    json += ",\"message\":\"";
    json += jsonEscape(String(message.c_str()));
    json += "\",\"mounts\":";
    String mounts = String(mros::shell::remote::fs_mounts_json(true, mros::platform::mros_fs_is_mounted()).c_str());
    int idx = mounts.indexOf("\"mounts\":");
    if (idx >= 0) {
      mounts = mounts.substring(idx + 9);
      if (mounts.endsWith("}")) mounts.remove(mounts.length() - 1);
      json += mounts;
    } else {
      json += "[]";
    }
    json += "}";
    request->send(ok ? 200 : 409, "application/json", json);
  });

  server.on("/api/files/list", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    std::string path = "/ESPUSER";
    if (request->hasParam("path")) {
      path = fm_normalize_path(request->getParam("path")->value().c_str());
    }
    size_t offset = 0U;
    size_t limit = 0U;
    if (request->hasParam("offset")) {
      offset = static_cast<size_t>(
          strtoull(request->getParam("offset")->value().c_str(), nullptr, 10));
    }
    if (request->hasParam("limit")) {
      limit = static_cast<size_t>(
          strtoull(request->getParam("limit")->value().c_str(), nullptr, 10));
      if (limit > 500U) limit = 500U;
    }
    String sort = request->hasParam("sort") ? request->getParam("sort")->value() : "name";
    String dir = request->hasParam("dir") ? request->getParam("dir")->value() : "asc";
    sort.trim();
    dir.trim();
    request->send(200, "application/json", fm_list_json(path, offset, limit, sort.c_str(), dir.c_str()));
  });

  server.on("/api/files/info", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    if (!request->hasParam("path")) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"MISSING_PATH\"}");
    }
    const std::string path = fm_normalize_path(request->getParam("path")->value().c_str());
    if (fm_is_remote_path(path)) {
      return request->send(
          409,
          "application/json",
          fm_remote_error_json(path, "info", "PEER_PROTOCOL_MISSING", "remote file info requires t41 MSHELL2 FS protocol"));
    }
    if (!fm_is_visible_user_path(path)) {
      return request->send(403, "application/json",
                           "{\"success\":false,\"error\":\"PERMISSION_DENIED\"}");
    }
    struct stat info {};
    if (!fm_stat_logical(path, &info)) {
      return request->send(404, "application/json",
                           "{\"success\":false,\"error\":\"NOT_FOUND\"}");
    }
    String json = "{\"success\":true,\"item\":";
    json += fm_entry_json(path, fm_basename(path).c_str(), info);
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/files/download", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    if (!request->hasParam("path")) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"MISSING_PATH\"}");
    }
    const std::string path = fm_normalize_path(request->getParam("path")->value().c_str());
    if (fm_is_remote_path(path)) {
      return request->send(
          409,
          "application/json",
          fm_remote_error_json(path, "download", "PEER_PROTOCOL_MISSING", "remote download requires t41 MSHELL2 FS protocol"));
    }
    if (!fm_is_visible_user_path(path)) {
      return request->send(403, "application/json",
                           "{\"success\":false,\"error\":\"PERMISSION_DENIED\"}");
    }
    struct stat info {};
    if (!fm_stat_logical(path, &info) || S_ISDIR(info.st_mode)) {
      return request->send(404, "application/json",
                           "{\"success\":false,\"error\":\"FILE_NOT_FOUND\"}");
    }
    const std::string name = fm_basename(path);
    String disposition = "attachment; filename=\"";
    disposition += jsonEscape(String(name.c_str()));
    disposition += "\"";
    if (!web_async_send_littlefs_stream_psram(
            request, path.c_str(), asset_mime_from_path(String(path.c_str())),
            true, nullptr, nullptr, nullptr, disposition.c_str())) {
      request->send(500, "application/json",
                    "{\"success\":false,\"error\":\"DOWNLOAD_FAILED\"}");
    }
  });

  server.on("/api/files/mkdir", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    if (!request->hasParam("path")) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"MISSING_PATH\"}");
    }
    const std::string path = fm_normalize_path(request->getParam("path")->value().c_str());
    if (fm_is_remote_path(path)) {
      return request->send(
          409,
          "application/json",
          fm_remote_error_json(path, "mkdir", "PEER_PROTOCOL_MISSING", "remote mkdir requires t41 MSHELL2 FS protocol"));
    }
    if (!fm_is_user_write_target(path)) {
      return request->send(403, "application/json",
                           "{\"success\":false,\"error\":\"PERMISSION_DENIED\"}");
    }
    const bool ok = mros::platform::mros_fs_mkdir(path.c_str());
    request->send(ok ? 200 : 500, "application/json",
                  ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"MKDIR_FAILED\"}");
  });

  server.on("/api/files/delete", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    if (!request->hasParam("path")) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"MISSING_PATH\"}");
    }
    const std::string path = fm_normalize_path(request->getParam("path")->value().c_str());
    if (fm_is_remote_path(path)) {
      return request->send(
          409,
          "application/json",
          fm_remote_error_json(path, "delete", "PEER_PROTOCOL_MISSING", "remote delete requires t41 MSHELL2 FS protocol"));
    }
    if (!fm_is_user_write_target(path)) {
      return request->send(403, "application/json",
                           "{\"success\":false,\"error\":\"PERMISSION_DENIED\"}");
    }
    const bool ok = fm_delete_recursive(path);
    request->send(ok ? 200 : 500, "application/json",
                  ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"DELETE_FAILED\"}");
  });

  server.on("/api/files/rename", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    if (!request->hasParam("path") || !request->hasParam("to")) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"MISSING_PATH\"}");
    }
    const std::string from = fm_normalize_path(request->getParam("path")->value().c_str());
    const std::string to = fm_normalize_path(request->getParam("to")->value().c_str());
    if (fm_is_remote_path(from) || fm_is_remote_path(to)) {
      return request->send(
          409,
          "application/json",
          fm_remote_error_json(from, "rename", "PEER_PROTOCOL_MISSING", "remote rename requires t41 MSHELL2 FS protocol"));
    }
    if (!fm_is_user_write_target(from) || !fm_is_user_write_target(to)) {
      return request->send(403, "application/json",
                           "{\"success\":false,\"error\":\"PERMISSION_DENIED\"}");
    }
    const bool ok = mros::platform::mros_fs_rename(from.c_str(), to.c_str());
    request->send(ok ? 200 : 500, "application/json",
                  ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"RENAME_FAILED\"}");
  });

  server.on("/api/files/copy", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    if (!request->hasParam("path") || !request->hasParam("to")) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"MISSING_PATH\"}");
    }
    const std::string from = fm_normalize_path(request->getParam("path")->value().c_str());
    const std::string to = fm_normalize_path(request->getParam("to")->value().c_str());
    if (fm_is_remote_path(from) || fm_is_remote_path(to)) {
      return request->send(
          409,
          "application/json",
          fm_remote_error_json(from, "copy", "PEER_PROTOCOL_MISSING", "remote copy requires t41 MSHELL2 FS protocol"));
    }
    if (!fm_is_visible_user_path(from) || !fm_is_user_write_target(to)) {
      return request->send(403, "application/json",
                           "{\"success\":false,\"error\":\"PERMISSION_DENIED\"}");
    }
    const bool ok = fm_copy_file(from, to);
    request->send(ok ? 200 : 500, "application/json",
                  ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"COPY_FAILED\"}");
  });

  server.on("/api/files/fetch/check", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    if (!request->hasParam("url")) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"MISSING_URL\"}");
    }
    const std::string url = request->getParam("url")->value().c_str();
    std::string target;
    std::string guard_error;
    if (!fm_validate_fetch_request(
            url,
            request->hasParam("target") ? request->getParam("target")->value().c_str() : "auto",
            request->hasParam("cwd") ? request->getParam("cwd")->value().c_str() : "/ESPUSER",
            &target,
            &guard_error)) {
      const int status = guard_error == "PERMISSION_DENIED" ? 403 : 400;
      return request->send(status, "application/json",
                           String("{\"success\":false,\"error\":\"") +
                               guard_error.c_str() + "\"}");
    }
    if (!wifi_manager_is_connected()) {
      return request->send(409, "application/json",
                           "{\"success\":false,\"error\":\"WIFI_NOT_CONNECTED\"}");
    }
    mros::platform::HttpClientStream stream {};
    mros::platform::HttpClientConfig http_config {};
    http_config.allow_insecure_tls = false;
    http_config.allow_private_hosts = kFileFetchPrivateHostsAllowed;
    http_config.max_redirects = kFileFetchMaxRedirects;
    http_config.timeout_ms = 12000;
    http_config.buffer_size = 512U;
    if (!mros::platform::mros_http_client_begin_get(url.c_str(), http_config, &stream)) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"CONNECT_FAILED\"}");
    }
    const int code = stream.status_code;
    const int64_t total = stream.content_length;
    mros::platform::mros_http_client_close(&stream);
    const uint64_t free_bytes = fm_free_bytes();
    const bool downloadable = (code >= 200 && code < 300);
    std::string content_guard = downloadable ? "OK" : "HTTP_ERROR";
    const bool enough =
        downloadable && fm_download_guard_content_length(target, total, &content_guard);
    String json = "{\"success\":true,";
    json += "\"downloadable\":" + String(downloadable ? "true" : "false") + ",";
    json += "\"enough_space\":" + String(enough ? "true" : "false") + ",";
    json += "\"guard\":\"" + jsonEscape(String(content_guard.c_str())) + "\",";
    json += "\"status_code\":" + String(code) + ",";
    json += "\"content_length\":" + fm_i64_json(total) + ",";
    json += "\"fs_free\":" + String(static_cast<uint32_t>(free_bytes > UINT32_MAX ? UINT32_MAX : free_bytes)) + ",";
    json += "\"tls_strict\":true,";
    json += "\"redirects_allowed\":" + String(kFileFetchRedirectsAllowed ? "true" : "false") + ",";
    json += "\"private_hosts_blocked\":" + String(kFileFetchPrivateHostsAllowed ? "false" : "true") + ",";
    json += "\"target\":\"" + fm_public_path_json(target) + "\"";
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/files/fetch/start", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    if (!request->hasParam("url")) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"MISSING_URL\"}");
    }
    portENTER_CRITICAL(&g_file_download_mux);
    const bool busy = g_file_download_status.active;
    portEXIT_CRITICAL(&g_file_download_mux);
    if (busy) {
      return request->send(409, "application/json",
                           "{\"success\":false,\"error\":\"DOWNLOAD_BUSY\"}");
    }
    if (!wifi_manager_is_connected()) {
      return request->send(409, "application/json",
                           "{\"success\":false,\"error\":\"WIFI_NOT_CONNECTED\"}");
    }
    const std::string url = request->getParam("url")->value().c_str();
    std::string target;
    std::string guard_error;
    if (!fm_validate_fetch_request(
            url,
            request->hasParam("target") ? request->getParam("target")->value().c_str() : "auto",
            request->hasParam("cwd") ? request->getParam("cwd")->value().c_str() : "/ESPUSER",
            &target,
            &guard_error)) {
      const int status = guard_error == "PERMISSION_DENIED" ? 403 : 400;
      return request->send(status, "application/json",
                           String("{\"success\":false,\"error\":\"") +
                               guard_error.c_str() + "\"}");
    }
    auto* args = new (std::nothrow) FileDownloadTaskArgs();
    if (args == nullptr) {
      return request->send(500, "application/json",
                           "{\"success\":false,\"error\":\"NO_MEMORY\"}");
    }
    args->url = url;
    args->target = target;
    args->temp_path = fm_temp_path_for(target, "download", 0U);
    portENTER_CRITICAL(&g_file_download_mux);
    g_file_download_status = {};
    g_file_download_status.active = true;
    g_file_download_status.started_ms = mros::platform::mros_millis();
    g_file_download_status.updated_ms = g_file_download_status.started_ms;
    strncpy(g_file_download_status.url, url.c_str(), sizeof(g_file_download_status.url) - 1U);
    strncpy(g_file_download_status.target, target.c_str(), sizeof(g_file_download_status.target) - 1U);
    strncpy(g_file_download_status.temp_path, args->temp_path.c_str(),
            sizeof(g_file_download_status.temp_path) - 1U);
    strncpy(g_file_download_status.phase, "queued", sizeof(g_file_download_status.phase) - 1U);
    strncpy(g_file_download_status.guard, "queued", sizeof(g_file_download_status.guard) - 1U);
    portEXIT_CRITICAL(&g_file_download_mux);
    BaseType_t created = xTaskCreatePinnedToCore(
        fm_download_task, "fm_downloader", 6144, args, 4, nullptr,
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
        0
#else
        tskNO_AFFINITY
#endif
    );
    if (created != pdPASS) {
      delete args;
      fm_download_set_status("failed", false, true, false, 0, -1, 0, "TASK_FAILED");
      return request->send(500, "application/json",
                           "{\"success\":false,\"error\":\"TASK_FAILED\"}");
    }
    String json = "{\"success\":true,\"target\":\"";
    json += fm_public_path_json(target);
    json += "\"}";
    request->send(200, "application/json", json);
  });

  server.on("/api/files/fetch/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    request->send(200, "application/json", fm_download_status_json());
  });

  server.on("/api/files/fetch/cancel", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401, "application/json",
                           "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
    bool active = false;
    portENTER_CRITICAL(&g_file_download_mux);
    active = g_file_download_status.active;
    if (active) {
      g_file_download_status.cancel_requested = true;
      std::snprintf(g_file_download_status.phase, sizeof(g_file_download_status.phase),
                    "%s", "cancelling");
      g_file_download_status.updated_ms = mros::platform::mros_millis();
    }
    portEXIT_CRITICAL(&g_file_download_mux);
    if (!active) {
      return request->send(409, "application/json",
                           "{\"success\":false,\"error\":\"NO_ACTIVE_DOWNLOAD\"}");
    }
    request->send(200, "application/json", "{\"success\":true,\"cancel_requested\":true}");
  });

  server.on(
      "/api/files/upload", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        // Body callback handles the raw upload stream.
      },
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
         size_t index, size_t total) {
        if (!isAuthenticated(request)) {
          request->send(401, "application/json",
                        "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
          return;
        }
        if (index == 0U) {
          if (!request->hasParam("path")) {
            request->send(400, "application/json",
                          "{\"success\":false,\"error\":\"MISSING_PATH\"}");
            return;
          }
          const std::string path = fm_normalize_path(request->getParam("path")->value().c_str());
          if (fm_is_remote_path(path)) {
            request->send(
                409,
                "application/json",
                fm_remote_error_json(path, "upload", "PEER_PROTOCOL_MISSING", "remote upload requires t41 MSHELL2 FS protocol"));
            return;
          }
          if (!fm_is_user_write_target(path)) {
            request->send(403, "application/json",
                          "{\"success\":false,\"error\":\"PERMISSION_DENIED\"}");
            return;
          }
          struct stat parent_info {};
          if (!fm_stat_logical(fm_parent_path(path), &parent_info) ||
              !S_ISDIR(parent_info.st_mode)) {
            request->send(404, "application/json",
                          "{\"success\":false,\"error\":\"PARENT_NOT_FOUND\"}");
            return;
          }
          String busy_json;
          int status_code = 409;
          FileUploadContext* context =
              fm_acquire_upload_context(request, path, "upload", &busy_json, &status_code);
          if (context == nullptr) {
            request->send(status_code, "application/json", busy_json);
            return;
          }
          context->file = mros::platform::mros_fs_open(context->temp_path.c_str(), "wb");
          if (context->file == nullptr) {
            fm_release_upload_context(context);
            request->send(500, "application/json",
                          "{\"success\":false,\"error\":\"OPEN_FAILED\"}");
            return;
          }
        }
        FileUploadContext* context = fm_find_upload_context(request);
        if (context == nullptr || context->failed || context->file == nullptr) {
          return;
        }
        if (len > 0U && fwrite(data, 1U, len, context->file) != len) {
          context->failed = true;
          fm_release_upload_context(context);
          request->send(500, "application/json",
                        "{\"success\":false,\"error\":\"WRITE_FAILED\"}");
          return;
        }
        context->written += len;
        if ((index + len) >= total) {
          const std::string final_path = context->path;
          const size_t written = context->written;
          if (!fm_finalize_upload_context(context)) {
            fm_release_upload_context(context);
            request->send(500, "application/json",
                          "{\"success\":false,\"error\":\"RENAME_FAILED\"}");
            return;
          }
          fm_release_upload_context(context, true);
          String json = "{\"success\":true,\"path\":\"";
          json += fm_public_path_json(final_path);
          json += "\",\"bytes\":";
          json += String(static_cast<uint32_t>(written));
          json += "}";
          request->send(200, "application/json", json);
        }
      });

  server.on(
      "/api/files/save", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        if (!isAuthenticated(request)) {
          return request->send(401, "application/json",
                               "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
        }
        if (request->hasHeader("Content-Length") &&
            request->header("Content-Length").toInt() > 0) {
          return;
        }
        if (!request->hasParam("path")) {
          return request->send(400, "application/json",
                               "{\"success\":false,\"error\":\"MISSING_PATH\"}");
        }
        const std::string path = fm_normalize_path(request->getParam("path")->value().c_str());
        if (fm_is_remote_path(path)) {
          return request->send(
              409,
              "application/json",
              fm_remote_error_json(path, "save", "PEER_PROTOCOL_MISSING", "remote save requires t41 MSHELL2 FS protocol"));
        }
        if (!fm_is_user_write_target(path)) {
          return request->send(403, "application/json",
                               "{\"success\":false,\"error\":\"PERMISSION_DENIED\"}");
        }
        struct stat parent_info {};
        if (!fm_stat_logical(fm_parent_path(path), &parent_info) ||
            !S_ISDIR(parent_info.st_mode)) {
          return request->send(404, "application/json",
                               "{\"success\":false,\"error\":\"PARENT_NOT_FOUND\"}");
        }
        if (fm_target_busy(path)) {
          return request->send(409, "application/json",
                               "{\"success\":false,\"error\":\"TARGET_BUSY\"}");
        }
        String stale_json;
        int stale_status = 409;
        if (!fm_save_precondition_ok(request, path, &stale_json, &stale_status)) {
          return request->send(stale_status, "application/json", stale_json);
        }
        if (!logger_write_text_file_atomic(String(path.c_str()), "")) {
          return request->send(500, "application/json",
                               "{\"success\":false,\"error\":\"WRITE_FAILED\"}");
        }
        request->send(200, "application/json", fm_save_success_json(path, 0U));
      },
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
         size_t index, size_t total) {
        if (!isAuthenticated(request)) {
          request->send(401, "application/json",
                        "{\"success\":false,\"error\":\"UNAUTHORIZED\"}");
          return;
        }
        if (index == 0U) {
          if (!request->hasParam("path")) {
            request->send(400, "application/json",
                          "{\"success\":false,\"error\":\"MISSING_PATH\"}");
            return;
          }
          const std::string path = fm_normalize_path(request->getParam("path")->value().c_str());
          if (fm_is_remote_path(path)) {
            request->send(
                409,
                "application/json",
                fm_remote_error_json(path, "save", "PEER_PROTOCOL_MISSING", "remote save requires t41 MSHELL2 FS protocol"));
            return;
          }
          if (!fm_is_user_write_target(path)) {
            request->send(403, "application/json",
                          "{\"success\":false,\"error\":\"PERMISSION_DENIED\"}");
            return;
          }
          struct stat parent_info {};
          if (!fm_stat_logical(fm_parent_path(path), &parent_info) ||
              !S_ISDIR(parent_info.st_mode)) {
            request->send(404, "application/json",
                          "{\"success\":false,\"error\":\"PARENT_NOT_FOUND\"}");
            return;
          }
          String busy_json;
          int status_code = 409;
          String stale_json;
          int stale_status = 409;
          if (!fm_save_precondition_ok(request, path, &stale_json, &stale_status)) {
            request->send(stale_status, "application/json", stale_json);
            return;
          }
          uint32_t expected_mtime = 0U;
          const FmExpectedMtimeStatus expected_status =
              fm_request_expected_mtime(request, &expected_mtime);
          FileUploadContext* context =
              fm_acquire_upload_context(request, path, "save", &busy_json, &status_code);
          if (context == nullptr) {
            request->send(status_code, "application/json", busy_json);
            return;
          }
          if (expected_status == FmExpectedMtimeStatus::kValid) {
            context->has_expected_mtime = true;
            context->expected_mtime = expected_mtime;
          }
          context->file = mros::platform::mros_fs_open(context->temp_path.c_str(), "wb");
          if (context->file == nullptr) {
            fm_release_upload_context(context);
            request->send(500, "application/json",
                          "{\"success\":false,\"error\":\"OPEN_FAILED\"}");
            return;
          }
        }
        FileUploadContext* context = fm_find_upload_context(request);
        if (context == nullptr || context->failed || context->file == nullptr) {
          return;
        }
        if (len > 0U && fwrite(data, 1U, len, context->file) != len) {
          context->failed = true;
          fm_release_upload_context(context);
          request->send(500, "application/json",
                        "{\"success\":false,\"error\":\"WRITE_FAILED\"}");
          return;
        }
        context->written += len;
        if ((index + len) >= total) {
          const std::string final_path = context->path;
          const size_t written = context->written;
          if (context->has_expected_mtime) {
            context->close();
            String commit_stale_json;
            if (!fm_expected_mtime_matches(final_path, context->expected_mtime,
                                           &commit_stale_json)) {
              fm_release_upload_context(context);
              request->send(409, "application/json", commit_stale_json);
              return;
            }
          }
          if (!fm_finalize_upload_context(context)) {
            fm_release_upload_context(context);
            request->send(500, "application/json",
                          "{\"success\":false,\"error\":\"RENAME_FAILED\"}");
            return;
          }
          fm_release_upload_context(context, true);
          request->send(200, "application/json", fm_save_success_json(final_path, written));
        }
      });

  ESP_LOGI("WEB", "Routes registered: controls console");

  server.on("/api/spi/errors", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String json = spi_s3_get_error_log_json();
    request->send(200, "application/json", json);
  });

  server.on("/api/c3/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    String json = "{\"crc_errors\":" + String(spi_c3_get_crc_errors()) + 
                  ",\"marker_errors\":" + String(spi_c3_get_marker_errors()) +
                  ",\"total_rx\":" + String(spi_c3_get_total_rx()) +
                  ",\"failsafe_option_tx\":" + String(spi_c3_get_failsafe_option()) +
                  ",\"cmd_flags_tx\":" + String(spi_c3_get_cmd_flags()) +
                  ",\"pid_setpoint_x100_tx\":" + String(spi_c3_get_pid_setpoint_x100()) +
                  ",\"connected\":" + String(spi_c3_is_connected() ? "true" : "false") + "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    logger_flush_pending();
    const String logs_path = logger_user_path("logs.csv");
    if (!web_async_send_littlefs_stream_psram(request, logs_path.c_str(),
                                              "text/csv")) {
      request->send(500, "application/json",
                    "{\"success\":false,\"error\":\"log stream failed\"}");
    }
  });

  server.on("/api/logs/tail", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    size_t max_bytes = 8192U;
    if (request->hasParam("bytes")) {
      const long requested = request->getParam("bytes")->value().toInt();
      if (requested > 0L) {
        max_bytes = static_cast<size_t>(std::min<long>(requested, 32768L));
      }
    }
    bool wants_text = false;
    if (request->hasParam("format")) {
      String format = request->getParam("format")->value();
      format.toLowerCase();
      wants_text = (format == "text" || format == "raw" || format == "plain");
    }
    if (!wants_text && request->hasHeader("Accept")) {
      String accept = request->header("Accept");
      accept.toLowerCase();
      wants_text = accept.indexOf("text/plain") >= 0;
    }
    request->send(200,
                  wants_text ? "text/plain; charset=utf-8" : "text/csv",
                  logger_read_csv_tail(max_bytes));
  });

  server.on("/api/svg", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request->hasParam("id")) {
      return request->send(400, "text/plain", "Missing id");
    }
    String id = request->getParam("id")->value();
    const char *path = nullptr;
    if (id == "esp32_pins") path = "/assets/svg/esp32_pins.svg";
    if (id == "esp32_s3") path = "/assets/svg/esp32_s3.svg";
    if (id == "mg996r") path = "/assets/svg/mg996r.svg";
    if (id == "pca9685_v") path = "/assets/svg/pca9685_v.svg";
    if (id == "pca9685") path = "/assets/svg/pca9685.svg";
    if (id == "t41f429i") path = "/assets/svg/t41f429i.svg";
    if (id == "omron_encoder") path = "/assets/svg/omron_encoder.svg";
    if (id == "xl4015") path = "/assets/svg/xl4015.svg";
    if (id == "esp32_t41") path = "/assets/svg/esp32_t41.svg";
    if (id == "esp32_t41_back") path = "/assets/svg/esp32_t41_back.svg";
    if (id == "xl4016") path = "/assets/svg/xl4016.svg";
    if (!path) {
      request->send(404, "text/plain", "SVG not found");
      return;
    }
    send_fs_asset(request, path, "image/svg+xml", false);
  });

  server.on("/api/config/download", HTTP_GET,
            [](AsyncWebServerRequest *request) {
              if (!isAuthenticated(request))
                return request->send(401);
              String data = config_read_json();
              request->send(200, "application/json", data);
            });

  server.on(
      "/api/config/upload", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        // Stub for POST payload handler: handled by AsyncBody
      },
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
         size_t index, size_t total) {
        if (!isAuthenticated(request)) {
          request->send(401);
          return;
        }
        static String json_buffer = "";
        if (index == 0)
          json_buffer = "";

        for (size_t i = 0; i < len; i++) {
          json_buffer += (char)data[i];
        }

        if (index + len == total) {
          config_write_json(json_buffer);
          request->send(200, "application/json", "{\"success\":true}");
        }
      });

  server.on(
      "/api/calibration/save", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        // Stub for POST payload handler: handled by AsyncBody
      },
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
         size_t index, size_t total) {
        if (!isAuthenticated(request)) {
          request->send(401);
          return;
        }
        static String json_buffer = "";
        if (index == 0)
          json_buffer = "";

        for (size_t i = 0; i < len; i++) {
          json_buffer += (char)data[i];
        }

        if (index + len == total) {
          // Assuming same JSON structure logic handling inside logger_driver
          config_write_json(json_buffer);
          request->send(
              200, "application/json",
              "{\"success\":true, \"message\":\"Calibration Saved\"}");
        }
      });

  // --- Change Credentials API ---
  // Requires an active session. Accepts new_user + new_pass as POST form params.
  // Immediately invalidates the current session token so the user must re-login.
  server.on("/api/credentials", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request))
      return request->send(401);
    if (!request_reauth_password_ok(request)) {
      return request->send(403, "application/json", auth_json_error("REAUTH_REQUIRED"));
    }
    if (!request->hasParam("new_user", true) ||
        !request->hasParam("new_pass", true)) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"new_user and new_pass required\"}");
    }
    String new_user = request->getParam("new_user", true)->value();
    String new_pass = request->getParam("new_pass", true)->value();
    String new_pass2 = request->hasParam("new_pass2", true)
                           ? request->getParam("new_pass2", true)->value()
                           : new_pass;
    new_user.trim();
    if (!mros::ssh::is_valid_username(new_user) || new_user == mros::ssh::root_username()) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"INVALID_USERNAME\"}");
    }
    if (new_pass != new_pass2) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"PASSWORD_CONFIRM_MISMATCH\"}");
    }
    if (new_pass.length() < 8 || new_pass.length() > kAuthPassMaxLen) {
      return request->send(400, "application/json",
                           "{\"success\":false,\"error\":\"PASSWORD_POLICY\"}");
    }
    String old_user = auth_current_username_copy();
    if (old_user.length() == 0U) {
      refresh_current_username_from_credentials();
      old_user = auth_current_username_copy();
    }
    if (old_user.length() == 0U) {
      return request->send(401, "application/json", auth_json_error("AUTH_REQUIRED"));
    }
    const mros::ssh::IdentityConfig identity = mros::ssh::identity_get();
    const bool rename_requested = new_user != old_user;
    if (rename_requested) {
      if (!web_current_user_is_admin()) {
        return request->send(403, "application/json", auth_json_error("ADMIN_REQUIRED"));
      }
      if (old_user != identity.username) {
        return request->send(409, "application/json", auth_json_error("PRIMARY_RENAME_ONLY"));
      }
      if (mros::ssh::user_exists(new_user)) {
        return request->send(409, "application/json", auth_json_error("USER_EXISTS"));
      }
      if (!mros::ssh::set_username(new_user)) {
        return request->send(400, "application/json", auth_json_error("USERNAME_UPDATE_FAILED"));
      }
      (void)mros::ssh::set_display_name(new_user);
      migrate_user_popup_settings_on_username_change(old_user, new_user);
    }
    if (!mros::ssh::set_password_for_user(new_user, new_pass)) {
      return request->send(400, "application/json", auth_json_error("PASSWORD_UPDATE_FAILED"));
    }
    const mros::ssh::IdentityConfig updated_identity = mros::ssh::identity_get();
    if (new_user == updated_identity.username) {
      if (!web_auth_save_primary_credentials(new_user, new_pass)) {
        return request->send(500, "application/json",
                             auth_json_error("Kimlik bilgisi kaydedilemedi."));
      }
    }
    auth_set_current_username(new_user);
    // Invalidate the current session: the user must re-login with new credentials.
    web_server_logout_all();
    login_fail_count = 0;
    login_block_until_ms = 0;
    web_security_audit("legacy-credentials-update", new_user);
    AsyncWebServerResponse *response = request->beginResponse(
        200, "application/json",
        "{\"success\":true,\"message\":\"Credentials updated. Please log in again.\"}");
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Set-Cookie", session_cookie_header("", true));
    request->send(response);
  });

  ESP_LOGI("WEB", "Routes registered: all");
  start_web_server_once("final");
}

void web_server_fk_loop() {
  bool do_compute = false;
  float local_targets[7] = {0.0f};

  portENTER_CRITICAL(&live_fk_mux);
  if (live_preview_enabled && live_fk_dirty) {
    do_compute = true;
    for (int i = 0; i < 7; i++) {
      local_targets[i] = fk_target_angles[i];
    }
  }
  portEXIT_CRITICAL(&live_fk_mux);

  if (!do_compute) return;

  FK_Result_t fk_local {};
  uint32_t fk_t0 = static_cast<uint32_t>(mros::platform::mros_micros());
  fk_compute(local_targets, &fk_local);
  uint32_t fk_dt_us = static_cast<uint32_t>(mros::platform::mros_micros()) - fk_t0;
  const float fk_last_ms_local = (float)fk_dt_us / 1000.0f;

  portENTER_CRITICAL(&live_fk_mux);
  live_fk_result = fk_local;
  dbg_fk_last_ms = fk_last_ms_local;
  dbg_fk_samples++;
  if (dbg_fk_samples == 1) {
    dbg_fk_avg_ms = dbg_fk_last_ms;
    dbg_fk_max_ms = dbg_fk_last_ms;
  } else {
    dbg_fk_avg_ms += (dbg_fk_last_ms - dbg_fk_avg_ms) / (float)dbg_fk_samples;
    if (dbg_fk_last_ms > dbg_fk_max_ms) {
      dbg_fk_max_ms = dbg_fk_last_ms;
    }
  }
  live_fk_dirty = false;
  portEXIT_CRITICAL(&live_fk_mux);
}

void web_server_set_fk_task_handle(TaskHandle_t task_handle) {
  g_fk_task_handle = task_handle;
}

void web_server_set_runtime_task_handle(TaskHandle_t task_handle) {
  g_runtime_task_handle = task_handle;
}

void web_server_notify_runtime_task() {
  if (g_runtime_task_handle != nullptr) {
    xTaskNotifyGive(g_runtime_task_handle);
  }
}

void web_server_notify_fk_task() {
  if (g_fk_task_handle != nullptr) {
    xTaskNotifyGive(g_fk_task_handle);
  }
}

void web_server_loop() {
  unsigned long now = mros::platform::mros_millis();
  logger_service(now);
  pm_probe_external_motion();
  pm_service(now);

  ws.cleanupClients();
  ws_telemetry.cleanupClients();
  ws_shell.cleanupClients();
  ws_shell_v2.cleanupClients();
  ws_debug.cleanupClients();
  ws_mcp.cleanupClients();
  closeExpiredWsAuthClients(ws_auth_clients, ws_auth_deadlines, ws, &ws_telemetry, now);
  closeExpiredWsAuthClients(ws_shell_auth_clients, ws_shell_auth_deadlines, ws_shell, &ws_shell_v2, now);
  closeExpiredWsAuthClients(ws_debug_auth_clients, ws_debug_auth_deadlines, ws_debug, nullptr, now);
  closeExpiredWsAuthClients(ws_mcp_auth_clients, ws_mcp_auth_deadlines, ws_mcp, nullptr, now);

  {
    uint32_t shell_client_id = 0;
    char *shell_json = ensure_shell_json_forward_buffer();
    mros::shell::service::ShellWebOutboundKind shell_kind =
        mros::shell::service::ShellWebOutboundKind::TextJson;
    size_t shell_payload_len = 0U;
    while (shell_json != nullptr &&
           mros::shell::service::dequeue_web_outbound(
               &shell_client_id,
               &shell_kind,
               reinterpret_cast<uint8_t*>(shell_json),
               kShellJsonForwardCapacity,
               &shell_payload_len)) {
      AsyncWebSocketClient *target = nullptr;
      if (shell_ws_is_encoded_client_id(shell_client_id)) {
        target = findWsClientById(ws_shell, ws_shell_v2,
                                  shell_ws_decode_client_id(shell_client_id));
      } else {
        target = findWsClientById(ws, ws_telemetry, shell_client_id);
      }
      if (target != nullptr && target->status() == WS_CONNECTED) {
        if (shell_kind == mros::shell::service::ShellWebOutboundKind::BinaryFrame) {
          target->binary(reinterpret_cast<const uint8_t*>(shell_json), shell_payload_len);
        } else {
          target->text(shell_json, shell_payload_len);
        }
      }
    }
  }

  mros::rtos::dpm::PolicyDecision dpm_decision {};
  mros::rtos::dpm::get_policy_decision(&dpm_decision);
  const uint32_t telemetry_fast_period_ms =
      dpm_decision.telemetry_fast_period_ms > 0U
          ? dpm_decision.telemetry_fast_period_ms
          : 50U;
  if (mros::platform::mros_millis() - last_ws_update > telemetry_fast_period_ms) {
    unsigned long now = mros::platform::mros_millis();
    last_ws_update = now;
    const bool medium_due =
        (last_ws_medium_update == 0UL) ||
        (now - last_ws_medium_update >= dpm_decision.telemetry_medium_period_ms);
    const bool slow_due =
        (last_ws_slow_update == 0UL) ||
        (now - last_ws_slow_update >= dpm_decision.telemetry_slow_period_ms);
    if (medium_due) last_ws_medium_update = now;
    if (slow_due) last_ws_slow_update = now;

    // Cache variables for Frontend / 3D viewer integration
    fw_var_turret = spi_s3_get_turret_deg();
    for (int i = 0; i < 6; i++)
      fw_var_joints[i] = spi_s3_get_joint_deg(i);
    fw_var_gripper = spi_s3_get_gripper();

    FK_Result_t live_fk_snapshot {};
    bool live_preview_snapshot = false;
    portENTER_CRITICAL(&live_fk_mux);
    live_fk_snapshot = live_fk_result;
    live_preview_snapshot = live_preview_enabled;
    portEXIT_CRITICAL(&live_fk_mux);

    char ik_compute_preference[16] = {};
    WebRobotUiCommand robot_ui_command {};
    WebRobotMathState robot_math_state {};
    portENTER_CRITICAL(&robot_ui_mux);
    strncpy(ik_compute_preference, g_ik_compute_preference, sizeof(ik_compute_preference) - 1U);
    ik_compute_preference[sizeof(ik_compute_preference) - 1U] = '\0';
    robot_ui_command = g_robot_ui_command;
    robot_math_state = g_robot_math_state;
    portEXIT_CRITICAL(&robot_ui_mux);

    if ((ws.count() + ws_telemetry.count()) > 0) {
      static bool telemetry_cache_valid = false;

      static int32_t prev_turret_q = 0;
      static int32_t prev_joint_q[6] = {0, 0, 0, 0, 0, 0};
      static int32_t prev_gripper_q = 0;
      static uint32_t prev_t41_loop_ms = 0;
      static uint32_t prev_uptime = 0;
      static int32_t prev_motor = 0;
      static uint32_t prev_spi_total = 0;
      static uint32_t prev_spi_crc_err = 0;
      static uint32_t prev_spi_marker_err = 0;
      static uint32_t prev_spi_err_rev = 0;
      static int32_t prev_spi_last_marker = 0;
      static int8_t prev_spi_connected = 0;
      static int8_t prev_espnow_connected = 0;
      static String prev_ik_backend = "";
      static String prev_ik_pref = "";
      static uint32_t prev_robot_ui_command_rev = 0;
      static uint32_t prev_robot_math_rev = 0;
      static int32_t prev_coord_x_q = 0;
      static int32_t prev_coord_y_q = 0;
      static int32_t prev_coord_z_q = 0;
      static int32_t prev_coord_roll_q = 0;
      static int32_t prev_coord_pitch_q = 0;
      static int32_t prev_coord_yaw_q = 0;
      static int32_t prev_alpha_q = 0;
      static int32_t prev_lp = 0;
      static int32_t prev_fk_x_q = 0;
      static int32_t prev_fk_y_q = 0;
      static int32_t prev_fk_z_q = 0;
      static int32_t prev_fk_a_q = 0;
      static int32_t prev_c3_pos_q = 0;
      static int32_t prev_c3_spd_q = 0;
      static int32_t prev_c3_acc_q = 0;
      static int8_t prev_c3_connected = 0;
      static int8_t prev_c3_espnow_active = 0;
      static uint32_t prev_c3_crc_err = 0;
      static uint32_t prev_c3_marker_err = 0;
      static uint32_t prev_c3_total_rx = 0;
      static int32_t prev_c3_quality_q = 0;
      static int32_t prev_c3_hz = -1;
      static int32_t prev_s3_devstat = -1;
      static int32_t prev_pid_out_q = 0;
      static uint32_t prev_console_rev = 0;
      static int8_t prev_pca_ready = 0;
      static int8_t prev_wifi_ap = 0;
      static int32_t prev_traj_scale_q = 0;
      static int32_t prev_oe = 0;

      bool has_auth_client = false;
      bool has_pending_full = false;
      bool has_scene_client = false;
      bool has_fast_client = false;
      bool has_medium_client = false;
      bool has_slow_client = false;
      bool has_bin_client = false;
      uint32_t scene_client_count = 0;
      uint32_t debug_client_count = 0;
      uint32_t background_client_count = 0;
      ws_auth_lock();
      for (auto it = ws_auth_clients.begin(); it != ws_auth_clients.end(); ++it) {
        if (!it->second) continue;
        has_auth_client = true;
        uint8_t profile = WS_SUB_RATE_DEFAULT;
        auto mit = ws_subscription_masks.find(it->first);
        if (mit != ws_subscription_masks.end()) profile = mit->second;
        has_fast_client = has_fast_client || ((profile & WS_SUB_RATE_FAST) != 0U);
        has_medium_client = has_medium_client || ((profile & WS_SUB_RATE_MEDIUM) != 0U);
        has_slow_client = has_slow_client || ((profile & WS_SUB_RATE_SLOW) != 0U);
        if ((profile & WS_SUB_RATE_FAST) == 0U) background_client_count++;
        if ((profile & WS_SUB_DEBUG) != 0U) debug_client_count++;
        auto fit = ws_full_snapshot_pending.find(it->first);
        if (fit != ws_full_snapshot_pending.end() && fit->second) {
          has_pending_full = true;
        }
        auto sit = ws_scene_subscriptions.find(it->first);
        if (sit != ws_scene_subscriptions.end() && sit->second) {
          has_scene_client = true;
          scene_client_count++;
        }
        auto fmt_it = ws_telemetry_formats.find(it->first);
        if (fmt_it != ws_telemetry_formats.end() &&
            fmt_it->second == WS_TELEMETRY_FORMAT_BIN_V1) {
          has_bin_client = true;
        }
      }
      ws_auth_unlock();
      g_ws_scene_client_count = scene_client_count;
      g_ws_debug_client_count = debug_client_count;
      g_ws_background_client_count = background_client_count;

      const uint32_t budget_free =
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const uint32_t budget_largest =
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      g_ws_memory_budget_last_free = budget_free;
      g_ws_memory_budget_last_largest = budget_largest;
      const bool budget_critical =
          budget_free < (64U * 1024U) || budget_largest < (16U * 1024U);
      const bool budget_degraded =
          budget_critical || budget_free < (80U * 1024U) ||
          budget_largest < (24U * 1024U);
      g_ws_memory_budget_mode = budget_critical ? "critical" :
                                (budget_degraded ? "degraded" : "normal");
      if (has_auth_client && budget_degraded) {
        g_ws_memory_budget_degrade_frames++;
      }
      if (has_auth_client && budget_critical) {
        g_ws_memory_budget_critical_frames++;
      }
      if (has_scene_client && budget_degraded && !slow_due && !has_pending_full) {
        has_scene_client = false;
        g_ws_memory_budget_scene_suppressed++;
        g_ws_telemetry_budget_deferred++;
      }
      if (has_fast_client && budget_critical && !slow_due && !has_pending_full) {
        has_fast_client = false;
        g_ws_memory_budget_fast_suppressed++;
        g_ws_telemetry_budget_dropped++;
      }
      if (has_medium_client && budget_critical && !slow_due && !has_pending_full) {
        has_medium_client = false;
        g_ws_memory_budget_medium_suppressed++;
        g_ws_telemetry_budget_deferred++;
      }

      if (has_auth_client && !has_scene_client) g_ws_gated_scene_frames++;
      if (has_auth_client && medium_due && !has_medium_client) g_ws_gated_medium_frames++;
      if (has_auth_client && slow_due && !has_slow_client) g_ws_gated_slow_frames++;

      const bool build_fast_delta = has_fast_client;
      const bool build_medium_delta = has_medium_client && medium_due;
      const bool build_slow_delta = has_slow_client && slow_due;

      if (has_auth_client) {
        const uint32_t spi_crc_err = spi_s3_get_crc_errors();
        const uint32_t spi_marker_err = spi_s3_get_marker_errors();
        const uint32_t spi_err_rev = spi_crc_err + spi_marker_err;

        const float turret = fw_var_turret;
        const float gripper = fw_var_gripper;
        const uint32_t t41_loop_ms = spi_s3_get_loop_ms();
        const uint32_t uptime_s = now / 1000;
        const int32_t motor = spi_s3_get_motor_state();
        const uint32_t spi_total = spi_s3_get_total_transactions();
        const int32_t spi_last_marker = spi_s3_get_last_rx_marker();
        const bool spi_connected = spi_s3_is_connected();
        const bool espnow_connected = spi_c3_is_espnow_connected();
        const String ik_backend = spi_connected
                                      ? "T41-QSPI"
                                      : (espnow_connected ? "T41-ESP-NOW" : "WEB");
        const String ik_pref = String(ik_compute_preference[0] != '\0' ? ik_compute_preference : "auto");
        float coord_x = 0.0f;
        float coord_y = 0.0f;
        float coord_z = 0.0f;
        float coord_roll = 0.0f;
        float coord_pitch = 0.0f;
        float coord_yaw = 0.0f;
        float alpha = 0.0f;
        int32_t lp = 0;
        float fk_x = 0.0f;
        float fk_y = 0.0f;
        float fk_z = 0.0f;
        float fk_a = 0.0f;
        if (has_scene_client) {
          coord_x = spi_s3_get_coord_x();
          coord_y = spi_s3_get_coord_y();
          coord_z = spi_s3_get_coord_z();
          coord_roll = spi_s3_get_coord_roll();
          coord_pitch = spi_s3_get_coord_pitch();
          coord_yaw = spi_s3_get_coord_yaw();
          alpha = coord_pitch;
          lp = live_preview_snapshot ? 1 : 0;
          fk_x = live_fk_snapshot.x;
          fk_y = live_fk_snapshot.y;
          fk_z = live_fk_snapshot.z;
          fk_a = live_fk_snapshot.alpha_deg;
        }
        const float c3_pos = spi_c3_get_position_deg();
        float c3_spd = spi_c3_get_speed_deg_s();
        float c3_acc = spi_c3_get_accel_deg_s2();
        const bool c3_connected = spi_c3_is_connected();
        const bool c3_espnow_active =
            spi_c3_is_espnow_active() ||
            (spi_c3_get_failsafe_option() == C3_FAILSAFE_C3SPI_T41_ESPNOW);
        const uint32_t c3_crc_err = spi_c3_get_crc_errors();
        const uint32_t c3_marker_err = spi_c3_get_marker_errors();
        const uint32_t c3_total = spi_c3_get_total_rx();
        const uint32_t c3_hz = (uint32_t)spi_c3_get_loop_hz();
        const int32_t s3_devstat = (int32_t)spi_s3_get_device_status_code();
        if (!c3_connected) {
          c3_spd = 0.0f;
          c3_acc = 0.0f;
        }
        if (fabsf(c3_spd) < 0.55f) c3_spd = 0.0f;
        if (fabsf(c3_acc) < 0.55f) c3_acc = 0.0f;
        float c3_quality = 100.0f;
        if (c3_total > 0) {
          c3_quality =
              100.0f - (100.0f * (float)c3_marker_err / (float)c3_total);
          if (c3_quality < 0.0f) c3_quality = 0.0f;
        }
        const float pid_out = spi_s3_get_turret_pid_output();
        const uint32_t console_rev = uart1_cobs_get_log_version();
        const bool pca_ready = pca9685_is_ready();
        const bool wifi_ap = wifi_manager_state().ap_active;
        const float traj_scale = spi_s3_get_joint_traj_time_scale();
        const int32_t oe = pca9685_get_output_enable() ? 1 : 0;

        auto q1 = [](float v) -> int32_t {
          return static_cast<int32_t>(lroundf(v * 10.0f));
        };
        auto q2 = [](float v) -> int32_t {
          return static_cast<int32_t>(lroundf(v * 100.0f));
        };

        PsramJsonWriter fast_json(1024U);
        PsramJsonWriter medium_json(2048U);
        PsramJsonWriter slow_json(768U);
        PsramJsonWriter sidecar_json(1024U);
        fast_json.begin();
        medium_json.begin();
        slow_json.begin();
        if (has_bin_client) sidecar_json.begin();
        bool fast_has = false;
        bool medium_has = false;
        bool slow_has = false;
        bool sidecar_has = false;
        auto delta_append_raw = [&](PsramJsonWriter& writer, bool& has,
                                    const char *key, const char *value) {
          writer.raw_field(key, value);
          has = true;
        };
        auto delta_append_raw_string = [&](PsramJsonWriter& writer, bool& has,
                                           const char *key, const String &value) {
          writer.raw_field(key, value);
          has = true;
        };
        auto delta_append_string = [&](PsramJsonWriter& writer, bool& has,
                                       const char *key, const char *value) {
          writer.string_field(key, value);
          has = true;
        };
        auto delta_append_bool = [&](PsramJsonWriter& writer, bool& has,
                                     const char *key, bool cur, int8_t &prev) {
          const int8_t cv = cur ? 1 : 0;
          if (!telemetry_cache_valid || cv != prev) {
            prev = cv;
            writer.bool_field(key, cur);
            has = true;
          }
        };
        auto delta_append_i32 = [&](PsramJsonWriter& writer, bool& has,
                                    const char *key, int32_t cur, int32_t &prev) {
          if (!telemetry_cache_valid || cur != prev) {
            prev = cur;
            writer.i32_field(key, cur);
            has = true;
          }
        };
        auto delta_append_u32 = [&](PsramJsonWriter& writer, bool& has,
                                    const char *key, uint32_t cur, uint32_t &prev) {
          if (!telemetry_cache_valid || cur != prev) {
            prev = cur;
            writer.u32_field(key, cur);
            has = true;
          }
        };
        auto delta_append_f1 = [&](PsramJsonWriter& writer, bool& has,
                                   const char *key, float cur, int32_t &prev_q) {
          const int32_t q = q1(cur);
          if (!telemetry_cache_valid || q != prev_q) {
            prev_q = q;
            writer.float_field(key, cur, 1U);
            has = true;
          }
        };
        auto delta_append_f2 = [&](PsramJsonWriter& writer, bool& has,
                                   const char *key, float cur, int32_t &prev_q) {
          const int32_t q = q2(cur);
          if (!telemetry_cache_valid || q != prev_q) {
            prev_q = q;
            writer.float_field(key, cur, 2U);
            has = true;
          }
        };

        if (has_bin_client && has_pending_full) {
          delta_append_string(sidecar_json, sidecar_has, "ik_backend",
                              ik_backend.c_str());
          delta_append_string(sidecar_json, sidecar_has, "ik_pref",
                              ik_pref.c_str());
          if (robot_ui_command.revision != 0U) {
            delta_append_raw_string(sidecar_json, sidecar_has, "robot_cmd",
                                    robot_ui_command_to_json(robot_ui_command));
          }
          if (robot_math_state.revision != 0U) {
            delta_append_raw_string(sidecar_json, sidecar_has, "robot_math",
                                    robot_math_state_to_json(robot_math_state));
          }
        }

        if (build_fast_delta || !telemetry_cache_valid) {
          delta_append_f1(fast_json, fast_has, "turret", turret, prev_turret_q);
        }
        bool joints_changed = has_scene_client && !telemetry_cache_valid;
        int32_t joints_q[6] = {};
        if (has_scene_client) {
          for (int i = 0; i < 6; i++) {
            joints_q[i] = q1(fw_var_joints[i]);
            if (joints_q[i] != prev_joint_q[i]) joints_changed = true;
          }
        }
        if ((build_fast_delta || !telemetry_cache_valid) && has_scene_client && joints_changed) {
          for (int i = 0; i < 6; i++) prev_joint_q[i] = joints_q[i];
          String joints_json = "[" + String(fw_var_joints[0], 1);
          joints_json.reserve(64U);
          for (int i = 1; i < 6; i++) joints_json += "," + String(fw_var_joints[i], 1);
          joints_json += "]";
          delta_append_raw_string(fast_json, fast_has, "joints", joints_json);
        }
        if (build_fast_delta || !telemetry_cache_valid) {
          delta_append_f1(fast_json, fast_has, "gripper", gripper, prev_gripper_q);
        }
        if (build_medium_delta || !telemetry_cache_valid) {
          delta_append_u32(medium_json, medium_has, "t41_loop_ms", t41_loop_ms, prev_t41_loop_ms);
          delta_append_i32(medium_json, medium_has, "motor", motor, prev_motor);
          delta_append_u32(medium_json, medium_has, "spi_total", spi_total, prev_spi_total);
          delta_append_u32(medium_json, medium_has, "spi_crc_err", spi_crc_err, prev_spi_crc_err);
          delta_append_u32(medium_json, medium_has, "spi_marker_err", spi_marker_err, prev_spi_marker_err);
          delta_append_u32(medium_json, medium_has, "spi_err_rev", spi_err_rev, prev_spi_err_rev);
          delta_append_i32(medium_json, medium_has, "spi_last_marker", spi_last_marker, prev_spi_last_marker);
          delta_append_bool(medium_json, medium_has, "spi_connected", spi_connected, prev_spi_connected);
          delta_append_i32(medium_json, medium_has, "s3_devstat", s3_devstat, prev_s3_devstat);
          delta_append_bool(medium_json, medium_has, "espnow_connected", espnow_connected, prev_espnow_connected);
        }
        if (build_medium_delta || !telemetry_cache_valid || ik_backend != prev_ik_backend) {
          prev_ik_backend = ik_backend;
          delta_append_string(medium_json, medium_has, "ik_backend", ik_backend.c_str());
          if (has_bin_client) {
            delta_append_string(sidecar_json, sidecar_has, "ik_backend", ik_backend.c_str());
          }
        }
        if (build_medium_delta || !telemetry_cache_valid || ik_pref != prev_ik_pref) {
          prev_ik_pref = ik_pref;
          delta_append_string(medium_json, medium_has, "ik_pref", ik_pref.c_str());
          if (has_bin_client) {
            delta_append_string(sidecar_json, sidecar_has, "ik_pref", ik_pref.c_str());
          }
        }
        if (robot_ui_command.revision != 0U &&
            (build_medium_delta || !telemetry_cache_valid ||
             robot_ui_command.revision != prev_robot_ui_command_rev)) {
          prev_robot_ui_command_rev = robot_ui_command.revision;
          const String robot_cmd_json = robot_ui_command_to_json(robot_ui_command);
          delta_append_raw_string(medium_json, medium_has, "robot_cmd",
                                  robot_cmd_json);
          if (has_bin_client) {
            delta_append_raw_string(sidecar_json, sidecar_has, "robot_cmd",
                                    robot_cmd_json);
          }
        }
        if (robot_math_state.revision != 0U &&
            (build_medium_delta || !telemetry_cache_valid ||
             robot_math_state.revision != prev_robot_math_rev)) {
          prev_robot_math_rev = robot_math_state.revision;
          const String robot_math_json = robot_math_state_to_json(robot_math_state);
          delta_append_raw_string(medium_json, medium_has, "robot_math",
                                  robot_math_json);
          if (has_bin_client) {
            delta_append_raw_string(sidecar_json, sidecar_has, "robot_math",
                                    robot_math_json);
          }
        }
        if ((build_fast_delta || !telemetry_cache_valid) && has_scene_client) {
          delta_append_f1(fast_json, fast_has, "coord_x", coord_x, prev_coord_x_q);
          delta_append_f1(fast_json, fast_has, "coord_y", coord_y, prev_coord_y_q);
          delta_append_f1(fast_json, fast_has, "coord_z", coord_z, prev_coord_z_q);
          delta_append_f1(fast_json, fast_has, "coord_roll", coord_roll, prev_coord_roll_q);
          delta_append_f1(fast_json, fast_has, "coord_pitch", coord_pitch, prev_coord_pitch_q);
          delta_append_f1(fast_json, fast_has, "coord_yaw", coord_yaw, prev_coord_yaw_q);
          delta_append_f1(fast_json, fast_has, "alpha", alpha, prev_alpha_q);
          delta_append_i32(fast_json, fast_has, "lp", lp, prev_lp);
          delta_append_f1(fast_json, fast_has, "fk_x", fk_x, prev_fk_x_q);
          delta_append_f1(fast_json, fast_has, "fk_y", fk_y, prev_fk_y_q);
          delta_append_f1(fast_json, fast_has, "fk_z", fk_z, prev_fk_z_q);
          delta_append_f1(fast_json, fast_has, "fk_a", fk_a, prev_fk_a_q);
        }
        bool c3_changed = !telemetry_cache_valid;
        const int32_t c3_pos_q = q2(c3_pos);
        const int32_t c3_spd_q = q1(c3_spd);
        const int32_t c3_acc_q = q1(c3_acc);
        const int8_t c3_connected_i = c3_connected ? 1 : 0;
        const int8_t c3_espnow_active_i = c3_espnow_active ? 1 : 0;
        const int32_t c3_quality_q = q2(c3_quality);
        if (c3_pos_q != prev_c3_pos_q || c3_spd_q != prev_c3_spd_q ||
            c3_acc_q != prev_c3_acc_q || c3_connected_i != prev_c3_connected ||
            c3_espnow_active_i != prev_c3_espnow_active ||
            c3_crc_err != prev_c3_crc_err ||
            c3_marker_err != prev_c3_marker_err ||
            c3_total != prev_c3_total_rx || c3_quality_q != prev_c3_quality_q ||
            c3_hz != prev_c3_hz) {
          c3_changed = true;
        }
        if ((build_medium_delta || !telemetry_cache_valid) && c3_changed) {
          prev_c3_pos_q = c3_pos_q;
          prev_c3_spd_q = c3_spd_q;
          prev_c3_acc_q = c3_acc_q;
          prev_c3_connected = c3_connected_i;
          prev_c3_espnow_active = c3_espnow_active_i;
          prev_c3_crc_err = c3_crc_err;
          prev_c3_marker_err = c3_marker_err;
          prev_c3_total_rx = c3_total;
          prev_c3_quality_q = c3_quality_q;
          prev_c3_hz = c3_hz;
          PsramJsonWriter c3_json(384U);
          c3_json.begin();
          c3_json.float_field("c3_pos", c3_pos, 2U);
          c3_json.float_field("c3_spd", c3_spd, 1U);
          c3_json.float_field("c3_acc", c3_acc, 1U);
          c3_json.bool_field("c3_connected", c3_connected);
          c3_json.bool_field("c3_espnow_active", c3_espnow_active);
          c3_json.u32_field("c3_crc_err", c3_crc_err);
          c3_json.u32_field("c3_marker_err", c3_marker_err);
          c3_json.u32_field("c3_total_rx", c3_total);
          c3_json.float_field("c3_quality", c3_quality, 2U);
          c3_json.u32_field("c3_hz", c3_hz);
          c3_json.end();
          delta_append_raw(medium_json, medium_has, "c3", c3_json.c_str());
        }
        if (build_medium_delta || !telemetry_cache_valid) {
          delta_append_f1(medium_json, medium_has, "pid_out", pid_out, prev_pid_out_q);
          delta_append_f2(medium_json, medium_has, "traj_scale", traj_scale, prev_traj_scale_q);
          delta_append_i32(medium_json, medium_has, "oe", oe, prev_oe);
        }
        if (build_slow_delta || !telemetry_cache_valid) {
          delta_append_u32(slow_json, slow_has, "uptime", uptime_s, prev_uptime);
          delta_append_u32(slow_json, slow_has, "console_rev", console_rev, prev_console_rev);
          delta_append_bool(slow_json, slow_has, "pca_ready", pca_ready, prev_pca_ready);
          delta_append_bool(slow_json, slow_has, "wifi_ap", wifi_ap, prev_wifi_ap);
          delta_append_string(slow_json, slow_has, "telemetry_budget_mode",
                              g_ws_memory_budget_mode);
          slow_json.u32_field("telemetry_budget_free", g_ws_memory_budget_last_free);
          slow_json.u32_field("telemetry_budget_largest", g_ws_memory_budget_last_largest);
          slow_has = true;
        }
        if (fast_has) fast_json.end();
        if (medium_has) medium_json.end();
        if (slow_has) slow_json.end();
        g_ws_last_fast_bytes = fast_has ? static_cast<uint32_t>(fast_json.length()) : 0U;
        g_ws_last_medium_bytes = medium_has ? static_cast<uint32_t>(medium_json.length()) : 0U;
        g_ws_last_slow_bytes = slow_has ? static_cast<uint32_t>(slow_json.length()) : 0U;
        if (fast_json.overflowed() || medium_json.overflowed() || slow_json.overflowed()) {
          g_json_overflow_count++;
        }
        telemetry_cache_valid = true;

        PsramJsonWriter full_json(has_scene_client ? 3072U : 2048U);
        bool full_has = false;
        if (has_pending_full) {
          full_json.begin();
          full_has = true;
          full_json.float_field("turret", turret, 1U);
          if (has_scene_client) {
            char joints_full[96] = {};
            snprintf(joints_full,
                     sizeof(joints_full),
                     "[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f]",
                     static_cast<double>(fw_var_joints[0]),
                     static_cast<double>(fw_var_joints[1]),
                     static_cast<double>(fw_var_joints[2]),
                     static_cast<double>(fw_var_joints[3]),
                     static_cast<double>(fw_var_joints[4]),
                     static_cast<double>(fw_var_joints[5]));
            full_json.raw_field("joints", joints_full);
          }
          full_json.float_field("gripper", gripper, 1U);
          full_json.u32_field("t41_loop_ms", t41_loop_ms);
          full_json.u32_field("uptime", uptime_s);
          full_json.i32_field("motor", motor);
          full_json.u32_field("spi_total", spi_total);
          full_json.u32_field("spi_crc_err", spi_crc_err);
          full_json.u32_field("spi_marker_err", spi_marker_err);
          full_json.u32_field("spi_err_rev", spi_err_rev);
          full_json.i32_field("spi_last_marker", spi_last_marker);
          full_json.bool_field("spi_connected", spi_connected);
          full_json.i32_field("s3_devstat", s3_devstat);
          full_json.bool_field("espnow_connected", espnow_connected);
          full_json.string_field("ik_backend", ik_backend.c_str());
          full_json.string_field("ik_pref", ik_pref.c_str());
          if (robot_ui_command.revision != 0U) {
            full_json.raw_field("robot_cmd", robot_ui_command_to_json(robot_ui_command));
          }
          if (robot_math_state.revision != 0U) {
            full_json.raw_field("robot_math", robot_math_state_to_json(robot_math_state));
          }
          if (has_scene_client) {
            full_json.float_field("coord_x", coord_x, 1U);
            full_json.float_field("coord_y", coord_y, 1U);
            full_json.float_field("coord_z", coord_z, 1U);
            full_json.float_field("coord_roll", coord_roll, 1U);
            full_json.float_field("coord_pitch", coord_pitch, 1U);
            full_json.float_field("coord_yaw", coord_yaw, 1U);
            full_json.float_field("alpha", alpha, 1U);
            full_json.i32_field("lp", lp);
            full_json.float_field("fk_x", fk_x, 1U);
            full_json.float_field("fk_y", fk_y, 1U);
            full_json.float_field("fk_z", fk_z, 1U);
            full_json.float_field("fk_a", fk_a, 1U);
          }
          full_json.float_field("c3_pos", c3_pos, 2U);
          full_json.float_field("c3_spd", c3_spd, 1U);
          full_json.float_field("c3_acc", c3_acc, 1U);
          full_json.bool_field("c3_connected", c3_connected);
          full_json.bool_field("c3_espnow_active", c3_espnow_active);
          full_json.u32_field("c3_crc_err", c3_crc_err);
          full_json.u32_field("c3_marker_err", c3_marker_err);
          full_json.u32_field("c3_total_rx", c3_total);
          full_json.float_field("c3_quality", c3_quality, 2U);
          full_json.u32_field("c3_hz", c3_hz);
          PsramJsonWriter c3_full_json(384U);
          c3_full_json.begin();
          c3_full_json.float_field("c3_pos", c3_pos, 2U);
          c3_full_json.float_field("c3_spd", c3_spd, 1U);
          c3_full_json.float_field("c3_acc", c3_acc, 1U);
          c3_full_json.bool_field("c3_connected", c3_connected);
          c3_full_json.bool_field("c3_espnow_active", c3_espnow_active);
          c3_full_json.u32_field("c3_crc_err", c3_crc_err);
          c3_full_json.u32_field("c3_marker_err", c3_marker_err);
          c3_full_json.u32_field("c3_total_rx", c3_total);
          c3_full_json.float_field("c3_quality", c3_quality, 2U);
          c3_full_json.u32_field("c3_hz", c3_hz);
          c3_full_json.end();
          full_json.raw_field("c3", c3_full_json.c_str());
          full_json.float_field("pid_out", pid_out, 1U);
          full_json.u32_field("console_rev", console_rev);
          full_json.bool_field("pca_ready", pca_ready);
          full_json.bool_field("wifi_ap", wifi_ap);
          full_json.string_field("telemetry_budget_mode", g_ws_memory_budget_mode);
          full_json.u32_field("telemetry_budget_free", g_ws_memory_budget_last_free);
          full_json.u32_field("telemetry_budget_largest", g_ws_memory_budget_last_largest);
          full_json.float_field("traj_scale", traj_scale, 2U);
          full_json.i32_field("oe", oe);
          full_json.end();
          if (full_json.overflowed()) g_json_overflow_count++;
        }

        TelemetryBinFrame full_bin(BIN_FRAME_FULL, ++g_ws_bin_seq,
                                   static_cast<uint32_t>(now));
        TelemetryBinFrame fast_bin(BIN_FRAME_FAST, ++g_ws_bin_seq,
                                   static_cast<uint32_t>(now));
        TelemetryBinFrame medium_bin(BIN_FRAME_MEDIUM, ++g_ws_bin_seq,
                                     static_cast<uint32_t>(now));
        TelemetryBinFrame slow_bin(BIN_FRAME_SLOW, ++g_ws_bin_seq,
                                   static_cast<uint32_t>(now));
        if (has_bin_client && has_pending_full) {
          full_bin.i32(BIN_FIELD_TURRET, telemetry_q10(turret));
          full_bin.i32(BIN_FIELD_GRIPPER, telemetry_q10(gripper));
          if (has_scene_client) {
            full_bin.i32_array6(BIN_FIELD_JOINTS, joints_q);
            full_bin.i32(BIN_FIELD_COORD_X, telemetry_q10(coord_x));
            full_bin.i32(BIN_FIELD_COORD_Y, telemetry_q10(coord_y));
            full_bin.i32(BIN_FIELD_COORD_Z, telemetry_q10(coord_z));
            full_bin.i32(BIN_FIELD_COORD_ROLL, telemetry_q10(coord_roll));
            full_bin.i32(BIN_FIELD_COORD_PITCH, telemetry_q10(coord_pitch));
            full_bin.i32(BIN_FIELD_COORD_YAW, telemetry_q10(coord_yaw));
            full_bin.i32(BIN_FIELD_ALPHA, telemetry_q10(alpha));
            full_bin.i32(BIN_FIELD_LP, lp);
            full_bin.i32(BIN_FIELD_FK_X, telemetry_q10(fk_x));
            full_bin.i32(BIN_FIELD_FK_Y, telemetry_q10(fk_y));
            full_bin.i32(BIN_FIELD_FK_Z, telemetry_q10(fk_z));
            full_bin.i32(BIN_FIELD_FK_A, telemetry_q10(fk_a));
          }
          full_bin.u32(BIN_FIELD_t41_loop_ms, t41_loop_ms);
          full_bin.i32(BIN_FIELD_MOTOR, motor);
          full_bin.u32(BIN_FIELD_SPI_TOTAL, spi_total);
          full_bin.u32(BIN_FIELD_SPI_CRC_ERR, spi_crc_err);
          full_bin.u32(BIN_FIELD_SPI_MARKER_ERR, spi_marker_err);
          full_bin.u32(BIN_FIELD_SPI_ERR_REV, spi_err_rev);
          full_bin.i32(BIN_FIELD_SPI_LAST_MARKER, spi_last_marker);
          full_bin.boolean(BIN_FIELD_SPI_CONNECTED, spi_connected);
          full_bin.i32(BIN_FIELD_S3_DEVSTAT, s3_devstat);
          full_bin.boolean(BIN_FIELD_ESPNOW_CONNECTED, espnow_connected);
          full_bin.i32(BIN_FIELD_C3_POS, telemetry_q100(c3_pos));
          full_bin.i32(BIN_FIELD_C3_SPD, telemetry_q10(c3_spd));
          full_bin.i32(BIN_FIELD_C3_ACC, telemetry_q10(c3_acc));
          full_bin.boolean(BIN_FIELD_C3_CONNECTED, c3_connected);
          full_bin.boolean(BIN_FIELD_C3_ESPNOW_ACTIVE, c3_espnow_active);
          full_bin.u32(BIN_FIELD_C3_CRC_ERR, c3_crc_err);
          full_bin.u32(BIN_FIELD_C3_MARKER_ERR, c3_marker_err);
          full_bin.u32(BIN_FIELD_C3_TOTAL_RX, c3_total);
          full_bin.i32(BIN_FIELD_C3_QUALITY, telemetry_q100(c3_quality));
          full_bin.u32(BIN_FIELD_C3_HZ, c3_hz);
          full_bin.i32(BIN_FIELD_PID_OUT, telemetry_q10(pid_out));
          full_bin.i32(BIN_FIELD_TRAJ_SCALE, telemetry_q100(traj_scale));
          full_bin.i32(BIN_FIELD_OE, oe);
          full_bin.u32(BIN_FIELD_UPTIME, uptime_s);
          full_bin.u32(BIN_FIELD_CONSOLE_REV, console_rev);
          full_bin.boolean(BIN_FIELD_PCA_READY, pca_ready);
          full_bin.boolean(BIN_FIELD_WIFI_AP, wifi_ap);
        }
        if (has_bin_client && fast_has) {
          fast_bin.i32(BIN_FIELD_TURRET, telemetry_q10(turret));
          fast_bin.i32(BIN_FIELD_GRIPPER, telemetry_q10(gripper));
          if (has_scene_client) {
            fast_bin.i32_array6(BIN_FIELD_JOINTS, joints_q);
            fast_bin.i32(BIN_FIELD_COORD_X, telemetry_q10(coord_x));
            fast_bin.i32(BIN_FIELD_COORD_Y, telemetry_q10(coord_y));
            fast_bin.i32(BIN_FIELD_COORD_Z, telemetry_q10(coord_z));
            fast_bin.i32(BIN_FIELD_COORD_ROLL, telemetry_q10(coord_roll));
            fast_bin.i32(BIN_FIELD_COORD_PITCH, telemetry_q10(coord_pitch));
            fast_bin.i32(BIN_FIELD_COORD_YAW, telemetry_q10(coord_yaw));
            fast_bin.i32(BIN_FIELD_ALPHA, telemetry_q10(alpha));
            fast_bin.i32(BIN_FIELD_LP, lp);
            fast_bin.i32(BIN_FIELD_FK_X, telemetry_q10(fk_x));
            fast_bin.i32(BIN_FIELD_FK_Y, telemetry_q10(fk_y));
            fast_bin.i32(BIN_FIELD_FK_Z, telemetry_q10(fk_z));
            fast_bin.i32(BIN_FIELD_FK_A, telemetry_q10(fk_a));
          }
        }
        if (has_bin_client && medium_has) {
          medium_bin.u32(BIN_FIELD_t41_loop_ms, t41_loop_ms);
          medium_bin.i32(BIN_FIELD_MOTOR, motor);
          medium_bin.u32(BIN_FIELD_SPI_TOTAL, spi_total);
          medium_bin.u32(BIN_FIELD_SPI_CRC_ERR, spi_crc_err);
          medium_bin.u32(BIN_FIELD_SPI_MARKER_ERR, spi_marker_err);
          medium_bin.u32(BIN_FIELD_SPI_ERR_REV, spi_err_rev);
          medium_bin.i32(BIN_FIELD_SPI_LAST_MARKER, spi_last_marker);
          medium_bin.boolean(BIN_FIELD_SPI_CONNECTED, spi_connected);
          medium_bin.i32(BIN_FIELD_S3_DEVSTAT, s3_devstat);
          medium_bin.boolean(BIN_FIELD_ESPNOW_CONNECTED, espnow_connected);
          medium_bin.i32(BIN_FIELD_C3_POS, telemetry_q100(c3_pos));
          medium_bin.i32(BIN_FIELD_C3_SPD, telemetry_q10(c3_spd));
          medium_bin.i32(BIN_FIELD_C3_ACC, telemetry_q10(c3_acc));
          medium_bin.boolean(BIN_FIELD_C3_CONNECTED, c3_connected);
          medium_bin.boolean(BIN_FIELD_C3_ESPNOW_ACTIVE, c3_espnow_active);
          medium_bin.u32(BIN_FIELD_C3_CRC_ERR, c3_crc_err);
          medium_bin.u32(BIN_FIELD_C3_MARKER_ERR, c3_marker_err);
          medium_bin.u32(BIN_FIELD_C3_TOTAL_RX, c3_total);
          medium_bin.i32(BIN_FIELD_C3_QUALITY, telemetry_q100(c3_quality));
          medium_bin.u32(BIN_FIELD_C3_HZ, c3_hz);
          medium_bin.i32(BIN_FIELD_PID_OUT, telemetry_q10(pid_out));
          medium_bin.i32(BIN_FIELD_TRAJ_SCALE, telemetry_q100(traj_scale));
          medium_bin.i32(BIN_FIELD_OE, oe);
        }
        if (has_bin_client && slow_has) {
          slow_bin.u32(BIN_FIELD_UPTIME, uptime_s);
          slow_bin.u32(BIN_FIELD_CONSOLE_REV, console_rev);
          slow_bin.boolean(BIN_FIELD_PCA_READY, pca_ready);
          slow_bin.boolean(BIN_FIELD_WIFI_AP, wifi_ap);
        }
        if (has_bin_client && sidecar_has) {
          sidecar_json.end();
          if (sidecar_json.overflowed()) g_json_overflow_count++;
        }

        // Broadcast only to authenticated WebSocket clients.
        // Unauthenticated clients are either still in the handshake or already
        // closed, so they must not receive robot state data.
        std::vector<WsTelemetryClientSnapshot> telemetry_targets;
        telemetry_targets.reserve(ws.count() + ws_telemetry.count());
        ws_auth_lock();
        for (auto it = ws_auth_clients.begin(); it != ws_auth_clients.end(); ++it) {
          if (!it->second) continue;
          bool pending_full = false;
          auto fit = ws_full_snapshot_pending.find(it->first);
          if (fit != ws_full_snapshot_pending.end() && fit->second) {
            pending_full = true;
            fit->second = false;
          }
          uint8_t profile = WS_SUB_RATE_DEFAULT;
          auto mit = ws_subscription_masks.find(it->first);
          if (mit != ws_subscription_masks.end()) profile = mit->second;
          uint8_t format = WS_TELEMETRY_FORMAT_JSON_V1;
          auto fmt_it = ws_telemetry_formats.find(it->first);
          if (fmt_it != ws_telemetry_formats.end()) format = fmt_it->second;
          telemetry_targets.push_back({it->first, profile, format, pending_full});
        }
        ws_auth_unlock();
        for (const WsTelemetryClientSnapshot &target : telemetry_targets) {
          AsyncWebSocketClient *target_client =
              findWsClientById(ws, ws_telemetry, target.id);
          if (target_client == nullptr) continue;
          const bool binary_client =
              target.format == WS_TELEMETRY_FORMAT_BIN_V1;
          if (binary_client) {
            if (target.pending_full) {
              ws_send_binary_telemetry(target_client, full_bin);
              if (sidecar_has) {
                target_client->text(sidecar_json.c_str(), sidecar_json.length());
              }
            } else {
              if (fast_has && (target.profile & WS_SUB_RATE_FAST) != 0U) {
                ws_send_binary_telemetry(target_client, fast_bin);
              }
              if (medium_has && (target.profile & WS_SUB_RATE_MEDIUM) != 0U) {
                ws_send_binary_telemetry(target_client, medium_bin);
              }
              if (slow_has && (target.profile & WS_SUB_RATE_SLOW) != 0U) {
                ws_send_binary_telemetry(target_client, slow_bin);
              }
              if (sidecar_has) {
                target_client->text(sidecar_json.c_str(), sidecar_json.length());
              }
            }
          } else if (target.pending_full && full_has) {
            target_client->text(full_json.c_str(), full_json.length());
          } else {
            if (fast_has && (target.profile & WS_SUB_RATE_FAST) != 0U) {
              target_client->text(fast_json.c_str(), fast_json.length());
            }
            if (medium_has && (target.profile & WS_SUB_RATE_MEDIUM) != 0U) {
              target_client->text(medium_json.c_str(), medium_json.length());
            }
            if (slow_has && (target.profile & WS_SUB_RATE_SLOW) != 0U) {
              target_client->text(slow_json.c_str(), slow_json.length());
            }
          }
        }
      }
    }

    // Log variables to CSV every 5 seconds
    static unsigned long last_log = 0;
    if (mros::platform::mros_millis() - last_log > 5000) {
      last_log = mros::platform::mros_millis();
      char csv_line[64] = {};
      std::snprintf(csv_line, sizeof(csv_line), "%lu,%.1f,%u",
                    static_cast<unsigned long>(mros::platform::mros_millis()),
                    static_cast<double>(fw_var_turret),
                    spi_s3_is_connected() ? 1U : 0U);
      logger_append_csv(String(csv_line));
    }
  }

  if ((ws_debug.count() > 0) && (now - last_ws_debug_update >= 1000UL)) {
    last_ws_debug_update = now;
    std::vector<WsDebugClientSnapshot> debug_targets;
    debug_targets.reserve(ws_debug.count());
    ws_auth_lock();
    for (auto it = ws_debug_auth_clients.begin();
         it != ws_debug_auth_clients.end(); ++it) {
      if (!it->second) continue;
      auto sit = ws_debug_subscriptions.find(it->first);
      if (sit == ws_debug_subscriptions.end() || !sit->second) continue;
      debug_targets.push_back({it->first});
    }
    ws_auth_unlock();
    bool has_debug_subscriber = false;
    for (const WsDebugClientSnapshot &target : debug_targets) {
      AsyncWebSocketClient *client = ws_debug.client(target.id);
      if (client != nullptr && client->status() == WS_CONNECTED) {
        has_debug_subscriber = true;
        break;
      }
    }
    if (!has_debug_subscriber) {
      return;
    }
    mros::shell::service::ShellServiceMetrics shell_metrics {};
    mros::shell::service::get_metrics(&shell_metrics);
    mros::shell::remote::RemoteTunnelMetrics remote_metrics {};
    mros::shell::remote::get_tunnel_metrics(&remote_metrics);
    MrosRtosAggregateSnapshot rtos_snapshot {};
    app_rtos_get_aggregate_diag(&rtos_snapshot);
    WifiManagerSnapshot wifi_snapshot {};
    wifi_manager_get_snapshot(&wifi_snapshot);
    mros::power::Status power_snapshot {};
    mros::power::get_status(&power_snapshot);
    ws_auth_lock();
    const uint32_t debug_ws_auth_count = ws_authenticated_count_locked(ws_auth_clients);
    ws_auth_unlock();
    PsramJsonWriter debug_body(3456U);
    debug_body.begin();
    debug_body.u32_field("uptime", now / 1000UL);
    debug_body.u32_field("heap_internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    debug_body.u32_field("heap_internal_total", heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    debug_body.u32_field("heap_internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    debug_body.u32_field("heap_internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    debug_body.u32_field("internal_free", power_snapshot.internal_free);
    debug_body.u32_field("internal_min", power_snapshot.internal_min_free);
    debug_body.u32_field("internal_largest_block", power_snapshot.internal_largest_block);
    debug_body.string_field("sram_floor_state", power_snapshot.sram_floor_state);
    debug_body.u32_field("psram_free", mros::platform::mros_system_psram_free());
    debug_body.u32_field("psram_total", mros::platform::mros_system_psram_total());
    debug_body.u32_field("psram_min", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    debug_body.u32_field("psram_largest", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    debug_body.u32_field("psram_migrated_bytes", power_snapshot.psram_migrated_bytes);
    debug_body.u32_field("actual_cpu_mhz", power_snapshot.actual_cpu_mhz);
    debug_body.u32_field("target_cpu_mhz", power_snapshot.target_mhz);
    debug_body.string_field("pm_locks", power_snapshot.active_locks);
    debug_body.string_field("per_core_load", "core0=network-web-storage,core1=robot-control");
    debug_body.u32_field("t41_loop_ms", spi_s3_get_loop_ms());
    debug_body.u32_field("ws_telemetry_clients", ws_telemetry.count());
    debug_body.u32_field("ws_legacy_clients", ws.count());
    debug_body.u32_field("ws_shell_clients", ws_shell.count() + ws_shell_v2.count());
    debug_body.u32_field("json_overflow",
                         g_json_overflow_count + mros::utils::json_overflow_count());
    debug_body.u32_field("telemetry_bin_frames", g_ws_bin_frame_count);
    debug_body.u32_field("telemetry_bin_bytes", g_ws_bin_byte_count);
    debug_body.u32_field("telemetry_json_fallbacks", g_ws_json_fallback_count);
    debug_body.u32_field("telemetry_format_errors", g_ws_format_error_count);
    debug_body.u32_field("telemetry_last_bin_bytes", g_ws_last_bin_bytes);
    debug_body.string_field("telemetry_memory_budget_mode", g_ws_memory_budget_mode);
    debug_body.u32_field("telemetry_budget_free", g_ws_memory_budget_last_free);
    debug_body.u32_field("telemetry_budget_largest", g_ws_memory_budget_last_largest);
    debug_body.u32_field("telemetry_budget_degrade_frames",
                         g_ws_memory_budget_degrade_frames);
    debug_body.u32_field("telemetry_budget_critical_frames",
                         g_ws_memory_budget_critical_frames);
    debug_body.u32_field("telemetry_budget_scene_suppressed",
                         g_ws_memory_budget_scene_suppressed);
    debug_body.u32_field("telemetry_budget_fast_suppressed",
                         g_ws_memory_budget_fast_suppressed);
    debug_body.u32_field("telemetry_budget_medium_suppressed",
                         g_ws_memory_budget_medium_suppressed);
    debug_body.u32_field("telemetry_budget_dropped", g_ws_telemetry_budget_dropped);
    debug_body.u32_field("telemetry_budget_deferred", g_ws_telemetry_budget_deferred);
    debug_body.u32_field("full_required_count", g_ws_full_required_count);
    debug_body.u32_field("shell_pool_active", shell_metrics.response_pool_active);
    debug_body.u32_field("shell_pool_capacity", shell_metrics.response_pool_capacity);
    debug_body.u32_field("shell_pool_allocated", shell_metrics.response_pool_allocated);
    debug_body.u32_field("shell_pool_miss", shell_metrics.response_pool_miss);
    debug_body.u32_field("shell_pool_fallback_alloc", shell_metrics.response_fallback_alloc);
    debug_body.u32_field("shell_drop", shell_metrics.response_drop);
    debug_body.u32_field("mshell_job_storage_bytes",
                         mros::shell::runtime::job_storage_bytes());
    debug_body.bool_field("mshell_job_storage_allocated",
                          mros::shell::runtime::job_storage_allocated());
    debug_body.bool_field("mshell_job_storage_psram",
                          mros::shell::runtime::job_storage_uses_psram());
    debug_body.u32_field("shell_bin_frames", shell_metrics.shell_bin_frames);
    debug_body.u32_field("shell_bin_bytes", shell_metrics.shell_bin_bytes);
    debug_body.u32_field("shell_json_frames", shell_metrics.shell_json_frames);
    debug_body.u32_field("shell_json_bytes", shell_metrics.shell_json_bytes);
    debug_body.u32_field("shell_bin_decode_errors", shell_metrics.shell_bin_decode_errors);
    debug_body.u32_field("shell_json_fallbacks", shell_metrics.shell_json_fallbacks);
    debug_body.u32_field("shell_ack_rx", shell_metrics.shell_ack_rx);
    debug_body.u32_field("shell_credit_rx", shell_metrics.shell_credit_rx);
    debug_body.u32_field("shell_credit_exhausted", shell_metrics.shell_credit_exhausted);
    debug_body.u32_field("shell_stream_final_suppressed", shell_metrics.stream_final_suppressed);
    debug_body.u32_field("remote_bin_frames", remote_metrics.remote_bin_frames);
    debug_body.u32_field("remote_bin_bytes", remote_metrics.remote_bin_bytes);
    debug_body.u32_field("remote_text_frames", remote_metrics.remote_text_frames);
    debug_body.u32_field("remote_text_bytes", remote_metrics.remote_text_bytes);
    debug_body.u32_field("remote_fallbacks", remote_metrics.remote_fallbacks);
    debug_body.u32_field("remote_cobs_decode_errors", remote_metrics.remote_cobs_decode_errors);
    debug_body.u32_field("remote_rx_drops", remote_metrics.remote_rx_drops);
    debug_body.bool_field("remote_cap_msh1", remote_metrics.remote_cap_msh1);
    debug_body.u32_field("file_list_paged", g_file_list_paged_count);
    debug_body.u32_field("ws_last_fast_bytes", g_ws_last_fast_bytes);
    debug_body.u32_field("ws_last_medium_bytes", g_ws_last_medium_bytes);
    debug_body.u32_field("ws_last_slow_bytes", g_ws_last_slow_bytes);
    debug_body.u32_field("rtos_deadline_miss", rtos_snapshot.total_slip_count);
    debug_body.u32_field("wifi_reconnect_ms", wifi_snapshot.last_connect_duration_ms);
    debug_body.bool_field("auth_session_active",
                          auth_active_session_count() > 0U);
    debug_body.u32_field("login_lockout_ms", login_lockout_remaining_ms());
    debug_body.u32_field("ws_auth_count", debug_ws_auth_count);
    debug_body.bool_field("serial_auth_required", mros::shell::serial_auth_required());
    debug_body.end();

    PsramJsonWriter debug_json(3712U);
    debug_json.begin();
    debug_json.raw_field("debug", debug_body.c_str());
    debug_json.end();
    if (debug_body.overflowed() || debug_json.overflowed()) {
      g_json_overflow_count++;
    }

    for (const WsDebugClientSnapshot &target : debug_targets) {
      AsyncWebSocketClient *client = ws_debug.client(target.id);
      if (client != nullptr && client->status() == WS_CONNECTED) {
        client->text(debug_json.c_str(), debug_json.length());
      }
    }
  }
}

void web_server_get_diag_snapshot(WebServerDiagSnapshot *snapshot) {
  if (snapshot == nullptr) {
    return;
  }

  snapshot->pid_cycle_avg_ms = 0.0f;
  snapshot->pid_cycle_last_ms = 0;
  snapshot->pid_cycle_exec_ms = 0;
  snapshot->pid_cycle_peak_ms = 0;
  snapshot->fk_last_ms = 0.0f;
  snapshot->fk_avg_ms = 0.0f;
  snapshot->fk_max_ms = 0.0f;
  snapshot->fk_samples = 0;
  snapshot->cpu_freq_core0_mhz = PM_FREQ_BASE_MHZ;
  snapshot->cpu_freq_core1_mhz = PM_FREQ_BASE_MHZ;
  snapshot->cpu_freq_target_mhz = PM_FREQ_BASE_MHZ;
  snapshot->cpu_freq_applied_mhz = PM_FREQ_BASE_MHZ;
  snapshot->ws_clients_legacy = 0U;
  snapshot->ws_clients_telemetry = 0U;
  snapshot->ws_clients_shell = 0U;
  snapshot->ws_clients_debug = 0U;
  snapshot->ws_clients_total = 0U;
  snapshot->ws_clients_auth = 0U;
  snapshot->ws_clients_scene = 0U;
  snapshot->ws_clients_shell_auth = 0U;
  snapshot->ws_clients_debug_auth = 0U;
  snapshot->ws_clients_debug_subscribed = 0U;
  snapshot->storage_ready = logger_storage_ready();
  snapshot->pca_ready = pca9685_is_ready();
  snapshot->last_web_feedback_ms = 0;

  portENTER_CRITICAL(&pm_scale_mux);
  snapshot->pid_cycle_avg_ms = pm_pid_cycle_avg_ms;
  snapshot->pid_cycle_last_ms = pm_pid_cycle_last_ms;
  snapshot->pid_cycle_exec_ms = pm_pid_exec_last_ms;
  snapshot->pid_cycle_peak_ms = pm_pid_cycle_peak_ms;
  snapshot->last_web_feedback_ms = pm_last_web_feedback_ms;
  portEXIT_CRITICAL(&pm_scale_mux);
  mros::power::Status power {};
  mros::power::get_status(&power);
  snapshot->cpu_freq_core0_mhz = power.net_demand_mhz;
  snapshot->cpu_freq_core1_mhz = power.rt_demand_mhz;
  snapshot->cpu_freq_target_mhz = power.target_mhz;
  snapshot->cpu_freq_applied_mhz = power.actual_cpu_mhz;

  portENTER_CRITICAL(&live_fk_mux);
  snapshot->fk_last_ms = dbg_fk_last_ms;
  snapshot->fk_avg_ms = dbg_fk_avg_ms;
  snapshot->fk_max_ms = dbg_fk_max_ms;
  snapshot->fk_samples = dbg_fk_samples;
  portEXIT_CRITICAL(&live_fk_mux);

  snapshot->ws_clients_legacy = static_cast<uint32_t>(ws.count());
  snapshot->ws_clients_telemetry = static_cast<uint32_t>(ws_telemetry.count());
  snapshot->ws_clients_shell =
      static_cast<uint32_t>(ws_shell.count() + ws_shell_v2.count());
  snapshot->ws_clients_debug = static_cast<uint32_t>(ws_debug.count());
  snapshot->ws_clients_total = snapshot->ws_clients_legacy +
                               snapshot->ws_clients_telemetry +
                               snapshot->ws_clients_shell +
                               snapshot->ws_clients_debug;
  ws_auth_lock();
  snapshot->ws_clients_auth = ws_authenticated_count_locked(ws_auth_clients);
  snapshot->ws_clients_scene = 0U;
  for (const auto &item : ws_scene_subscriptions) {
    if (item.second && ws_auth_map_is_authenticated(ws_auth_clients, item.first)) {
      snapshot->ws_clients_scene++;
    }
  }
  snapshot->ws_clients_shell_auth = ws_authenticated_count_locked(ws_shell_auth_clients);
  snapshot->ws_clients_debug_auth = ws_authenticated_count_locked(ws_debug_auth_clients);
  snapshot->ws_clients_debug_subscribed = ws_debug_subscribed_count_locked();
  ws_auth_unlock();
}

void web_server_get_security_snapshot(WebSecuritySnapshot *snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  snapshot->auth_session_active = auth_active_session_count() > 0U;
  snapshot->login_fail_count = login_fail_count;
  snapshot->login_lockout_ms = login_lockout_remaining_ms();
  snapshot->serial_auth_required = mros::shell::serial_auth_required();
  snapshot->ws_auth_count = 0U;
  snapshot->ws_shell_auth_count = 0U;
  snapshot->ws_debug_auth_count = 0U;
  ws_auth_lock();
  snapshot->ws_auth_count = ws_authenticated_count_locked(ws_auth_clients);
  snapshot->ws_shell_auth_count = ws_authenticated_count_locked(ws_shell_auth_clients);
  snapshot->ws_debug_auth_count = ws_authenticated_count_locked(ws_debug_auth_clients);
  ws_auth_unlock();
}

const char* web_server_system_version() { return k_system_version; }
