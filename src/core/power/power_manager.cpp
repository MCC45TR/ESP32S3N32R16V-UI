#include "src/core/power/power_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#if __has_include(<driver/temperature_sensor.h>)
#include <driver/temperature_sensor.h>
#define MROS_HAS_TSENS 1
#else
#define MROS_HAS_TSENS 0
#endif

#include "src/core/rtos/device_process_manager.h"
#include "src/core/memory/memory_monitor.h"
#include "src/platform/mros_nvs.h"
#include "src/platform/mros_system.h"
#include "src/platform/mros_time.h"
#include "src/utils/mros_json_writer.h"

#ifndef CONFIG_PM_ENABLE
#define CONFIG_PM_ENABLE 0
#endif
#ifndef CONFIG_FREERTOS_USE_TICKLESS_IDLE
#define CONFIG_FREERTOS_USE_TICKLESS_IDLE 0
#endif

namespace mros::power {
namespace {

constexpr const char* kTag = "MROS_POWER";
constexpr uint32_t kDefaultBoostMs = 1800U;
constexpr uint32_t kWebHotMs = 2500U;
constexpr uint32_t kWebWarmMs = 15000U;
constexpr uint32_t kTraceCap = 16U;
constexpr uint32_t kTraceReasonLen = 40U;
constexpr uint32_t kLockCount = static_cast<uint32_t>(LockOwner::Count);
constexpr uint32_t kLazyTaskSavedBytesV1 =
    (4096U + 3072U + 3072U + 4096U) * sizeof(StackType_t);
constexpr uint32_t kStackReclaimedBytesV1 = 12288U;
constexpr uint32_t kKnownPsramMigratedBytesV1 =
    (16U * 1024U) +      // shell JSON forward buffer
    (12U * 1024U * 8U) + // storage write batching payload slots
    (4U * 1024U);        // CSV log buffer
constexpr uint32_t kWifiPsRetryMs = 10000U;
// ESP-IDF v6 TSENS init/read has been observed to abort on this firmware path
// on some S3 boots. Keep the power status path diagnostic-only and stable until
// TSENS is re-enabled behind an explicit hardware validation gate.
constexpr bool kTemperatureSensorRuntimeEnabled = false;

struct LockRecord {
  const char* name = nullptr;
  bool active = false;
  bool expiring = false;
  uint32_t expires_ms = 0;
  uint32_t ttl_ms = 0;
  uint32_t acquire_count = 0;
  uint32_t release_count = 0;
  uint32_t expired_count = 0;
  uint32_t last_change_ms = 0;
#if CONFIG_PM_ENABLE
  esp_pm_lock_handle_t cpu_lock = nullptr;
  esp_pm_lock_handle_t no_sleep_lock = nullptr;
#endif
  char reason[kTraceReasonLen] = {};
};

struct TraceRecord {
  uint32_t seq = 0;
  uint32_t ms = 0;
  char event[18] = {};
  char detail[kTraceReasonLen] = {};
};

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
Mode g_mode = Mode::Balanced;
bool g_initialized = false;
bool g_temp_ready = false;
bool g_temp_init_attempted = false;
bool g_web_stream_hint = false;
uint32_t g_last_web_feedback_ms = 0;
uint32_t g_boost_until_ms = 0;
uint32_t g_last_service_ms = 0;
uint32_t g_last_config_result = ESP_ERR_NOT_SUPPORTED;
uint32_t g_last_wifi_ps_result = ESP_ERR_INVALID_STATE;
uint32_t g_last_wifi_ps_attempt_ms = 0;
wifi_ps_type_t g_applied_wifi_ps = WIFI_PS_NONE;
bool g_wifi_ps_applied = false;
uint32_t g_trace_seq = 0;
uint32_t g_trace_write = 0;
float g_pid_cycle_avg_ms = 0.0f;
uint32_t g_pid_cycle_last_ms = 0;
char g_boost_reason[kTraceReasonLen] = {};
char g_active_locks_text[160] = {};
uint32_t g_runtime_state_since_ms = 0;
char g_runtime_state_text[24] = "idle";
TraceRecord g_trace[kTraceCap] = {};
LockRecord g_locks[kLockCount] = {
    {"robot-motion"},
    {"ik-benchmark"},
    {"web-shell-stream"},
    {"update"},
    {"flash-write"},
    {"wifi-reconnect"},
    {"remote-fs-transfer"},
    {"web-telemetry"},
    {"storage-io"},
    {"critical-operation"},
};
#if MROS_HAS_TSENS
temperature_sensor_handle_t g_temp_sensor = nullptr;
#endif

void copy_small(char* dst, const size_t cap, const char* src) {
  if (dst == nullptr || cap == 0U) return;
  const char* text = src != nullptr ? src : "";
  std::strncpy(dst, text, cap - 1U);
  dst[cap - 1U] = '\0';
}

void push_trace_locked(const char* event, const char* detail) {
  TraceRecord& rec = g_trace[g_trace_write % kTraceCap];
  rec.seq = ++g_trace_seq;
  rec.ms = platform::mros_millis();
  copy_small(rec.event, sizeof(rec.event), event);
  copy_small(rec.detail, sizeof(rec.detail), detail);
  ++g_trace_write;
}

uint32_t mode_min_mhz(const Mode mode) {
  switch (mode) {
    case Mode::Cool:
      return 40U;
    case Mode::Balanced:
      return 80U;
    case Mode::Performance:
    case Mode::MotionSafe:
    case Mode::UpdateSafe:
      return 160U;
    default:
      return 80U;
  }
}

uint32_t mode_max_mhz(const Mode mode) {
  switch (mode) {
    case Mode::Cool:
      return 160U;
    case Mode::Balanced:
    case Mode::Performance:
    case Mode::MotionSafe:
    case Mode::UpdateSafe:
      return 240U;
    default:
      return 240U;
  }
}

bool mode_light_sleep(const Mode mode) {
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
  return mode == Mode::Cool || mode == Mode::Balanced;
#else
  (void)mode;
  return false;
#endif
}

wifi_ps_type_t mode_wifi_ps(const Mode mode, const bool active_lock) {
  if (mode == Mode::Performance || mode == Mode::MotionSafe ||
      mode == Mode::UpdateSafe || active_lock) {
    return WIFI_PS_NONE;
  }
  if (mode == Mode::Cool) {
    return WIFI_PS_MAX_MODEM;
  }
  return WIFI_PS_MIN_MODEM;
}

const char* wifi_ps_name(const wifi_ps_type_t ps) {
  switch (ps) {
    case WIFI_PS_NONE:
      return "none";
    case WIFI_PS_MIN_MODEM:
      return "min-modem";
    case WIFI_PS_MAX_MODEM:
      return "max-modem";
    default:
      return "unknown";
  }
}

uint32_t active_lock_mask_locked();
uint32_t active_lock_count_locked();
uint32_t expiring_lock_count_locked();
uint32_t expired_lock_count_locked();

void update_runtime_state_locked(const char* state, const uint32_t now_ms) {
  if (state == nullptr || state[0] == '\0') state = "idle";
  if (std::strcmp(g_runtime_state_text, state) == 0) return;
  copy_small(g_runtime_state_text, sizeof(g_runtime_state_text), state);
  g_runtime_state_since_ms = now_ms;
  push_trace_locked("runtime", state);
}

const char* runtime_state_name_locked(const uint32_t now_ms) {
  const uint32_t mask = active_lock_mask_locked();
  const uint32_t update_mask =
      (1UL << static_cast<uint32_t>(LockOwner::Update)) |
      (1UL << static_cast<uint32_t>(LockOwner::FlashWrite));
  const uint32_t motion_mask =
      (1UL << static_cast<uint32_t>(LockOwner::RobotMotion)) |
      (1UL << static_cast<uint32_t>(LockOwner::IkBenchmark));
  if ((mask & update_mask) != 0U || g_mode == Mode::UpdateSafe) return "update_active";
  if ((mask & motion_mask) != 0U || g_mode == Mode::MotionSafe) return "motion_active";
  if ((mask & (1UL << static_cast<uint32_t>(LockOwner::WebShellStream))) != 0U) return "shell_active";
  if ((mask & (1UL << static_cast<uint32_t>(LockOwner::WebTelemetry))) != 0U) return "web_active";
  if (g_last_web_feedback_ms != 0U && (now_ms - g_last_web_feedback_ms) <= kWebWarmMs) return "web_active";
  if (active_lock_count_locked() > 0U) return "critical";
  return "idle";
}

void load_mode() {
  platform::NvsNamespace ns;
  uint8_t stored = static_cast<uint8_t>(Mode::Balanced);
  if (ns.open("mros_power", true,
              platform::NvsPartitionMode::UserPartitionsThenDefault) &&
      ns.get_u8("mode", &stored) &&
      stored <= static_cast<uint8_t>(Mode::UpdateSafe)) {
    g_mode = static_cast<Mode>(stored);
  }
}

void save_mode(const Mode mode) {
  platform::NvsNamespace ns;
  if (ns.open("mros_power", false,
              platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    (void)ns.set_u8("mode", static_cast<uint8_t>(mode));
  }
}

uint32_t configure_pm(const Mode mode) {
#if CONFIG_PM_ENABLE
  esp_pm_config_t cfg {};
  cfg.max_freq_mhz = static_cast<int>(mode_max_mhz(mode));
  cfg.min_freq_mhz = static_cast<int>(mode_min_mhz(mode));
  cfg.light_sleep_enable = mode_light_sleep(mode);
  return static_cast<uint32_t>(esp_pm_configure(&cfg));
#else
  (void)mode;
  return static_cast<uint32_t>(ESP_ERR_NOT_SUPPORTED);
#endif
}

uint32_t configure_wifi_ps_if_needed(const Mode mode,
                                     const bool active_lock,
                                     const uint32_t now_ms,
                                     const bool force) {
  const wifi_ps_type_t ps = mode_wifi_ps(mode, active_lock);
  uint32_t last_result = ESP_OK;
  bool should_apply = force;
  portENTER_CRITICAL(&g_mux);
  last_result = g_last_wifi_ps_result;
  if (!g_wifi_ps_applied || g_applied_wifi_ps != ps) {
    should_apply = true;
  } else if (g_last_wifi_ps_result != static_cast<uint32_t>(ESP_OK) &&
             static_cast<int32_t>(now_ms - g_last_wifi_ps_attempt_ms) >=
                 static_cast<int32_t>(kWifiPsRetryMs)) {
    should_apply = true;
  }
  if (should_apply) {
    g_last_wifi_ps_attempt_ms = now_ms;
  }
  portEXIT_CRITICAL(&g_mux);
  if (!should_apply) {
    return last_result;
  }

  const uint32_t result = static_cast<uint32_t>(esp_wifi_set_ps(ps));
  portENTER_CRITICAL(&g_mux);
  g_last_wifi_ps_result = result;
  if (result == static_cast<uint32_t>(ESP_OK)) {
    g_applied_wifi_ps = ps;
    g_wifi_ps_applied = true;
    push_trace_locked("wifi-ps", wifi_ps_name(ps));
  }
  portEXIT_CRITICAL(&g_mux);
  return result;
}

void init_locks_unlocked() {
#if CONFIG_PM_ENABLE
  for (uint32_t i = 0; i < kLockCount; ++i) {
    LockRecord& rec = g_locks[i];
    if (rec.cpu_lock == nullptr) {
      (void)esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, rec.name, &rec.cpu_lock);
    }
    if (rec.no_sleep_lock == nullptr) {
      (void)esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, rec.name,
                               &rec.no_sleep_lock);
    }
  }
#endif
}

void init_temperature_unlocked() {
#if MROS_HAS_TSENS
  portENTER_CRITICAL(&g_mux);
  if (g_temp_sensor != nullptr) {
    g_temp_ready = true;
    g_temp_init_attempted = true;
    portEXIT_CRITICAL(&g_mux);
    return;
  }
  if (g_temp_init_attempted) {
    portEXIT_CRITICAL(&g_mux);
    return;
  }
  g_temp_init_attempted = true;
  portEXIT_CRITICAL(&g_mux);

  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 110);
  temperature_sensor_handle_t sensor = nullptr;
  const bool ok = temperature_sensor_install(&cfg, &sensor) == ESP_OK &&
                  temperature_sensor_enable(sensor) == ESP_OK;
  portENTER_CRITICAL(&g_mux);
  if (ok) {
    g_temp_sensor = sensor;
    g_temp_ready = true;
    push_trace_locked("temperature", "enabled");
  } else {
    g_temp_sensor = nullptr;
    g_temp_ready = false;
    push_trace_locked("temperature", "unavailable");
  }
  portEXIT_CRITICAL(&g_mux);
#else
  portENTER_CRITICAL(&g_mux);
  g_temp_init_attempted = true;
  g_temp_ready = false;
  portEXIT_CRITICAL(&g_mux);
#endif
}

