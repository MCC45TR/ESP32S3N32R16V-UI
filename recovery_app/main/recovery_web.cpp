#include "recovery_web.h"

#include <mros_update_shared.h>

extern "C" {
#include <cJSON.h>
}

#include <esp_app_desc.h>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>
#include <mbedtls/md.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "led_strip.h"
#include "led_strip_rmt.h"

extern const unsigned char recovery_html_start[] asm("_binary_recovery_html_start");
extern const unsigned char recovery_html_end[] asm("_binary_recovery_html_end");

namespace mros::recovery {
namespace {

constexpr const char* kTag = "recovery_web";
constexpr const char* kRecoverySsidPrefix = "MROS-Recovery";

#ifndef MROS_RECOVERY_AP_PASSWORD
#define MROS_RECOVERY_AP_PASSWORD "CHANGE_ME_RECOVERY_AP"
#define MROS_RECOVERY_AP_PASSWORD_PLACEHOLDER 1
#else
#define MROS_RECOVERY_AP_PASSWORD_PLACEHOLDER 0
#endif

#if defined(MROS_PRODUCTION_BUILD) && MROS_RECOVERY_AP_PASSWORD_PLACEHOLDER
#error "Production recovery builds require a device-specific MROS_RECOVERY_AP_PASSWORD"
#endif

#ifndef MROS_RECOVERY_PAIRING_TOKEN
#define MROS_RECOVERY_PAIRING_TOKEN ""
#define MROS_RECOVERY_PAIRING_TOKEN_PLACEHOLDER 1
#else
#define MROS_RECOVERY_PAIRING_TOKEN_PLACEHOLDER 0
#endif

#if defined(MROS_PRODUCTION_BUILD) && MROS_RECOVERY_PAIRING_TOKEN_PLACEHOLDER
#error "Production recovery builds require a device-specific MROS_RECOVERY_PAIRING_TOKEN"
#endif

#ifndef MROS_RECOVERY_ALLOW_UNSIGNED_DIRECT_UPLOAD
#define MROS_RECOVERY_ALLOW_UNSIGNED_DIRECT_UPLOAD 0
#endif

constexpr const char* kRecoveryPass = MROS_RECOVERY_AP_PASSWORD;
constexpr const char* kRecoveryPairingToken = MROS_RECOVERY_PAIRING_TOKEN;
constexpr int kRecoveryLedGpio = 38;
constexpr size_t kUploadChunkBytes = 4096U;
constexpr const char* kMountPoint = "/littlefs";
constexpr const char* kPortalUrl = "http://192.168.4.1/";
constexpr size_t kMaxFileCandidates = 64U;
constexpr const char* kWifiCfgNamespace = "wifi_cfg";
constexpr const char* kUserNvsPartition = "nvs_sys_usr";
constexpr const char* kDeviceSettingsNamespace = "web_cfg";
constexpr const char* kDeviceSettingsKey = "device_v1";
constexpr uint8_t kRecoveryStaMaxRetries = 3U;
constexpr size_t kRecoveryPairingTokenMinLen = 16U;
constexpr size_t kRecoveryPairingTokenMaxLen = 96U;

struct RecoveryWebStatus {
  char state[32] = "idle";
  char phase[32] = "boot";
  char message[160] = "Recovery portal starting";
  char confirmation[32] = "waiting";
  uint32_t progress = 0;
  int32_t eta_sec = -1;
  bool busy = false;
};

RecoveryWebStatus g_status;
portMUX_TYPE g_status_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_portal_mux = portMUX_INITIALIZER_UNLOCKED;
const esp_partition_t* g_app0 = nullptr;
bool g_client_seen = false;
bool g_filesystem_ready = false;
uint8_t g_station_count = 0;
httpd_handle_t g_server = nullptr;
esp_netif_t* g_sta_netif = nullptr;
esp_netif_t* g_ap_netif = nullptr;
char g_recovery_ssid[33] = "MROS-Recovery";

struct RecoveryStaProfile {
  bool available = false;
  char ssid[33] = {};
  char pass[65] = {};
  char bssid[18] = {};
  uint8_t channel = 0;
};

struct RecoveryStaState {
  bool connecting = false;
  bool connected = false;
  uint8_t retry_count = 0;
  int32_t rssi = -127;
  uint32_t last_disconnect_reason = 0;
  char ip[16] = "0.0.0.0";
};

RecoveryStaProfile g_sta_profile;
RecoveryStaState g_sta_state;

struct RecoveryDeviceSettings {
  bool available = false;
  bool recovery_reads_device_settings = true;
  bool rollback_guard = true;
  bool auto_check_recovery = true;
  bool require_battery_safe = true;
  bool auto_ota_scan = false;
  bool scheduled_reboot = false;
  char firmware_path[129] = "/ESPUSER/firmware";
  char ota_scan_hour[8] = "03:00";
  char scheduled_reboot_hour[8] = "04:00";
  char update_window[24] = "night";
  char last_error_code[32] = "OK";
};

RecoveryDeviceSettings g_device_settings;

struct FirmwareFile {
  std::string actual_path;
  std::string display_path;
  std::string basename;
  std::string version;
  uint64_t size = 0U;
  long long mtime = 0;
  int score = 0;
};

bool recovery_pairing_configured() {
  const size_t len = std::strlen(kRecoveryPairingToken);
  return len >= kRecoveryPairingTokenMinLen &&
         len <= kRecoveryPairingTokenMaxLen &&
         std::strstr(kRecoveryPairingToken, "CHANGE_ME") == nullptr &&
         std::strstr(kRecoveryPairingToken, "REPLACE") == nullptr &&
         std::strstr(kRecoveryPairingToken, "PLACEHOLDER") == nullptr;
}

std::string status_json() {
  RecoveryWebStatus snap;
  portENTER_CRITICAL(&g_status_mux);
  snap = g_status;
  portEXIT_CRITICAL(&g_status_mux);

  bool client_seen = false;
  uint8_t station_count = 0;
  bool fs_ready = false;
  RecoveryStaProfile sta_profile;
  RecoveryStaState sta_state;
  portENTER_CRITICAL(&g_portal_mux);
  client_seen = g_client_seen;
  station_count = g_station_count;
  fs_ready = g_filesystem_ready;
  sta_profile = g_sta_profile;
  sta_state = g_sta_state;
  portEXIT_CRITICAL(&g_portal_mux);

  RecoveryDeviceSettings device_settings = g_device_settings;

  char buffer[1792] = {};
  std::snprintf(buffer,
                sizeof(buffer),
                "{\"state\":\"%s\",\"phase\":\"%s\",\"message\":\"%s\","
                "\"confirmation\":\"%s\",\"progress\":%u,\"eta_sec\":%ld,"
                "\"busy\":%s,\"target\":\"app0\",\"ap_ssid\":\"%s\","
                "\"portal_url\":\"%s\",\"client_seen\":%s,\"station_count\":%u,"
                "\"autoboot_paused\":%s,\"fs_ready\":%s,"
                "\"auth_required\":true,\"pairing_configured\":%s,"
                "\"direct_unsigned_upload_enabled\":%s,"
                "\"sta_available\":%s,\"sta_connected\":%s,\"sta_connecting\":%s,"
                "\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\",\"sta_rssi\":%ld,"
                "\"sta_retries\":%u,\"sta_disconnect_reason\":%lu,"
                "\"device_settings_available\":%s,"
                "\"recovery_reads_device_settings\":%s,"
                "\"firmware_path\":\"%s\",\"rollback_guard\":%s,"
                "\"auto_check_recovery\":%s,\"require_battery_safe\":%s,"
                "\"auto_ota_scan\":%s,\"ota_scan_hour\":\"%s\","
                "\"scheduled_reboot\":%s,\"scheduled_reboot_hour\":\"%s\","
                "\"update_window\":\"%s\",\"last_error_code\":\"%s\"}",
                snap.state,
                snap.phase,
                snap.message,
                snap.confirmation,
                static_cast<unsigned>(snap.progress),
                static_cast<long>(snap.eta_sec),
                snap.busy ? "true" : "false",
                g_recovery_ssid,
                kPortalUrl,
                client_seen ? "true" : "false",
                static_cast<unsigned>(station_count),
                (client_seen || station_count > 0U) ? "true" : "false",
                fs_ready ? "true" : "false",
                recovery_pairing_configured() ? "true" : "false",
#if MROS_RECOVERY_ALLOW_UNSIGNED_DIRECT_UPLOAD
                "true",
#else
                "false",
#endif
                sta_profile.available ? "true" : "false",
                sta_state.connected ? "true" : "false",
                sta_state.connecting ? "true" : "false",
                sta_profile.ssid,
                sta_state.ip,
                static_cast<long>(sta_state.rssi),
                static_cast<unsigned>(sta_state.retry_count),
                static_cast<unsigned long>(sta_state.last_disconnect_reason),
                device_settings.available ? "true" : "false",
                device_settings.recovery_reads_device_settings ? "true" : "false",
                device_settings.firmware_path,
                device_settings.rollback_guard ? "true" : "false",
                device_settings.auto_check_recovery ? "true" : "false",
                device_settings.require_battery_safe ? "true" : "false",
                device_settings.auto_ota_scan ? "true" : "false",
                device_settings.ota_scan_hour,
                device_settings.scheduled_reboot ? "true" : "false",
                device_settings.scheduled_reboot_hour,
                device_settings.update_window,
                device_settings.last_error_code);
  return buffer;
}

void mark_client_seen() {
  portENTER_CRITICAL(&g_portal_mux);
  g_client_seen = true;
  portEXIT_CRITICAL(&g_portal_mux);
}

void set_station_count(uint8_t count) {
  portENTER_CRITICAL(&g_portal_mux);
  g_station_count = count;
  if (count > 0U) {
    g_client_seen = true;
  }
  portEXIT_CRITICAL(&g_portal_mux);
}

bool filesystem_ready() {
  bool ready = false;
  portENTER_CRITICAL(&g_portal_mux);
  ready = g_filesystem_ready;
  portEXIT_CRITICAL(&g_portal_mux);
  return ready;
}

void set_sta_disconnect_reason(const uint32_t reason) {
  portENTER_CRITICAL(&g_portal_mux);
  g_sta_state.connected = false;
  g_sta_state.last_disconnect_reason = reason;
  g_sta_state.rssi = -127;
  std::snprintf(g_sta_state.ip, sizeof(g_sta_state.ip), "%s", "0.0.0.0");
  portEXIT_CRITICAL(&g_portal_mux);
}

bool partition_has_valid_app(const esp_partition_t* partition) {
  if (partition == nullptr) {
    return false;
  }
  esp_app_desc_t desc {};
  return esp_ota_get_partition_description(partition, &desc) == ESP_OK;
}

void set_no_store(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
}

const char* http_status_text(const int code) {
  switch (code) {
    case 200:
      return "200 OK";
    case 400:
      return "400 Bad Request";
    case 401:
      return "401 Unauthorized";
    case 403:
      return "403 Forbidden";
    case 404:
      return "404 Not Found";
    case 405:
      return "405 Method Not Allowed";
    case 503:
      return "503 Service Unavailable";
    default:
      return "500 Internal Server Error";
  }
}

esp_err_t send_text(httpd_req_t* req, const int code, const char* text) {
  set_no_store(req);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_status(req, http_status_text(code));
  return httpd_resp_sendstr(req, text);
}

esp_err_t send_json(httpd_req_t* req, const int code, const char* body) {
  set_no_store(req);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_status(req, http_status_text(code));
  return httpd_resp_sendstr(req, body != nullptr ? body : "{}");
}

std::string url_decode_copy(const char* raw);

bool read_header_string(httpd_req_t* req,
                        const char* name,
                        std::string* out,
                        const size_t max_len = 160U) {
  if (req == nullptr || name == nullptr || out == nullptr) {
    return false;
  }
  const size_t len = httpd_req_get_hdr_value_len(req, name);
  if (len == 0U || len > max_len) {
    return false;
  }
  std::vector<char> buffer(len + 1U, '\0');
  if (httpd_req_get_hdr_value_str(req, name, buffer.data(), buffer.size()) != ESP_OK) {
    return false;
  }
  out->assign(buffer.data());
  return !out->empty();
}

std::string trim_copy(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.pop_back();
  }
  return value;
}

std::string recovery_request_token(httpd_req_t* req) {
  std::string token;
  if (read_header_string(req, "X-MROS-Recovery-Token", &token) ||
      read_header_string(req, "X-Recovery-Token", &token)) {
    return trim_copy(token);
  }
  std::string auth;
  if (read_header_string(req, "Authorization", &auth)) {
    const std::string prefix = "Bearer ";
    if (auth.size() > prefix.size() &&
        auth.compare(0U, prefix.size(), prefix) == 0) {
      return trim_copy(auth.substr(prefix.size()));
    }
  }
  char query[256] = {};
  char raw_token[128] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      (httpd_query_key_value(query, "token", raw_token, sizeof(raw_token)) == ESP_OK ||
       httpd_query_key_value(query, "pairing", raw_token, sizeof(raw_token)) == ESP_OK)) {
    return trim_copy(url_decode_copy(raw_token));
  }
  return {};
}

