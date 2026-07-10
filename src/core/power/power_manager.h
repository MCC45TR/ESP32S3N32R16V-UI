#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mros::power {

enum class Mode : uint8_t {
  Cool = 0,
  Balanced,
  Performance,
  MotionSafe,
  UpdateSafe,
};

enum class LockOwner : uint8_t {
  RobotMotion = 0,
  IkBenchmark,
  WebShellStream,
  Update,
  FlashWrite,
  WifiReconnect,
  RemoteFsTransfer,
  WebTelemetry,
  StorageIo,
  CriticalOperation,
  Count,
};

struct Status {
  const char* mode = "balanced";
  bool pm_enabled = false;
  bool light_sleep_enabled = false;
  bool tickless_idle_enabled = false;
  bool temperature_valid = false;
  float temperature_c = 0.0f;
  uint32_t actual_cpu_mhz = 0;
  uint32_t net_demand_mhz = 80;
  uint32_t rt_demand_mhz = 80;
  uint32_t target_mhz = 80;
  uint32_t min_mhz = 80;
  uint32_t max_mhz = 240;
  const char* wifi_ps_mode = "none";
  const char* runtime_state = "idle";
  uint32_t active_lock_count = 0;
  uint32_t active_lock_mask = 0;
  const char* active_locks = "";
  uint32_t expiring_lock_count = 0;
  uint32_t expired_lock_count = 0;
  uint32_t runtime_state_age_ms = 0;
  uint32_t telemetry_fast_period_ms = 50;
  uint32_t telemetry_medium_period_ms = 200;
  uint32_t telemetry_slow_period_ms = 1000;
  uint32_t dpm_web_wait_floor_ms = 0;
  uint32_t dpm_wifi_wait_floor_ms = 0;
  uint32_t dpm_storage_wait_floor_ms = 0;
  bool wifi_power_save_allowed = true;
  bool light_sleep_policy_allowed = false;
  const char* boost_reason = "";
  uint32_t boost_remaining_ms = 0;
  uint32_t last_config_result = 0;
  uint32_t last_wifi_ps_result = 0;
  uint32_t internal_total = 0;
  uint32_t internal_free = 0;
  uint32_t internal_min_free = 0;
  uint32_t internal_largest_block = 0;
  uint32_t psram_total = 0;
  uint32_t psram_free = 0;
  uint32_t psram_largest_block = 0;
  uint32_t lazy_tasks_saved_bytes = 0;
  uint32_t stack_reclaimed_bytes = 0;
  uint32_t psram_migrated_bytes = 0;
  const char* sram_floor_state = "OK";
};

void init();
void service(uint32_t now_ms);
void mark_web_feedback(uint32_t now_ms = 0);
void request_boost(const char* reason, uint32_t hold_ms);
void report_pid_cycle(uint32_t cycle_ms, uint32_t exec_ms);

bool set_mode(Mode mode, bool persist);
Mode mode();
bool parse_mode(const char* text, Mode* out);
const char* mode_name(Mode mode);

bool acquire_lock(LockOwner owner, const char* reason);
bool hold_lock(LockOwner owner, const char* reason, uint32_t ttl_ms);
bool release_lock(LockOwner owner);
const char* owner_name(LockOwner owner);

void get_status(Status* out);
bool status_json(char* buffer, size_t capacity);
bool locks_json(char* buffer, size_t capacity);
bool sram_json(char* buffer, size_t capacity);
bool trace_json(char* buffer, size_t capacity);

}  // namespace mros::power
