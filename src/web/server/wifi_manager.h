#pragma once

#include "WString.h"
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct WifiManagerState {
  bool sta_connected;
  bool sta_connecting;
  bool ap_active;
  bool scan_in_progress;
  bool test_in_progress;
  int32_t rssi;
  uint32_t reconnect_attempts;
  uint32_t last_disconnect_reason;
  uint8_t ap_station_count;
};

struct WifiManagerSnapshot {
  WifiManagerState state;
  uint32_t scan_age_ms = 0;
  uint32_t manual_scan_age_ms = 0;
  uint32_t bootstrap_scan_age_ms = 0;
  uint32_t ap_grace_remaining_ms = 0;
  uint32_t reconnect_backoff_ms = 0;
  uint32_t last_connect_duration_ms = 0;
  uint32_t fast_path_attempts = 0;
  uint32_t fast_path_successes = 0;
  uint32_t event_revision = 0;
  uint32_t state_json_builds = 0;
  uint32_t state_json_cache_hits = 0;
  uint32_t runtime_publishes = 0;
  bool native_event_active = false;
  bool dns_lwip_active = false;
  const char *snapshot_source = "idf-event";
  const char *dns_backend = "lwip-udp";
  uint32_t idf_event_count = 0;
  uint32_t arduino_event_count = 0;
  uint32_t dns_queries = 0;
  uint32_t dns_replies = 0;
  uint32_t dns_errors = 0;
  uint8_t current_channel = 0;
  uint8_t last_good_channel = 0;
  String phase;
  String ssid;
  String ip;
  String ap_ip;
  String last_good_ssid;
  String last_good_bssid;
};

void wifi_manager_set_task_handle(TaskHandle_t task_handle);
uint32_t wifi_manager_wait_timeout_ms(unsigned long now_ms);
void wifi_manager_init();
void wifi_manager_loop(unsigned long now_ms);
const WifiManagerState &wifi_manager_state();
void wifi_manager_get_state(WifiManagerState *out_state);
void wifi_manager_get_snapshot(WifiManagerSnapshot *out_snapshot);
bool wifi_manager_is_connected();
const char *wifi_manager_ssid();
const char *wifi_manager_ip();
const char *wifi_manager_ap_ip();
bool wifi_manager_request_scan();
bool wifi_manager_is_scan_in_progress();
void wifi_manager_get_scan_results_json(String *out_json);
void wifi_manager_get_scan_results_text(String *out_text);
void wifi_manager_get_saved_networks_text(String *out_text);
bool wifi_manager_request_test_connect(const String &ssid, const String &pass);
bool wifi_manager_save_credentials(const String &ssid, const String &pass,
                                   bool connect_now);
void wifi_manager_set_enabled(bool enabled);
bool wifi_manager_is_enabled();
void wifi_manager_force_hotspot();
void wifi_manager_request_reconnect();
void wifi_manager_build_state_json(String *out_json);
void wifi_manager_build_diag_json(String *out_json);
