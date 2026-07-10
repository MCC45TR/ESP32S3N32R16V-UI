#pragma once

#include <cstdint>

#include "IPAddress.h"
#include "WString.h"

extern "C" {
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
}

enum wl_status_t : uint8_t {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6,
};

enum arduino_event_id_t : int32_t {
  ARDUINO_EVENT_WIFI_STA_CONNECTED = 0,
  ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 1,
  ARDUINO_EVENT_WIFI_STA_GOT_IP = 2,
  ARDUINO_EVENT_WIFI_AP_STACONNECTED = 3,
  ARDUINO_EVENT_WIFI_AP_STADISCONNECTED = 4,
};

typedef struct arduino_event_info_t {
  wifi_event_sta_disconnected_t wifi_sta_disconnected;
  wifi_event_ap_staconnected_t wifi_ap_staconnected;
  wifi_event_ap_stadisconnected_t wifi_ap_stadisconnected;
  ip_event_got_ip_t got_ip;
} arduino_event_info_t;

typedef struct arduino_event_t {
  arduino_event_id_t event_id;
  arduino_event_info_t event_info;
} arduino_event_t;

using WiFiEventCb = void (*)(arduino_event_t* event);

#ifndef WIFI_OFF
#define WIFI_OFF WIFI_MODE_NULL
#endif

#ifndef WIFI_STA
#define WIFI_STA WIFI_MODE_STA
#endif

#ifndef WIFI_AP
#define WIFI_AP WIFI_MODE_AP
#endif

#ifndef WIFI_AP_STA
#define WIFI_AP_STA WIFI_MODE_APSTA
#endif

class WiFiClass {
 public:
  WiFiClass();

  void persistent(bool enable);
  void setSleep(bool enable);
  void setAutoReconnect(bool enable);
  void setHostname(const char* hostname);
  void mode(wifi_mode_t mode);
  bool softAP(const char* ssid, const char* password = nullptr);
  void softAPdisconnect(bool wifioff = false);
  IPAddress softAPIP();
  uint8_t softAPgetStationNum();
  void disconnect(bool wifioff = false, bool eraseap = false);
  void begin(const char* ssid,
             const char* password = nullptr,
             int32_t channel = 0,
             const uint8_t* bssid = nullptr,
             bool connect = true);
  wl_status_t status();
  String SSID();
  String SSID(int index);
  int32_t RSSI();
  int32_t RSSI(int index);
  IPAddress localIP();
  String BSSIDstr();
  int32_t channel();
  int32_t channel(int index);
  wifi_auth_mode_t encryptionType(int index);
  int16_t scanComplete();
  bool scanNetworks(bool async = false);
  void scanDelete();
  int onEvent(WiFiEventCb cb);

 private:
  void ensure_initialized();
};

extern WiFiClass WiFi;