uint32_t active_lock_mask_locked() {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < kLockCount; ++i) {
    if (g_locks[i].active) mask |= (1UL << i);
  }
  return mask;
}

uint32_t active_lock_count_locked() {
  uint32_t count = 0;
  for (uint32_t i = 0; i < kLockCount; ++i) {
    if (g_locks[i].active) ++count;
  }
  return count;
}

uint32_t expiring_lock_count_locked() {
  uint32_t count = 0;
  for (uint32_t i = 0; i < kLockCount; ++i) {
    if (g_locks[i].active && g_locks[i].expiring) ++count;
  }
  return count;
}

uint32_t expired_lock_count_locked() {
  uint32_t count = 0;
  for (uint32_t i = 0; i < kLockCount; ++i) {
    count += g_locks[i].expired_count;
  }
  return count;
}

void rebuild_lock_text_locked() {
  g_active_locks_text[0] = '\0';
  size_t used = 0;
  for (uint32_t i = 0; i < kLockCount; ++i) {
    if (!g_locks[i].active) continue;
    const char* name = g_locks[i].name != nullptr ? g_locks[i].name : "lock";
    const int n = std::snprintf(g_active_locks_text + used,
                                sizeof(g_active_locks_text) - used,
                                used == 0U ? "%s" : ",%s", name);
    if (n <= 0) break;
    used += static_cast<size_t>(n);
    if (used >= sizeof(g_active_locks_text)) {
      g_active_locks_text[sizeof(g_active_locks_text) - 1U] = '\0';
      break;
    }
  }
}

