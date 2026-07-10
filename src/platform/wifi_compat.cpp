#include "WiFi.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
#include <esp_err.h>
#include <esp_log.h>
}

namespace {

constexpr char kTag[] = "WiFiCompat";

struct WifiCompatState {
  std::mutex mutex;
  bool initialized = false;
  bool started = false;
  bool event_registered = false;
  bool auto_reconnect = false;
  bool persistent = false;
  wl_status_t status = WL_DISCONNECTED;
  wifi_mode_t mode = WIFI_MODE_NULL;
  String hostname = "esp32";
  String sta_ssid;
  String sta_pass;
  String connected_ssid;
  IPAddress sta_ip;
  IPAddress ap_ip = IPAddress(192, 168, 4, 1);
  std::array<uint8_t, 6> bssid = {0, 0, 0, 0, 0, 0};
  int32_t rssi = -127;
  uint8_t channel = 0;
  bool scan_running = false;
  int16_t scan_status = -2;
  std::vector<wifi_ap_record_t> scan_results;
  WiFiEventCb callback = nullptr;
  esp_netif_t* sta_netif = nullptr;
  esp_netif_t* ap_netif = nullptr;
};

WifiCompatState g_state;

void emit_event(arduino_event_id_t event_id,
                const arduino_event_info_t* info = nullptr) {
  WiFiEventCb callback = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    callback = g_state.callback;
  }
  if (callback == nullptr) {
    return;
  }
  arduino_event_t event = {};
  event.event_id = event_id;
  if (info != nullptr) {
    event.event_info = *info;
  }
  callback(&event);
}

IPAddress ip_from_raw(const uint32_t ip_raw) {
  return IPAddress(ip_raw);
}

String bssid_to_string(const uint8_t bssid[6]) {
  char buffer[18];
  std::snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
                bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
  return String(buffer);
}

void refresh_connection_info_locked() {
  wifi_ap_record_t ap_info = {};
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    g_state.connected_ssid = String(reinterpret_cast<const char*>(ap_info.ssid));
    g_state.rssi = ap_info.rssi;
    g_state.channel = ap_info.primary;
    std::copy(std::begin(ap_info.bssid), std::end(ap_info.bssid),
              g_state.bssid.begin());
  } else {
    g_state.rssi = -127;
    g_state.channel = 0;
    g_state.bssid.fill(0);
  }

  if (g_state.sta_netif != nullptr) {
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(g_state.sta_netif, &ip_info) == ESP_OK) {
      g_state.sta_ip = ip_from_raw(ip_info.ip.addr);
    } else {
      g_state.sta_ip = IPAddress();
    }
  }

  if (g_state.ap_netif != nullptr) {
    esp_netif_ip_info_t ap_info = {};
    if (esp_netif_get_ip_info(g_state.ap_netif, &ap_info) == ESP_OK) {
      g_state.ap_ip = ip_from_raw(ap_info.ip.addr);
    }
  }
}

wl_status_t status_from_disconnect_reason(uint8_t reason) {
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

void wifi_event_handler(void*,
                        esp_event_base_t event_base,
                        int32_t event_id,
                        void* event_data) {
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
      case WIFI_EVENT_STA_CONNECTED: {
        {
          std::lock_guard<std::mutex> lock(g_state.mutex);
          g_state.status = WL_IDLE_STATUS;
          refresh_connection_info_locked();
        }
        emit_event(ARDUINO_EVENT_WIFI_STA_CONNECTED);
        break;
      }
      case WIFI_EVENT_STA_DISCONNECTED: {
        arduino_event_info_t info = {};
        if (event_data != nullptr) {
          info.wifi_sta_disconnected =
              *static_cast<wifi_event_sta_disconnected_t*>(event_data);
        }
        {
          std::lock_guard<std::mutex> lock(g_state.mutex);
          g_state.status =
              status_from_disconnect_reason(info.wifi_sta_disconnected.reason);
          g_state.sta_ip = IPAddress();
          g_state.rssi = -127;
          g_state.channel = 0;
          g_state.bssid.fill(0);
        }
        emit_event(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, &info);
        break;
      }
      case WIFI_EVENT_SCAN_DONE: {
        std::vector<wifi_ap_record_t> records;
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        records.resize(count);
        if (count > 0) {
          esp_wifi_scan_get_ap_records(&count, records.data());
          records.resize(count);
        }
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.scan_running = false;
        g_state.scan_results = std::move(records);
        g_state.scan_status = static_cast<int16_t>(g_state.scan_results.size());
        break;
      }
      case WIFI_EVENT_AP_STACONNECTED: {
        arduino_event_info_t info = {};
        if (event_data != nullptr) {
          info.wifi_ap_staconnected =
              *static_cast<wifi_event_ap_staconnected_t*>(event_data);
        }
        emit_event(ARDUINO_EVENT_WIFI_AP_STACONNECTED, &info);
        break;
      }
      case WIFI_EVENT_AP_STADISCONNECTED: {
        arduino_event_info_t info = {};
        if (event_data != nullptr) {
          info.wifi_ap_stadisconnected =
              *static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
        }
        emit_event(ARDUINO_EVENT_WIFI_AP_STADISCONNECTED, &info);
        break;
      }
      default:
        break;
    }
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    arduino_event_info_t info = {};
    if (event_data != nullptr) {
      info.got_ip = *static_cast<ip_event_got_ip_t*>(event_data);
    }
    {
      std::lock_guard<std::mutex> lock(g_state.mutex);
      g_state.status = WL_CONNECTED;
      g_state.sta_ip = ip_from_esp(info.got_ip.ip_info.ip);
      refresh_connection_info_locked();
    }
    emit_event(ARDUINO_EVENT_WIFI_STA_GOT_IP, &info);
  }
}