bool recovery_pairing_token_matches(const std::string& supplied) {
  if (!recovery_pairing_configured() ||
      supplied.size() < kRecoveryPairingTokenMinLen ||
      supplied.size() > kRecoveryPairingTokenMaxLen) {
    return false;
  }
  const std::string expected(kRecoveryPairingToken);
  uint8_t diff = supplied.size() == expected.size() ? 0U : 1U;
  const size_t max_len = supplied.size() > expected.size() ? supplied.size() : expected.size();
  for (size_t i = 0; i < max_len; ++i) {
    const uint8_t a = i < supplied.size() ? static_cast<uint8_t>(supplied[i]) : 0U;
    const uint8_t b = i < expected.size() ? static_cast<uint8_t>(expected[i]) : 0U;
    diff |= static_cast<uint8_t>(a ^ b);
  }
  return diff == 0U;
}

bool require_recovery_auth(httpd_req_t* req) {
  if (!recovery_pairing_configured()) {
    ESP_LOGW(kTag, "Recovery API denied: pairing token is not provisioned");
    send_json(req, 403, "{\"ok\":false,\"error\":\"pairing_unconfigured\"}");
    return false;
  }
  const std::string token = recovery_request_token(req);
  if (token.empty()) {
    send_json(req, 401, "{\"ok\":false,\"error\":\"pairing_required\"}");
    return false;
  }
  if (!recovery_pairing_token_matches(token)) {
    ESP_LOGW(kTag, "Recovery API denied: invalid pairing token");
    send_json(req, 403, "{\"ok\":false,\"error\":\"pairing_denied\"}");
    return false;
  }
  return true;
}