void apply_lock_state(LockRecord& rec, const bool active) {
#if CONFIG_PM_ENABLE
  if (rec.cpu_lock != nullptr) {
    if (active) (void)esp_pm_lock_acquire(rec.cpu_lock);
    else (void)esp_pm_lock_release(rec.cpu_lock);
  }
  if (rec.no_sleep_lock != nullptr) {
    if (active) (void)esp_pm_lock_acquire(rec.no_sleep_lock);
    else (void)esp_pm_lock_release(rec.no_sleep_lock);
  }
#else
  (void)rec;
  (void)active;
#endif
}

void expire_locks_locked(const uint32_t now_ms, uint32_t* expired_mask) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < kLockCount; ++i) {
    LockRecord& rec = g_locks[i];
    if (!rec.active || !rec.expiring) continue;
    if (static_cast<int32_t>(rec.expires_ms - now_ms) > 0) continue;
    rec.active = false;
    rec.expiring = false;
    rec.expires_ms = 0;
    rec.ttl_ms = 0;
    rec.reason[0] = '\0';
    rec.last_change_ms = now_ms;
    rec.release_count++;
    rec.expired_count++;
    mask |= (1UL << i);
    push_trace_locked("lock-expire", rec.name);
  }
  if (mask != 0U) rebuild_lock_text_locked();
  if (expired_mask != nullptr) *expired_mask = mask;
}

