#include "wifi_manager.h"

#include "src/config/pin_config.h"
#if __has_include("src/config/wifi_secrets.h")
#include "src/config/wifi_secrets.h"
#else
#include "src/config/wifi_defaults.h"
#endif
#include "src/drivers/sd_logger.h"
#include "src/core/power/power_manager.h"
#include "src/drivers/utils/mros_console.h"
#include "src/platform/mros_rgb_led.h"
#include "src/platform/mros_system.h"
#include "src/platform/mros_time.h"
#include "src/utils/mros_json_writer.h"

#include "WString.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>
#include <esp_wifi_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <unistd.h>

namespace {

using wl_status_t = uint8_t;
constexpr wl_status_t WL_IDLE_STATUS = 0;
constexpr wl_status_t WL_NO_SSID_AVAIL = 1;
constexpr wl_status_t WL_CONNECTED = 3;
constexpr wl_status_t WL_CONNECT_FAILED = 4;
constexpr wl_status_t WL_DISCONNECTED = 6;

constexpr int kAssocComebackTooLongReason = 208;
constexpr int kNoApFoundReason = 201;
constexpr int kAuthFailReason = 202;
constexpr int kAuthExpireReason = 2;
constexpr int kAssocExpireReason = 4;
constexpr int kHandshakeTimeoutReason = 15;
constexpr int kBeaconTimeoutReason = 200;

constexpr uint32_t kBootScanTimeoutMs = 10000UL;
constexpr uint32_t kConnectTimeoutMs = 30000UL;
constexpr uint32_t kConnectStatusGraceMs = 5000UL;
constexpr uint32_t kApFallbackDelayMs = 8000UL;
constexpr uint32_t kApGracePeriodMs = 30000UL;
constexpr uint32_t kDefaultReconnectScanPeriodMs = 15000UL;
constexpr uint32_t kNoApReconnectScanPeriodMs = 8000UL;
constexpr uint32_t kAuthReconnectScanPeriodMs = 30000UL;
constexpr uint32_t kNoApCandidateCooldownMs = 45000UL;
constexpr uint32_t kAuthCandidateCooldownMs = 180000UL;
constexpr uint32_t kGenericCandidateCooldownMs = 20000UL;
constexpr uint32_t kAssocRetryBaseDelayMs = 500UL;
constexpr uint32_t kStatePollPeriodMs = 250UL;
constexpr uint32_t kStableStatePollPeriodMs = 1000UL;
constexpr uint32_t kStateJsonCacheTtlMs = 250UL;
constexpr uint32_t kLedBlinkSlowMs = 1000UL;
constexpr uint32_t kLedBlinkFastMs = 250UL;
constexpr uint32_t kIdleWaitMs = 2000UL;
constexpr uint32_t kApWaitMs = 100UL;
constexpr uint32_t kBusyWaitMs = 100UL;
constexpr uint32_t kStableWaitMs = 500UL;
constexpr uint32_t kLongWaitMs = 1000UL;
constexpr uint8_t kAssocRetryLimit = 6;
constexpr uint8_t kCachedScanRecordMax = 32;

enum class WifiPhase : uint8_t {
  kBootstrap,
  kConnecting,
  kConnected,
  kApFallback,
  kFinalizeSuccess,
  kFinalizeFailure,
};

enum class ScanPurpose : uint8_t {
  kNone,
  kBootstrap,
  kManual,
};

WifiManagerState g_state = {false, false, false, false, false, -127, 0, 0, 0};
SemaphoreHandle_t g_lock = nullptr;
TaskHandle_t g_task_handle = nullptr;
bool g_dns_running = false;
bool g_started = false;
bool g_init_in_progress = false;
bool g_wifi_event_registered = false;
bool g_native_event_active = false;
bool g_dns_lwip_active = false;
volatile bool g_dns_stop_requested = false;
TaskHandle_t g_dns_task_handle = nullptr;
int g_dns_socket_fd = -1;
esp_netif_t* g_sta_netif = nullptr;
esp_netif_t* g_ap_netif = nullptr;
bool g_wifi_driver_ready = false;
esp_event_handler_instance_t g_wifi_event_instance = nullptr;
esp_event_handler_instance_t g_ip_event_instance = nullptr;
portMUX_TYPE g_init_mux = portMUX_INITIALIZER_UNLOCKED;
bool g_manager_enabled = true;
bool g_hotspot_only = false;
bool g_assoc_comeback_too_long = false;
bool g_disconnect_event_pending = false;
bool g_bootstrap_scan_pending = true;
bool g_manual_scan_requested = false;
bool g_led_state = false;
bool g_current_attempt_fast_path = false;

WifiPhase g_phase = WifiPhase::kBootstrap;
ScanPurpose g_scan_purpose = ScanPurpose::kNone;

uint32_t g_phase_started_ms = 0;
uint32_t g_scan_started_ms = 0;
uint32_t g_last_state_poll_ms = 0;
uint32_t g_last_led_ms = 0;
uint32_t g_last_reconnect_scan_ms = 0;
uint32_t g_sta_lost_since_ms = 0;
uint32_t g_assoc_retry_count = 0;
uint32_t g_retry_due_ms = 0;
uint32_t g_next_connect_allowed_ms = 0;
uint32_t g_reconnect_scan_period_ms = kDefaultReconnectScanPeriodMs;
uint32_t g_current_backoff_ms = 0;
uint32_t g_ap_grace_until_ms = 0;
uint8_t g_finalize_blink_count = 0;
uint32_t g_failed_candidate_until_ms = 0;
size_t g_known_fallback_cursor = 0;

String g_bootstrap_scan_results = "[]";
String g_manual_scan_results = "[]";
String g_bootstrap_scan_text = "No scan data yet.\n";
String g_manual_scan_text = "No scan data yet.\n";
uint32_t g_bootstrap_scan_cached_ms = 0;
uint32_t g_manual_scan_cached_ms = 0;

String g_state_json_cache = "{}";
uint32_t g_state_json_cache_ms = 0;
bool g_state_json_cache_valid = false;

String g_saved_ssid = "";
String g_saved_pass = "";
String g_candidate_ssid = "";
String g_candidate_pass = "";
String g_test_ssid = "";
String g_test_pass = "";
String g_failed_candidate_ssid = "";
String g_last_ssid = "";
String g_last_ip = "0.0.0.0";
String g_last_ap_ip = "0.0.0.0";
String g_last_good_ssid = "";
String g_last_good_bssid = "";
String g_last_good_pass = "";
uint8_t g_last_good_channel = 0;

uint32_t g_event_sta_connected = 0;
uint32_t g_event_sta_got_ip = 0;
uint32_t g_event_sta_disconnected = 0;
uint32_t g_event_ap_sta_connected = 0;
uint32_t g_event_ap_sta_disconnected = 0;
uint32_t g_disconnect_auth_fail_count = 0;
uint32_t g_disconnect_no_ap_count = 0;
uint32_t g_disconnect_beacon_timeout_count = 0;
uint32_t g_disconnect_assoc_comeback_count = 0;
uint32_t g_disconnect_other_count = 0;
uint32_t g_scan_started_count = 0;
uint32_t g_scan_done_count = 0;
uint32_t g_scan_timeout_count = 0;
uint32_t g_scan_denied_count = 0;
uint32_t g_manual_scan_denied_count = 0;
uint32_t g_connect_attempt_count = 0;
uint32_t g_connect_success_count = 0;
uint32_t g_connect_fail_count = 0;
uint32_t g_backoff_apply_count = 0;
uint32_t g_assoc_retry_schedule_count = 0;
uint32_t g_last_scan_duration_ms = 0;
uint32_t g_last_connect_duration_ms = 0;
uint32_t g_fast_path_attempt_count = 0;
uint32_t g_fast_path_success_count = 0;
uint32_t g_last_event_ms = 0;
uint32_t g_last_disconnect_reason_seen = 0;
uint32_t g_wifi_event_revision = 0;
uint32_t g_wifi_state_json_build_count = 0;
uint32_t g_wifi_state_json_cache_hit_count = 0;
uint32_t g_wifi_runtime_publish_count = 0;
uint32_t g_idf_event_count = 0;
uint32_t g_idf_scan_start_count = 0;
uint32_t g_dns_query_count = 0;
uint32_t g_dns_reply_count = 0;
uint32_t g_dns_error_count = 0;
int32_t g_power_save_disable_status = ESP_ERR_INVALID_STATE;
char g_last_event_name[32] = "boot";
bool g_idf_scan_done_pending = false;
uint16_t g_idf_scan_done_ap_count = 0;
bool g_scan_started_with_idf = false;
String g_scan_cached_ssid[kCachedScanRecordMax];
String g_scan_cached_bssid[kCachedScanRecordMax];
int32_t g_scan_cached_rssi[kCachedScanRecordMax] = {};
int32_t g_scan_cached_channel[kCachedScanRecordMax] = {};
int32_t g_scan_cached_enc[kCachedScanRecordMax] = {};
uint16_t g_scan_cached_count = 0;
uint8_t g_ap_ip_octets[4] = {192, 168, 4, 1};

static void notify_task() {
  if (g_task_handle != nullptr) {
    xTaskNotifyGive(g_task_handle);
  }
}

static void state_lock() {
  if (g_lock != nullptr) {
    xSemaphoreTake(g_lock, portMAX_DELAY);
  }
}

static void state_unlock() {
  if (g_lock != nullptr) {
    xSemaphoreGive(g_lock);
  }
}

static void note_event_locked(const char *name, uint32_t now_ms) {
  std::snprintf(g_last_event_name, sizeof(g_last_event_name), "%s",
                (name != nullptr) ? name : "");
  g_last_event_ms = now_ms;
  g_state_json_cache_valid = false;
  g_wifi_event_revision++;
}

static void note_disconnect_reason_locked(uint32_t reason) {
  g_last_disconnect_reason_seen = reason;
  if (reason == static_cast<uint32_t>(kAuthFailReason) ||
      reason == static_cast<uint32_t>(kAuthExpireReason) ||
      reason == static_cast<uint32_t>(kHandshakeTimeoutReason)) {
    g_disconnect_auth_fail_count++;
  } else if (reason == static_cast<uint32_t>(kNoApFoundReason)) {
    g_disconnect_no_ap_count++;
  } else if (reason == static_cast<uint32_t>(kBeaconTimeoutReason)) {
    g_disconnect_beacon_timeout_count++;
  } else if (reason == static_cast<uint32_t>(kAssocComebackTooLongReason)) {
    g_disconnect_assoc_comeback_count++;
  } else {
    g_disconnect_other_count++;
  }
}

static bool is_ap_active_locked() { return g_state.ap_active; }

static bool is_ap_active() {
  state_lock();
  const bool active = is_ap_active_locked();
  state_unlock();
  return active;
}

static uint32_t state_poll_period_ms() {
  if (g_disconnect_event_pending || g_manual_scan_requested ||
      g_scan_purpose != ScanPurpose::kNone || g_phase == WifiPhase::kBootstrap ||
      g_phase == WifiPhase::kConnecting || g_phase == WifiPhase::kApFallback ||
      g_hotspot_only) {
    return kStatePollPeriodMs;
  }
  return kStableStatePollPeriodMs;
}

static void set_rgb(uint8_t r, uint8_t g, uint8_t b) {
  mros::platform::mros_rgb_led_set(r, g, b);
}

static const char *phase_text(WifiPhase phase) {
  switch (phase) {
    case WifiPhase::kBootstrap:
      return "bootstrap";
    case WifiPhase::kConnecting:
      return "connecting";
    case WifiPhase::kConnected:
      return "connected";
    case WifiPhase::kApFallback:
      return "ap_fallback";
    case WifiPhase::kFinalizeSuccess:
      return "finalize_success";
    case WifiPhase::kFinalizeFailure:
      return "finalize_failure";
    default:
      return "unknown";
  }
}

static bool parse_bssid(const String &text, uint8_t out_bssid[6]) {
  if (text.length() != 17) {
    return false;
  }
  for (int i = 0; i < 6; ++i) {
    const int offset = i * 3;
    if (i < 5 && text[offset + 2] != ':') {
      return false;
    }
    char part[3] = {text[offset], text[offset + 1], '\0'};
    char *end_ptr = nullptr;
    const long value = strtol(part, &end_ptr, 16);
    if (end_ptr == nullptr || *end_ptr != '\0' || value < 0 || value > 255) {
      return false;
    }
    out_bssid[i] = static_cast<uint8_t>(value);
  }
  return true;
}

static uint32_t compute_reconnect_backoff_ms(uint32_t reason, wl_status_t status) {
  if (reason == static_cast<uint32_t>(kAssocComebackTooLongReason)) {
    return 1500UL;
  }
  if (reason == static_cast<uint32_t>(kAuthFailReason) ||
      reason == static_cast<uint32_t>(kAuthExpireReason) ||
      reason == static_cast<uint32_t>(kHandshakeTimeoutReason)) {
    return kAuthReconnectScanPeriodMs;
  }
  if (reason == static_cast<uint32_t>(kNoApFoundReason) || status == WL_NO_SSID_AVAIL) {
    return kNoApReconnectScanPeriodMs;
  }
  if (reason == static_cast<uint32_t>(kAssocExpireReason) ||
      reason == static_cast<uint32_t>(kBeaconTimeoutReason)) {
    return 5000UL;
  }
  return 7000UL;
}

static String ipv4_to_string(const uint32_t raw_ip) {
  char buffer[16] = {};
  std::snprintf(buffer,
                sizeof(buffer),
                "%u.%u.%u.%u",
                static_cast<unsigned>(raw_ip & 0xFFU),
                static_cast<unsigned>((raw_ip >> 8U) & 0xFFU),
                static_cast<unsigned>((raw_ip >> 16U) & 0xFFU),
                static_cast<unsigned>((raw_ip >> 24U) & 0xFFU));
  return String(buffer);
}

static void update_ap_ip_from_netif() {
  if (g_ap_netif == nullptr) {
    g_last_ap_ip = "0.0.0.0";
    g_ap_ip_octets[0] = 192;
    g_ap_ip_octets[1] = 168;
    g_ap_ip_octets[2] = 4;
    g_ap_ip_octets[3] = 1;
    return;
  }
  esp_netif_ip_info_t info = {};
  if (esp_netif_get_ip_info(g_ap_netif, &info) == ESP_OK) {
    const uint32_t raw_ip = info.ip.addr;
    g_last_ap_ip = ipv4_to_string(raw_ip);
    g_ap_ip_octets[0] = static_cast<uint8_t>(raw_ip & 0xFFU);
    g_ap_ip_octets[1] = static_cast<uint8_t>((raw_ip >> 8U) & 0xFFU);
    g_ap_ip_octets[2] = static_cast<uint8_t>((raw_ip >> 16U) & 0xFFU);
    g_ap_ip_octets[3] = static_cast<uint8_t>((raw_ip >> 24U) & 0xFFU);
  }
}

static String current_sta_ip_string() {
  if (g_sta_netif == nullptr) return "0.0.0.0";
  esp_netif_ip_info_t info = {};
  if (esp_netif_get_ip_info(g_sta_netif, &info) != ESP_OK || info.ip.addr == 0) {
    return "0.0.0.0";
  }
  return ipv4_to_string(info.ip.addr);
}

static String idf_bssid_to_string(const uint8_t bssid[6]) {
  char text[18] = {};
  std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
  return String(text);
}

static wl_status_t status_from_disconnect_reason(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
      return WL_NO_SSID_AVAIL;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
      return WL_CONNECT_FAILED;
    default:
      return WL_DISCONNECTED;
  }
}

