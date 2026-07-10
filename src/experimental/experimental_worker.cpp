#include "experimental_worker.h"

#include "src/comm_interfaces/spi/spi_c3_master.h"
#include "src/comm_interfaces/spi/spi_t41_link.h"
#include "src/drivers/i2c/pca9685_driver.h"
#include "src/kinematics/forward/fw_kinematics.h"
#include "src/kinematics/inverse/inv_kinematics.h"
#include "src/kinematics/robot_model.h"
#include "src/platform/mros_nvs.h"
#include "src/platform/mros_time.h"
#include "src/utils/mros_json_writer.h"
#include "src/web/server/wifi_manager.h"
#include "src/web/web_server.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mros::experimental {
namespace {

constexpr const char* kNvsNamespace = "exp_worker";
constexpr const char* kNvsEnabledKey = "enabled";
constexpr uint32_t kDefaultTimeoutMs = 5000U;

struct QueueItem {
  WorkerRequest request;
  WorkerResult* result = nullptr;
  SemaphoreHandle_t done = nullptr;
};

QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task_handle = nullptr;
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
bool g_enabled = false;
bool g_active = false;
bool g_cancel = false;
bool g_psram_result = false;
uint32_t g_seq = 0;
uint32_t g_wake_count = 0;
uint32_t g_rejected_disabled = 0;
uint32_t g_rejected_busy = 0;
WorkerOpStat g_last3[kLastOpCount] = {};

uint32_t saturating_duration_us(const int64_t start_us, const int64_t end_us) {
  if (end_us <= start_us) return 0U;
  const uint64_t delta = static_cast<uint64_t>(end_us - start_us);
  return delta > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<uint32_t>(delta);
}

void copy_text(char* dst, const size_t dst_len, const char* src) {
  if (dst == nullptr || dst_len == 0U) return;
  std::snprintf(dst, dst_len, "%s", src != nullptr ? src : "");
}

bool load_enabled_from_nvs() {
  mros::platform::NvsNamespace ns;
  bool value = false;
  if (!ns.open(kNvsNamespace, true,
               mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    return false;
  }
  if (!ns.get_bool(kNvsEnabledKey, &value)) {
    return false;
  }
  return value;
}

bool save_enabled_to_nvs(const bool enabled) {
  mros::platform::NvsNamespace ns;
  if (!ns.open(kNvsNamespace, false,
               mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    return false;
  }
  return ns.set_bool(kNvsEnabledKey, enabled);
}

void record_stat_locked(const WorkerRequest& request, const WorkerResult& result) {
  for (uint8_t i = kLastOpCount - 1U; i > 0U; --i) {
    g_last3[i] = g_last3[i - 1U];
  }
  WorkerOpStat& stat = g_last3[0];
  stat.seq = result.seq;
  stat.duration_us = result.duration_us;
  stat.ended_ms = mros::platform::mros_millis();
  stat.ok = result.ok;
  copy_text(stat.op, sizeof(stat.op), worker_job_name(request.type));
  copy_text(stat.source, sizeof(stat.source), request.source[0] != '\0' ? request.source : "unknown");
}

void fill_common_result(WorkerResult* result, const WorkerRequest& request) {
  if (result == nullptr) return;
  *result = WorkerResult {};
  result->type = request.type;
  result->accepted = true;
  result->psram_result = g_psram_result;
}

void run_fk(const WorkerRequest& request, WorkerResult* result) {
  FK_Result_t fk {};
  fk_compute(request.joints_deg, &fk);
  result->fk_x = fk.x;
  result->fk_y = fk.y;
  result->fk_z = fk.z;
  result->fk_alpha_deg = fk.alpha_deg;
  result->ok = true;
  copy_text(result->message, sizeof(result->message), "onboard fk complete");
}

void run_ik(const WorkerRequest& request, WorkerResult* result) {
  InvKinematicsOptions opts {};
  opts.pos_tol_mm = mros::kinematics::robot_model().ik_pos_tol_mm;
  opts.max_iter = 500U;
  opts.max_step_deg = 10.0f;
  opts.null_gain = 0.1f;

  InvKinematicsResult ik {};
  const bool ok = inv_kinematics_solve_with_seed(request.x, request.y, request.z,
                                                 request.joints_deg, &opts, &ik);
  result->ok = ok;
  result->ik_sigma_min = ik.sigma_min;
  result->ik_pos_err_mm = ik.pos_err_mm;
  result->ik_iterations = ik.iterations;
  result->ik_warnings = ik.warnings;
  for (uint8_t i = 0; i < 7U; ++i) {
    result->ik_joints_deg[i] = ik.joints_deg[i];
  }
  copy_text(result->message, sizeof(result->message),
            ok ? "onboard ik complete" : "onboard ik did not converge");
}

void run_trajectory(const WorkerRequest& request, WorkerResult* result) {
  const float duration = (request.t_ms > 1.0f) ? request.t_ms : 1000.0f;
  result->ok = true;
  result->fk_x = request.x;
  result->fk_y = request.y;
  result->fk_z = request.z;
  result->fk_alpha_deg = duration;
  copy_text(result->message, sizeof(result->message),
            "onboard trajectory diagnostic complete");
}

void run_device_test(const WorkerRequest& request, WorkerResult* result) {
  const uint8_t mask = request.device_mask != 0U ? request.device_mask
                                                 : static_cast<uint8_t>(kDeviceTestAll);
  WifiManagerState wifi {};
  wifi_manager_get_state(&wifi);

  result->t41_connected = ((mask & kDeviceTestT41) == 0U) || spi_s3_is_connected();
  result->c3_connected = ((mask & kDeviceTestC3) == 0U) || spi_c3_is_connected();
  result->wifi_connected = wifi.sta_connected;
  result->wifi_ap_active = wifi.ap_active;
  result->pca_ready = ((mask & kDeviceTestPca) == 0U) || pca9685_is_ready();
  result->espnow_active = spi_c3_is_espnow_active();
  result->espnow_connected = spi_c3_is_espnow_connected();
  result->ws_clients = web_server_total_ws_client_count();
  result->t41_qspi_txn = spi_s3_get_total_transactions();
  result->t41_qspi_crc_errors = spi_s3_get_crc_errors();
  result->c3_total_rx = spi_c3_get_total_rx();
  result->c3_crc_errors = spi_c3_get_crc_errors();

  bool ok = true;
  if ((mask & kDeviceTestT41) != 0U) ok = ok && result->t41_connected;
  if ((mask & kDeviceTestC3) != 0U) ok = ok && result->c3_connected;
  if ((mask & kDeviceTestWifi) != 0U) ok = ok && (result->wifi_connected || result->wifi_ap_active);
  if ((mask & kDeviceTestPca) != 0U) ok = ok && result->pca_ready;
  result->ok = ok;
  copy_text(result->message, sizeof(result->message),
            ok ? "device test complete" : "device test found warnings");
}

void execute_item(const QueueItem& item) {
  WorkerResult local {};
  WorkerResult* result = item.result != nullptr ? item.result : &local;
  fill_common_result(result, item.request);

  const int64_t start_us = esp_timer_get_time();
  portENTER_CRITICAL(&g_lock);
  g_active = true;
  g_cancel = false;
  ++g_wake_count;
  result->seq = ++g_seq;
  portEXIT_CRITICAL(&g_lock);

  switch (item.request.type) {
    case WorkerJobType::Fk:
      run_fk(item.request, result);
      break;
    case WorkerJobType::Ik:
      run_ik(item.request, result);
      break;
    case WorkerJobType::Trajectory:
      run_trajectory(item.request, result);
      break;
    case WorkerJobType::DeviceTest:
      run_device_test(item.request, result);
      break;
    default:
      result->ok = false;
      copy_text(result->message, sizeof(result->message), "unsupported worker job");
      break;
  }

  const int64_t end_us = esp_timer_get_time();
  result->duration_us = saturating_duration_us(start_us, end_us);

  portENTER_CRITICAL(&g_lock);
  record_stat_locked(item.request, *result);
  g_active = false;
  portEXIT_CRITICAL(&g_lock);
}

void append_last3_json(mros::utils::FixedJsonWriter& writer) {
  WorkerOpStat stats[kLastOpCount] = {};
  portENTER_CRITICAL(&g_lock);
  for (uint8_t i = 0; i < kLastOpCount; ++i) stats[i] = g_last3[i];
  portEXIT_CRITICAL(&g_lock);

  writer.append_raw("[");
  for (uint8_t i = 0; i < kLastOpCount; ++i) {
    if (i > 0U) writer.append_raw(",");
    writer.append_raw("{\"seq\":");
    writer.u32(stats[i].seq);
    writer.append_raw(",\"op\":\"");
    writer.append_escaped(stats[i].op);
    writer.append_raw("\",\"source\":\"");
    writer.append_escaped(stats[i].source);
    writer.append_raw("\",\"ok\":");
    writer.append_raw(stats[i].ok ? "true" : "false");
    writer.append_raw(",\"duration_us\":");
    writer.u32(stats[i].duration_us);
    writer.append_raw(",\"ended_ms\":");
    writer.u32(stats[i].ended_ms);
    writer.append_raw("}");
  }
  writer.append_raw("]");
}

}  // namespace

const char* worker_job_name(const WorkerJobType type) {
  switch (type) {
    case WorkerJobType::Fk:
      return "fk";
    case WorkerJobType::Ik:
      return "ik";
    case WorkerJobType::Trajectory:
      return "trajectory";
    case WorkerJobType::DeviceTest:
      return "device-test";
    default:
      return "none";
  }
}

void worker_init() {
  if (g_queue == nullptr) {
    g_queue = xQueueCreate(1, sizeof(QueueItem));
  }
  const bool enabled = load_enabled_from_nvs();
  portENTER_CRITICAL(&g_lock);
  g_enabled = enabled;
  g_psram_result = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0U;
  portEXIT_CRITICAL(&g_lock);
}

void worker_set_task_handle(TaskHandle_t task_handle) { g_task_handle = task_handle; }

void worker_task_loop() {
  if (g_queue == nullptr) {
    g_queue = xQueueCreate(1, sizeof(QueueItem));
  }
  while (true) {
    QueueItem item {};
    if (xQueueReceive(g_queue, &item, portMAX_DELAY) == pdTRUE) {
      execute_item(item);
      if (item.done != nullptr) {
        xSemaphoreGive(item.done);
      }
    }
  }
}

bool worker_is_enabled() {
  portENTER_CRITICAL(&g_lock);
  const bool enabled = g_enabled;
  portEXIT_CRITICAL(&g_lock);
  return enabled;
}

bool worker_set_enabled(const bool enabled) {
  const bool saved = save_enabled_to_nvs(enabled);
  portENTER_CRITICAL(&g_lock);
  g_enabled = enabled;
  portEXIT_CRITICAL(&g_lock);
  return saved;
}

void worker_clear_stats() {
  portENTER_CRITICAL(&g_lock);
  std::memset(g_last3, 0, sizeof(g_last3));
  g_rejected_disabled = 0;
  g_rejected_busy = 0;
  portEXIT_CRITICAL(&g_lock);
}

void worker_cancel() {
  portENTER_CRITICAL(&g_lock);
  g_cancel = true;
  portEXIT_CRITICAL(&g_lock);
}

void worker_get_snapshot(WorkerSnapshot* out) {
  if (out == nullptr) return;
  const bool queue_ready = g_queue != nullptr;
  const bool queue_has_pending = queue_ready && uxQueueMessagesWaiting(g_queue) > 0U;
  portENTER_CRITICAL(&g_lock);
  out->enabled = g_enabled;
  out->active = g_active;
  out->busy = queue_has_pending || g_active;
  out->cancel_requested = g_cancel;
  out->psram_result = g_psram_result;
  out->queue_ready = queue_ready;
  out->seq = g_seq;
  out->wake_count = g_wake_count;
  out->rejected_disabled = g_rejected_disabled;
  out->rejected_busy = g_rejected_busy;
  for (uint8_t i = 0; i < kLastOpCount; ++i) out->last3[i] = g_last3[i];
  portEXIT_CRITICAL(&g_lock);
}

String worker_last3_json() {
  char buffer[384] = {};
  mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
  append_last3_json(writer);
  return writer.overflow() ? String("[]") : String(writer.c_str());
}

String worker_status_json() {
  WorkerSnapshot snapshot {};
  worker_get_snapshot(&snapshot);
  char buffer[768] = {};
  mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
  writer.begin();
  writer.bool_field("enabled", snapshot.enabled);
  writer.bool_field("active", snapshot.active);
  writer.bool_field("busy", snapshot.busy);
  writer.bool_field("cancel_requested", snapshot.cancel_requested);
  writer.bool_field("worker_awake", snapshot.active);
  writer.bool_field("queue_ready", snapshot.queue_ready);
  writer.bool_field("psram_result", snapshot.psram_result);
  writer.string_field("backend", "onboard-s3");
  writer.u32_field("seq", snapshot.seq);
  writer.u32_field("wake_count", snapshot.wake_count);
  writer.u32_field("rejected_disabled", snapshot.rejected_disabled);
  writer.u32_field("rejected_busy", snapshot.rejected_busy);
  writer.raw_field("last3", "");
  append_last3_json(writer);
  writer.end();
  return writer.overflow() ? String("{\"error\":\"WORKER_STATUS_JSON_OVERFLOW\"}") : String(writer.c_str());
}

String worker_result_json(const WorkerResult& result) {
  char buffer[1536] = {};
  mros::utils::FixedJsonWriter writer(buffer, sizeof(buffer));
  writer.begin();
  writer.bool_field("accepted", result.accepted);
  writer.bool_field("ok", result.ok);
  writer.bool_field("disabled", result.disabled);
  writer.bool_field("busy", result.busy);
  writer.bool_field("timed_out", result.timed_out);
  writer.string_field("backend", "onboard-s3");
  writer.string_field("op", worker_job_name(result.type));
  writer.u32_field("seq", result.seq);
  writer.u32_field("duration_us", result.duration_us);
  writer.bool_field("psram_result", result.psram_result);
  writer.string_field("model_revision", mros::kinematics::kRobotModelRevision);
  writer.string_field("message", result.message);
  writer.append_raw(",\"fk\":{\"x\":");
  char num[32] = {};
  std::snprintf(num, sizeof(num), "%.3f", static_cast<double>(result.fk_x));
  writer.append_raw(num);
  writer.append_raw(",\"y\":");
  std::snprintf(num, sizeof(num), "%.3f", static_cast<double>(result.fk_y));
  writer.append_raw(num);
  writer.append_raw(",\"z\":");
  std::snprintf(num, sizeof(num), "%.3f", static_cast<double>(result.fk_z));
  writer.append_raw(num);
  writer.append_raw(",\"alpha\":");
  std::snprintf(num, sizeof(num), "%.3f", static_cast<double>(result.fk_alpha_deg));
  writer.append_raw(num);
  writer.append_raw("},\"ik\":{\"sigma_min\":");
  std::snprintf(num, sizeof(num), "%.6f", static_cast<double>(result.ik_sigma_min));
  writer.append_raw(num);
  writer.append_raw(",\"pos_err_mm\":");
  std::snprintf(num, sizeof(num), "%.3f", static_cast<double>(result.ik_pos_err_mm));
  writer.append_raw(num);
  writer.append_raw(",\"iterations\":");
  writer.u32(result.ik_iterations);
  writer.append_raw(",\"warnings\":");
  writer.u32(result.ik_warnings);
  writer.append_raw(",\"joints_deg\":[");
  for (uint8_t i = 0; i < 7U; ++i) {
    if (i > 0U) writer.append_raw(",");
    std::snprintf(num, sizeof(num), "%.3f", static_cast<double>(result.ik_joints_deg[i]));
    writer.append_raw(num);
  }
  writer.append_raw("]},\"devices\":{\"t41_connected\":");
  writer.append_raw(result.t41_connected ? "true" : "false");
  writer.append_raw(",\"c3_connected\":");
  writer.append_raw(result.c3_connected ? "true" : "false");
  writer.append_raw(",\"wifi_connected\":");
  writer.append_raw(result.wifi_connected ? "true" : "false");
  writer.append_raw(",\"wifi_ap_active\":");
  writer.append_raw(result.wifi_ap_active ? "true" : "false");
  writer.append_raw(",\"pca_ready\":");
  writer.append_raw(result.pca_ready ? "true" : "false");
  writer.append_raw(",\"espnow_active\":");
  writer.append_raw(result.espnow_active ? "true" : "false");
  writer.append_raw(",\"espnow_connected\":");
  writer.append_raw(result.espnow_connected ? "true" : "false");
  writer.append_raw(",\"ws_clients\":");
  writer.u32(result.ws_clients);
  writer.append_raw(",\"t41_qspi_txn\":");
  writer.u32(result.t41_qspi_txn);
  writer.append_raw(",\"t41_qspi_crc_errors\":");
  writer.u32(result.t41_qspi_crc_errors);
  writer.append_raw(",\"c3_total_rx\":");
  writer.u32(result.c3_total_rx);
  writer.append_raw(",\"c3_crc_errors\":");
  writer.u32(result.c3_crc_errors);
  writer.append_raw("},\"last3\":");
  append_last3_json(writer);
  writer.end();
  return writer.overflow() ? String("{\"error\":\"WORKER_RESULT_JSON_OVERFLOW\"}") : String(writer.c_str());
}

bool worker_submit_sync(const WorkerRequest& request, const uint32_t timeout_ms,
                        WorkerResult* out) {
  if (out == nullptr) return false;
  *out = WorkerResult {};
  out->type = request.type;
  if (!worker_is_enabled() && request.type != WorkerJobType::DeviceTest) {
    out->disabled = true;
    copy_text(out->message, sizeof(out->message), "experimental worker disabled");
    portENTER_CRITICAL(&g_lock);
    ++g_rejected_disabled;
    portEXIT_CRITICAL(&g_lock);
    return false;
  }
  if (g_queue == nullptr) {
    out->busy = true;
    copy_text(out->message, sizeof(out->message), "experimental worker queue not ready");
    return false;
  }

  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (done == nullptr) {
    out->busy = true;
    copy_text(out->message, sizeof(out->message), "experimental worker semaphore allocation failed");
    return false;
  }

  QueueItem item {};
  item.request = request;
  item.result = out;
  item.done = done;
  const BaseType_t sent = xQueueSend(g_queue, &item, 0);
  if (sent != pdTRUE) {
    out->busy = true;
    copy_text(out->message, sizeof(out->message), "experimental worker busy");
    portENTER_CRITICAL(&g_lock);
    ++g_rejected_busy;
    portEXIT_CRITICAL(&g_lock);
    vSemaphoreDelete(done);
    return false;
  }
  if (g_task_handle != nullptr) {
    xTaskNotifyGive(g_task_handle);
  }

  const TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms > 0U ? timeout_ms : kDefaultTimeoutMs);
  if (xSemaphoreTake(done, wait_ticks) != pdTRUE) {
    (void)xSemaphoreTake(done, portMAX_DELAY);
    out->timed_out = true;
    if (out->ok) {
      copy_text(out->message, sizeof(out->message), "experimental worker completed after timeout budget");
    } else {
      copy_text(out->message, sizeof(out->message), "experimental worker timed out");
    }
    vSemaphoreDelete(done);
    return false;
  }
  vSemaphoreDelete(done);
  return out->ok;
}

}  // namespace mros::experimental