void compute_demands_locked(uint32_t now_ms,
                            uint32_t* net_mhz,
                            uint32_t* rt_mhz,
                            uint32_t* target_mhz) {
  const Mode mode = g_mode;
  const uint32_t base = mode_min_mhz(mode);
  uint32_t net = base;
  uint32_t rt = base;
  const uint32_t feedback_age =
      g_last_web_feedback_ms == 0U ? 0xFFFFFFFFUL : now_ms - g_last_web_feedback_ms;
  const bool boosted = g_boost_until_ms != 0U &&
                       static_cast<int32_t>(g_boost_until_ms - now_ms) > 0;
  const bool locks = active_lock_count_locked() > 0U;
  if (mode == Mode::Performance || mode == Mode::MotionSafe ||
      mode == Mode::UpdateSafe) {
    net = 240U;
    rt = 240U;
  } else if (boosted || locks || g_web_stream_hint) {
    net = 240U;
    rt = std::max<uint32_t>(rt, 160U);
  } else if (feedback_age <= kWebHotMs) {
    net = mode == Mode::Cool ? 160U : 240U;
  } else if (feedback_age <= kWebWarmMs) {
    net = mode == Mode::Cool ? 80U : 160U;
  }
  if (g_pid_cycle_last_ms >= 8U || g_pid_cycle_avg_ms >= 6.0f) {
    rt = std::max<uint32_t>(rt, mode == Mode::Cool ? 160U : 240U);
  }
  *net_mhz = net;
  *rt_mhz = rt;
  *target_mhz = std::max(net, rt);
}

float read_temperature_unlocked(bool* valid) {
  if (valid != nullptr) *valid = false;
#if MROS_HAS_TSENS
  portENTER_CRITICAL(&g_mux);
  const bool ready = g_temp_ready;
  temperature_sensor_handle_t sensor = g_temp_sensor;
  portEXIT_CRITICAL(&g_mux);
  if (!ready || sensor == nullptr) return 0.0f;
  float c = 0.0f;
  if (temperature_sensor_get_celsius(sensor, &c) == ESP_OK) {
    if (valid != nullptr) *valid = true;
    return c;
  }
#endif
  return 0.0f;
}

}  // namespace

const char* mode_name(const Mode mode) {
  switch (mode) {
    case Mode::Cool:
      return "cool";
    case Mode::Balanced:
      return "balanced";
    case Mode::Performance:
      return "performance";
    case Mode::MotionSafe:
      return "motion-safe";
    case Mode::UpdateSafe:
      return "update-safe";
    default:
      return "balanced";
  }
}

bool parse_mode(const char* text, Mode* out) {
  if (text == nullptr || out == nullptr) return false;
  if (std::strcmp(text, "cool") == 0 || std::strcmp(text, "power-save") == 0) {
    *out = Mode::Cool;
  } else if (std::strcmp(text, "balanced") == 0 ||
             std::strcmp(text, "adaptive") == 0) {
    *out = Mode::Balanced;
  } else if (std::strcmp(text, "performance") == 0) {
    *out = Mode::Performance;
  } else if (std::strcmp(text, "motion-safe") == 0 ||
             std::strcmp(text, "motion") == 0) {
    *out = Mode::MotionSafe;
  } else if (std::strcmp(text, "update-safe") == 0 ||
             std::strcmp(text, "update") == 0) {
    *out = Mode::UpdateSafe;
  } else {
    return false;
  }
  return true;
}

const char* owner_name(const LockOwner owner) {
  const uint32_t index = static_cast<uint32_t>(owner);
  return index < kLockCount ? g_locks[index].name : "unknown";
}