static wl_status_t idf_wifi_status() {
  wifi_ap_record_t ap_info = {};
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    return WL_CONNECTED;
  }
  if (g_phase == WifiPhase::kConnecting) {
    const uint32_t reason = g_state.last_disconnect_reason;
    if (reason != 0U) {
      return status_from_disconnect_reason(static_cast<uint8_t>(reason));
    }
    return WL_IDLE_STATUS;
  }
  const uint32_t reason = g_state.last_disconnect_reason;
  if (reason != 0U) {
    return status_from_disconnect_reason(static_cast<uint8_t>(reason));
  }
  return WL_DISCONNECTED;
}

static int32_t idf_sta_rssi() {
  wifi_ap_record_t ap_info = {};
  return (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ? ap_info.rssi : -127;
}

static uint8_t idf_sta_channel() {
  wifi_ap_record_t ap_info = {};
  return (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ? ap_info.primary : 0;
}

static String idf_sta_ssid() {
  wifi_ap_record_t ap_info = {};
  if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return "";
  return String(reinterpret_cast<const char*>(ap_info.ssid));
}

static String idf_sta_bssid() {
  wifi_ap_record_t ap_info = {};
  if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return "";
  return idf_bssid_to_string(ap_info.bssid);
}

static uint8_t idf_ap_station_count() {
  wifi_sta_list_t list = {};
  return (esp_wifi_ap_get_sta_list(&list) == ESP_OK) ? list.num : 0;
}

static bool ensure_idf_wifi_driver() {
  if (g_wifi_driver_ready) return true;

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    mros_console.printf("[WiFi] esp_netif_init failed: %d\n", static_cast<int>(err));
    return false;
  }
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    mros_console.printf("[WiFi] esp_event_loop_create_default failed: %d\n",
                        static_cast<int>(err));
    return false;
  }
  if (g_sta_netif == nullptr) {
    g_sta_netif = esp_netif_create_default_wifi_sta();
  }
  if (g_ap_netif == nullptr) {
    g_ap_netif = esp_netif_create_default_wifi_ap();
  }
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    mros_console.printf("[WiFi] esp_wifi_init failed: %d\n", static_cast<int>(err));
    return false;
  }
  (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);
  (void)esp_wifi_set_ps(WIFI_PS_NONE);
  if (g_sta_netif != nullptr) {
    (void)esp_netif_set_hostname(g_sta_netif, "MROS-S3-Bridge");
  }
  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    mros_console.printf("[WiFi] esp_wifi_set_mode(STA) failed: %d\n",
                        static_cast<int>(err));
    return false;
  }
  err = esp_wifi_start();
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    mros_console.printf("[WiFi] esp_wifi_start failed: %d\n", static_cast<int>(err));
    return false;
  }
  update_ap_ip_from_netif();
  g_wifi_driver_ready = true;
  return true;
}

static bool idf_set_mode(const bool sta, const bool ap) {
  if (!ensure_idf_wifi_driver()) return false;
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (sta && ap) {
    mode = WIFI_MODE_APSTA;
  } else if (sta) {
    mode = WIFI_MODE_STA;
  } else if (ap) {
    mode = WIFI_MODE_AP;
  }
  const esp_err_t err = esp_wifi_set_mode(mode);
  if (err != ESP_OK) {
    mros_console.printf("[WiFi] esp_wifi_set_mode(%d) failed: %d\n",
                        static_cast<int>(mode), static_cast<int>(err));
    return false;
  }
  update_ap_ip_from_netif();
  return true;
}

static bool ap_credentials_ready() {
  const size_t ssid_length = std::strlen(WIFI_AP_SSID);
  const size_t password_length = std::strlen(WIFI_AP_PASSWORD);
  if (ssid_length == 0U || ssid_length > 32U || password_length < 12U ||
      password_length > 63U) {
    return false;
  }
  return std::strchr(WIFI_AP_SSID, '<') == nullptr &&
         std::strchr(WIFI_AP_PASSWORD, '<') == nullptr &&
         std::strcmp(WIFI_AP_PASSWORD, "CHANGE_ME_LOCAL_ONLY") != 0;
}

static bool idf_configure_ap() {
  wifi_config_t config = {};
  std::snprintf(reinterpret_cast<char*>(config.ap.ssid), sizeof(config.ap.ssid),
                "%s", WIFI_AP_SSID);
  config.ap.ssid_len = std::strlen(WIFI_AP_SSID);
  std::snprintf(reinterpret_cast<char*>(config.ap.password), sizeof(config.ap.password),
                "%s", WIFI_AP_PASSWORD);
  config.ap.channel = 1;
  config.ap.max_connection = 4;
  config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  config.ap.pmf_cfg.required = false;
  const esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &config);
  if (err != ESP_OK) {
    mros_console.printf("[WiFi] esp_wifi_set_config(AP) failed: %d\n",
                        static_cast<int>(err));
    return false;
  }
  update_ap_ip_from_netif();
  return true;
}

static void idf_disconnect_sta() {
  (void)esp_wifi_disconnect();
}

static uint32_t compute_scan_period_ms(uint32_t reason, wl_status_t status) {
  if (reason == static_cast<uint32_t>(kAuthFailReason) ||
      reason == static_cast<uint32_t>(kAuthExpireReason) ||
      reason == static_cast<uint32_t>(kHandshakeTimeoutReason)) {
    return kAuthReconnectScanPeriodMs;
  }
  if (reason == static_cast<uint32_t>(kNoApFoundReason) || status == WL_NO_SSID_AVAIL) {
    return kNoApReconnectScanPeriodMs;
  }
  return kDefaultReconnectScanPeriodMs;
}

static void apply_disconnect_backoff(uint32_t reason, wl_status_t status, unsigned long now_ms) {
  g_current_backoff_ms = compute_reconnect_backoff_ms(reason, status);
  g_reconnect_scan_period_ms = compute_scan_period_ms(reason, status);
  g_next_connect_allowed_ms = now_ms + g_current_backoff_ms;
  state_lock();
  g_backoff_apply_count++;
  g_state_json_cache_valid = false;
  state_unlock();
  mros_console.printf("[WiFi] Backoff set to %lu ms (reason=%lu status=%d)\n",
                      static_cast<unsigned long>(g_current_backoff_ms),
                      static_cast<unsigned long>(reason),
                      static_cast<int>(status));
}

static uint32_t compute_candidate_cooldown_ms(uint32_t reason, wl_status_t status) {
  if (reason == static_cast<uint32_t>(kAuthFailReason) ||
      reason == static_cast<uint32_t>(kAuthExpireReason) ||
      reason == static_cast<uint32_t>(kHandshakeTimeoutReason)) {
    return kAuthCandidateCooldownMs;
  }
  if (reason == static_cast<uint32_t>(kNoApFoundReason) || status == WL_NO_SSID_AVAIL) {
    return kNoApCandidateCooldownMs;
  }
  return kGenericCandidateCooldownMs;
}

static bool candidate_blocked(const String &ssid, unsigned long now_ms) {
  if (ssid.isEmpty() || g_failed_candidate_ssid.isEmpty()) {
    return false;
  }
  if (now_ms >= g_failed_candidate_until_ms) {
    return false;
  }
  return ssid == g_failed_candidate_ssid;
}

static void clear_candidate_failure(const String &ssid = "") {
  if (!ssid.isEmpty() && ssid != g_failed_candidate_ssid) {
    return;
  }
  g_failed_candidate_ssid = "";
  g_failed_candidate_until_ms = 0;
}