esp_err_t portal_handler(httpd_req_t* req) {
  mark_client_seen();
  set_no_store(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req,
                         reinterpret_cast<const char*>(recovery_html_start),
                         recovery_html_end - recovery_html_start);
}

esp_err_t portal_fallback_handler(httpd_req_t* req) {
  const char* uri = req != nullptr ? req->uri : nullptr;
  if (uri != nullptr && std::strncmp(uri, "/api/", 5) == 0) {
    return send_json(req, 404, "{\"ok\":false,\"error\":\"not_found\"}");
  }
  return portal_handler(req);
}

std::string to_lower_copy(const std::string& text) {
  std::string out = text;
  std::transform(
      out.begin(),
      out.end(),
      out.begin(),
      [](const unsigned char ch) -> char { return static_cast<char>(std::tolower(ch)); });
  return out;
}

int hex_value(const char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return 10 + ch - 'a';
  }
  if (ch >= 'A' && ch <= 'F') {
    return 10 + ch - 'A';
  }
  return -1;
}

std::string url_decode_copy(const char* raw) {
  std::string out;
  if (raw == nullptr) {
    return out;
  }
  for (size_t i = 0; raw[i] != '\0'; ++i) {
    if (raw[i] == '%' && raw[i + 1] != '\0' && raw[i + 2] != '\0') {
      const int hi = hex_value(raw[i + 1]);
      const int lo = hex_value(raw[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2U;
        continue;
      }
    }
    out.push_back(raw[i] == '+' ? ' ' : raw[i]);
  }
  return out;
}

bool nvs_get_string_copy(nvs_handle_t handle, const char* key, char* out, const size_t out_size) {
  if (out == nullptr || out_size == 0U) {
    return false;
  }
  out[0] = '\0';
  size_t required = out_size;
  const esp_err_t err = nvs_get_str(handle, key, out, &required);
  if (err != ESP_OK || required == 0U) {
    out[0] = '\0';
    return false;
  }
  out[out_size - 1U] = '\0';
  return out[0] != '\0';
}

RecoveryStaProfile load_recovery_sta_profile() {
  RecoveryStaProfile profile {};
  nvs_handle_t handle = 0;
  if (nvs_open(kWifiCfgNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return profile;
  }

  char saved_ssid[sizeof(profile.ssid)] = {};
  char saved_pass[sizeof(profile.pass)] = {};
  (void)nvs_get_string_copy(handle, "ssid", saved_ssid, sizeof(saved_ssid));
  (void)nvs_get_string_copy(handle, "pass", saved_pass, sizeof(saved_pass));

  (void)nvs_get_string_copy(handle, "last_ssid", profile.ssid, sizeof(profile.ssid));
  (void)nvs_get_string_copy(handle, "last_pass", profile.pass, sizeof(profile.pass));
  (void)nvs_get_string_copy(handle, "last_bssid", profile.bssid, sizeof(profile.bssid));
  (void)nvs_get_u8(handle, "last_ch", &profile.channel);
  nvs_close(handle);

  if (profile.ssid[0] == '\0' && saved_ssid[0] != '\0') {
    std::snprintf(profile.ssid, sizeof(profile.ssid), "%s", saved_ssid);
  }
  if (profile.pass[0] == '\0' && saved_ssid[0] != '\0' &&
      std::strcmp(profile.ssid, saved_ssid) == 0) {
    std::snprintf(profile.pass, sizeof(profile.pass), "%s", saved_pass);
  }
  profile.available = profile.ssid[0] != '\0';
  return profile;
}

void cjson_copy_string(const cJSON* parent,
                       const char* key,
                       char* out,
                       const size_t out_size) {
  if (parent == nullptr || key == nullptr || out == nullptr || out_size == 0U) {
    return;
  }
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(parent, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return;
  }
  std::snprintf(out, out_size, "%s", item->valuestring);
  out[out_size - 1U] = '\0';
  for (size_t i = 0; out[i] != '\0'; ++i) {
    const unsigned char ch = static_cast<unsigned char>(out[i]);
    if (ch < 0x20U || out[i] == '"' || out[i] == '\\') {
      out[i] = '_';
    }
  }
}

bool cjson_bool_or_default(const cJSON* parent, const char* key, const bool fallback) {
  const cJSON* item = parent != nullptr ? cJSON_GetObjectItemCaseSensitive(parent, key) : nullptr;
  if (cJSON_IsBool(item)) {
    return cJSON_IsTrue(item);
  }
  return fallback;
}

RecoveryDeviceSettings load_recovery_device_settings() {
  RecoveryDeviceSettings settings {};
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open_from_partition(kUserNvsPartition,
                                          kDeviceSettingsNamespace,
                                          NVS_READONLY,
                                          &handle);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "recovery device settings unavailable: %s", esp_err_to_name(err));
    return settings;
  }

  size_t len = 0U;
  err = nvs_get_str(handle, kDeviceSettingsKey, nullptr, &len);
  if (err != ESP_OK || len == 0U || len > 16U * 1024U) {
    nvs_close(handle);
    return settings;
  }
  std::vector<char> payload(len + 1U, '\0');
  err = nvs_get_str(handle, kDeviceSettingsKey, payload.data(), &len);
  nvs_close(handle);
  if (err != ESP_OK) {
    return settings;
  }
  cJSON* root = cJSON_ParseWithLength(payload.data(), std::strlen(payload.data()));
  if (root == nullptr || !cJSON_IsObject(root)) {
    if (root != nullptr) cJSON_Delete(root);
    return settings;
  }
  const cJSON* update = cJSON_GetObjectItemCaseSensitive(root, "update");
  if (cJSON_IsObject(update)) {
    settings.available = true;
    cjson_copy_string(update, "firmwarePath", settings.firmware_path, sizeof(settings.firmware_path));
    cjson_copy_string(update, "otaScanHour", settings.ota_scan_hour, sizeof(settings.ota_scan_hour));
    cjson_copy_string(update, "scheduledRebootHour", settings.scheduled_reboot_hour, sizeof(settings.scheduled_reboot_hour));
    cjson_copy_string(update, "updateWindow", settings.update_window, sizeof(settings.update_window));
    cjson_copy_string(update, "lastErrorCode", settings.last_error_code, sizeof(settings.last_error_code));
    settings.recovery_reads_device_settings =
        cjson_bool_or_default(update, "recoveryReadsDeviceSettings", settings.recovery_reads_device_settings);
    settings.rollback_guard = cjson_bool_or_default(update, "rollbackGuard", settings.rollback_guard);
    settings.auto_check_recovery = cjson_bool_or_default(update, "autoCheckRecovery", settings.auto_check_recovery);
    settings.require_battery_safe = cjson_bool_or_default(update, "requireBatterySafe", settings.require_battery_safe);
    settings.auto_ota_scan = cjson_bool_or_default(update, "autoOtaScan", settings.auto_ota_scan);
    settings.scheduled_reboot = cjson_bool_or_default(update, "scheduledReboot", settings.scheduled_reboot);
  }
  cJSON_Delete(root);
  return settings;
}