void init() {
  portENTER_CRITICAL(&g_mux);
  if (g_initialized) {
    portEXIT_CRITICAL(&g_mux);
    return;
  }
  portEXIT_CRITICAL(&g_mux);

  load_mode();
  memory::init();
  portENTER_CRITICAL(&g_mux);
  const Mode initial_mode = g_mode;
  portEXIT_CRITICAL(&g_mux);

  init_locks_unlocked();
  const uint32_t pm_result = configure_pm(initial_mode);
  const uint32_t wifi_result = configure_wifi_ps_if_needed(
      initial_mode, false, platform::mros_millis(), true);

  portENTER_CRITICAL(&g_mux);
  g_last_config_result = pm_result;
  g_last_wifi_ps_result = wifi_result;
  g_initialized = true;
  push_trace_locked("init", mode_name(g_mode));
  portEXIT_CRITICAL(&g_mux);
  ESP_LOGI(kTag, "Power manager initialized mode=%s pm=%s",
           mode_name(g_mode), CONFIG_PM_ENABLE ? "on" : "off");
}

void service(const uint32_t now_ms) {
  if (!g_initialized) init();
  memory::service(now_ms);
  if (g_last_service_ms != 0U && (now_ms - g_last_service_ms) < 500U) {
    return;
  }
  bool expired_boost = false;
  bool active_lock = false;
  Mode current_mode = Mode::Balanced;
  portENTER_CRITICAL(&g_mux);
  g_last_service_ms = now_ms;
  if (g_boost_until_ms != 0U &&
      static_cast<int32_t>(g_boost_until_ms - now_ms) <= 0) {
    g_boost_until_ms = 0;
    g_boost_reason[0] = '\0';
    g_web_stream_hint = false;
    expired_boost = true;
  }
  active_lock = active_lock_count_locked() > 0U;
  current_mode = g_mode;
  uint32_t expired_mask = 0;
  expire_locks_locked(now_ms, &expired_mask);
  active_lock = active_lock_count_locked() > 0U;
  update_runtime_state_locked(runtime_state_name_locked(now_ms), now_ms);
  portEXIT_CRITICAL(&g_mux);
  for (uint32_t i = 0; i < kLockCount; ++i) {
    if ((expired_mask & (1UL << i)) != 0U) {
      apply_lock_state(g_locks[i], false);
    }
  }
  const uint32_t wifi_result =
      configure_wifi_ps_if_needed(current_mode, active_lock, now_ms, false);
  portENTER_CRITICAL(&g_mux);
  g_last_wifi_ps_result = wifi_result;
  portEXIT_CRITICAL(&g_mux);
  if (kTemperatureSensorRuntimeEnabled && !g_temp_init_attempted && now_ms > 2500U) {
    init_temperature_unlocked();
  }
  if (expired_boost) {
    (void)release_lock(LockOwner::WebShellStream);
  }
}

void mark_web_feedback(uint32_t now_ms) {
  if (now_ms == 0U) now_ms = platform::mros_millis();
  portENTER_CRITICAL(&g_mux);
  g_last_web_feedback_ms = now_ms;
  update_runtime_state_locked(runtime_state_name_locked(now_ms), now_ms);
  portEXIT_CRITICAL(&g_mux);
}

void request_boost(const char* reason, const uint32_t hold_ms) {
  const uint32_t now = platform::mros_millis();
  const uint32_t until = now + (hold_ms == 0U ? kDefaultBoostMs : hold_ms);
  portENTER_CRITICAL(&g_mux);
  if (static_cast<int32_t>(until - g_boost_until_ms) > 0) {
    g_boost_until_ms = until;
    copy_small(g_boost_reason, sizeof(g_boost_reason),
               reason != nullptr ? reason : "boost");
    push_trace_locked("boost", g_boost_reason);
  }
  portEXIT_CRITICAL(&g_mux);
  (void)acquire_lock(LockOwner::WebShellStream, reason != nullptr ? reason : "boost");
}

void report_pid_cycle(const uint32_t cycle_ms, const uint32_t /*exec_ms*/) {
  if (cycle_ms == 0U) return;
  portENTER_CRITICAL(&g_mux);
  g_pid_cycle_last_ms = cycle_ms;
  if (g_pid_cycle_avg_ms <= 0.0f) {
    g_pid_cycle_avg_ms = static_cast<float>(cycle_ms);
  } else {
    g_pid_cycle_avg_ms =
        (g_pid_cycle_avg_ms * 0.85f) + (static_cast<float>(cycle_ms) * 0.15f);
  }
  portEXIT_CRITICAL(&g_mux);
}

