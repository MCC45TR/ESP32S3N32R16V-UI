#include "src/core/rtos/device_process_manager.h"

#include <algorithm>
#include <cstring>

#include <freertos/event_groups.h>
#include <freertos/queue.h>

#include "src/comm_interfaces/spi/spi_c3_master.h"
#include "src/comm_interfaces/spi/spi_t41_link.h"
#include "src/platform/mros_nvs.h"
#include "src/platform/mros_time.h"
#include "src/utils/mros_json_writer.h"

namespace mros::rtos::dpm {
namespace {

constexpr size_t kTaskCount = static_cast<size_t>(MrosRtosTaskDiagId::Count);
constexpr size_t kTraceCap = 16;
constexpr size_t kReasonLen = 40;
constexpr size_t kSourceLen = 16;
constexpr uint32_t kNeverWaitMs = 0xFFFFFFFFUL;
constexpr uint32_t kRuntimeActiveWindowMs = 3000U;
constexpr uint32_t kRuntimeWebWindowMs = 8000U;
constexpr uint32_t kRuntimeMotionWindowMs = 750U;
constexpr EventBits_t kEventPolicy = BIT0;
constexpr EventBits_t kEventWake = BIT1;
constexpr EventBits_t kEventCycle = BIT2;

enum class EventKind : uint8_t {
  Cycle = 0,
  Sleep,
  Wake,
  Policy,
  Fault,
};

struct QueueEvent {
  EventKind kind = EventKind::Cycle;
  uint8_t id = 0;
  uint32_t value = 0;
  char reason[kReasonLen] = {};
  char source[kSourceLen] = {};
};

struct TaskRecord {
  const char* name = nullptr;
  TaskHandle_t handle = nullptr;
  State state = State::Off;
  bool managed = false;
  bool critical = false;
  const char* critical_reason = "";
  uint32_t wake_count = 0;
  uint32_t sleep_count = 0;
  uint32_t last_wake_ms = 0;
  uint32_t last_active_ms = 0;
  uint32_t wait_ms = 0;
  uint32_t last_exec_ms = 0;
  uint32_t deadline_miss = 0;
  char last_wake_reason[kReasonLen] = "boot";
};

struct TraceRecord {
  uint32_t seq = 0;
  uint32_t ms = 0;
  char task[32] = {};
  char event[16] = {};
  char reason[kReasonLen] = {};
  char source[kSourceLen] = {};
};

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
TaskRecord g_tasks[kTaskCount] = {};
TraceRecord g_trace[kTraceCap] = {};
uint32_t g_trace_seq = 0;
uint32_t g_trace_write = 0;
Policy g_policy = Policy::Observe;
QueueHandle_t g_queue = nullptr;
EventGroupHandle_t g_events = nullptr;

const char* event_name(const EventKind kind) {
  switch (kind) {
    case EventKind::Cycle:
      return "cycle";
    case EventKind::Sleep:
      return "sleep";
    case EventKind::Wake:
      return "wake";
    case EventKind::Policy:
      return "policy";
    case EventKind::Fault:
      return "fault";
    default:
      return "event";
  }
}

void copy_small(char* dst, const size_t cap, const char* src) {
  if (dst == nullptr || cap == 0U) return;
  const char* text = src != nullptr ? src : "";
  std::strncpy(dst, text, cap - 1U);
  dst[cap - 1U] = '\0';
}

bool valid_id(const MrosRtosTaskDiagId id) {
  return static_cast<size_t>(id) < kTaskCount;
}

uint32_t saturating_inc(uint32_t value) {
  return value == 0xFFFFFFFFUL ? value : value + 1U;
}

void push_trace_locked(const MrosRtosTaskDiagId id,
                       const char* event,
                       const char* reason,
                       const char* source) {
  TraceRecord& rec = g_trace[g_trace_write % kTraceCap];
  rec.seq = ++g_trace_seq;
  rec.ms = mros::platform::mros_millis();
  const size_t index = static_cast<size_t>(id);
  copy_small(rec.task, sizeof(rec.task),
             index < kTaskCount && g_tasks[index].name != nullptr
                 ? g_tasks[index].name
                 : "system");
  copy_small(rec.event, sizeof(rec.event), event);
  copy_small(rec.reason, sizeof(rec.reason), reason);
  copy_small(rec.source, sizeof(rec.source), source);
  ++g_trace_write;
}

void queue_event(EventKind kind,
                 MrosRtosTaskDiagId id,
                 const char* reason,
                 const char* source,
                 uint32_t value = 0) {
  if (g_queue == nullptr) return;
  QueueEvent event {};
  event.kind = kind;
  event.id = static_cast<uint8_t>(id);
  event.value = value;
  copy_small(event.reason, sizeof(event.reason), reason);
  copy_small(event.source, sizeof(event.source), source);
  (void)xQueueSend(g_queue, &event, 0);
}

void load_policy() {
  mros::platform::NvsNamespace ns;
  uint8_t stored = 0;
  if (ns.open("mros_dpm", true, mros::platform::NvsPartitionMode::UserPartitionsThenDefault) &&
      ns.get_u8("policy", &stored) && stored <= static_cast<uint8_t>(Policy::UpdateSafe)) {
    g_policy = static_cast<Policy>(stored);
  }
}

void save_policy(const Policy policy) {
  mros::platform::NvsNamespace ns;
  if (ns.open("mros_dpm", false, mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    (void)ns.set_u8("policy", static_cast<uint8_t>(policy));
  }
}

bool task_name_match(const char* wanted, const char* actual) {
  if (wanted == nullptr || actual == nullptr) return false;
  if (std::strcmp(wanted, actual) == 0) return true;
  const size_t len = std::strlen(wanted);
  return len > 0U && std::strncmp(wanted, actual, len) == 0;
}

bool peer_active(const MrosRtosTaskDiagId id) {
  if (id == MrosRtosTaskDiagId::CommSpiT41) return spi_s3_is_connected();
  if (id == MrosRtosTaskDiagId::CommSpiC3) return spi_c3_is_connected();
  return false;
}

bool task_recently_active_locked(const MrosRtosTaskDiagId id,
                                 const uint32_t now_ms,
                                 const uint32_t window_ms) {
  const size_t index = static_cast<size_t>(id);
  if (index >= kTaskCount) return false;
  const TaskRecord& rec = g_tasks[index];
  if (rec.name == nullptr || rec.last_active_ms == 0U) return false;
  return static_cast<int32_t>(now_ms - rec.last_active_ms) <=
         static_cast<int32_t>(window_ms);
}

bool task_state_active_locked(const MrosRtosTaskDiagId id) {
  const size_t index = static_cast<size_t>(id);
  if (index >= kTaskCount) return false;
  const TaskRecord& rec = g_tasks[index];
  return rec.name != nullptr &&
         (rec.state == State::Active || rec.state == State::BlockedIo ||
          rec.state == State::Critical || rec.state == State::Degraded);
}

const char* runtime_state_locked(const uint32_t now_ms) {
  if (g_policy == Policy::UpdateSafe) return "update_active";
  if (task_state_active_locked(MrosRtosTaskDiagId::TurretPid) ||
      task_state_active_locked(MrosRtosTaskDiagId::JointTraj) ||
      task_state_active_locked(MrosRtosTaskDiagId::ServoDrive) ||
      task_recently_active_locked(MrosRtosTaskDiagId::TurretPid, now_ms,
                                  kRuntimeMotionWindowMs) ||
      task_recently_active_locked(MrosRtosTaskDiagId::JointTraj, now_ms,
                                  kRuntimeMotionWindowMs)) {
    return "motion_active";
  }
  if (task_state_active_locked(MrosRtosTaskDiagId::Shell) ||
      task_state_active_locked(MrosRtosTaskDiagId::Ssh) ||
      task_state_active_locked(MrosRtosTaskDiagId::Mcp) ||
      task_recently_active_locked(MrosRtosTaskDiagId::Shell, now_ms,
                                  kRuntimeActiveWindowMs)) {
    return "shell_active";
  }
  if (task_state_active_locked(MrosRtosTaskDiagId::WebRuntime) ||
      task_recently_active_locked(MrosRtosTaskDiagId::WebRuntime, now_ms,
                                  kRuntimeWebWindowMs)) {
    return "web_active";
  }
  for (size_t i = 0; i < kTaskCount; ++i) {
    const TaskRecord& rec = g_tasks[i];
    if (rec.name != nullptr && rec.critical &&
        (rec.state == State::Active || rec.state == State::Critical)) {
      return "critical";
    }
  }
  return "idle";
}

bool runtime_is_active(const char* runtime_state) {
  return runtime_state != nullptr && std::strcmp(runtime_state, "idle") != 0;
}

uint32_t policy_wait_floor(const MrosRtosTaskDiagId id,
                           const Policy policy,
                           const bool active) {
  if (active || policy == Policy::Observe || policy == Policy::Performance ||
      policy == Policy::MotionSafe || policy == Policy::UpdateSafe) {
    return 0;
  }
  switch (id) {
    case MrosRtosTaskDiagId::Storage:
      return policy == Policy::PowerSave ? 1000U : 500U;
    case MrosRtosTaskDiagId::CommSpiT41:
    case MrosRtosTaskDiagId::CommSpiC3:
      return policy == Policy::PowerSave ? 1000U : 250U;
    case MrosRtosTaskDiagId::CommUartT41:
      return policy == Policy::PowerSave ? 250U : 100U;
    case MrosRtosTaskDiagId::WebRuntime:
      return policy == Policy::PowerSave ? 500U : 250U;
    default:
      return 0;
  }
}

uint32_t telemetry_fast_period_for(const Policy policy,
                                   const char* runtime_state) {
  if (policy == Policy::Performance || policy == Policy::MotionSafe ||
      policy == Policy::UpdateSafe ||
      (runtime_state != nullptr &&
       std::strcmp(runtime_state, "motion_active") == 0)) {
    return 50U;
  }
  if (policy == Policy::PowerSave) {
    return runtime_is_active(runtime_state) ? 250U : 500U;
  }
  if (policy == Policy::Conservative) {
    return runtime_is_active(runtime_state) ? 100U : 250U;
  }
  if (policy == Policy::Adaptive) {
    return runtime_is_active(runtime_state) ? 50U : 250U;
  }
  return 50U;
}

uint32_t telemetry_medium_period_for(const Policy policy,
                                     const char* runtime_state) {
  if (policy == Policy::Performance || policy == Policy::MotionSafe ||
      (runtime_state != nullptr &&
       std::strcmp(runtime_state, "motion_active") == 0)) {
    return 200U;
  }
  if (policy == Policy::PowerSave) return 1000U;
  if (policy == Policy::Conservative) return 500U;
  return runtime_is_active(runtime_state) ? 200U : 500U;
}

uint32_t telemetry_slow_period_for(const Policy policy,
                                   const char* runtime_state) {
  if (policy == Policy::PowerSave) return 5000U;
  if (policy == Policy::Conservative) return 2500U;
  return runtime_is_active(runtime_state) ? 1000U : 2000U;
}

bool wifi_ps_allowed_for(const Policy policy, const char* runtime_state) {
  if (policy == Policy::Performance || policy == Policy::MotionSafe ||
      policy == Policy::UpdateSafe) {
    return false;
  }
  if (runtime_state == nullptr) return true;
  return std::strcmp(runtime_state, "motion_active") != 0 &&
         std::strcmp(runtime_state, "update_active") != 0 &&
         std::strcmp(runtime_state, "critical") != 0;
}

PolicyDecision build_decision_locked(const uint32_t now_ms) {
  const char* runtime = runtime_state_locked(now_ms);
  PolicyDecision decision {};
  decision.policy = policy_name(g_policy);
  decision.runtime_state = runtime;
  decision.web_wait_floor_ms =
      policy_wait_floor(MrosRtosTaskDiagId::WebRuntime, g_policy,
                        runtime_is_active(runtime));
  decision.wifi_wait_floor_ms =
      policy_wait_floor(MrosRtosTaskDiagId::WifiRuntime, g_policy,
                        runtime_is_active(runtime));
  decision.storage_wait_floor_ms =
      policy_wait_floor(MrosRtosTaskDiagId::Storage, g_policy,
                        runtime_is_active(runtime));
  decision.telemetry_fast_period_ms = telemetry_fast_period_for(g_policy, runtime);
  decision.telemetry_medium_period_ms =
      telemetry_medium_period_for(g_policy, runtime);
  decision.telemetry_slow_period_ms = telemetry_slow_period_for(g_policy, runtime);
  decision.wifi_power_save_allowed = wifi_ps_allowed_for(g_policy, runtime);
  decision.light_sleep_allowed =
      decision.wifi_power_save_allowed && !runtime_is_active(runtime) &&
      (g_policy == Policy::PowerSave || g_policy == Policy::Adaptive ||
       g_policy == Policy::Conservative);
  decision.telemetry_fast_allowed =
      decision.telemetry_fast_period_ms <= 250U || runtime_is_active(runtime);
  decision.seq = g_trace_seq;
  return decision;
}

}  // namespace

const char* policy_name(const Policy policy) {
  switch (policy) {
    case Policy::Observe:
      return "observe";
    case Policy::Conservative:
      return "conservative";
    case Policy::Adaptive:
      return "adaptive";
    case Policy::Performance:
      return "performance";
    case Policy::PowerSave:
      return "power-save";
    case Policy::MotionSafe:
      return "motion-safe";
    case Policy::UpdateSafe:
      return "update-safe";
    default:
      return "observe";
  }
}

const char* state_name(const State state) {
  switch (state) {
    case State::Off:
      return "OFF";
    case State::Sleeping:
      return "SLEEPING";
    case State::Idle:
      return "IDLE";
    case State::Active:
      return "ACTIVE";
    case State::BlockedIo:
      return "BLOCKED_IO";
    case State::Critical:
      return "CRITICAL";
    case State::Degraded:
      return "DEGRADED";
    case State::Fault:
      return "FAULT";
    default:
      return "UNKNOWN";
  }
}

bool parse_policy(const char* text, Policy* out) {
  if (text == nullptr || out == nullptr) return false;
  if (std::strcmp(text, "observe") == 0) *out = Policy::Observe;
  else if (std::strcmp(text, "conservative") == 0) *out = Policy::Conservative;
  else if (std::strcmp(text, "adaptive") == 0 || std::strcmp(text, "balanced") == 0) *out = Policy::Adaptive;
  else if (std::strcmp(text, "performance") == 0) *out = Policy::Performance;
  else if (std::strcmp(text, "power-save") == 0 || std::strcmp(text, "powersave") == 0 || std::strcmp(text, "cool") == 0) *out = Policy::PowerSave;
  else if (std::strcmp(text, "motion-safe") == 0 || std::strcmp(text, "motion") == 0) *out = Policy::MotionSafe;
  else if (std::strcmp(text, "update-safe") == 0 || std::strcmp(text, "update") == 0) *out = Policy::UpdateSafe;
  else return false;
  return true;
}

void init() {
  if (g_queue == nullptr) {
    g_queue = xQueueCreate(8, sizeof(QueueEvent));
  }
  if (g_events == nullptr) {
    g_events = xEventGroupCreate();
  }
  load_policy();
}

void register_task(const MrosRtosTaskDiagId id,
                   const char* name,
                   const bool managed,
                   const bool critical,
                   const char* critical_reason) {
  if (!valid_id(id)) return;
  portENTER_CRITICAL(&g_mux);
  TaskRecord& rec = g_tasks[static_cast<size_t>(id)];
  rec.name = name;
  rec.managed = managed;
  rec.critical = critical;
  rec.critical_reason = critical_reason != nullptr ? critical_reason : "";
  rec.state = critical ? State::Critical : State::Idle;
  copy_small(rec.last_wake_reason, sizeof(rec.last_wake_reason), "registered");
  push_trace_locked(id, "register", critical ? "critical" : "managed", "boot");
  portEXIT_CRITICAL(&g_mux);
}

void set_task_handle(const MrosRtosTaskDiagId id, TaskHandle_t handle) {
  if (!valid_id(id)) return;
  portENTER_CRITICAL(&g_mux);
  g_tasks[static_cast<size_t>(id)].handle = handle;
  portEXIT_CRITICAL(&g_mux);
}

void record_cycle(const MrosRtosTaskDiagId id,
                  const uint32_t expected_period_ms,
                  const uint32_t /*actual_period_ms*/,
                  const uint32_t exec_ms,
                  const uint32_t deadline_miss_ms) {
  if (!valid_id(id)) return;
  portENTER_CRITICAL(&g_mux);
  TaskRecord& rec = g_tasks[static_cast<size_t>(id)];
  if (rec.name != nullptr) {
    rec.state = rec.critical ? State::Critical : State::Active;
    rec.last_exec_ms = exec_ms;
    rec.deadline_miss = deadline_miss_ms;
    rec.wait_ms = expected_period_ms;
    rec.last_active_ms = mros::platform::mros_millis();
    if (deadline_miss_ms > 0U && !rec.critical) rec.state = State::Degraded;
  }
  portEXIT_CRITICAL(&g_mux);
  if (g_events != nullptr) {
    xEventGroupSetBits(g_events, kEventCycle);
  }
}

void record_sleep(const MrosRtosTaskDiagId id,
                  const uint32_t wait_ms,
                  const char* reason) {
  if (!valid_id(id)) return;
  portENTER_CRITICAL(&g_mux);
  TaskRecord& rec = g_tasks[static_cast<size_t>(id)];
  if (rec.name != nullptr && !rec.critical) {
    const State prev_state = rec.state;
    const uint32_t prev_wait = rec.wait_ms;
    rec.state = wait_ms == 0U || wait_ms == kNeverWaitMs ? State::Sleeping : State::Idle;
    rec.wait_ms = wait_ms;
    rec.sleep_count = saturating_inc(rec.sleep_count);
    if (prev_state != rec.state || prev_wait != wait_ms) {
      push_trace_locked(id, "sleep", reason, "task");
    }
  }
  portEXIT_CRITICAL(&g_mux);
}

void record_wake(const MrosRtosTaskDiagId id,
                 const char* reason,
                 const char* source) {
  if (!valid_id(id)) return;
  portENTER_CRITICAL(&g_mux);
  TaskRecord& rec = g_tasks[static_cast<size_t>(id)];
  if (rec.name != nullptr) {
    const State prev_state = rec.state;
    rec.state = rec.critical ? State::Critical : State::Active;
    rec.wake_count = saturating_inc(rec.wake_count);
    rec.last_wake_ms = mros::platform::mros_millis();
    copy_small(rec.last_wake_reason, sizeof(rec.last_wake_reason), reason);
    const bool notify = reason != nullptr && std::strcmp(reason, "notify") == 0;
    if (notify || prev_state == State::Sleeping || rec.critical) {
      push_trace_locked(id, "wake", reason, source);
    }
  }
  portEXIT_CRITICAL(&g_mux);
  if (g_events != nullptr) {
    xEventGroupSetBits(g_events, kEventWake);
  }
}

uint32_t adjust_wait_ms(const MrosRtosTaskDiagId id,
                        const uint32_t proposed_wait_ms,
                        const bool active) {
  if (!valid_id(id) || proposed_wait_ms == 0U || proposed_wait_ms == kNeverWaitMs) {
    return proposed_wait_ms;
  }
  const Policy current = policy();
  const bool task_active = active || peer_active(id);
  const uint32_t floor_ms = policy_wait_floor(id, current, task_active);
  if (floor_ms == 0U) return proposed_wait_ms;
  return proposed_wait_ms < floor_ms ? floor_ms : proposed_wait_ms;
}

bool wake_task_by_name(const char* name, const char* reason, const char* source) {
  if (name == nullptr || name[0] == '\0') return false;
  bool ok = false;
  portENTER_CRITICAL(&g_mux);
  for (size_t i = 0; i < kTaskCount; ++i) {
    TaskRecord& rec = g_tasks[i];
    if (!task_name_match(name, rec.name)) continue;
    if (rec.handle != nullptr) {
      xTaskNotifyGive(rec.handle);
      ok = true;
    }
    copy_small(rec.last_wake_reason, sizeof(rec.last_wake_reason), reason);
    push_trace_locked(static_cast<MrosRtosTaskDiagId>(i), "manual-wake", reason, source);
  }
  portEXIT_CRITICAL(&g_mux);
  if (ok && g_events != nullptr) {
    xEventGroupSetBits(g_events, kEventWake);
  }
  return ok;
}

Policy policy() {
  portENTER_CRITICAL(&g_mux);
  const Policy out = g_policy;
  portEXIT_CRITICAL(&g_mux);
  return out;
}

bool set_policy(const Policy next, const bool persist) {
  portENTER_CRITICAL(&g_mux);
  g_policy = next;
  push_trace_locked(MrosRtosTaskDiagId::DeviceProcessManager, "policy",
                    policy_name(next), "shell");
  portEXIT_CRITICAL(&g_mux);
  if (persist) save_policy(next);
  if (g_events != nullptr) {
    xEventGroupSetBits(g_events, kEventPolicy);
  }
  queue_event(EventKind::Policy, MrosRtosTaskDiagId::DeviceProcessManager,
              policy_name(next), "control");
  return true;
}

void get_summary(Summary* summary) {
  if (summary == nullptr) return;
  Summary out {};
  portENTER_CRITICAL(&g_mux);
  const PolicyDecision decision = build_decision_locked(mros::platform::mros_millis());
  out.mode = policy_name(g_policy);
  out.runtime_state = decision.runtime_state;
  out.telemetry_fast_period_ms = decision.telemetry_fast_period_ms;
  out.telemetry_medium_period_ms = decision.telemetry_medium_period_ms;
  out.telemetry_slow_period_ms = decision.telemetry_slow_period_ms;
  out.wifi_power_save_allowed = decision.wifi_power_save_allowed;
  out.light_sleep_allowed = decision.light_sleep_allowed;
  out.seq = g_trace_seq;
  for (size_t i = 0; i < kTaskCount; ++i) {
    const TaskRecord& rec = g_tasks[i];
    if (rec.name == nullptr) continue;
    if (rec.managed) ++out.managed_tasks;
    if (rec.critical) ++out.critical;
    switch (rec.state) {
      case State::Sleeping:
        ++out.sleeping;
        break;
      case State::Active:
      case State::BlockedIo:
      case State::Critical:
        ++out.active;
        break;
      case State::Degraded:
        ++out.degraded;
        break;
      case State::Fault:
        ++out.fault;
        break;
      default:
        break;
    }
  }
  portEXIT_CRITICAL(&g_mux);
  *summary = out;
}

void get_policy_decision(PolicyDecision* decision) {
  if (decision == nullptr) return;
  portENTER_CRITICAL(&g_mux);
  *decision = build_decision_locked(mros::platform::mros_millis());
  portEXIT_CRITICAL(&g_mux);
}

size_t get_task_snapshots(TaskSnapshot* out, const size_t cap) {
  if (out == nullptr || cap == 0U) return 0;
  size_t count = 0;
  portENTER_CRITICAL(&g_mux);
  const char* mode = policy_name(g_policy);
  for (size_t i = 0; i < kTaskCount && count < cap; ++i) {
    const TaskRecord& rec = g_tasks[i];
    if (rec.name == nullptr) continue;
    TaskSnapshot& snap = out[count++];
    snap.name = rec.name;
    snap.state = state_name(rec.state);
    snap.policy = mode;
    snap.last_wake_reason = rec.last_wake_reason;
    snap.critical_reason = rec.critical_reason;
    snap.managed = rec.managed;
    snap.critical = rec.critical;
    snap.wake_count = rec.wake_count;
    snap.sleep_count = rec.sleep_count;
    snap.last_wake_ms = rec.last_wake_ms;
    snap.last_active_ms = rec.last_active_ms;
    snap.wait_ms = rec.wait_ms;
    snap.last_exec_ms = rec.last_exec_ms;
    snap.deadline_miss = rec.deadline_miss;
  }
  portEXIT_CRITICAL(&g_mux);
  return count;
}

size_t get_trace(TraceEntry* out, const size_t cap) {
  if (out == nullptr || cap == 0U) return 0;
  portENTER_CRITICAL(&g_mux);
  const uint32_t total = std::min<uint32_t>(g_trace_write, kTraceCap);
  const uint32_t start = g_trace_write > total ? (g_trace_write - total) : 0U;
  size_t count = 0;
  for (uint32_t i = 0; i < total && count < cap; ++i) {
    const TraceRecord& rec = g_trace[(start + i) % kTraceCap];
    out[count++] = TraceEntry {rec.seq, rec.ms, rec.task, rec.event, rec.reason, rec.source};
  }
  portEXIT_CRITICAL(&g_mux);
  return count;
}

void reset_stats() {
  portENTER_CRITICAL(&g_mux);
  for (TaskRecord& rec : g_tasks) {
    rec.wake_count = 0;
    rec.sleep_count = 0;
    rec.last_wake_ms = 0;
    rec.last_active_ms = 0;
    rec.deadline_miss = 0;
    rec.last_exec_ms = 0;
    copy_small(rec.last_wake_reason, sizeof(rec.last_wake_reason), "reset");
  }
  g_trace_write = 0;
  g_trace_seq = 0;
  portEXIT_CRITICAL(&g_mux);
}

bool status_json(char* buffer, const size_t capacity) {
  Summary summary {};
  get_summary(&summary);
  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", true);
  writer.string_field("mode", summary.mode);
  writer.string_field("runtime_state", summary.runtime_state);
  writer.u32_field("managed_tasks", summary.managed_tasks);
  writer.u32_field("sleeping", summary.sleeping);
  writer.u32_field("active", summary.active);
  writer.u32_field("critical", summary.critical);
  writer.u32_field("degraded", summary.degraded);
  writer.u32_field("fault", summary.fault);
  writer.u32_field("telemetry_fast_period_ms",
                   summary.telemetry_fast_period_ms);
  writer.u32_field("telemetry_medium_period_ms",
                   summary.telemetry_medium_period_ms);
  writer.u32_field("telemetry_slow_period_ms",
                   summary.telemetry_slow_period_ms);
  writer.bool_field("wifi_power_save_allowed",
                    summary.wifi_power_save_allowed);
  writer.bool_field("light_sleep_allowed", summary.light_sleep_allowed);
  writer.u32_field("trace_seq", summary.seq);
  writer.end();
  return !writer.overflow();
}

bool policy_decision_json(char* buffer, const size_t capacity) {
  PolicyDecision decision {};
  get_policy_decision(&decision);
  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", true);
  writer.string_field("policy", decision.policy);
  writer.string_field("runtime_state", decision.runtime_state);
  writer.u32_field("web_wait_floor_ms", decision.web_wait_floor_ms);
  writer.u32_field("wifi_wait_floor_ms", decision.wifi_wait_floor_ms);
  writer.u32_field("storage_wait_floor_ms", decision.storage_wait_floor_ms);
  writer.u32_field("telemetry_fast_period_ms",
                   decision.telemetry_fast_period_ms);
  writer.u32_field("telemetry_medium_period_ms",
                   decision.telemetry_medium_period_ms);
  writer.u32_field("telemetry_slow_period_ms",
                   decision.telemetry_slow_period_ms);
  writer.bool_field("wifi_power_save_allowed",
                    decision.wifi_power_save_allowed);
  writer.bool_field("light_sleep_allowed", decision.light_sleep_allowed);
  writer.bool_field("telemetry_fast_allowed", decision.telemetry_fast_allowed);
  writer.u32_field("trace_seq", decision.seq);
  writer.end();
  return !writer.overflow();
}

bool tasks_json(char* buffer, const size_t capacity) {
  TaskSnapshot snaps[kTaskCount] = {};
  const size_t count = get_task_snapshots(snaps, kTaskCount);
  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", true);
  writer.string_field("mode", policy_name(policy()));
  writer.u32_field("count", static_cast<uint32_t>(count));
  writer.raw_field("tasks", "[");
  for (size_t i = 0; i < count; ++i) {
    if (i > 0U) writer.append_raw(",");
    writer.append_raw("{");
    writer.append_raw("\"name\":\""); writer.append_escaped(snaps[i].name); writer.append_raw("\",");
    writer.append_raw("\"state\":\""); writer.append_escaped(snaps[i].state); writer.append_raw("\",");
    writer.append_raw("\"policy\":\""); writer.append_escaped(snaps[i].policy); writer.append_raw("\",");
    writer.append_raw("\"managed\":"); writer.append_raw(snaps[i].managed ? "true," : "false,");
    writer.append_raw("\"critical\":"); writer.append_raw(snaps[i].critical ? "true," : "false,");
    writer.append_raw("\"critical_reason\":\""); writer.append_escaped(snaps[i].critical_reason); writer.append_raw("\",");
    writer.append_raw("\"wake_count\":"); writer.u32(snaps[i].wake_count); writer.append_raw(",");
    writer.append_raw("\"sleep_count\":"); writer.u32(snaps[i].sleep_count); writer.append_raw(",");
    writer.append_raw("\"last_wake_ms\":"); writer.u32(snaps[i].last_wake_ms); writer.append_raw(",");
    writer.append_raw("\"last_active_ms\":"); writer.u32(snaps[i].last_active_ms); writer.append_raw(",");
    writer.append_raw("\"wait_ms\":"); writer.u32(snaps[i].wait_ms); writer.append_raw(",");
    writer.append_raw("\"last_exec_ms\":"); writer.u32(snaps[i].last_exec_ms); writer.append_raw(",");
    writer.append_raw("\"deadline_miss\":"); writer.u32(snaps[i].deadline_miss); writer.append_raw(",");
    writer.append_raw("\"last_wake_reason\":\""); writer.append_escaped(snaps[i].last_wake_reason); writer.append_raw("\"");
    writer.append_raw("}");
  }
  writer.append_raw("]");
  writer.end();
  return !writer.overflow();
}

bool trace_json(char* buffer, const size_t capacity) {
  TraceEntry trace[kTraceCap] = {};
  const size_t count = get_trace(trace, kTraceCap);
  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", true);
  writer.u32_field("count", static_cast<uint32_t>(count));
  writer.raw_field("trace", "[");
  for (size_t i = 0; i < count; ++i) {
    if (i > 0U) writer.append_raw(",");
    writer.append_raw("{");
    writer.append_raw("\"seq\":"); writer.u32(trace[i].seq); writer.append_raw(",");
    writer.append_raw("\"ms\":"); writer.u32(trace[i].ms); writer.append_raw(",");
    writer.append_raw("\"task\":\""); writer.append_escaped(trace[i].task); writer.append_raw("\",");
    writer.append_raw("\"event\":\""); writer.append_escaped(trace[i].event); writer.append_raw("\",");
    writer.append_raw("\"reason\":\""); writer.append_escaped(trace[i].reason); writer.append_raw("\",");
    writer.append_raw("\"source\":\""); writer.append_escaped(trace[i].source); writer.append_raw("\"");
    writer.append_raw("}");
  }
  writer.append_raw("]");
  writer.end();
  return !writer.overflow();
}

void task_loop() {
  register_task(MrosRtosTaskDiagId::DeviceProcessManager,
                "device_process_manager_task", true, false, "");
  set_task_handle(MrosRtosTaskDiagId::DeviceProcessManager, xTaskGetCurrentTaskHandle());
  while (true) {
    QueueEvent event {};
    record_sleep(MrosRtosTaskDiagId::DeviceProcessManager, kNeverWaitMs, "queue");
    if (xQueueReceive(g_queue, &event, portMAX_DELAY) == pdTRUE) {
      record_wake(MrosRtosTaskDiagId::DeviceProcessManager, event_name(event.kind),
                  event.source[0] != '\0' ? event.source : "queue");
      if (g_events != nullptr) {
        xEventGroupClearBits(g_events, kEventPolicy | kEventWake | kEventCycle);
      }
    }
  }
}

}  // namespace mros::rtos::dpm