void ensure_netifs_locked() {
  if (g_state.sta_netif == nullptr) {
    g_state.sta_netif = esp_netif_create_default_wifi_sta();
  }
  if (g_state.ap_netif == nullptr) {
    g_state.ap_netif = esp_netif_create_default_wifi_ap();
  }
}

void ensure_started_locked() {
  if (!g_state.started) {
    ESP_ERROR_CHECK(esp_wifi_start());
    g_state.started = true;
  }
}

}  // namespace

WiFiClass WiFi;

WiFiClass::WiFiClass() = default;

void WiFiClass::ensure_initialized() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (g_state.initialized) {
    return;
  }

  const esp_err_t netif_err = esp_netif_init();
  if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(netif_err);
  }
  const esp_err_t event_err = esp_event_loop_create_default();
  if (event_err != ESP_OK && event_err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(event_err);
  }

  ensure_netifs_locked();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  g_state.initialized = true;
  g_state.mode = WIFI_MODE_NULL;
  g_state.status = WL_DISCONNECTED;

  if (!g_state.event_registered) {
    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   &wifi_event_handler, nullptr));
    g_state.event_registered = true;
  }
}

void WiFiClass::persistent(bool enable) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.persistent = enable;
}

void WiFiClass::setSleep(bool enable) {
  ensure_initialized();
  esp_wifi_set_ps(enable ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

void WiFiClass::setAutoReconnect(bool enable) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.auto_reconnect = enable;
}

void WiFiClass::setHostname(const char* hostname) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.hostname = hostname != nullptr ? hostname : "";
  if (g_state.sta_netif != nullptr) {
    esp_netif_set_hostname(g_state.sta_netif, g_state.hostname.c_str());
  }
}

void WiFiClass::mode(wifi_mode_t mode_value) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (mode_value == WIFI_MODE_NULL) {
    if (g_state.started) {
      esp_wifi_stop();
      g_state.started = false;
    }
    esp_wifi_set_mode(WIFI_MODE_NULL);
    g_state.mode = WIFI_MODE_NULL;
    g_state.status = WL_DISCONNECTED;
    g_state.sta_ip = IPAddress();
    return;
  }
  ensure_netifs_locked();
  ESP_ERROR_CHECK(esp_wifi_set_mode(mode_value));
  if (g_state.sta_netif != nullptr && !g_state.hostname.isEmpty()) {
    esp_netif_set_hostname(g_state.sta_netif, g_state.hostname.c_str());
  }
  ensure_started_locked();
  g_state.mode = mode_value;
}

bool WiFiClass::softAP(const char* ssid, const char* password) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  ensure_netifs_locked();
  const wifi_mode_t target_mode =
      (g_state.mode == WIFI_MODE_STA || g_state.mode == WIFI_MODE_APSTA)
          ? WIFI_MODE_APSTA
          : WIFI_MODE_AP;
  ESP_ERROR_CHECK(esp_wifi_set_mode(target_mode));
  ensure_started_locked();

  wifi_config_t config = {};
  std::snprintf(reinterpret_cast<char*>(config.ap.ssid), sizeof(config.ap.ssid),
                "%s", ssid != nullptr ? ssid : "");
  config.ap.ssid_len = std::strlen(reinterpret_cast<char*>(config.ap.ssid));
  std::snprintf(reinterpret_cast<char*>(config.ap.password),
                sizeof(config.ap.password), "%s",
                password != nullptr ? password : "");
  config.ap.channel = 1;
  config.ap.max_connection = 4;
  config.ap.authmode =
      (password != nullptr && password[0] != '\0') ? WIFI_AUTH_WPA_WPA2_PSK
                                                   : WIFI_AUTH_OPEN;
  if (config.ap.authmode == WIFI_AUTH_OPEN) {
    config.ap.password[0] = '\0';
  }

  if (esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK) {
    return false;
  }
  g_state.mode = target_mode;
  refresh_connection_info_locked();
  return true;
}

void WiFiClass::softAPdisconnect(bool) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  wifi_config_t config = {};
  esp_wifi_set_config(WIFI_IF_AP, &config);
  if (g_state.mode == WIFI_MODE_APSTA) {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    g_state.mode = WIFI_MODE_STA;
  } else if (g_state.mode == WIFI_MODE_AP) {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    if (g_state.started) {
      esp_wifi_stop();
      g_state.started = false;
    }
    g_state.mode = WIFI_MODE_NULL;
  }
}

