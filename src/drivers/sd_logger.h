#pragma once

#include <cstddef>
#include <cstdint>
#include "WString.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

typedef struct {
  uint32_t queue_depth;
  uint32_t queue_capacity;
  uint32_t queue_high_watermark;
  uint32_t enqueue_count;
  uint32_t drop_count;
  uint32_t processed_count;
  uint32_t batched_write_count;
  uint32_t batch_flush_count;
  uint32_t direct_write_fallback_count;
  uint32_t csv_flush_count;
  uint32_t atomic_write_count;
  uint32_t atomic_write_fail_count;
  bool last_atomic_write_ok;
  char last_atomic_write_error[32];
  bool migration_attempted;
  bool migration_migrated;
  char migration_source[32];
  char migration_error[32];
} LoggerDiagSnapshot;

void logger_init();
void logger_service(unsigned long now_ms);
void logger_set_task_handle(TaskHandle_t task_handle);
void logger_notify_task();
void logger_process_pending();
void logger_flush_pending();
void logger_get_diag_snapshot(LoggerDiagSnapshot *snapshot);
void logger_append_csv(const String &data);
String logger_read_csv();
String logger_read_csv_tail(size_t max_bytes);
void logger_clear_csv();
bool logger_enqueue_text_file_write(const String &path, const String &payload);
bool logger_write_text_file_atomic(const String &path, const String &payload);
const char *logger_user_root_path();
String logger_user_path(const char *relative_name);

// Preferences wrappers
void prefs_save_wifi(const String &ssid, const String &pass);
bool prefs_load_wifi(String &ssid, String &pass);
void prefs_save_wifi_runtime(const String &ssid, const String &bssid,
                             uint8_t channel, const String &pass = "");
bool prefs_load_wifi_runtime(String &ssid, String &bssid, uint8_t &channel,
                             String *pass = nullptr);
void prefs_save_token(const String &token);
String prefs_load_token();

// JSON Configuration Handlers (for PCA9685 Calibration & Motion Planner limit
// configs)
void config_write_json(const String &json_data);
String config_read_json();

// PID Configurations (stored in LittleFS)
void prefs_save_pid(float kp, float ki, float kd, float imax, float dspc);
bool prefs_load_pid(float &kp, float &ki, float &kd, float &imax, float &dspc);

bool logger_storage_ready();
bool logger_storage_info(uint64_t *total_bytes, uint64_t *used_bytes);

// Credentials (username + SHA-256 password hash stored in nvs_sys_usr partition)
// pass_hash must be the hex-encoded SHA-256 digest of the plaintext password.
void prefs_save_credentials(const String &user, const String &pass_hash);
bool prefs_load_credentials(String &user, String &pass_hash);
bool prefs_clear_credentials();