static void note_candidate_failure(uint32_t reason, wl_status_t status, unsigned long now_ms) {
  if (g_candidate_ssid.isEmpty()) {
    return;
  }

  const uint32_t cooldown_ms = compute_candidate_cooldown_ms(reason, status);
  g_failed_candidate_ssid = g_candidate_ssid;
  g_failed_candidate_until_ms = now_ms + cooldown_ms;

  for (size_t i = 0; i < known_networks_count; ++i) {
    if (known_networks[i].ssid != nullptr && g_candidate_ssid == known_networks[i].ssid) {
      g_known_fallback_cursor = (i + 1U) % known_networks_count;
      break;
    }
  }

  mros_console.printf("[WiFi] Candidate '%s' cooled down for %lu ms; rotating\n",
                      g_candidate_ssid.c_str(),
                      static_cast<unsigned long>(cooldown_ms));
}

static void refresh_saved_credentials() {
  String ssid;
  String pass;
  if (prefs_load_wifi(ssid, pass)) {
    g_saved_ssid = ssid;
    g_saved_pass = pass;
  } else {
    g_saved_ssid = "";
    g_saved_pass = "";
  }

  String runtime_ssid;
  String runtime_bssid;
  String runtime_pass;
  uint8_t runtime_channel = 0;
  if (prefs_load_wifi_runtime(runtime_ssid, runtime_bssid, runtime_channel,
                              &runtime_pass)) {
    g_last_good_ssid = runtime_ssid;
    g_last_good_bssid = runtime_bssid;
    g_last_good_pass = runtime_pass;
    g_last_good_channel = runtime_channel;
  } else {
    g_last_good_ssid = "";
    g_last_good_bssid = "";
    g_last_good_pass = "";
    g_last_good_channel = 0;
  }
}

static bool update_state_snapshot_locked(wl_status_t status) {
  const WifiManagerState before = g_state;
  g_state.sta_connected = (status == WL_CONNECTED);
  g_state.sta_connecting = (g_phase == WifiPhase::kConnecting);
  g_state.scan_in_progress = (g_scan_purpose != ScanPurpose::kNone);
  g_state.test_in_progress =
      (!g_test_ssid.isEmpty() && g_phase == WifiPhase::kConnecting) ||
      g_phase == WifiPhase::kFinalizeSuccess ||
      g_phase == WifiPhase::kFinalizeFailure;
  g_state.rssi = g_state.sta_connected ? idf_sta_rssi() : -127;
  g_state.ap_station_count = g_state.ap_active ? idf_ap_station_count() : 0;
  return memcmp(&before, &g_state, sizeof(g_state)) != 0;
}

static void publish_runtime_state(unsigned long now_ms) {
  const wl_status_t status = idf_wifi_status();
  if (status == WL_CONNECTED) {
    g_last_ssid = idf_sta_ssid();
    g_last_ip = current_sta_ip_string();
    g_sta_lost_since_ms = 0;
  }

  state_lock();
  g_wifi_runtime_publish_count++;
  if (update_state_snapshot_locked(status)) {
    g_state_json_cache_valid = false;
    g_wifi_event_revision++;
  }
  state_unlock();
  (void)now_ms;
}

static void set_ap_active(bool active) {
  state_lock();
  g_state.ap_active = active;
  g_state.ap_station_count = active ? idf_ap_station_count() : 0;
  state_unlock();
}

static size_t build_dns_reply(const uint8_t *request,
                              const size_t request_len,
                              uint8_t *reply,
                              const size_t reply_len) {
  if (request == nullptr || reply == nullptr || request_len < 12U || reply_len < 32U) {
    return 0U;
  }
  size_t question_end = 12U;
  while (question_end < request_len && request[question_end] != 0U) {
    question_end += static_cast<size_t>(request[question_end]) + 1U;
  }
  question_end += 5U;  // null label + qtype + qclass
  if (question_end > request_len || (question_end + 16U) > reply_len) {
    return 0U;
  }
  std::memcpy(reply, request, question_end);
  reply[2] = 0x81U;
  reply[3] = 0x80U;
  reply[6] = 0x00U;
  reply[7] = 0x01U;
  reply[8] = 0x00U;
  reply[9] = 0x00U;
  reply[10] = 0x00U;
  reply[11] = 0x00U;
  size_t w = question_end;
  const uint8_t answer[] = {
      0xC0U, 0x0CU, 0x00U, 0x01U, 0x00U, 0x01U,
      0x00U, 0x00U, 0x00U, 0x3CU, 0x00U, 0x04U,
      g_ap_ip_octets[0], g_ap_ip_octets[1], g_ap_ip_octets[2], g_ap_ip_octets[3]};
  std::memcpy(reply + w, answer, sizeof(answer));
  w += sizeof(answer);
  return w;
}