bool set_mode(const Mode next, const bool persist) {
  portENTER_CRITICAL(&g_mux);
  g_mode = next;
  const bool active_lock = active_lock_count_locked() > 0U;
  push_trace_locked("mode", mode_name(next));
  portEXIT_CRITICAL(&g_mux);
  const uint32_t pm_result = configure_pm(next);
  const uint32_t wifi_result = configure_wifi_ps_if_needed(
      next, active_lock, platform::mros_millis(), true);
  portENTER_CRITICAL(&g_mux);
  g_last_config_result = pm_result;
  g_last_wifi_ps_result = wifi_result;
  portEXIT_CRITICAL(&g_mux);
  if (persist) save_mode(next);
  if (next == Mode::Cool) {
    (void)rtos::dpm::set_policy(rtos::dpm::Policy::PowerSave, persist);
  } else if (next == Mode::Balanced) {
    (void)rtos::dpm::set_policy(rtos::dpm::Policy::Adaptive, persist);
  } else if (next == Mode::MotionSafe) {
    (void)rtos::dpm::set_policy(rtos::dpm::Policy::MotionSafe, persist);
  } else if (next == Mode::UpdateSafe) {
    (void)rtos::dpm::set_policy(rtos::dpm::Policy::UpdateSafe, persist);
  } else if (next == Mode::Performance) {
    (void)rtos::dpm::set_policy(rtos::dpm::Policy::Performance, persist);
  }
  return true;
}

Mode mode() {
  portENTER_CRITICAL(&g_mux);
  const Mode out = g_mode;
  portEXIT_CRITICAL(&g_mux);
  return out;
}

bool acquire_lock(const LockOwner owner, const char* reason) {
  const uint32_t index = static_cast<uint32_t>(owner);
  if (index >= kLockCount) return false;
  bool changed = false;
  portENTER_CRITICAL(&g_mux);
  LockRecord& rec = g_locks[index];
  if (!rec.active) {
    rec.active = true;
    rec.expiring = false;
    rec.expires_ms = 0;
    rec.ttl_ms = 0;
    rec.acquire_count++;
    rec.last_change_ms = platform::mros_millis();
    copy_small(rec.reason, sizeof(rec.reason), reason);
    push_trace_locked("lock+", rec.name);
    rebuild_lock_text_locked();
    changed = true;
  }
  portEXIT_CRITICAL(&g_mux);
  if (changed) {
    apply_lock_state(g_locks[index], true);
  }
  return true;
}

bool hold_lock(const LockOwner owner, const char* reason, const uint32_t ttl_ms) {
  if (ttl_ms == 0U) return acquire_lock(owner, reason);
  const uint32_t index = static_cast<uint32_t>(owner);
  if (index >= kLockCount) return false;
  const uint32_t now_ms = platform::mros_millis();
  const uint32_t until_ms = now_ms + ttl_ms;
  bool activate = false;
  portENTER_CRITICAL(&g_mux);
  LockRecord& rec = g_locks[index];
  if (!rec.active) {
    rec.active = true;
    activate = true;
    rec.acquire_count++;
    push_trace_locked("lock+", rec.name);
  }
  rec.expiring = true;
  rec.expires_ms = until_ms;
  rec.ttl_ms = ttl_ms;
  rec.last_change_ms = now_ms;
  copy_small(rec.reason, sizeof(rec.reason), reason);
  rebuild_lock_text_locked();
  update_runtime_state_locked(runtime_state_name_locked(now_ms), now_ms);
  portEXIT_CRITICAL(&g_mux);
  if (activate) {
    apply_lock_state(g_locks[index], true);
  }
  return true;
}

bool release_lock(const LockOwner owner) {
  const uint32_t index = static_cast<uint32_t>(owner);
  if (index >= kLockCount) return false;
  bool changed = false;
  portENTER_CRITICAL(&g_mux);
  LockRecord& rec = g_locks[index];
  if (rec.active) {
    rec.active = false;
    rec.expiring = false;
    rec.expires_ms = 0;
    rec.ttl_ms = 0;
    rec.release_count++;
    rec.last_change_ms = platform::mros_millis();
    push_trace_locked("lock-", rec.name);
    rec.reason[0] = '\0';
    rebuild_lock_text_locked();
    changed = true;
  }
  portEXIT_CRITICAL(&g_mux);
  if (changed) {
    apply_lock_state(g_locks[index], false);
  }
  return true;
}

