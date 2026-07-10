#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "src/core/rtos/task_manager.h"

namespace mros::rtos::dpm {

enum class Policy : uint8_t {
  Observe = 0,
  Conservative,
  Adaptive,
  Performance,
  PowerSave,
  MotionSafe,
  UpdateSafe,
  Cool = PowerSave,
  Balanced = Adaptive,
};

enum class State : uint8_t {
  Off = 0,
  Sleeping,
  Idle,
  Active,
  BlockedIo,
  Critical,
  Degraded,
  Fault,
};

struct Summary {
  const char* mode = "observe";
  const char* runtime_state = "idle";
  uint32_t managed_tasks = 0;
  uint32_t sleeping = 0;
  uint32_t active = 0;
  uint32_t critical = 0;
  uint32_t degraded = 0;
  uint32_t fault = 0;
  uint32_t telemetry_fast_period_ms = 50;
  uint32_t telemetry_medium_period_ms = 200;
  uint32_t telemetry_slow_period_ms = 1000;
  bool wifi_power_save_allowed = true;
  bool light_sleep_allowed = false;
  uint32_t seq = 0;
};

struct TaskSnapshot {
  const char* name = nullptr;
  const char* state = nullptr;
  const char* policy = nullptr;
  const char* last_wake_reason = nullptr;
  const char* critical_reason = nullptr;
  bool managed = false;
  bool critical = false;
  uint32_t wake_count = 0;
  uint32_t sleep_count = 0;
  uint32_t last_wake_ms = 0;
  uint32_t last_active_ms = 0;
  uint32_t wait_ms = 0;
  uint32_t last_exec_ms = 0;
  uint32_t deadline_miss = 0;
};

struct TraceEntry {
  uint32_t seq = 0;
  uint32_t ms = 0;
  const char* task = nullptr;
  const char* event = nullptr;
  const char* reason = nullptr;
  const char* source = nullptr;
};

struct PolicyDecision {
  const char* policy = "observe";
  const char* runtime_state = "idle";
  uint32_t web_wait_floor_ms = 0;
  uint32_t wifi_wait_floor_ms = 0;
  uint32_t storage_wait_floor_ms = 0;
  uint32_t telemetry_fast_period_ms = 50;
  uint32_t telemetry_medium_period_ms = 200;
  uint32_t telemetry_slow_period_ms = 1000;
  bool wifi_power_save_allowed = true;
  bool light_sleep_allowed = false;
  bool telemetry_fast_allowed = true;
  uint32_t seq = 0;
};

void init();
void task_loop();
void register_task(MrosRtosTaskDiagId id,
                   const char* name,
                   bool managed,
                   bool critical,
                   const char* critical_reason);
void set_task_handle(MrosRtosTaskDiagId id, TaskHandle_t handle);
void record_cycle(MrosRtosTaskDiagId id,
                  uint32_t expected_period_ms,
                  uint32_t actual_period_ms,
                  uint32_t exec_ms,
                  uint32_t deadline_miss_ms);
void record_sleep(MrosRtosTaskDiagId id, uint32_t wait_ms, const char* reason);
void record_wake(MrosRtosTaskDiagId id, const char* reason, const char* source);
uint32_t adjust_wait_ms(MrosRtosTaskDiagId id,
                        uint32_t proposed_wait_ms,
                        bool active);
bool wake_task_by_name(const char* name, const char* reason, const char* source);

Policy policy();
bool set_policy(Policy policy, bool persist);
bool parse_policy(const char* text, Policy* out);
const char* policy_name(Policy policy);
const char* state_name(State state);

void get_summary(Summary* summary);
void get_policy_decision(PolicyDecision* decision);
size_t get_task_snapshots(TaskSnapshot* out, size_t cap);
size_t get_trace(TraceEntry* out, size_t cap);
void reset_stats();

bool status_json(char* buffer, size_t capacity);
bool policy_decision_json(char* buffer, size_t capacity);
bool tasks_json(char* buffer, size_t capacity);
bool trace_json(char* buffer, size_t capacity);

}  // namespace mros::rtos::dpm