IPAddress WiFiClass::softAPIP() {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  refresh_connection_info_locked();
  return g_state.ap_ip;
}

uint8_t WiFiClass::softAPgetStationNum() {
  wifi_sta_list_t list = {};
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
    return 0;
  }
  return static_cast<uint8_t>(list.num);
}

void WiFiClass::disconnect(bool, bool) {
  ensure_initialized();
  esp_wifi_disconnect();
}

void WiFiClass::begin(const char* ssid,
                      const char* password,
                      int32_t channel_value,
                      const uint8_t* bssid,
                      bool connect_now) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  ensure_netifs_locked();
  const wifi_mode_t target_mode =
      (g_state.mode == WIFI_MODE_AP || g_state.mode == WIFI_MODE_APSTA)
          ? WIFI_MODE_APSTA
          : WIFI_MODE_STA;
  ESP_ERROR_CHECK(esp_wifi_set_mode(target_mode));
  ensure_started_locked();

  wifi_config_t config = {};
  std::snprintf(reinterpret_cast<char*>(config.sta.ssid), sizeof(config.sta.ssid),
                "%s", ssid != nullptr ? ssid : "");
  std::snprintf(reinterpret_cast<char*>(config.sta.password),
                sizeof(config.sta.password), "%s",
                password != nullptr ? password : "");
  config.sta.channel = (channel_value > 0 && channel_value <= 14)
                           ? static_cast<uint8_t>(channel_value)
                           : 0;
  config.sta.bssid_set = bssid != nullptr;
  if (bssid != nullptr) {
    std::memcpy(config.sta.bssid, bssid, 6U);
  }

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
  g_state.mode = target_mode;
  g_state.sta_ssid = ssid != nullptr ? ssid : "";
  g_state.sta_pass = password != nullptr ? password : "";
  g_state.connected_ssid = g_state.sta_ssid;
  g_state.status = WL_IDLE_STATUS;

  if (connect_now) {
    esp_wifi_connect();
  }
}

wl_status_t WiFiClass::status() {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  return g_state.status;
}

String WiFiClass::SSID() {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  return !g_state.connected_ssid.isEmpty() ? g_state.connected_ssid : g_state.sta_ssid;
}

String WiFiClass::SSID(int index) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (index < 0 || static_cast<size_t>(index) >= g_state.scan_results.size()) {
    return String();
  }
  return String(reinterpret_cast<const char*>(g_state.scan_results[index].ssid));
}

int32_t WiFiClass::RSSI() {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  refresh_connection_info_locked();
  return g_state.rssi;
}

int32_t WiFiClass::RSSI(int index) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (index < 0 || static_cast<size_t>(index) >= g_state.scan_results.size()) {
    return -127;
  }
  return g_state.scan_results[index].rssi;
}

IPAddress WiFiClass::localIP() {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  refresh_connection_info_locked();
  return g_state.sta_ip;
}

String WiFiClass::BSSIDstr() {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  refresh_connection_info_locked();
  return bssid_to_string(g_state.bssid.data());
}

int32_t WiFiClass::channel() {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  refresh_connection_info_locked();
  return g_state.channel;
}

int32_t WiFiClass::channel(int index) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (index < 0 || static_cast<size_t>(index) >= g_state.scan_results.size()) {
    return 0;
  }
  return g_state.scan_results[index].primary;
}

wifi_auth_mode_t WiFiClass::encryptionType(int index) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (index < 0 || static_cast<size_t>(index) >= g_state.scan_results.size()) {
    return WIFI_AUTH_OPEN;
  }
  return g_state.scan_results[index].authmode;
}

int16_t WiFiClass::scanComplete() {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  return g_state.scan_running ? -1 : g_state.scan_status;
}

bool WiFiClass::scanNetworks(bool async) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (g_state.scan_running) {
    return false;
  }
  if (g_state.mode == WIFI_MODE_NULL) {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ensure_started_locked();
    g_state.mode = WIFI_MODE_STA;
  }
  wifi_scan_config_t scan_config = {};
  const esp_err_t err = esp_wifi_scan_start(&scan_config, !async);
  if (err != ESP_OK) {
    g_state.scan_status = -2;
    return false;
  }
  if (async) {
    g_state.scan_running = true;
    g_state.scan_status = -1;
    g_state.scan_results.clear();
    return true;
  }

  uint16_t count = 0;
  esp_wifi_scan_get_ap_num(&count);
  g_state.scan_results.resize(count);
  if (count > 0) {
    esp_wifi_scan_get_ap_records(&count, g_state.scan_results.data());
    g_state.scan_results.resize(count);
  }
  g_state.scan_running = false;
  g_state.scan_status = static_cast<int16_t>(g_state.scan_results.size());
  return true;
}

void WiFiClass::scanDelete() {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.scan_results.clear();
  g_state.scan_status = -2;
  g_state.scan_running = false;
}

int WiFiClass::onEvent(WiFiEventCb cb) {
  ensure_initialized();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.callback = cb;
  return 1;
}