bool parse_bssid_copy(const char* text, uint8_t out[6]) {
  if (text == nullptr || out == nullptr || std::strlen(text) != 17U) {
    return false;
  }
  for (size_t i = 0; i < 6U; ++i) {
    const size_t pos = i * 3U;
    const int hi = hex_value(text[pos]);
    const int lo = hex_value(text[pos + 1U]);
    if (hi < 0 || lo < 0 || (i < 5U && text[pos + 2U] != ':')) {
      return false;
    }
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool ends_with_bin(const std::string& path) {
  const std::string lower = to_lower_copy(path);
  return lower.size() >= 4U && lower.substr(lower.size() - 4U) == ".bin";
}

std::string basename_of(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1U);
}

std::string sanitize_filename(const std::string& raw) {
  std::string base = raw;
  const size_t slash = base.find_last_of("/\\");
  if (slash != std::string::npos) {
    base = base.substr(slash + 1U);
  }
  std::string out;
  out.reserve(base.size());
  for (const char ch : base) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) != 0 || ch == '.' || ch == '_' || ch == '-') {
      out.push_back(ch);
    } else {
      out.push_back('_');
    }
  }
  if (out.empty() || out == "." || out == "..") {
    out = "firmware.bin";
  }
  if (!ends_with_bin(out)) {
    out += ".bin";
  }
  return out;
}

std::string firmware_root_actual() {
  return mros::update::storage_actual_path(kMountPoint, mros::update::kFirmwareRootRelativePath);
}

std::string inbox_root_actual() {
  return mros::update::storage_actual_path(kMountPoint, mros::update::kInboxRootRelativePath);
}

std::string staging_root_actual() {
  return mros::update::storage_actual_path(kMountPoint, mros::update::kStagingRootRelativePath);
}

bool starts_with_path(const std::string& path, const std::string& root) {
  return path == root || (path.size() > root.size() && path.compare(0U, root.size(), root) == 0 &&
                          path[root.size()] == '/');
}

std::string display_path_for_actual(const std::string& actual) {
  const std::string mount(kMountPoint);
  if (actual == mount) {
    return "/";
  }
  if (actual.size() > mount.size() && actual.compare(0U, mount.size(), mount) == 0) {
    return actual.substr(mount.size());
  }
  return actual;
}

bool resolve_firmware_api_path(const std::string& raw, std::string* actual, std::string* error) {
  if (actual == nullptr) {
    return false;
  }
  if (raw.empty()) {
    if (error != nullptr) {
      *error = "path_missing";
    }
    return false;
  }
  if (raw.find("..") != std::string::npos || raw.find('\\') != std::string::npos) {
    if (error != nullptr) {
      *error = "unsafe_path";
    }
    return false;
  }

  std::string candidate;
  if (raw.rfind(kMountPoint, 0U) == 0) {
    candidate = raw;
  } else if (raw.rfind("/ESPUSER/", 0U) == 0) {
    candidate = std::string(kMountPoint) + raw;
  } else if (raw.rfind("ESPUSER/", 0U) == 0) {
    candidate = std::string(kMountPoint) + "/" + raw;
  } else {
    if (error != nullptr) {
      *error = "path_outside_firmware_root";
    }
    return false;
  }

  if (!starts_with_path(candidate, firmware_root_actual())) {
    if (error != nullptr) {
      *error = "path_outside_firmware_root";
    }
    return false;
  }
  *actual = candidate;
  return true;
}

bool is_delete_allowed(const std::string& actual) {
  return starts_with_path(actual, inbox_root_actual()) || starts_with_path(actual, staging_root_actual());
}

std::string parse_version_token(const std::string& basename) {
  constexpr const char* prefix = "mros_esp32s3n32r16v_";
  const std::string lower = to_lower_copy(basename);
  if (lower.rfind(prefix, 0U) != 0 || !ends_with_bin(lower)) {
    return {};
  }
  std::string token = basename.substr(std::strlen(prefix));
  return token.substr(0U, token.size() - 4U);
}

int firmware_score(const std::string& display, const std::string& basename, const uint64_t size) {
  const std::string lower_path = to_lower_copy(display);
  const std::string lower_name = to_lower_copy(basename);
  int score = 0;
  if (lower_path.find("/firmware/") != std::string::npos) {
    score += 250;
  }
  if (lower_name.rfind("mros_esp32s3n32r16v_", 0U) == 0) {
    score += 400;
  } else if (lower_name.find("mros") != std::string::npos || lower_name.find("bridge") != std::string::npos) {
    score += 80;
  }
  if (lower_path.find("/current/") != std::string::npos || lower_path.find("/inbox/") != std::string::npos) {
    score += 80;
  }
  if (lower_name.find("littlefs") != std::string::npos || lower_name.find("data.bin") != std::string::npos) {
    score -= 300;
  }
  if (size >= (256U * 1024U)) {
    score += 40;
  }
  return score;
}