static void dns_lwip_task(void *) {
  sockaddr_in server_addr = {};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(53);

  const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    state_lock();
    g_dns_error_count++;
    g_dns_lwip_active = false;
    state_unlock();
    g_dns_running = false;
    g_dns_task_handle = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  g_dns_socket_fd = sock;
  int reuse = 1;
  (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  timeval timeout = {};
  timeout.tv_sec = 0;
  timeout.tv_usec = 250000;
  (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  if (bind(sock, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) != 0) {
    close(sock);
    g_dns_socket_fd = -1;
    state_lock();
    g_dns_error_count++;
    g_dns_lwip_active = false;
    state_unlock();
    g_dns_running = false;
    g_dns_task_handle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  state_lock();
  g_dns_lwip_active = true;
  state_unlock();

  while (!g_dns_stop_requested) {
    uint8_t query[256] = {};
    sockaddr_in source_addr = {};
    socklen_t source_len = sizeof(source_addr);
    const int received = recvfrom(sock, query, sizeof(query), 0,
                                  reinterpret_cast<sockaddr *>(&source_addr),
                                  &source_len);
    if (received <= 0) {
      continue;
    }
    uint8_t reply[320] = {};
    const size_t reply_len = build_dns_reply(query, static_cast<size_t>(received),
                                             reply, sizeof(reply));
    state_lock();
    g_dns_query_count++;
    state_unlock();
    if (reply_len == 0U) {
      state_lock();
      g_dns_error_count++;
      state_unlock();
      continue;
    }
    const int sent = sendto(sock, reply, reply_len, 0,
                            reinterpret_cast<sockaddr *>(&source_addr),
                            source_len);
    state_lock();
    if (sent > 0) {
      g_dns_reply_count++;
    } else {
      g_dns_error_count++;
    }
    state_unlock();
  }
  close(sock);
  g_dns_socket_fd = -1;
  state_lock();
  g_dns_lwip_active = false;
  state_unlock();
  g_dns_task_handle = nullptr;
  vTaskDelete(nullptr);
}

static void stop_dns() {
  if (!g_dns_running) {
    return;
  }
  g_dns_stop_requested = true;
  if (g_dns_socket_fd >= 0) {
    close(g_dns_socket_fd);
    g_dns_socket_fd = -1;
  }
  state_lock();
  g_dns_lwip_active = false;
  state_unlock();
  g_dns_running = false;
}

static bool start_dns() {
  if (g_dns_running) {
    return true;
  }
  g_dns_stop_requested = false;
  if (xTaskCreate(dns_lwip_task, "wifi_dns_lwip", 4096, nullptr, 2,
                  &g_dns_task_handle) == pdPASS) {
    g_dns_running = true;
    return true;
  }
  state_lock();
  g_dns_error_count++;
  state_unlock();
  return false;
}

static void service_dns() {}

static void start_ap_mode() {
  if (is_ap_active()) {
    return;
  }
  if (!ap_credentials_ready()) {
    mros_console.println(
        "[WiFi] Hotspot disabled: provision a device-specific AP password");
    return;
  }
  if (!idf_set_mode(true, true) || !idf_configure_ap()) {
    mros_console.println("[WiFi] Hotspot start failed");
    return;
  }
  update_ap_ip_from_netif();
  (void)start_dns();
  set_ap_active(true);
  mros_console.printf("[WiFi] Hotspot active SSID='%s' AP=%s\n", WIFI_AP_SSID,
                      g_last_ap_ip.c_str());
}

static void stop_ap_mode() {
  if (!is_ap_active()) {
    return;
  }
  stop_dns();
  (void)idf_set_mode(true, false);
  g_last_ap_ip = "0.0.0.0";
  set_ap_active(false);
}

static const char* wifi_security_name(const int enc) {
  switch (enc) {
    case WIFI_AUTH_OPEN:
      return "open";
    case WIFI_AUTH_WEP:
      return "wep";
    case WIFI_AUTH_WPA_PSK:
      return "wpa";
    case WIFI_AUTH_WPA2_PSK:
      return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "wpa-wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "wpa2-enterprise";
#if defined(WIFI_AUTH_WPA3_PSK)
    case WIFI_AUTH_WPA3_PSK:
      return "wpa3";
#endif
#if defined(WIFI_AUTH_WPA2_WPA3_PSK)
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "wpa2-wpa3";
#endif
#if defined(WIFI_AUTH_WAPI_PSK)
    case WIFI_AUTH_WAPI_PSK:
      return "wapi";
#endif
    default:
      return "unknown";
  }
}

static uint32_t wifi_signal_quality(const int32_t rssi) {
  if (rssi <= -100) {
    return 0U;
  }
  if (rssi >= -50) {
    return 100U;
  }
  return static_cast<uint32_t>((rssi + 100) * 2);
}

static bool wifi_scan_ssid_known(const String& ssid) {
  if (!g_saved_ssid.isEmpty() && ssid == g_saved_ssid) {
    return true;
  }
  for (size_t i = 0; i < known_networks_count; ++i) {
    if (known_networks[i].ssid != nullptr && ssid == known_networks[i].ssid) {
      return true;
    }
  }
  return false;
}

static String mac_to_string(const uint8_t mac[6]) {
  char text[18] = {};
  std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(text);
}

static void cache_scan_results_from_driver(int count, ScanPurpose purpose,
                                           unsigned long now_ms) {
  uint16_t ap_count = 0;
  if (count > 0) {
    ap_count = static_cast<uint16_t>(std::min<int>(count, kCachedScanRecordMax));
  }
  wifi_ap_record_t* records = nullptr;
  if (ap_count > 0U) {
    records = static_cast<wifi_ap_record_t*>(
        heap_caps_calloc(ap_count, sizeof(wifi_ap_record_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (records == nullptr) {
      records = static_cast<wifi_ap_record_t*>(
          heap_caps_calloc(ap_count, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (records != nullptr) {
      uint16_t requested = ap_count;
      if (esp_wifi_scan_get_ap_records(&requested, records) == ESP_OK) {
        ap_count = requested;
      } else {
        ap_count = 0U;
      }
    } else {
      ap_count = 0U;
    }
  }

  for (uint8_t i = 0; i < kCachedScanRecordMax; ++i) {
    g_scan_cached_ssid[i] = "";
    g_scan_cached_bssid[i] = "";
    g_scan_cached_rssi[i] = -127;
    g_scan_cached_channel[i] = 0;
    g_scan_cached_enc[i] = WIFI_AUTH_OPEN;
  }
  g_scan_cached_count = std::min<uint16_t>(ap_count, kCachedScanRecordMax);

  String text = "";
  const int visible_count = static_cast<int>(g_scan_cached_count);
  text.reserve(static_cast<size_t>(std::max(visible_count, 1)) * 64U + 24U);
  const size_t json_capacity =
      std::min<size_t>(8192U, 128U + static_cast<size_t>(std::max(visible_count, 1)) * 224U);
  char* json_buffer = static_cast<char*>(
      heap_caps_malloc(json_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (json_buffer == nullptr) {
    json_buffer = static_cast<char*>(
        heap_caps_malloc(json_capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  mros::utils::FixedJsonWriter writer(json_buffer, json_capacity);
  writer.append_raw("[");
  if (g_scan_cached_count == 0U) {
    text = "No networks visible.\n";
  }
  for (uint16_t i = 0; i < g_scan_cached_count; ++i) {
    if (i > 0) {
      writer.append_raw(",");
    }
    const wifi_ap_record_t& rec = records[i];
    const String ssid = String(reinterpret_cast<const char*>(rec.ssid));
    const int32_t rssi = rec.rssi;
    const int channel = rec.primary;
    const int enc = static_cast<int>(rec.authmode);
    const String bssid = mac_to_string(rec.bssid);
    g_scan_cached_ssid[i] = ssid;
    g_scan_cached_bssid[i] = bssid;
    g_scan_cached_rssi[i] = rssi;
    g_scan_cached_channel[i] = channel;
    g_scan_cached_enc[i] = enc;
    const bool known = wifi_scan_ssid_known(ssid);
    const bool last_good = (!g_last_good_ssid.isEmpty() && ssid == g_last_good_ssid);
    const bool current = (g_state.sta_connected && ssid == g_last_ssid);
    writer.append_raw("{\"ssid\":\"");
    writer.append_escaped(ssid.c_str());
    writer.append_raw("\",\"rssi\":");
    writer.i32(rssi);
    writer.append_raw(",\"channel\":");
    writer.i32(channel);
    writer.append_raw(",\"enc\":");
    writer.i32(enc);
    writer.append_raw(",\"security\":\"");
    writer.append_escaped(wifi_security_name(enc));
    writer.append_raw("\",\"bssid\":\"");
    writer.append_escaped(bssid.c_str());
    writer.append_raw("\",\"quality\":");
    writer.u32(wifi_signal_quality(rssi));
    writer.append_raw(",\"known\":");
    writer.append_raw(known ? "true" : "false");
    writer.append_raw(",\"last_good\":");
    writer.append_raw(last_good ? "true" : "false");
    writer.append_raw(",\"current\":");
    writer.append_raw(current ? "true" : "false");
    writer.append_raw("}");
    text += String(i + 1);
    text += ") ";
    text += ssid;
    text += "  rssi=";
    text += String(rssi);
    text += "dBm  ch=";
    text += String(channel);
    text += "  enc=";
    text += String(enc);
    text += "  sec=";
    text += wifi_security_name(enc);
    text += "  bssid=";
    text += bssid;
    if (known) text += "  known";
    if (last_good) text += "  last-good";
    if (current) text += "  current";
    text += "\n";
  }
  writer.append_raw("]");
  const String json =
      (json_buffer != nullptr && !writer.overflow()) ? String(writer.c_str()) : String("[]");
  if (json_buffer != nullptr) {
    heap_caps_free(json_buffer);
  }
  if (records != nullptr) {
    heap_caps_free(records);
  }

  state_lock();
  if (purpose == ScanPurpose::kManual) {
    g_manual_scan_results = json;
    g_manual_scan_text = text;
    g_manual_scan_cached_ms = now_ms;
  } else {
    g_bootstrap_scan_results = json;
    g_bootstrap_scan_text = text;
    g_bootstrap_scan_cached_ms = now_ms;
  }
  state_unlock();
}

static bool start_async_scan(ScanPurpose purpose, unsigned long now_ms) {
  if (g_phase == WifiPhase::kConnecting) {
    state_lock();
    g_scan_denied_count++;
    if (purpose == ScanPurpose::kManual) g_manual_scan_denied_count++;
    g_state_json_cache_valid = false;
    state_unlock();
    return false;
  }

  if (g_scan_started_with_idf || g_scan_purpose != ScanPurpose::kNone) {
    state_lock();
    g_scan_denied_count++;
    if (purpose == ScanPurpose::kManual) g_manual_scan_denied_count++;
    g_state_json_cache_valid = false;
    state_unlock();
    return false;
  }

  wifi_scan_config_t scan_config = {};
  scan_config.show_hidden = true;
  const esp_err_t idf_scan_status = esp_wifi_scan_start(&scan_config, false);
  if (idf_scan_status == ESP_OK) {
    g_scan_started_with_idf = true;
    g_idf_scan_done_pending = false;
    g_idf_scan_done_ap_count = 0;
    g_scan_purpose = purpose;
    g_scan_started_ms = now_ms;
    if (purpose == ScanPurpose::kManual) {
      g_manual_scan_requested = false;
    } else {
      g_bootstrap_scan_pending = false;
    }
    state_lock();
    g_scan_started_count++;
    g_idf_scan_start_count++;
    g_state.scan_in_progress = true;
    g_state_json_cache_valid = false;
    state_unlock();
    notify_task();
    return true;
  }

  state_lock();
  g_scan_denied_count++;
  if (purpose == ScanPurpose::kManual) g_manual_scan_denied_count++;
  g_state_json_cache_valid = false;
  state_unlock();
  mros_console.printf("[WiFi] IDF scan start failed: %d\n", static_cast<int>(idf_scan_status));
  return false;
}

static bool select_candidate_from_scan(int scan_count, unsigned long now_ms) {
  (void)scan_count;
  const uint16_t visible = g_scan_cached_count;
  const bool has_saved = !g_saved_ssid.isEmpty();
  const bool has_last_good = !g_last_good_ssid.isEmpty();

  g_candidate_ssid = "";
  g_candidate_pass = "";

  if (has_last_good && !candidate_blocked(g_last_good_ssid, now_ms)) {
    for (uint16_t i = 0; i < visible; ++i) {
      if (g_scan_cached_ssid[i] == g_last_good_ssid) {
        g_candidate_ssid = g_last_good_ssid;
        g_candidate_pass = g_last_good_pass;
        if (g_scan_cached_channel[i] > 0) {
          g_last_good_channel = static_cast<uint8_t>(g_scan_cached_channel[i]);
        }
        if (!g_scan_cached_bssid[i].isEmpty()) {
          g_last_good_bssid = g_scan_cached_bssid[i];
        }
        if (g_candidate_pass.isEmpty() && g_saved_ssid == g_last_good_ssid) {
          g_candidate_pass = g_saved_pass;
        }
        if (g_candidate_pass.isEmpty()) {
          for (size_t j = 0; j < known_networks_count; ++j) {
            if (known_networks[j].ssid != nullptr &&
                g_last_good_ssid == known_networks[j].ssid) {
              g_candidate_pass = known_networks[j].pass != nullptr
                                     ? known_networks[j].pass
                                     : "";
              break;
            }
          }
        }
        return true;
      }
    }
  }

  if (has_saved) {
    for (uint16_t i = 0; i < visible; ++i) {
      if (g_scan_cached_ssid[i] == g_saved_ssid && !candidate_blocked(g_saved_ssid, now_ms)) {
        g_candidate_ssid = g_saved_ssid;
        g_candidate_pass = g_saved_pass;
        return true;
      }
    }
  }

  int best_rssi = -200;
  size_t best_index = known_networks_count;
  for (uint16_t i = 0; i < visible; ++i) {
    const String ssid = g_scan_cached_ssid[i];
    const int rssi = g_scan_cached_rssi[i];
    for (size_t j = 0; j < known_networks_count; ++j) {
      if (known_networks[j].ssid == nullptr || strlen(known_networks[j].ssid) == 0) {
        continue;
      }
      if (candidate_blocked(known_networks[j].ssid, now_ms)) {
        continue;
      }
      if (ssid == known_networks[j].ssid && rssi > best_rssi) {
        best_rssi = rssi;
        best_index = j;
      }
    }
  }

  if (best_index < known_networks_count) {
    g_candidate_ssid = known_networks[best_index].ssid;
    g_candidate_pass = known_networks[best_index].pass != nullptr ? known_networks[best_index].pass : "";
    return true;
  }

  if (has_saved && !candidate_blocked(g_saved_ssid, now_ms)) {
    g_candidate_ssid = g_saved_ssid;
    g_candidate_pass = g_saved_pass;
    return true;
  }

  if (known_networks_count > 0) {
    const size_t start_index = g_known_fallback_cursor % known_networks_count;
    for (size_t step = 0; step < known_networks_count; ++step) {
      const size_t index = (start_index + step) % known_networks_count;
      if (known_networks[index].ssid == nullptr || strlen(known_networks[index].ssid) == 0) {
        continue;
      }
      if (candidate_blocked(known_networks[index].ssid, now_ms)) {
        continue;
      }
      g_candidate_ssid = known_networks[index].ssid;
      g_candidate_pass = known_networks[index].pass != nullptr ? known_networks[index].pass : "";
      g_known_fallback_cursor = (index + 1U) % known_networks_count;
      return true;
    }
  }

  return false;
}

static void begin_sta_attempt(const String &ssid, const String &pass,
                              unsigned long now_ms, bool preserve_ap) {
  if (ssid.isEmpty()) {
    return;
  }
  (void)mros::power::acquire_lock(mros::power::LockOwner::WifiReconnect,
                                  "wifi-connect");

  g_phase_started_ms = now_ms;
  g_assoc_comeback_too_long = false;
  g_disconnect_event_pending = false;
  g_retry_due_ms = 0;
  g_current_backoff_ms = 0;
  state_lock();
  g_state.last_disconnect_reason = 0;
  g_connect_attempt_count++;
  g_state_json_cache_valid = false;
  state_unlock();

  const bool keep_ap = preserve_ap || is_ap_active();
  if (keep_ap) {
    (void)idf_set_mode(true, true);
    set_ap_active(true);
  } else {
    (void)idf_set_mode(true, false);
  }

  const wl_status_t previous_status = idf_wifi_status();
  if (previous_status == WL_CONNECTED || previous_status == WL_IDLE_STATUS) {
    idf_disconnect_sta();
    mros::platform::mros_delay_ms(20);
  }

  bool used_fast_path = false;
  wifi_config_t sta_config = {};
  std::snprintf(reinterpret_cast<char*>(sta_config.sta.ssid),
                sizeof(sta_config.sta.ssid),
                "%s",
                ssid.c_str());
  std::snprintf(reinterpret_cast<char*>(sta_config.sta.password),
                sizeof(sta_config.sta.password),
                "%s",
                pass.c_str());
  if (ssid == g_last_good_ssid && g_last_good_channel > 0 && g_last_good_bssid.length() > 0) {
    uint8_t bssid[6] = {};
    if (parse_bssid(g_last_good_bssid, bssid)) {
      sta_config.sta.channel = g_last_good_channel;
      sta_config.sta.bssid_set = true;
      std::memcpy(sta_config.sta.bssid, bssid, sizeof(sta_config.sta.bssid));
      used_fast_path = true;
    }
  }
  const esp_err_t cfg_status = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
  const esp_err_t connect_status = (cfg_status == ESP_OK) ? esp_wifi_connect() : cfg_status;
  if (connect_status != ESP_OK) {
    state_lock();
    g_connect_fail_count++;
    g_state.last_disconnect_reason = static_cast<uint32_t>(connect_status);
    g_state_json_cache_valid = false;
    state_unlock();
    mros_console.printf("[WiFi] esp_wifi_connect failed: cfg=%d connect=%d SSID='%s'\n",
                        static_cast<int>(cfg_status),
                        static_cast<int>(connect_status),
                        ssid.c_str());
    (void)mros::power::release_lock(mros::power::LockOwner::WifiReconnect);
    return;
  }

  state_lock();
  g_state.reconnect_attempts++;
  g_current_attempt_fast_path = used_fast_path;
  if (used_fast_path && g_fast_path_attempt_count != 0xFFFFFFFFUL) {
    ++g_fast_path_attempt_count;
  }
  const unsigned long attempts = g_state.reconnect_attempts;
  g_state_json_cache_valid = false;
  state_unlock();

  g_phase = WifiPhase::kConnecting;
  mros_console.printf("[WiFi] STA attempt #%lu SSID='%s'%s%s\n", attempts,
                      ssid.c_str(), keep_ap ? " (AP kept alive)" : "",
                      used_fast_path ? " [BSSID fast-path]" : "");
  notify_task();
}

static void schedule_bootstrap_scan(unsigned long now_ms, bool force) {
  if (g_scan_purpose != ScanPurpose::kNone) {
    return;
  }
  if (!force && (now_ms - g_last_reconnect_scan_ms) < g_reconnect_scan_period_ms) {
    return;
  }
  g_last_reconnect_scan_ms = now_ms;
  g_bootstrap_scan_pending = true;
  notify_task();
}

static void save_last_good_profile_if_needed() {
  if (idf_wifi_status() != WL_CONNECTED) {
    return;
  }
  const String ssid = idf_sta_ssid();
  const String bssid = idf_sta_bssid();
  const uint8_t channel = idf_sta_channel();
  if (ssid.isEmpty() || bssid.isEmpty() || channel == 0) {
    return;
  }
  if (ssid == g_last_good_ssid && bssid == g_last_good_bssid &&
      channel == g_last_good_channel) {
    return;
  }
  g_last_good_ssid = ssid;
  g_last_good_bssid = bssid;
  g_last_good_channel = channel;
  String pass = g_test_ssid.isEmpty() ? g_candidate_pass : g_test_pass;
  if (pass.isEmpty() && ssid == g_saved_ssid) {
    pass = g_saved_pass;
  }
  if (pass.isEmpty()) {
    for (size_t i = 0; i < known_networks_count; ++i) {
      if (known_networks[i].ssid != nullptr && ssid == known_networks[i].ssid) {
        pass = known_networks[i].pass != nullptr ? known_networks[i].pass : "";
        break;
      }
    }
  }
  prefs_save_wifi_runtime(ssid, bssid, channel, pass);
}

static void handle_scan_result(unsigned long now_ms, int scan_count, ScanPurpose purpose) {
  cache_scan_results_from_driver(scan_count > 0 ? scan_count : 0, purpose, now_ms);
  g_scan_purpose = ScanPurpose::kNone;
  g_scan_started_with_idf = false;
  g_idf_scan_done_pending = false;
  g_idf_scan_done_ap_count = 0;
  state_lock();
  g_scan_done_count++;
  g_last_scan_duration_ms = now_ms - g_scan_started_ms;
  g_state.scan_in_progress = false;
  g_state_json_cache_valid = false;
  state_unlock();

  if (purpose == ScanPurpose::kManual) {
    return;
  }

  if (idf_wifi_status() == WL_CONNECTED || g_phase == WifiPhase::kConnected) {
    g_bootstrap_scan_pending = false;
    return;
  }

  if (now_ms < g_next_connect_allowed_ms) {
    g_bootstrap_scan_pending = true;
    return;
  }

  if (select_candidate_from_scan(scan_count, now_ms)) {
    begin_sta_attempt(g_candidate_ssid, g_candidate_pass, now_ms, is_ap_active());
    return;
  }

  if (!is_ap_active()) {
    start_ap_mode();
  }
  g_phase = WifiPhase::kApFallback;
}

static void service_scan(unsigned long now_ms) {
  if (g_scan_purpose == ScanPurpose::kNone) {
    if (g_manual_scan_requested &&
        g_phase != WifiPhase::kConnecting &&
        g_phase != WifiPhase::kFinalizeSuccess &&
        g_phase != WifiPhase::kFinalizeFailure) {
      (void)start_async_scan(ScanPurpose::kManual, now_ms);
    } else if (g_bootstrap_scan_pending &&
               now_ms >= g_next_connect_allowed_ms &&
               idf_wifi_status() != WL_CONNECTED &&
               g_phase != WifiPhase::kConnected &&
               g_phase != WifiPhase::kConnecting &&
               g_phase != WifiPhase::kFinalizeSuccess &&
               g_phase != WifiPhase::kFinalizeFailure) {
      (void)start_async_scan(ScanPurpose::kBootstrap, now_ms);
    }
  }

  if (g_scan_purpose == ScanPurpose::kNone) {
    return;
  }

  if (g_scan_started_with_idf && g_idf_scan_done_pending) {
    const int idf_count = static_cast<int>(g_idf_scan_done_ap_count);
    handle_scan_result(now_ms, idf_count, g_scan_purpose);
    return;
  }

  if (g_scan_started_with_idf) {
    if ((now_ms - g_scan_started_ms) >= kBootScanTimeoutMs) {
      mros_console.println("[WiFi] IDF scan timed out");
      (void)esp_wifi_scan_stop();
      g_scan_purpose = ScanPurpose::kNone;
      g_scan_started_with_idf = false;
      g_idf_scan_done_pending = false;
      state_lock();
      g_scan_timeout_count++;
      g_last_scan_duration_ms = now_ms - g_scan_started_ms;
      g_state.scan_in_progress = false;
      g_state_json_cache_valid = false;
      state_unlock();
      if (!is_ap_active()) {
        start_ap_mode();
      }
      g_phase = WifiPhase::kApFallback;
      g_bootstrap_scan_pending = true;
    }
    return;
  }

}

static void service_connecting(unsigned long now_ms) {
  if (g_retry_due_ms != 0) {
    if (now_ms < g_retry_due_ms) {
      return;
    }
    const String retry_ssid = g_test_ssid.isEmpty() ? g_candidate_ssid : g_test_ssid;
    const String retry_pass = g_test_ssid.isEmpty() ? g_candidate_pass : g_test_pass;
    begin_sta_attempt(retry_ssid, retry_pass, now_ms, !g_test_ssid.isEmpty());
    return;
  }

  if (g_disconnect_event_pending) {
    g_disconnect_event_pending = false;
    const uint32_t reason = g_state.last_disconnect_reason;
    if (reason == static_cast<uint32_t>(kNoApFoundReason) ||
        reason == static_cast<uint32_t>(kAuthFailReason) ||
        reason == static_cast<uint32_t>(kAuthExpireReason) ||
        reason == static_cast<uint32_t>(kHandshakeTimeoutReason)) {
      state_lock();
      g_connect_fail_count++;
      g_current_attempt_fast_path = false;
      g_last_connect_duration_ms = now_ms - g_phase_started_ms;
      g_state_json_cache_valid = false;
      state_unlock();
      mros_console.printf("[WiFi] STA disconnected while connecting, reason=%lu\n",
                          static_cast<unsigned long>(reason));
      if (g_test_ssid.isEmpty()) {
        const wl_status_t status = idf_wifi_status();
        note_candidate_failure(reason, status, now_ms);
        apply_disconnect_backoff(reason, status, now_ms);
        start_ap_mode();
        g_phase = WifiPhase::kApFallback;
        g_bootstrap_scan_pending = true;
      } else {
        g_phase = WifiPhase::kFinalizeFailure;
        g_finalize_blink_count = 0;
        g_last_led_ms = now_ms;
      }
      (void)mros::power::release_lock(mros::power::LockOwner::WifiReconnect);
      return;
    }
  }

  if (idf_wifi_status() == WL_CONNECTED) {
    publish_runtime_state(now_ms);
    save_last_good_profile_if_needed();
    const String ssid = idf_sta_ssid();
    const String ip = current_sta_ip_string();
    const String bssid = idf_sta_bssid();
    const uint8_t channel = idf_sta_channel();
    clear_candidate_failure(ssid);
    g_assoc_retry_count = 0;
    g_bootstrap_scan_pending = false;
    g_next_connect_allowed_ms = 0;
    g_finalize_blink_count = 0;
    g_last_led_ms = now_ms;
    g_led_state = false;
    g_ap_grace_until_ms = is_ap_active() ? (now_ms + kApGracePeriodMs) : 0;
    state_lock();
    g_connect_success_count++;
    if (g_current_attempt_fast_path && g_fast_path_success_count != 0xFFFFFFFFUL) {
      ++g_fast_path_success_count;
    }
    g_current_attempt_fast_path = false;
    g_last_connect_duration_ms = now_ms - g_phase_started_ms;
    g_state_json_cache_valid = false;
    state_unlock();

    mros_console.printf("[WiFi] Connected to '%s' | IP=%s channel=%u bssid=%s\n",
                        ssid.c_str(), ip.c_str(),
                        static_cast<unsigned>(channel), bssid.c_str());

    if (!g_test_ssid.isEmpty()) {
      prefs_save_wifi(g_test_ssid, g_test_pass);
      g_phase = WifiPhase::kFinalizeSuccess;
      mros_console.println("[WiFi] Test credentials saved");
    } else {
      g_phase = WifiPhase::kConnected;
    }
    (void)mros::power::release_lock(mros::power::LockOwner::WifiReconnect);
    return;
  }

  if (g_assoc_comeback_too_long) {
    g_assoc_comeback_too_long = false;
    if (++g_assoc_retry_count <= kAssocRetryLimit) {
      state_lock();
      g_assoc_retry_schedule_count++;
      g_state_json_cache_valid = false;
      state_unlock();
      const uint32_t retry_delay_ms =
          kAssocRetryBaseDelayMs + (g_assoc_retry_count * 250UL);
      mros_console.printf("[WiFi] Reason 208 retry %u/%u after %lu ms\n",
                          static_cast<unsigned>(g_assoc_retry_count),
                          static_cast<unsigned>(kAssocRetryLimit),
                          static_cast<unsigned long>(retry_delay_ms));
      const wl_status_t previous_status = idf_wifi_status();
      if (previous_status == WL_CONNECTED || previous_status == WL_IDLE_STATUS) {
        idf_disconnect_sta();
      }
      g_retry_due_ms = now_ms + retry_delay_ms;
      notify_task();
      return;
    }

    mros_console.println("[WiFi] Reason 208 repeated, entering fallback");
    if (g_test_ssid.isEmpty()) {
      const wl_status_t status = idf_wifi_status();
      note_candidate_failure(kAssocComebackTooLongReason, status, now_ms);
      apply_disconnect_backoff(kAssocComebackTooLongReason, status, now_ms);
      start_ap_mode();
      g_phase = WifiPhase::kApFallback;
      g_bootstrap_scan_pending = true;
    } else {
      g_phase = WifiPhase::kFinalizeFailure;
      g_finalize_blink_count = 0;
      g_last_led_ms = now_ms;
    }
    (void)mros::power::release_lock(mros::power::LockOwner::WifiReconnect);
    return;
  }

  const wl_status_t status = idf_wifi_status();
  const uint32_t connect_elapsed_ms = now_ms - g_phase_started_ms;
  const bool terminal_status =
      (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) &&
      connect_elapsed_ms >= kConnectStatusGraceMs;
  const bool connect_timed_out = connect_elapsed_ms >= kConnectTimeoutMs;
  if (terminal_status || connect_timed_out) {
    const uint32_t reason = g_state.last_disconnect_reason;
    state_lock();
    g_connect_fail_count++;
    g_current_attempt_fast_path = false;
    g_last_connect_duration_ms = now_ms - g_phase_started_ms;
    g_state_json_cache_valid = false;
    state_unlock();
    mros_console.printf("[WiFi] Connect failed status=%d reason=%lu\n",
                        static_cast<int>(status),
                        static_cast<unsigned long>(reason));
    if (g_test_ssid.isEmpty()) {
      note_candidate_failure(reason, status, now_ms);
      apply_disconnect_backoff(reason, status, now_ms);
      start_ap_mode();
      g_phase = WifiPhase::kApFallback;
      g_bootstrap_scan_pending = true;
    } else {
      g_phase = WifiPhase::kFinalizeFailure;
      g_finalize_blink_count = 0;
      g_last_led_ms = now_ms;
    }
    (void)mros::power::release_lock(mros::power::LockOwner::WifiReconnect);
  }
}

static void service_connected(unsigned long now_ms) {
  if (idf_wifi_status() == WL_CONNECTED) {
    if (is_ap_active() && g_ap_grace_until_ms != 0 && now_ms >= g_ap_grace_until_ms) {
      stop_ap_mode();
      g_ap_grace_until_ms = 0;
    }
    return;
  }

  if (g_sta_lost_since_ms == 0) {
    g_sta_lost_since_ms = now_ms;
  }

  if (g_disconnect_event_pending) {
    g_disconnect_event_pending = false;
    mros_console.printf("[WiFi] STA lost, reason=%lu. Re-scanning.\n",
                        static_cast<unsigned long>(g_state.last_disconnect_reason));
    apply_disconnect_backoff(g_state.last_disconnect_reason, idf_wifi_status(), now_ms);
  }

  if ((now_ms - g_sta_lost_since_ms) >= kApFallbackDelayMs && !is_ap_active()) {
    start_ap_mode();
  }

  g_phase = WifiPhase::kApFallback;
  g_bootstrap_scan_pending = true;
  schedule_bootstrap_scan(now_ms, true);
}

static void service_ap_fallback(unsigned long now_ms) {
  if (!is_ap_active()) {
    start_ap_mode();
  }
  if (idf_wifi_status() == WL_CONNECTED) {
    g_ap_grace_until_ms = now_ms + kApGracePeriodMs;
    g_phase = WifiPhase::kConnected;
    return;
  }
  if (now_ms >= g_next_connect_allowed_ms) {
    schedule_bootstrap_scan(now_ms, false);
  }
}

static void service_finalize(unsigned long now_ms) {
  const uint32_t blink_period =
      (g_phase == WifiPhase::kFinalizeSuccess) ? 500UL : 400UL;
  if ((now_ms - g_last_led_ms) < blink_period) {
    return;
  }

  g_last_led_ms = now_ms;
  g_led_state = !g_led_state;

  if (g_phase == WifiPhase::kFinalizeSuccess) {
    set_rgb(0, g_led_state ? 255 : 0, 0);
    if (!g_led_state && ++g_finalize_blink_count >= 2) {
      mros::platform::mros_system_restart();
    }
    return;
  }

  set_rgb(255, g_led_state ? 100 : 0, 0);
  if (!g_led_state && ++g_finalize_blink_count >= 3) {
    g_test_ssid = "";
    g_test_pass = "";
    g_finalize_blink_count = 0;
    g_phase = WifiPhase::kApFallback;
    start_ap_mode();
  }
}

static void service_leds(unsigned long now_ms) {
  if (g_phase == WifiPhase::kFinalizeSuccess || g_phase == WifiPhase::kFinalizeFailure) {
    return;
  }

  uint32_t interval_ms = 0;
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  if (g_scan_purpose != ScanPurpose::kNone) {
    interval_ms = kLedBlinkSlowMs;
    g = 128;
    b = 128;
  } else if (g_phase == WifiPhase::kConnecting) {
    interval_ms = kLedBlinkFastMs;
    g = 128;
  } else if (is_ap_active() && idf_wifi_status() != WL_CONNECTED) {
    interval_ms = kLedBlinkSlowMs;
    r = 255;
    g = 100;
  } else if (idf_wifi_status() == WL_CONNECTED) {
    set_rgb(0, 48, 0);
    g_led_state = true;
    return;
  } else {
    set_rgb(0, 0, 0);
    return;
  }

  if ((now_ms - g_last_led_ms) < interval_ms) {
    return;
  }

  g_last_led_ms = now_ms;
  g_led_state = !g_led_state;
  if (g_led_state) {
    set_rgb(r, g, b);
  } else {
    set_rgb(0, 0, 0);
  }
}

static void on_idf_wifi_event(void *, esp_event_base_t event_base, int32_t event_id,
                              void *event_data) {
  const uint32_t now_ms = mros::platform::mros_millis();
  g_idf_event_count++;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    uint32_t reason = 0;
    if (event_data != nullptr) {
      const auto *disc = static_cast<const wifi_event_sta_disconnected_t *>(event_data);
      reason = static_cast<uint32_t>(disc->reason);
    }
    state_lock();
    g_event_sta_disconnected++;
    g_state.last_disconnect_reason = reason;
    note_disconnect_reason_locked(reason);
    note_event_locked("idf_sta_disconnected", now_ms);
    state_unlock();
    g_disconnect_event_pending = true;
    g_assoc_comeback_too_long = (reason == static_cast<uint32_t>(kAssocComebackTooLongReason));
    notify_task();
    return;
  }
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
    state_lock();
    g_event_sta_connected++;
    note_event_locked("idf_sta_connected", now_ms);
    state_unlock();
    notify_task();
    return;
  }
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    state_lock();
    g_event_sta_got_ip++;
    note_event_locked("idf_sta_got_ip", now_ms);
    state_unlock();
    notify_task();
    return;
  }
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
    state_lock();
    g_event_ap_sta_connected++;
    note_event_locked("idf_ap_sta_connected", now_ms);
    state_unlock();
    notify_task();
    return;
  }
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    state_lock();
    g_event_ap_sta_disconnected++;
    note_event_locked("idf_ap_sta_disconnected", now_ms);
    state_unlock();
    notify_task();
    return;
  }
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
    uint16_t ap_count = 0;
    if (event_data != nullptr) {
      const auto *scan_done = static_cast<const wifi_event_sta_scan_done_t *>(event_data);
      ap_count = scan_done->number;
    }
    g_idf_scan_done_ap_count = ap_count;
    g_idf_scan_done_pending = true;
    state_lock();
    note_event_locked("idf_scan_done", now_ms);
    state_unlock();
    notify_task();
  }
}

static bool register_wifi_event_handlers() {
  const esp_err_t wifi_status = esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &on_idf_wifi_event, nullptr,
      &g_wifi_event_instance);
  const esp_err_t ip_status = esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &on_idf_wifi_event, nullptr,
      &g_ip_event_instance);
  if (wifi_status == ESP_OK && ip_status == ESP_OK) {
    state_lock();
    g_native_event_active = true;
    state_unlock();
    return true;
  }
  if (g_wifi_event_instance != nullptr) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          g_wifi_event_instance);
    g_wifi_event_instance = nullptr;
  }
  if (g_ip_event_instance != nullptr) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          g_ip_event_instance);
    g_ip_event_instance = nullptr;
  }
  state_lock();
  g_native_event_active = false;
  state_unlock();
  return false;
}

}  // namespace

void wifi_manager_set_task_handle(TaskHandle_t task_handle) {
  g_task_handle = task_handle;
}

uint32_t wifi_manager_wait_timeout_ms(unsigned long now_ms) {
  if (g_phase == WifiPhase::kFinalizeSuccess || g_phase == WifiPhase::kFinalizeFailure) {
    return kBusyWaitMs;
  }
  if (g_scan_purpose != ScanPurpose::kNone || g_phase == WifiPhase::kConnecting) {
    return kBusyWaitMs;
  }
  if (is_ap_active()) {
    return kApWaitMs;
  }
  if (g_ap_grace_until_ms != 0 && now_ms < g_ap_grace_until_ms) {
    const uint32_t remaining = g_ap_grace_until_ms - now_ms;
    return remaining < kStableWaitMs ? remaining : kStableWaitMs;
  }
  if (g_bootstrap_scan_pending && now_ms < g_next_connect_allowed_ms) {
    const uint32_t remaining = g_next_connect_allowed_ms - now_ms;
    return remaining < kLongWaitMs ? remaining : kLongWaitMs;
  }
  return kIdleWaitMs;
}

void wifi_manager_init() {
  bool do_init = false;
  while (true) {
    portENTER_CRITICAL(&g_init_mux);
    if (g_started) {
      portEXIT_CRITICAL(&g_init_mux);
      return;
    }
    if (!g_init_in_progress) {
      g_init_in_progress = true;
      do_init = true;
    }
    portEXIT_CRITICAL(&g_init_mux);
    if (do_init) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (g_lock == nullptr) {
    g_lock = xSemaphoreCreateMutex();
  }

  refresh_saved_credentials();

  const bool idf_ready = ensure_idf_wifi_driver();
  state_lock();
  g_power_save_disable_status = idf_ready ? static_cast<int32_t>(esp_wifi_set_ps(WIFI_PS_NONE))
                                          : static_cast<int32_t>(ESP_ERR_WIFI_NOT_INIT);
  g_state_json_cache_valid = false;
  state_unlock();
  idf_disconnect_sta();

  if (!g_wifi_event_registered) {
    (void)register_wifi_event_handlers();
    g_wifi_event_registered = true;
  }

  g_phase = WifiPhase::kBootstrap;
  g_bootstrap_scan_pending = true;
  g_manual_scan_requested = false;
  g_phase_started_ms = mros::platform::mros_millis();
  publish_runtime_state(g_phase_started_ms);

  portENTER_CRITICAL(&g_init_mux);
  g_started = true;
  g_init_in_progress = false;
  portEXIT_CRITICAL(&g_init_mux);

  mros_console.println("[WiFi] Manager initialized");
  notify_task();
}

void wifi_manager_loop(unsigned long now_ms) {
  if (!g_started) {
    wifi_manager_init();
  }

  if (!g_manager_enabled) {
    service_dns();
    if ((now_ms - g_last_state_poll_ms) >= state_poll_period_ms()) {
      g_last_state_poll_ms = now_ms;
      publish_runtime_state(now_ms);
    }
    return;
  }

  if (g_hotspot_only) {
    if (!is_ap_active()) {
      start_ap_mode();
    }
    if (g_manual_scan_requested || g_scan_purpose != ScanPurpose::kNone) {
      service_scan(now_ms);
    }
    service_dns();
    service_leds(now_ms);
    if ((now_ms - g_last_state_poll_ms) >= state_poll_period_ms()) {
      g_last_state_poll_ms = now_ms;
      publish_runtime_state(now_ms);
    }
    return;
  }

  if ((now_ms - g_last_state_poll_ms) >= state_poll_period_ms()) {
    g_last_state_poll_ms = now_ms;
    publish_runtime_state(now_ms);
  }

  service_dns();

  service_scan(now_ms);

  switch (g_phase) {
    case WifiPhase::kBootstrap:
      if (idf_wifi_status() == WL_CONNECTED) {
        g_phase = WifiPhase::kConnected;
        g_ap_grace_until_ms = is_ap_active() ? (now_ms + kApGracePeriodMs) : 0;
      } else if (g_scan_purpose == ScanPurpose::kNone && !g_bootstrap_scan_pending) {
        schedule_bootstrap_scan(now_ms, true);
      }
      break;

    case WifiPhase::kConnecting:
      service_connecting(now_ms);
      break;

    case WifiPhase::kConnected:
      service_connected(now_ms);
      break;

    case WifiPhase::kApFallback:
      service_ap_fallback(now_ms);
      break;

    case WifiPhase::kFinalizeSuccess:
    case WifiPhase::kFinalizeFailure:
      service_finalize(now_ms);
      break;
  }

  service_leds(now_ms);
}

const WifiManagerState &wifi_manager_state() { return g_state; }

void wifi_manager_get_state(WifiManagerState *out_state) {
  if (out_state == nullptr) {
    return;
  }
  state_lock();
  *out_state = g_state;
  state_unlock();
}

void wifi_manager_get_snapshot(WifiManagerSnapshot *out_snapshot) {
  if (out_snapshot == nullptr) {
    return;
  }

  const unsigned long now_ms = mros::platform::mros_millis();
  WifiManagerState state_copy = {};
  state_lock();
  state_copy = g_state;
  state_unlock();

  out_snapshot->state = state_copy;
  out_snapshot->scan_age_ms = (g_manual_scan_cached_ms != 0)
                                  ? (now_ms - g_manual_scan_cached_ms)
                                  : ((g_bootstrap_scan_cached_ms != 0)
                                         ? (now_ms - g_bootstrap_scan_cached_ms)
                                         : 0);
  out_snapshot->manual_scan_age_ms =
      (g_manual_scan_cached_ms != 0) ? (now_ms - g_manual_scan_cached_ms) : 0;
  out_snapshot->bootstrap_scan_age_ms =
      (g_bootstrap_scan_cached_ms != 0) ? (now_ms - g_bootstrap_scan_cached_ms) : 0;
  out_snapshot->ap_grace_remaining_ms =
      (g_ap_grace_until_ms > now_ms) ? (g_ap_grace_until_ms - now_ms) : 0;
  out_snapshot->reconnect_backoff_ms =
      (g_next_connect_allowed_ms > now_ms) ? (g_next_connect_allowed_ms - now_ms) : 0;
  out_snapshot->last_connect_duration_ms = g_last_connect_duration_ms;
  out_snapshot->fast_path_attempts = g_fast_path_attempt_count;
  out_snapshot->fast_path_successes = g_fast_path_success_count;
  out_snapshot->event_revision = g_wifi_event_revision;
  out_snapshot->state_json_builds = g_wifi_state_json_build_count;
  out_snapshot->state_json_cache_hits = g_wifi_state_json_cache_hit_count;
  out_snapshot->runtime_publishes = g_wifi_runtime_publish_count;
  out_snapshot->native_event_active = g_native_event_active;
  out_snapshot->dns_lwip_active = g_dns_lwip_active;
  out_snapshot->snapshot_source =
      g_native_event_active ? "idf-event+idf-control" : "idf-event-unavailable";
  out_snapshot->dns_backend = g_dns_lwip_active ? "lwip-udp" : (g_dns_running ? "lwip-starting" : "off");
  out_snapshot->idf_event_count = g_idf_event_count;
  out_snapshot->arduino_event_count = 0;
  out_snapshot->dns_queries = g_dns_query_count;
  out_snapshot->dns_replies = g_dns_reply_count;
  out_snapshot->dns_errors = g_dns_error_count;
  out_snapshot->current_channel =
      (idf_wifi_status() == WL_CONNECTED) ? idf_sta_channel() : 0;
  out_snapshot->last_good_channel = g_last_good_channel;
  out_snapshot->phase = phase_text(g_phase);
  out_snapshot->ssid = g_last_ssid;
  out_snapshot->ip = g_last_ip;
  out_snapshot->ap_ip = g_last_ap_ip;
  out_snapshot->last_good_ssid = g_last_good_ssid;
  out_snapshot->last_good_bssid = g_last_good_bssid;
}

bool wifi_manager_is_connected() { return idf_wifi_status() == WL_CONNECTED; }

const char *wifi_manager_ssid() { return g_last_ssid.c_str(); }

const char *wifi_manager_ip() { return g_last_ip.c_str(); }

const char *wifi_manager_ap_ip() { return g_last_ap_ip.c_str(); }

bool wifi_manager_request_scan() {
  g_manual_scan_requested = true;
  notify_task();
  return true;
}

bool wifi_manager_is_scan_in_progress() {
  state_lock();
  const bool scan_in_progress = g_state.scan_in_progress;
  state_unlock();
  return scan_in_progress;
}

void wifi_manager_get_scan_results_json(String *out_json) {
  if (out_json == nullptr) {
    return;
  }
  state_lock();
  if (g_manual_scan_results != "[]") {
    *out_json = g_manual_scan_results;
  } else {
    *out_json = g_bootstrap_scan_results;
  }
  state_unlock();
}

void wifi_manager_get_scan_results_text(String *out_text) {
  if (out_text == nullptr) {
    return;
  }
  state_lock();
  if (g_manual_scan_cached_ms != 0) {
    *out_text = g_manual_scan_text;
  } else {
    *out_text = g_bootstrap_scan_text;
  }
  state_unlock();
}

void wifi_manager_get_saved_networks_text(String *out_text) {
  if (out_text == nullptr) {
    return;
  }
  String saved_ssid;
  String saved_pass;
  const bool has_saved = prefs_load_wifi(saved_ssid, saved_pass);
  String text = "Saved/runtime networks:\n";
  if (has_saved) {
    text += "  [stored] ";
    text += saved_ssid;
    text += saved_pass.length() > 0 ? "  pass=***\n" : "  pass=(open)\n";
  } else {
    text += "  [stored] none\n";
  }
  text += "Known networks from firmware:\n";
  for (size_t i = 0; i < known_networks_count; ++i) {
    if (known_networks[i].ssid == nullptr || known_networks[i].ssid[0] == '\0') {
      continue;
    }
    text += "  [known ";
    text += String(i);
    text += "] ";
    text += known_networks[i].ssid;
    text += (known_networks[i].pass != nullptr && known_networks[i].pass[0] != '\0')
                ? "  pass=***\n"
                : "  pass=(open)\n";
  }
  *out_text = text;
}

bool wifi_manager_request_test_connect(const String &ssid, const String &pass) {
  if (ssid.isEmpty()) {
    return false;
  }
  if (g_phase == WifiPhase::kFinalizeSuccess || g_phase == WifiPhase::kFinalizeFailure) {
    return false;
  }

  g_test_ssid = ssid;
  g_test_pass = pass;
  g_assoc_retry_count = 0;
  g_finalize_blink_count = 0;
  g_retry_due_ms = 0;
  start_ap_mode();
  begin_sta_attempt(g_test_ssid, g_test_pass, mros::platform::mros_millis(), true);
  return true;
}

bool wifi_manager_save_credentials(const String &ssid, const String &pass,
                                   bool connect_now) {
  if (ssid.isEmpty()) {
    return false;
  }
  prefs_save_wifi(ssid, pass);
  g_saved_ssid = ssid;
  g_saved_pass = pass;
  clear_candidate_failure(ssid);
  if (connect_now) {
    g_manager_enabled = true;
    g_hotspot_only = false;
    g_candidate_ssid = ssid;
    g_candidate_pass = pass;
    g_assoc_retry_count = 0;
    begin_sta_attempt(g_candidate_ssid,
                      g_candidate_pass,
                      mros::platform::mros_millis(),
                      is_ap_active());
  }
  notify_task();
  return true;
}

void wifi_manager_set_enabled(bool enabled) {
  g_manager_enabled = enabled;
  g_hotspot_only = false;
  if (!enabled) {
    g_bootstrap_scan_pending = false;
    g_manual_scan_requested = false;
    g_scan_purpose = ScanPurpose::kNone;
    (void)esp_wifi_scan_stop();
    stop_ap_mode();
    idf_disconnect_sta();
    (void)idf_set_mode(false, false);
    g_phase = WifiPhase::kApFallback;
  } else {
    (void)idf_set_mode(true, is_ap_active());
    clear_candidate_failure();
    g_phase = WifiPhase::kBootstrap;
    g_bootstrap_scan_pending = true;
    g_next_connect_allowed_ms = 0;
  }
  publish_runtime_state(mros::platform::mros_millis());
  notify_task();
}

bool wifi_manager_is_enabled() { return g_manager_enabled; }

void wifi_manager_force_hotspot() {
  g_manager_enabled = true;
  g_hotspot_only = true;
  g_bootstrap_scan_pending = false;
  g_manual_scan_requested = false;
  g_scan_purpose = ScanPurpose::kNone;
  (void)esp_wifi_scan_stop();
  idf_disconnect_sta();
  start_ap_mode();
  g_phase = WifiPhase::kApFallback;
  publish_runtime_state(mros::platform::mros_millis());
  notify_task();
}

void wifi_manager_request_reconnect() {
  g_manager_enabled = true;
  g_hotspot_only = false;
  g_retry_due_ms = 0;
  g_next_connect_allowed_ms = 0;
  g_assoc_retry_count = 0;
  clear_candidate_failure();
  idf_disconnect_sta();
  g_phase = WifiPhase::kBootstrap;
  g_bootstrap_scan_pending = true;
  notify_task();
}

void wifi_manager_build_state_json(String *out_json) {
  if (out_json == nullptr) {
    return;
  }
  const uint32_t now_ms = mros::platform::mros_millis();
  state_lock();
  if (g_state_json_cache_valid &&
      (now_ms - g_state_json_cache_ms) < kStateJsonCacheTtlMs) {
    g_wifi_state_json_cache_hit_count++;
    *out_json = g_state_json_cache;
    state_unlock();
    return;
  }
  state_unlock();

  WifiManagerSnapshot snapshot = {};
  wifi_manager_get_snapshot(&snapshot);
  state_lock();
  g_wifi_state_json_build_count++;
  state_unlock();

  char buffer[1024] = {};
  mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
  writer.begin();
  writer.string_field("phase", snapshot.phase.c_str());
  writer.bool_field("sta_connected", snapshot.state.sta_connected);
  writer.bool_field("sta_connecting", snapshot.state.sta_connecting);
  writer.bool_field("ap_active", snapshot.state.ap_active);
  writer.bool_field("scan_in_progress", snapshot.state.scan_in_progress);
  writer.bool_field("test_in_progress", snapshot.state.test_in_progress);
  writer.string_field("ssid", snapshot.ssid.c_str());
  writer.string_field("ip", snapshot.ip.c_str());
  writer.string_field("ap_ip", snapshot.ap_ip.c_str());
  writer.i32_field("rssi", snapshot.state.rssi);
  writer.u32_field("reconnect_attempts", snapshot.state.reconnect_attempts);
  writer.u32_field("last_disconnect_reason", snapshot.state.last_disconnect_reason);
  writer.u32_field("ap_clients", snapshot.state.ap_station_count);
  writer.u32_field("scan_age_ms", snapshot.scan_age_ms);
  writer.u32_field("manual_scan_age_ms", snapshot.manual_scan_age_ms);
  writer.u32_field("bootstrap_scan_age_ms", snapshot.bootstrap_scan_age_ms);
  writer.u32_field("ap_grace_remaining_ms", snapshot.ap_grace_remaining_ms);
  writer.u32_field("reconnect_backoff_ms", snapshot.reconnect_backoff_ms);
  writer.u32_field("last_connect_duration_ms", snapshot.last_connect_duration_ms);
  writer.u32_field("fast_path_attempts", snapshot.fast_path_attempts);
  writer.u32_field("fast_path_successes", snapshot.fast_path_successes);
  writer.u32_field("wifi_event_rev", snapshot.event_revision);
  writer.string_field("wifi_snapshot_source", snapshot.snapshot_source);
  writer.string_field("wifi_dns_backend", snapshot.dns_backend);
  writer.bool_field("native_event_active", snapshot.native_event_active);
  writer.bool_field("dns_lwip_active", snapshot.dns_lwip_active);
  writer.u32_field("idf_event_count", snapshot.idf_event_count);
  writer.u32_field("arduino_event_count", snapshot.arduino_event_count);
  writer.u32_field("dns_queries", snapshot.dns_queries);
  writer.u32_field("dns_replies", snapshot.dns_replies);
  writer.u32_field("dns_errors", snapshot.dns_errors);
  writer.u32_field("current_channel", snapshot.current_channel);
  writer.u32_field("last_good_channel", snapshot.last_good_channel);
  writer.string_field("last_good_ssid", snapshot.last_good_ssid.c_str());
  writer.string_field("last_good_bssid", snapshot.last_good_bssid.c_str());
  writer.end();
  String json(writer.overflow() ? "{\"error\":\"WIFI_STATE_JSON_OVERFLOW\"}" : writer.c_str());
  state_lock();
  g_state_json_cache = json;
  g_state_json_cache_ms = now_ms;
  g_state_json_cache_valid = true;
  state_unlock();
  *out_json = json;
}

void wifi_manager_build_diag_json(String *out_json) {
  if (out_json == nullptr) {
    return;
  }

  WifiManagerSnapshot snapshot = {};
  wifi_manager_get_snapshot(&snapshot);
  const uint32_t now_ms = mros::platform::mros_millis();

  uint32_t event_sta_connected = 0;
  uint32_t event_sta_got_ip = 0;
  uint32_t event_sta_disconnected = 0;
  uint32_t event_ap_sta_connected = 0;
  uint32_t event_ap_sta_disconnected = 0;
  uint32_t disconnect_auth_fail = 0;
  uint32_t disconnect_no_ap = 0;
  uint32_t disconnect_beacon_timeout = 0;
  uint32_t disconnect_assoc_comeback = 0;
  uint32_t disconnect_other = 0;
  uint32_t scan_started = 0;
  uint32_t scan_done = 0;
  uint32_t scan_timeout = 0;
  uint32_t scan_denied = 0;
  uint32_t manual_scan_denied = 0;
  uint32_t idf_scan_starts = 0;
  uint32_t cached_scan_count = 0;
  uint32_t connect_attempt = 0;
  uint32_t connect_success = 0;
  uint32_t connect_fail = 0;
  uint32_t backoff_apply = 0;
  uint32_t assoc_retry_scheduled = 0;
  uint32_t last_scan_duration = 0;
  uint32_t last_connect_duration = 0;
  uint32_t fast_path_attempt = 0;
  uint32_t fast_path_success = 0;
  uint32_t event_revision = 0;
  uint32_t state_json_builds = 0;
  uint32_t state_json_cache_hits = 0;
  uint32_t runtime_publishes = 0;
  uint32_t last_event_age = 0;
  uint32_t last_disconnect_reason_seen = 0;
  uint32_t current_backoff_ms = 0;
  uint32_t next_connect_allowed_remaining_ms = 0;
  int32_t power_save_disable_status = 0;
  char last_event_name[32] = {};

  state_lock();
  event_sta_connected = g_event_sta_connected;
  event_sta_got_ip = g_event_sta_got_ip;
  event_sta_disconnected = g_event_sta_disconnected;
  event_ap_sta_connected = g_event_ap_sta_connected;
  event_ap_sta_disconnected = g_event_ap_sta_disconnected;
  disconnect_auth_fail = g_disconnect_auth_fail_count;
  disconnect_no_ap = g_disconnect_no_ap_count;
  disconnect_beacon_timeout = g_disconnect_beacon_timeout_count;
  disconnect_assoc_comeback = g_disconnect_assoc_comeback_count;
  disconnect_other = g_disconnect_other_count;
  scan_started = g_scan_started_count;
  scan_done = g_scan_done_count;
  scan_timeout = g_scan_timeout_count;
  scan_denied = g_scan_denied_count;
  manual_scan_denied = g_manual_scan_denied_count;
  idf_scan_starts = g_idf_scan_start_count;
  cached_scan_count = g_scan_cached_count;
  connect_attempt = g_connect_attempt_count;
  connect_success = g_connect_success_count;
  connect_fail = g_connect_fail_count;
  backoff_apply = g_backoff_apply_count;
  assoc_retry_scheduled = g_assoc_retry_schedule_count;
  last_scan_duration = g_last_scan_duration_ms;
  last_connect_duration = g_last_connect_duration_ms;
  fast_path_attempt = g_fast_path_attempt_count;
  fast_path_success = g_fast_path_success_count;
  event_revision = g_wifi_event_revision;
  state_json_builds = g_wifi_state_json_build_count;
  state_json_cache_hits = g_wifi_state_json_cache_hit_count;
  runtime_publishes = g_wifi_runtime_publish_count;
  last_event_age = (g_last_event_ms != 0U) ? (now_ms - g_last_event_ms) : 0U;
  last_disconnect_reason_seen = g_last_disconnect_reason_seen;
  current_backoff_ms = g_current_backoff_ms;
  next_connect_allowed_remaining_ms =
      (g_next_connect_allowed_ms > now_ms) ? (g_next_connect_allowed_ms - now_ms) : 0U;
  power_save_disable_status = g_power_save_disable_status;
  std::snprintf(last_event_name, sizeof(last_event_name), "%s", g_last_event_name);
  state_unlock();

  const bool connected = idf_wifi_status() == WL_CONNECTED;
  const String current_bssid = connected ? idf_sta_bssid() : "";

  char buffer[2304] = {};
  mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
  writer.begin();
  writer.bool_field("success", true);
  writer.string_field("phase", snapshot.phase.c_str());
  writer.bool_field("sta_connected", snapshot.state.sta_connected);
  writer.bool_field("sta_connecting", snapshot.state.sta_connecting);
  writer.bool_field("ap_active", snapshot.state.ap_active);
  writer.bool_field("scan_in_progress", snapshot.state.scan_in_progress);
  writer.i32_field("rssi", snapshot.state.rssi);
  writer.u32_field("channel", snapshot.current_channel);
  writer.string_field("bssid", current_bssid.c_str());
  writer.string_field("ssid", snapshot.ssid.c_str());
  writer.string_field("ip", snapshot.ip.c_str());
  writer.string_field("ap_ip", snapshot.ap_ip.c_str());
  writer.u32_field("ap_station_count", snapshot.state.ap_station_count);
  writer.u32_field("reconnect_backoff_ms", snapshot.reconnect_backoff_ms);
  writer.u32_field("current_backoff_ms", current_backoff_ms);
  writer.u32_field("next_connect_allowed_ms", next_connect_allowed_remaining_ms);
  writer.u32_field("last_connect_duration_ms", snapshot.last_connect_duration_ms);
  writer.u32_field("wifi_event_rev", event_revision);
  writer.string_field("wifi_snapshot_source", snapshot.snapshot_source);
  writer.string_field("wifi_dns_backend", snapshot.dns_backend);
  writer.bool_field("native_event_active", snapshot.native_event_active);
  writer.bool_field("dns_lwip_active", snapshot.dns_lwip_active);
  writer.u32_field("idf_event_count", snapshot.idf_event_count);
  writer.u32_field("arduino_event_count", snapshot.arduino_event_count);
  writer.u32_field("dns_queries", snapshot.dns_queries);
  writer.u32_field("dns_replies", snapshot.dns_replies);
  writer.u32_field("dns_errors", snapshot.dns_errors);
  writer.u32_field("state_json_builds", state_json_builds);
  writer.u32_field("state_json_cache_hits", state_json_cache_hits);
  writer.u32_field("runtime_publishes", runtime_publishes);
  writer.u32_field("idf_scan_starts", idf_scan_starts);
  writer.u32_field("arduino_scan_fallbacks", 0);
  writer.u32_field("cached_scan_records", cached_scan_count);
  writer.string_field("scan_source", "idf-only");
  writer.string_field("last_good_ssid", snapshot.last_good_ssid.c_str());
  writer.string_field("last_good_bssid", snapshot.last_good_bssid.c_str());
  writer.u32_field("last_good_channel", snapshot.last_good_channel);
  writer.i32_field("power_save_disable_status", power_save_disable_status);
  writer.append_raw(",\"last_event\":{\"name\":\"");
  writer.append_escaped(last_event_name);
  writer.append_raw("\",\"age_ms\":");
  writer.u32(last_event_age);
  writer.append_raw(",\"disconnect_reason\":");
  writer.u32(last_disconnect_reason_seen);
  writer.append_raw("},\"events\":{\"sta_connected\":");
  writer.u32(event_sta_connected);
  writer.append_raw(",\"sta_got_ip\":");
  writer.u32(event_sta_got_ip);
  writer.append_raw(",\"sta_disconnected\":");
  writer.u32(event_sta_disconnected);
  writer.append_raw(",\"ap_sta_connected\":");
  writer.u32(event_ap_sta_connected);
  writer.append_raw(",\"ap_sta_disconnected\":");
  writer.u32(event_ap_sta_disconnected);
  writer.append_raw("},\"disconnect_reasons\":{\"auth_fail\":");
  writer.u32(disconnect_auth_fail);
  writer.append_raw(",\"no_ap\":");
  writer.u32(disconnect_no_ap);
  writer.append_raw(",\"beacon_timeout\":");
  writer.u32(disconnect_beacon_timeout);
  writer.append_raw(",\"assoc_comeback_208\":");
  writer.u32(disconnect_assoc_comeback);
  writer.append_raw(",\"other\":");
  writer.u32(disconnect_other);
  writer.append_raw("},\"scan\":{\"started\":");
  writer.u32(scan_started);
  writer.append_raw(",\"done\":");
  writer.u32(scan_done);
  writer.append_raw(",\"timeout\":");
  writer.u32(scan_timeout);
  writer.append_raw(",\"denied\":");
  writer.u32(scan_denied);
  writer.append_raw(",\"manual_denied\":");
  writer.u32(manual_scan_denied);
  writer.append_raw(",\"last_duration_ms\":");
  writer.u32(last_scan_duration);
  writer.append_raw("},\"connect\":{\"attempt\":");
  writer.u32(connect_attempt);
  writer.append_raw(",\"success\":");
  writer.u32(connect_success);
  writer.append_raw(",\"fail\":");
  writer.u32(connect_fail);
  writer.append_raw(",\"fast_path_attempt\":");
  writer.u32(fast_path_attempt);
  writer.append_raw(",\"fast_path_success\":");
  writer.u32(fast_path_success);
  writer.append_raw(",\"backoff_apply\":");
  writer.u32(backoff_apply);
  writer.append_raw(",\"assoc_retry_scheduled\":");
  writer.u32(assoc_retry_scheduled);
  writer.append_raw(",\"last_duration_ms\":");
  writer.u32(last_connect_duration);
  writer.append_raw("}");
  writer.end();
  *out_json = writer.overflow() ? "{\"error\":\"WIFI_DIAG_JSON_OVERFLOW\"}" : String(writer.c_str());
}