void get_status(Status* out) {
  if (out == nullptr) return;
  Status s {};
  uint32_t now = platform::mros_millis();
  bool temp_valid = false;
  if (kTemperatureSensorRuntimeEnabled) {
    s.temperature_c = read_temperature_unlocked(&temp_valid);
  } else {
    s.temperature_c = 0.0f;
  }
  s.temperature_valid = temp_valid;
  portENTER_CRITICAL(&g_mux);
  s.mode = mode_name(g_mode);
  s.pm_enabled = CONFIG_PM_ENABLE;
  s.tickless_idle_enabled = CONFIG_FREERTOS_USE_TICKLESS_IDLE;
  s.light_sleep_enabled = mode_light_sleep(g_mode);
  s.min_mhz = mode_min_mhz(g_mode);
  s.max_mhz = mode_max_mhz(g_mode);
  compute_demands_locked(now, &s.net_demand_mhz, &s.rt_demand_mhz,
                         &s.target_mhz);
  s.active_lock_count = active_lock_count_locked();
  s.active_lock_mask = active_lock_mask_locked();
  s.active_locks = g_active_locks_text;
  s.expiring_lock_count = expiring_lock_count_locked();
  s.expired_lock_count = expired_lock_count_locked();
  s.boost_reason = g_boost_reason;
  s.boost_remaining_ms =
      g_boost_until_ms > now ? static_cast<uint32_t>(g_boost_until_ms - now) : 0U;
  s.last_config_result = g_last_config_result;
  s.last_wifi_ps_result = g_last_wifi_ps_result;
  s.wifi_ps_mode = wifi_ps_name(mode_wifi_ps(g_mode, s.active_lock_count > 0U));
  update_runtime_state_locked(runtime_state_name_locked(now), now);
  s.runtime_state = g_runtime_state_text;
  s.runtime_state_age_ms =
      g_runtime_state_since_ms == 0U ? 0U : now - g_runtime_state_since_ms;
  portEXIT_CRITICAL(&g_mux);
  rtos::dpm::PolicyDecision decision {};
  rtos::dpm::get_policy_decision(&decision);
  s.telemetry_fast_period_ms = decision.telemetry_fast_period_ms;
  s.telemetry_medium_period_ms = decision.telemetry_medium_period_ms;
  s.telemetry_slow_period_ms = decision.telemetry_slow_period_ms;
  s.dpm_web_wait_floor_ms = decision.web_wait_floor_ms;
  s.dpm_wifi_wait_floor_ms = decision.wifi_wait_floor_ms;
  s.dpm_storage_wait_floor_ms = decision.storage_wait_floor_ms;
  s.wifi_power_save_allowed = decision.wifi_power_save_allowed;
  s.light_sleep_policy_allowed = decision.light_sleep_allowed;
  s.actual_cpu_mhz = platform::mros_system_cpu_freq_mhz();
  s.internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s.internal_min_free =
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s.internal_largest_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  s.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  s.psram_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  s.lazy_tasks_saved_bytes = kLazyTaskSavedBytesV1;
  s.stack_reclaimed_bytes = kStackReclaimedBytesV1;
  s.psram_migrated_bytes = kKnownPsramMigratedBytesV1;
  const memory::Snapshot mem = memory::capture("power");
  s.sram_floor_state = memory::floor_state_name(mem.floor_state);
  *out = s;
}

bool status_json(char* buffer, const size_t capacity) {
  Status s {};
  get_status(&s);
  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", true);
  writer.string_field("mode", s.mode);
  writer.bool_field("pm_enabled", s.pm_enabled);
  writer.bool_field("light_sleep_enabled", s.light_sleep_enabled);
  writer.bool_field("tickless_idle_enabled", s.tickless_idle_enabled);
  writer.u32_field("actual_cpu_mhz", s.actual_cpu_mhz);
  writer.u32_field("net_demand_mhz", s.net_demand_mhz);
  writer.u32_field("rt_demand_mhz", s.rt_demand_mhz);
  writer.u32_field("target_mhz", s.target_mhz);
  writer.u32_field("min_mhz", s.min_mhz);
  writer.u32_field("max_mhz", s.max_mhz);
  writer.string_field("wifi_ps_mode", s.wifi_ps_mode);
  writer.string_field("power_runtime_state", s.runtime_state);
  writer.u32_field("runtime_state_age_ms", s.runtime_state_age_ms);
  writer.bool_field("temperature_valid", s.temperature_valid);
  writer.float_field("temperature_c", s.temperature_c, 1);
  writer.u32_field("active_lock_count", s.active_lock_count);
  writer.u32_field("active_lock_mask", s.active_lock_mask);
  writer.u32_field("expiring_lock_count", s.expiring_lock_count);
  writer.u32_field("expired_lock_count", s.expired_lock_count);
  writer.string_field("active_pm_locks", s.active_locks);
  writer.string_field("dpm_owner_locks", s.active_locks);
  writer.u32_field("telemetry_fast_period_ms", s.telemetry_fast_period_ms);
  writer.u32_field("telemetry_medium_period_ms", s.telemetry_medium_period_ms);
  writer.u32_field("telemetry_slow_period_ms", s.telemetry_slow_period_ms);
  writer.u32_field("dpm_web_wait_floor_ms", s.dpm_web_wait_floor_ms);
  writer.u32_field("dpm_wifi_wait_floor_ms", s.dpm_wifi_wait_floor_ms);
  writer.u32_field("dpm_storage_wait_floor_ms", s.dpm_storage_wait_floor_ms);
  writer.bool_field("wifi_power_save_allowed", s.wifi_power_save_allowed);
  writer.bool_field("light_sleep_policy_allowed", s.light_sleep_policy_allowed);
  writer.string_field("boost_reason", s.boost_reason);
  writer.u32_field("boost_remaining_ms", s.boost_remaining_ms);
  writer.u32_field("last_config_result", s.last_config_result);
  writer.u32_field("last_wifi_ps_result", s.last_wifi_ps_result);
  writer.u32_field("internal_free", s.internal_free);
  writer.u32_field("internal_min", s.internal_min_free);
  writer.u32_field("internal_largest_block", s.internal_largest_block);
  writer.string_field("sram_floor_state", s.sram_floor_state);
  writer.u32_field("psram_migrated_bytes", s.psram_migrated_bytes);
  writer.u32_field("lazy_tasks_saved_bytes", s.lazy_tasks_saved_bytes);
  writer.u32_field("stack_reclaimed_bytes", s.stack_reclaimed_bytes);
  writer.end();
  return !writer.overflow();
}