void scan_firmware_files(const std::string& dir,
                         const uint8_t depth,
                         std::vector<FirmwareFile>* out_files) {
  if (out_files == nullptr || out_files->size() >= kMaxFileCandidates || depth > 5U) {
    return;
  }
  DIR* handle = ::opendir(dir.c_str());
  if (handle == nullptr) {
    return;
  }
  struct dirent* entry = nullptr;
  while ((entry = ::readdir(handle)) != nullptr && out_files->size() < kMaxFileCandidates) {
    const char* name = entry->d_name;
    if (name == nullptr || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
      continue;
    }
    const std::string child = dir + "/" + name;
    struct stat info {};
    if (::stat(child.c_str(), &info) != 0) {
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      scan_firmware_files(child, static_cast<uint8_t>(depth + 1U), out_files);
      continue;
    }
    if (!S_ISREG(info.st_mode) || !ends_with_bin(name)) {
      continue;
    }
    FirmwareFile file {};
    file.actual_path = child;
    file.display_path = display_path_for_actual(child);
    file.basename = name;
    file.version = parse_version_token(file.basename);
    file.size = static_cast<uint64_t>(info.st_size);
    file.mtime = static_cast<long long>(info.st_mtime);
    file.score = firmware_score(file.display_path, file.basename, file.size);
    out_files->push_back(std::move(file));
  }
  ::closedir(handle);
}

std::string sha256_file(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md_info == nullptr) {
    std::fclose(file);
    return {};
  }

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = mbedtls_md_setup(&ctx, md_info, 0) == 0 && mbedtls_md_starts(&ctx) == 0;
  std::vector<uint8_t> buffer(kUploadChunkBytes);
  while (ok) {
    const size_t read_count = std::fread(buffer.data(), 1U, buffer.size(), file);
    if (read_count > 0U && mbedtls_md_update(&ctx, buffer.data(), read_count) != 0) {
      ok = false;
    }
    if (read_count < buffer.size()) {
      if (std::ferror(file) != 0) {
        ok = false;
      }
      break;
    }
  }
  std::fclose(file);

  uint8_t digest[32] = {};
  if (ok && mbedtls_md_finish(&ctx, digest) != 0) {
    ok = false;
  }
  mbedtls_md_free(&ctx);
  if (!ok) {
    return {};
  }

  char out[65] = {};
  for (size_t i = 0; i < sizeof(digest); ++i) {
    std::snprintf(out + (i * 2U), sizeof(out) - (i * 2U), "%02x", digest[i]);
  }
  return out;
}

esp_err_t status_handler(httpd_req_t* req) {
  mark_client_seen();
  const std::string body = status_json();
  set_no_store(req);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t confirm_handler(httpd_req_t* req) {
  mark_client_seen();
  if (!require_recovery_auth(req)) {
    return ESP_OK;
  }
  web_set_confirmation("confirmed");
  web_set_phase("ready", "confirmed", "Install confirmation recorded by operator", 100, 0, false);
  return send_text(req, 200, "Kurulum onayi kaydedildi.");
}

esp_err_t reboot_handler(httpd_req_t* req) {
  mark_client_seen();
  if (!require_recovery_auth(req)) {
    return ESP_OK;
  }
  char query[96] = {};
  char target[32] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    (void)httpd_query_key_value(query, "target", target, sizeof(target));
  }

  if (std::strcmp(target, "app0") == 0) {
    if (!partition_has_valid_app(g_app0)) {
      return send_text(req, 400, "app0 bolumunde gecerli uygulama yok.");
    }
    const esp_err_t err = esp_ota_set_boot_partition(g_app0);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "Failed to select app0: %s", esp_err_to_name(err));
      return send_text(req, 500, "app0 boot secimi basarisiz.");
    }
  }

  (void)send_text(req, 200, "Yeniden baslatiliyor.");
  vTaskDelay(pdMS_TO_TICKS(250));
  esp_restart();
  return ESP_OK;
}

esp_err_t upload_handler(httpd_req_t* req) {
  mark_client_seen();
  if (!require_recovery_auth(req)) {
    return ESP_OK;
  }
#if !MROS_RECOVERY_ALLOW_UNSIGNED_DIRECT_UPLOAD
  return send_text(req, 403, "Unsigned direct upload disabled; use staged signed manifest install.");
#endif
  if (g_app0 == nullptr) {
    return send_text(req, 500, "app0 partition bulunamadi.");
  }
  if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > g_app0->size) {
    return send_text(req, 400, "Firmware boyutu gecersiz veya app0 icin buyuk.");
  }

  web_set_phase("uploading", "write", "Firmware upload accepted; writing app0", 1, -1, true);

  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(g_app0, req->content_len, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_begin failed: %s", esp_err_to_name(err));
    web_set_phase("failed", "write", "esp_ota_begin failed", 0, -1, false);
    return send_text(req, 500, "OTA baslatilamadi.");
  }

  std::vector<uint8_t> buffer(kUploadChunkBytes);
  int remaining = req->content_len;
  int written = 0;
  bool ok = true;
  while (remaining > 0) {
    const int want = std::min<int>(remaining, buffer.size());
    const int got = httpd_req_recv(req, reinterpret_cast<char*>(buffer.data()), want);
    if (got <= 0) {
      if (got == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      ok = false;
      break;
    }
    err = esp_ota_write(handle, buffer.data(), got);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "esp_ota_write failed: %s", esp_err_to_name(err));
      ok = false;
      break;
    }
    written += got;
    remaining -= got;
    const uint32_t progress =
        5U + static_cast<uint32_t>((static_cast<uint64_t>(written) * 90ULL) /
                                   static_cast<uint64_t>(req->content_len));
    const int32_t eta = progress >= 95U ? 1 : static_cast<int32_t>((95U - progress) / 3U + 1U);
    web_set_phase("uploading", "write", "Firmware is being written", progress, eta, true);
  }

  if (ok) {
    err = esp_ota_end(handle);
    ok = err == ESP_OK;
    if (!ok) {
      ESP_LOGE(kTag, "esp_ota_end failed: %s", esp_err_to_name(err));
    }
  } else {
    (void)esp_ota_abort(handle);
  }

  if (ok && partition_has_valid_app(g_app0)) {
    web_set_phase("ready", "verify", "Firmware uploaded and verified; ready to boot app0", 100, 0, false);
    web_set_confirmation("manual");
    return send_text(req, 200, "Firmware yuklendi ve dogrulandi.");
  }

  web_set_phase("failed", "verify", "Upload failed or image is not a valid app", 0, -1, false);
  return send_text(req, 500, "Firmware yukleme basarisiz.");
}

