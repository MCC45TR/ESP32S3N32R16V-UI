#pragma once

#include <stdint.h>

#include "WString.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace mros::experimental {

constexpr uint8_t kLastOpCount = 3;

enum class WorkerJobType : uint8_t {
  None = 0,
  Fk,
  Ik,
  Trajectory,
  DeviceTest,
};

enum DeviceTestMask : uint8_t {
  kDeviceTestT41 = 1U << 0,
  kDeviceTestC3 = 1U << 1,
  kDeviceTestWifi = 1U << 2,
  kDeviceTestPca = 1U << 3,
  kDeviceTestWeb = 1U << 4,
  kDeviceTestAll = kDeviceTestT41 | kDeviceTestC3 | kDeviceTestWifi |
                   kDeviceTestPca | kDeviceTestWeb,
};

struct WorkerOpStat {
  uint32_t seq = 0;
  uint32_t duration_us = 0;
  uint32_t ended_ms = 0;
  bool ok = false;
  char op[16] = {};
  char source[16] = {};
};

struct WorkerSnapshot {
  bool enabled = false;
  bool active = false;
  bool busy = false;
  bool cancel_requested = false;
  bool psram_result = false;
  bool queue_ready = false;
  uint32_t seq = 0;
  uint32_t wake_count = 0;
  uint32_t rejected_disabled = 0;
  uint32_t rejected_busy = 0;
  WorkerOpStat last3[kLastOpCount] = {};
};

struct WorkerRequest {
  WorkerJobType type = WorkerJobType::None;
  char source[16] = {};
  float joints_deg[7] = {};
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float t_ms = 1000.0f;
  uint8_t device_mask = 0U;
};

struct WorkerResult {
  WorkerJobType type = WorkerJobType::None;
  bool accepted = false;
  bool ok = false;
  bool disabled = false;
  bool busy = false;
  bool timed_out = false;
  bool psram_result = false;
  uint32_t seq = 0;
  uint32_t duration_us = 0;
  char message[128] = {};

  float fk_x = 0.0f;
  float fk_y = 0.0f;
  float fk_z = 0.0f;
  float fk_alpha_deg = 0.0f;

  float ik_joints_deg[7] = {};
  float ik_sigma_min = 0.0f;
  float ik_pos_err_mm = 0.0f;
  uint16_t ik_iterations = 0;
  uint32_t ik_warnings = 0;

  bool t41_connected = false;
  bool c3_connected = false;
  bool wifi_connected = false;
  bool wifi_ap_active = false;
  bool pca_ready = false;
  bool espnow_active = false;
  bool espnow_connected = false;
  uint32_t ws_clients = 0;
  uint32_t t41_qspi_txn = 0;
  uint32_t t41_qspi_crc_errors = 0;
  uint32_t c3_total_rx = 0;
  uint32_t c3_crc_errors = 0;
};

void worker_init();
void worker_set_task_handle(TaskHandle_t task_handle);
void worker_task_loop();

bool worker_is_enabled();
bool worker_set_enabled(bool enabled);
void worker_clear_stats();
void worker_cancel();
void worker_get_snapshot(WorkerSnapshot* out);
String worker_status_json();
String worker_result_json(const WorkerResult& result);
String worker_last3_json();

bool worker_submit_sync(const WorkerRequest& request, uint32_t timeout_ms,
                        WorkerResult* out);

const char* worker_job_name(WorkerJobType type);

}  // namespace mros::experimental
