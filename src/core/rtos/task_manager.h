#pragma once

#include <stdbool.h>
#include <stdint.h>

enum class MrosRtosTaskDiagId : uint8_t {
  WebRuntime = 0,
  WifiRuntime,
  CommUartT41,
  Storage,
  FkPreview,
  Shell,
  Ssh,
  Mcp,
  CommSpiT41,
  CommSpiC3,
  JointTraj,
  ExperimentalWorker,
  BearingHealth,
  TurretPid,
  ServoDrive,
  DeviceProcessManager,
  Count,
};

struct MrosRtosTaskDiagSnapshot {
  const char* name = nullptr;
  uint32_t expected_period_ms = 0;
  uint32_t last_period_ms = 0;
  uint32_t last_exec_ms = 0;
  uint32_t last_slip_ms = 0;
  uint32_t max_slip_ms = 0;
  uint32_t wake_count = 0;
  uint32_t slip_count = 0;
};

struct MrosRtosAggregateSnapshot {
  uint32_t task_count = 0;
  uint32_t total_wake_count = 0;
  uint32_t total_slip_count = 0;
  uint32_t max_slip_ms = 0;
  uint32_t max_exec_ms = 0;
  const char* max_slip_task = nullptr;
  const char* max_exec_task = nullptr;
};

void app_init();
void app_loop();
void app_rtos_record_task_cycle(MrosRtosTaskDiagId id,
                                uint32_t expected_period_ms,
                                uint32_t actual_period_ms,
                                uint32_t exec_ms);
bool app_rtos_get_task_diag_by_name(const char* task_name,
                                    MrosRtosTaskDiagSnapshot* snapshot);
void app_rtos_get_aggregate_diag(MrosRtosAggregateSnapshot* snapshot);