esp_err_t files_handler(httpd_req_t* req) {
  mark_client_seen();
  if (!require_recovery_auth(req)) {
    return ESP_OK;
  }
  if (!filesystem_ready()) {
    return send_json(req, 200, "{\"ok\":false,\"error\":\"fs_unavailable\",\"files\":[]}");
  }
  std::string dir_error;
  (void)mros::update::ensure_update_dirs(kMountPoint, &dir_error);

  std::vector<FirmwareFile> files;
  scan_firmware_files(firmware_root_actual(), 0, &files);
  std::sort(files.begin(), files.end(), [](const FirmwareFile& left, const FirmwareFile& right) {
    if (left.score != right.score) {
      return left.score > right.score;
    }
    if (left.mtime != right.mtime) {
      return left.mtime > right.mtime;
    }
    if (left.size != right.size) {
      return left.size > right.size;
    }
    return left.display_path < right.display_path;
  });

  cJSON* root = cJSON_CreateObject();
  cJSON* array = cJSON_CreateArray();
  if (root == nullptr || array == nullptr) {
    cJSON_Delete(root);
    cJSON_Delete(array);
    return send_json(req, 500, "{\"ok\":false,\"error\":\"json_alloc\"}");
  }
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddBoolToObject(root, "fs_ready", true);
  cJSON_AddStringToObject(root, "root", "/ESPUSER/firmware");
  cJSON_AddItemToObject(root, "files", array);
  for (const FirmwareFile& file : files) {
    cJSON* item = cJSON_CreateObject();
    if (item == nullptr) {
      continue;
    }
    cJSON_AddStringToObject(item, "path", file.display_path.c_str());
    cJSON_AddStringToObject(item, "name", file.basename.c_str());
    cJSON_AddStringToObject(item, "version", file.version.empty() ? "-" : file.version.c_str());
    cJSON_AddNumberToObject(item, "size", static_cast<double>(file.size));
    cJSON_AddNumberToObject(item, "mtime", static_cast<double>(file.mtime));
    cJSON_AddNumberToObject(item, "score", file.score);
    cJSON_AddBoolToObject(item, "delete_allowed", is_delete_allowed(file.actual_path));
    cJSON_AddItemToArray(array, item);
  }

  char* printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == nullptr) {
    return send_json(req, 500, "{\"ok\":false,\"error\":\"json_print\"}");
  }
  const esp_err_t err = send_json(req, 200, printed);
  cJSON_free(printed);
  return err;
}

esp_err_t files_upload_handler(httpd_req_t* req) {
  mark_client_seen();
  if (!require_recovery_auth(req)) {
    return ESP_OK;
  }
  if (!filesystem_ready()) {
    return send_text(req, 503, "LittleFS hazir degil.");
  }
  if (g_app0 == nullptr || req->content_len <= 0 ||
      static_cast<size_t>(req->content_len) > g_app0->size) {
    return send_text(req, 400, "Firmware boyutu gecersiz veya app0 icin buyuk.");
  }

  std::string dir_error;
  if (!mros::update::ensure_update_dirs(kMountPoint, &dir_error)) {
    ESP_LOGE(kTag, "Update dir prepare failed: %s", dir_error.c_str());
    return send_text(req, 500, "Firmware klasoru hazirlanamadi.");
  }

  char query[192] = {};
  char raw_name[96] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    (void)httpd_query_key_value(query, "name", raw_name, sizeof(raw_name));
  }
  const std::string filename = sanitize_filename(raw_name[0] != '\0' ? url_decode_copy(raw_name) : "firmware.bin");
  const std::string target = inbox_root_actual() + "/" + filename;

  FILE* file = std::fopen(target.c_str(), "wb");
  if (file == nullptr) {
    ESP_LOGE(kTag, "Staged upload open failed: %s", target.c_str());
    return send_text(req, 500, "Staging dosyasi acilamadi.");
  }

  std::vector<uint8_t> buffer(kUploadChunkBytes);
  int remaining = req->content_len;
  bool ok = true;
  while (remaining > 0) {
    const int want = std::min<int>(remaining, buffer.size());
    const int got = httpd_req_recv(req, reinterpret_cast<char*>(buffer.data()), want);
    if (got <= 0) {
      if (got == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      ok = false;
      break;
    }
    if (std::fwrite(buffer.data(), 1U, static_cast<size_t>(got), file) != static_cast<size_t>(got)) {
      ok = false;
      break;
    }
    remaining -= got;
  }
  std::fclose(file);
  if (!ok) {
    (void)std::remove(target.c_str());
    return send_text(req, 500, "Firmware staging upload basarisiz.");
  }

  char response[256] = {};
  std::snprintf(response,
                sizeof(response),
                "{\"ok\":true,\"path\":\"%s\",\"size\":%ld}",
                display_path_for_actual(target).c_str(),
                static_cast<long>(req->content_len));
  return send_json(req, 200, response);
}

esp_err_t files_delete_handler(httpd_req_t* req) {
  mark_client_seen();
  if (!require_recovery_auth(req)) {
    return ESP_OK;
  }
  if (!filesystem_ready()) {
    return send_text(req, 503, "LittleFS hazir degil.");
  }
  char query[256] = {};
  char raw_path[192] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", raw_path, sizeof(raw_path)) != ESP_OK) {
    return send_text(req, 400, "path parametresi gerekli.");
  }
  std::string actual;
  std::string error;
  if (!resolve_firmware_api_path(url_decode_copy(raw_path), &actual, &error) || !is_delete_allowed(actual)) {
    return send_text(req, 400, "Silme sadece inbox/staging altindaki firmware dosyalari icin izinli.");
  }
  struct stat info {};
  if (::stat(actual.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return send_text(req, 404, "Dosya bulunamadi.");
  }
  if (::remove(actual.c_str()) != 0) {
    return send_text(req, 500, "Dosya silinemedi.");
  }
  return send_json(req, 200, "{\"ok\":true}");
}