bool locks_json(char* buffer, const size_t capacity) {
  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", true);
  writer.raw_field("locks", "[");
  portENTER_CRITICAL(&g_mux);
  for (uint32_t i = 0; i < kLockCount; ++i) {
    if (i > 0U) writer.append_raw(",");
    writer.append_raw("{\"name\":\"");
    writer.append_escaped(g_locks[i].name);
    writer.append_raw("\",\"active\":");
    writer.append_raw(g_locks[i].active ? "true" : "false");
    writer.append_raw(",\"expiring\":");
    writer.append_raw(g_locks[i].expiring ? "true" : "false");
    writer.append_raw(",\"ttl_ms\":");
    writer.u32(g_locks[i].ttl_ms);
    writer.append_raw(",\"expires_in_ms\":");
    const uint32_t now = platform::mros_millis();
    writer.u32((g_locks[i].active && g_locks[i].expiring &&
                g_locks[i].expires_ms > now)
                   ? (g_locks[i].expires_ms - now)
                   : 0U);
    writer.append_raw(",\"acquire_count\":");
    writer.u32(g_locks[i].acquire_count);
    writer.append_raw(",\"release_count\":");
    writer.u32(g_locks[i].release_count);
    writer.append_raw(",\"expired_count\":");
    writer.u32(g_locks[i].expired_count);
    writer.append_raw(",\"reason\":\"");
    writer.append_escaped(g_locks[i].reason);
    writer.append_raw("\"}");
  }
  portEXIT_CRITICAL(&g_mux);
  writer.append_raw("]");
  writer.end();
  return !writer.overflow();
}

bool sram_json(char* buffer, const size_t capacity) {
  Status s {};
  get_status(&s);
  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", true);
  writer.u32_field("internal_total", s.internal_total);
  writer.u32_field("internal_free", s.internal_free);
  writer.u32_field("internal_min_free", s.internal_min_free);
  writer.u32_field("internal_largest_block", s.internal_largest_block);
  writer.string_field("sram_floor_state", s.sram_floor_state);
  writer.u32_field("psram_total", s.psram_total);
  writer.u32_field("psram_free", s.psram_free);
  writer.u32_field("psram_largest_block", s.psram_largest_block);
  writer.u32_field("lazy_tasks_saved_bytes", s.lazy_tasks_saved_bytes);
  writer.u32_field("stack_reclaimed_bytes", s.stack_reclaimed_bytes);
  writer.u32_field("psram_migrated_bytes", s.psram_migrated_bytes);
  writer.string_field("policy", "task stacks stay internal; large buffers use PSRAM first");
  writer.end();
  return !writer.overflow();
}

bool trace_json(char* buffer, const size_t capacity) {
  TraceRecord trace[kTraceCap] = {};
  uint32_t count = 0;
  portENTER_CRITICAL(&g_mux);
  count = std::min<uint32_t>(g_trace_write, kTraceCap);
  const uint32_t start = g_trace_write > count ? g_trace_write - count : 0U;
  for (uint32_t i = 0; i < count; ++i) {
    trace[i] = g_trace[(start + i) % kTraceCap];
  }
  portEXIT_CRITICAL(&g_mux);
  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", true);
  writer.u32_field("count", count);
  writer.raw_field("trace", "[");
  for (uint32_t i = 0; i < count; ++i) {
    if (i > 0U) writer.append_raw(",");
    writer.append_raw("{\"seq\":");
    writer.u32(trace[i].seq);
    writer.append_raw(",\"ms\":");
    writer.u32(trace[i].ms);
    writer.append_raw(",\"event\":\"");
    writer.append_escaped(trace[i].event);
    writer.append_raw("\",\"detail\":\"");
    writer.append_escaped(trace[i].detail);
    writer.append_raw("\"}");
  }
  writer.append_raw("]");
  writer.end();
  return !writer.overflow();
}

}  // namespace mros::power