esp_err_t install_file_handler(httpd_req_t* req) {
  mark_client_seen();
  if (!require_recovery_auth(req)) {
    return ESP_OK;
  }
  if (!filesystem_ready()) {
    return send_text(req, 503, "LittleFS hazir degil.");
  }
  if (g_app0 == nullptr) {
    return send_text(req, 500, "app0 partition bulunamadi.");
  }
  char query[256] = {};
  char raw_path[192] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", raw_path, sizeof(raw_path)) != ESP_OK) {
    return send_text(req, 400, "path parametresi gerekli.");
  }

  std::string actual;
  std::string error;
  if (!resolve_firmware_api_path(url_decode_copy(raw_path), &actual, &error) || !ends_with_bin(actual)) {
    return send_text(req, 400, "Yalnizca /ESPUSER/firmware altindaki .bin firmware dosyalari kurulabilir.");
  }
  struct stat info {};
  if (::stat(actual.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
    return send_text(req, 404, "Firmware dosyasi bulunamadi.");
  }
  if (static_cast<size_t>(info.st_size) > g_app0->size) {
    return send_text(req, 400, "Firmware app0 partition icin buyuk.");
  }

  web_set_phase("staging", "manifest", "Hashing selected firmware", 1, -1, true);
  const std::string digest = sha256_file(actual);
  if (digest.empty()) {
    web_set_phase("failed", "manifest", "Firmware hash failed", 0, -1, false);
    return send_text(req, 500, "Firmware hash hesaplanamadi.");
  }

  std::string dir_error;
  if (!mros::update::ensure_update_dirs(kMountPoint, &dir_error)) {
    web_set_phase("failed", "manifest", "Update directories unavailable", 0, -1, false);
    return send_text(req, 500, "Update klasorleri hazirlanamadi.");
  }
  if (!mros::update::clear_boot_guard(&error)) {
    web_set_phase("failed", "manifest", "Boot guard clear failed", 0, -1, false);
    return send_text(req, 500, "Eski boot guard temizlenemedi.");
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  mros::update::UpdateManifest manifest {};
  manifest.state = "pending_install";
  manifest.target = actual;
  manifest.target_display = display_path_for_actual(actual);
  manifest.sha256 = digest;
  manifest.size = static_cast<uint64_t>(info.st_size);
  manifest.version = parse_version_token(basename_of(actual));
  manifest.target_partition = mros::update::kAppLabel;
  manifest.recovery_partition = mros::update::kRecoveryLabel;
  manifest.running_partition = running != nullptr ? running->label : "";
  manifest.max_attempts = mros::update::kDefaultMaxAttempts;
  manifest.max_boots = mros::update::kDefaultMaxBoots;
  manifest.confirm_timeout_sec = mros::update::kDefaultConfirmTimeoutSec;
  if (!mros::update::sign_manifest(&manifest, &error)) {
    web_set_phase("failed", "manifest", "Manifest signature failed", 0, -1, false);
    ESP_LOGE(kTag, "Manifest signature failed: %s", error.c_str());
    return send_text(req, 403, "Manifest imzalanamadi; MROS_UPDATE_MANIFEST_HMAC_KEY gerekli.");
  }
  if (!mros::update::save_manifest_to_mount(kMountPoint, manifest, &error)) {
    web_set_phase("failed", "manifest", "Manifest save failed", 0, -1, false);
    return send_text(req, 500, "Manifest yazilamadi.");
  }

  const esp_partition_t* recovery =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, mros::update::kRecoveryLabel);
  if (recovery != nullptr) {
    (void)esp_ota_set_boot_partition(recovery);
  }
  web_set_phase("staging", "manifest", "Manifest staged; restarting recovery installer", 100, 1, false);
  (void)send_text(req, 200, "Firmware secildi. Recovery installer yeniden baslatiliyor.");
  vTaskDelay(pdMS_TO_TICKS(250));
  esp_restart();
  return ESP_OK;
}

void led_task(void*) {
  led_strip_handle_t strip = nullptr;
  led_strip_config_t strip_config = {};
  strip_config.strip_gpio_num = kRecoveryLedGpio;
  strip_config.max_leds = 1;
  strip_config.led_model = LED_MODEL_WS2812;
  strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

  led_strip_rmt_config_t rmt_config = {};
  rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt_config.resolution_hz = 10000000;
  rmt_config.mem_block_symbols = 64;
  rmt_config.flags.with_dma = false;

  if (led_strip_new_rmt_device(&strip_config, &rmt_config, &strip) != ESP_OK || strip == nullptr) {
    ESP_LOGW(kTag, "Recovery LED unavailable on GPIO%d", kRecoveryLedGpio);
    vTaskDelete(nullptr);
    return;
  }

  float phase = 0.0F;
  while (true) {
    const float mix = (std::sin(phase) + 1.0F) * 0.5F;
    const uint8_t red = static_cast<uint8_t>(24.0F + mix * 92.0F);
    const uint8_t green = static_cast<uint8_t>(24.0F + mix * 8.0F);
    const uint8_t blue = static_cast<uint8_t>(160.0F + mix * 58.0F);
    (void)led_strip_set_pixel(strip, 0, red, green, blue);
    (void)led_strip_refresh(strip);
    phase += 0.055F;
    if (phase > 6.28318F) {
      phase -= 6.28318F;
    }
    vTaskDelay(pdMS_TO_TICKS(35));
  }
}

void build_recovery_ssid() {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
    std::snprintf(g_recovery_ssid,
                  sizeof(g_recovery_ssid),
                  "%s-%02X%02X%02X",
                  kRecoverySsidPrefix,
                  mac[3],
                  mac[4],
                  mac[5]);
  } else {
    std::snprintf(g_recovery_ssid, sizeof(g_recovery_ssid), "%s", kRecoverySsidPrefix);
  }
}

bool start_wifi_ap() {
#if MROS_RECOVERY_AP_PASSWORD_PLACEHOLDER
  ESP_LOGE(kTag,
           "Recovery hotspot disabled: provision a device-specific AP password");
  return false;
#endif
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "nvs init failed: %s", esp_err_to_name(err));
    return false;
  }
  const esp_err_t user_nvs_err = nvs_flash_init_partition(kUserNvsPartition);
  if (user_nvs_err != ESP_OK && user_nvs_err != ESP_ERR_NVS_NO_FREE_PAGES &&
      user_nvs_err != ESP_ERR_NVS_NEW_VERSION_FOUND && user_nvs_err != ESP_ERR_NOT_FOUND) {
    ESP_LOGW(kTag, "nvs_sys_usr init warning: %s", esp_err_to_name(user_nvs_err));
  }

  ESP_ERROR_CHECK(esp_netif_init());
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "event loop init failed: %s", esp_err_to_name(err));
    return false;
  }
  g_device_settings = load_recovery_device_settings();
  g_sta_profile = load_recovery_sta_profile();
  if (g_sta_profile.available) {
    g_sta_netif = esp_netif_create_default_wifi_sta();
  }
  g_ap_netif = esp_netif_create_default_wifi_ap();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  build_recovery_ssid();

  err = esp_event_handler_register(
      WIFI_EVENT,
      ESP_EVENT_ANY_ID,
      [](void*, esp_event_base_t, int32_t event_id, void* event_data) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
          wifi_sta_list_t stations {};
          if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK) {
            set_station_count(stations.num);
          } else {
            set_station_count(1);
          }
          ESP_LOGI(kTag, "Recovery AP client connected; autoboot paused");
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
          wifi_sta_list_t stations {};
          if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK) {
            set_station_count(stations.num);
          } else {
            set_station_count(0);
          }
        } else if (event_id == WIFI_EVENT_STA_START && g_sta_profile.available) {
          portENTER_CRITICAL(&g_portal_mux);
          g_sta_state.connecting = true;
          g_sta_state.retry_count = 1U;
          portEXIT_CRITICAL(&g_portal_mux);
          (void)esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
          uint32_t reason = 0;
          if (event_data != nullptr) {
            reason = static_cast<wifi_event_sta_disconnected_t*>(event_data)->reason;
          }
          set_sta_disconnect_reason(reason);
          bool retry = false;
          portENTER_CRITICAL(&g_portal_mux);
          if (g_sta_profile.available && g_sta_state.retry_count < kRecoveryStaMaxRetries) {
            g_sta_state.retry_count++;
            g_sta_state.connecting = true;
            retry = true;
          } else {
            g_sta_state.connecting = false;
          }
          portEXIT_CRITICAL(&g_portal_mux);
          if (retry) {
            vTaskDelay(pdMS_TO_TICKS(300));
            (void)esp_wifi_connect();
          }
        }
      },
      nullptr);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "WiFi event handler register failed: %s", esp_err_to_name(err));
  }

  err = esp_event_handler_register(
      IP_EVENT,
      IP_EVENT_STA_GOT_IP,
      [](void*, esp_event_base_t, int32_t, void* event_data) {
        char ip[16] = "0.0.0.0";
        if (event_data != nullptr) {
          ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
          std::snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        }
        portENTER_CRITICAL(&g_portal_mux);
        g_sta_state.connected = true;
        g_sta_state.connecting = false;
        g_sta_state.rssi = -127;
        std::snprintf(g_sta_state.ip, sizeof(g_sta_state.ip), "%s", ip);
        portEXIT_CRITICAL(&g_portal_mux);
        wifi_ap_record_t ap_info {};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
          portENTER_CRITICAL(&g_portal_mux);
          g_sta_state.rssi = ap_info.rssi;
          portEXIT_CRITICAL(&g_portal_mux);
        }
        ESP_LOGI(kTag, "Recovery STA connected to '%s' ip=%s", g_sta_profile.ssid, ip);
      },
      nullptr);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "IP event handler register failed: %s", esp_err_to_name(err));
  }

  wifi_config_t ap_config = {};
  std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), g_recovery_ssid, sizeof(ap_config.ap.ssid));
  std::strncpy(reinterpret_cast<char*>(ap_config.ap.password), kRecoveryPass, sizeof(ap_config.ap.password));
  ap_config.ap.ssid_len = std::strlen(g_recovery_ssid);
  ap_config.ap.channel = 6;
  ap_config.ap.max_connection = 3;
  ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  ap_config.ap.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(g_sta_profile.available ? WIFI_MODE_APSTA : WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  if (g_sta_profile.available) {
    wifi_config_t sta_config = {};
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.ssid),
                 g_sta_profile.ssid,
                 sizeof(sta_config.sta.ssid) - 1U);
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.password),
                 g_sta_profile.pass,
                 sizeof(sta_config.sta.password) - 1U);
    sta_config.sta.scan_method = WIFI_FAST_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    if (g_sta_profile.channel > 0U) {
      sta_config.sta.channel = g_sta_profile.channel;
    }
    uint8_t bssid[6] = {};
    if (parse_bssid_copy(g_sta_profile.bssid, bssid)) {
      sta_config.sta.bssid_set = true;
      std::memcpy(sta_config.sta.bssid, bssid, sizeof(bssid));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  }
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(kTag,
           "Recovery AP started: ssid=%s pairing=%s%s%s",
           g_recovery_ssid,
           recovery_pairing_configured() ? "configured" : "unconfigured",
           g_sta_profile.available ? " | STA auto-connect=" : "",
           g_sta_profile.available ? g_sta_profile.ssid : "");
  return true;
}

void register_uri(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t route = {};
  route.uri = uri;
  route.method = method;
  route.handler = handler;
  route.user_ctx = nullptr;
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &route));
}

bool start_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 12;
  config.stack_size = 8192;
  config.task_priority = 4;
  config.uri_match_fn = httpd_uri_match_wildcard;
#if !CONFIG_FREERTOS_UNICORE
  config.core_id = 0;
#endif
  if (httpd_start(&g_server, &config) != ESP_OK || g_server == nullptr) {
    ESP_LOGE(kTag, "HTTP server start failed");
    return false;
  }
  register_uri("/api/recovery/status", HTTP_GET, status_handler);
  register_uri("/api/recovery/confirm", HTTP_POST, confirm_handler);
  register_uri("/api/recovery/reboot", HTTP_POST, reboot_handler);
  register_uri("/api/recovery/upload", HTTP_POST, upload_handler);
  register_uri("/api/recovery/files", HTTP_GET, files_handler);
  register_uri("/api/recovery/files", HTTP_DELETE, files_delete_handler);
  register_uri("/api/recovery/files/upload", HTTP_POST, files_upload_handler);
  register_uri("/api/recovery/install-file", HTTP_POST, install_file_handler);
  register_uri("/", HTTP_GET, portal_handler);
  register_uri("/index.html", HTTP_GET, portal_handler);
  register_uri("/*", HTTP_GET, portal_fallback_handler);
  ESP_LOGI(kTag, "Recovery web server ready on http://192.168.4.1/");
  return true;
}

}  // namespace

void web_set_phase(const char* state,
                   const char* phase,
                   const char* message,
                   uint32_t progress,
                   int32_t eta_sec,
                   bool busy) {
  portENTER_CRITICAL(&g_status_mux);
  std::snprintf(g_status.state, sizeof(g_status.state), "%s", state != nullptr ? state : "unknown");
  std::snprintf(g_status.phase, sizeof(g_status.phase), "%s", phase != nullptr ? phase : "unknown");
  std::snprintf(g_status.message, sizeof(g_status.message), "%s", message != nullptr ? message : "");
  g_status.progress = std::min<uint32_t>(progress, 100U);
  g_status.eta_sec = eta_sec;
  g_status.busy = busy;
  portEXIT_CRITICAL(&g_status_mux);
}

void web_set_confirmation(const char* confirmation) {
  portENTER_CRITICAL(&g_status_mux);
  std::snprintf(g_status.confirmation,
                sizeof(g_status.confirmation),
                "%s",
                confirmation != nullptr ? confirmation : "waiting");
  portEXIT_CRITICAL(&g_status_mux);
}

void web_set_filesystem_ready(bool ready) {
  portENTER_CRITICAL(&g_portal_mux);
  g_filesystem_ready = ready;
  portEXIT_CRITICAL(&g_portal_mux);
}

void web_start(const esp_partition_t* app0_partition) {
  g_app0 = app0_partition;
  web_set_phase("starting", "network", "Starting recovery AP and web server", 0, -1, true);
  (void)xTaskCreatePinnedToCore(led_task, "rec_led", 2048, nullptr, 2, nullptr, 0);
  if (start_wifi_ap() && start_http_server()) {
    web_set_phase("idle", "portal", "Recovery portal ready at http://192.168.4.1/", 0, -1, false);
  } else {
    web_set_phase("degraded", "portal", "Recovery web portal could not fully start", 0, -1, false);
  }
}

bool web_client_seen() {
  bool seen = false;
  uint8_t station_count = 0;
  portENTER_CRITICAL(&g_portal_mux);
  seen = g_client_seen;
  station_count = g_station_count;
  portEXIT_CRITICAL(&g_portal_mux);
  return seen || station_count > 0U;
}

}  // namespace mros::recovery
