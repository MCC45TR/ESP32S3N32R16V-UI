#include "src/shell/mshell_runtime.h"

#include "src/shell/mros_shell_internal.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "src/platform/mros_time.h"

namespace mros::shell::runtime {
namespace {

constexpr size_t kJobSlots = 4U;
constexpr size_t kJobCommandBytes = 256U;
constexpr size_t kJobOutputBytes = 3072U;
constexpr size_t kJobErrorBytes = 128U;
constexpr size_t kJobOwnerBytes = 32U;
constexpr size_t kTxNoteBytes = 192U;

struct JobRecord {
  uint32_t id = 0U;
  JobState state = JobState::Empty;
  uint32_t capability_mask = kShellCapabilityUserDefault;
  uint32_t started_ms = 0U;
  uint32_t ended_ms = 0U;
  uint32_t duration_ms = 0U;
  bool cancel_requested = false;
  bool truncated = false;
  char owner[kJobOwnerBytes] = {};
  char command[kJobCommandBytes] = {};
  char output[kJobOutputBytes] = {};
  char error[kJobErrorBytes] = {};
};

struct TxRecord {
  bool active = false;
  uint32_t id = 0U;
  uint32_t started_ms = 0U;
  uint32_t staged_count = 0U;
  char owner[kJobOwnerBytes] = {};
  char note[kTxNoteBytes] = {};
};

SemaphoreHandle_t g_lock = nullptr;
JobRecord* g_jobs = nullptr;
bool g_jobs_in_psram = false;
TxRecord g_tx {};
uint32_t g_next_job_id = 1U;
uint32_t g_next_tx_id = 1U;
uint32_t g_completed_count = 0U;
uint32_t g_drop_count = 0U;

bool ensure_lock() {
  if (g_lock != nullptr) {
    return true;
  }
  g_lock = xSemaphoreCreateMutex();
  return g_lock != nullptr;
}

bool ensure_jobs_storage_locked() {
  if (g_jobs != nullptr) {
    return true;
  }
  const size_t bytes = sizeof(JobRecord) * kJobSlots;
  void* storage = heap_caps_calloc(kJobSlots, sizeof(JobRecord),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (storage != nullptr) {
    g_jobs = static_cast<JobRecord*>(storage);
    g_jobs_in_psram = true;
    return true;
  }
  storage = heap_caps_calloc(kJobSlots, sizeof(JobRecord),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (storage != nullptr) {
    g_jobs = static_cast<JobRecord*>(storage);
    g_jobs_in_psram = false;
    return true;
  }
  (void)bytes;
  return false;
}

struct LockGuard {
  bool locked = false;
  explicit LockGuard(const bool require_jobs = false) {
    locked = ensure_lock() && xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) == pdTRUE;
    if (locked && require_jobs && !ensure_jobs_storage_locked()) {
      xSemaphoreGive(g_lock);
      locked = false;
    }
  }
  ~LockGuard() {
    if (locked) xSemaphoreGive(g_lock);
  }
};

void copy_text(char* dst, const size_t dst_size, const char* src) {
  if (dst == nullptr || dst_size == 0U) return;
  std::snprintf(dst, dst_size, "%s", src != nullptr ? src : "");
}

void set_error(char* error, const size_t error_size, const char* text) {
  copy_text(error, error_size, text);
}

std::string json_escape(const char* text) {
  std::string out;
  if (text == nullptr) return out;
  for (const char ch : std::string(text)) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          char tmp[8] = {};
          std::snprintf(tmp, sizeof(tmp), "\\u%04x", static_cast<unsigned char>(ch));
          out += tmp;
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

JobRecord* find_job_locked(const uint32_t job_id) {
  if (g_jobs == nullptr) {
    return nullptr;
  }
  for (size_t i = 0; i < kJobSlots; ++i) {
    JobRecord& job = g_jobs[i];
    if (job.state != JobState::Empty && job.id == job_id) return &job;
  }
  return nullptr;
}

JobRecord* allocate_job_locked() {
  if (g_jobs == nullptr) {
    return nullptr;
  }
  JobRecord* reusable = nullptr;
  uint32_t oldest_end = UINT32_MAX;
  for (size_t i = 0; i < kJobSlots; ++i) {
    JobRecord& job = g_jobs[i];
    if (job.state == JobState::Empty) return &job;
    if (job.state == JobState::Done || job.state == JobState::Failed || job.state == JobState::Cancelled) {
      if (job.ended_ms <= oldest_end) {
        oldest_end = job.ended_ms;
        reusable = &job;
      }
    }
  }
  return reusable;
}

void mark_job_finished(
    JobRecord* job,
    const JobState state,
    const char* output,
    const char* error,
    const uint32_t started_ms) {
  if (job == nullptr) return;
  LockGuard guard(false);
  if (!guard.locked) return;
  job->state = job->cancel_requested ? JobState::Cancelled : state;
  job->ended_ms = mros::platform::mros_millis();
  job->duration_ms = job->ended_ms - started_ms;
  const std::string out = output != nullptr ? output : "";
  job->truncated = out.size() >= (kJobOutputBytes - 1U);
  copy_text(job->output, sizeof(job->output), out.c_str());
  copy_text(job->error, sizeof(job->error), error != nullptr ? error : "");
  g_completed_count++;
}

void job_task(void* arg) {
  JobRecord* job = static_cast<JobRecord*>(arg);
  if (job == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  char command[kJobCommandBytes] = {};
  uint32_t started = 0U;
  {
    LockGuard guard;
    if (guard.locked) {
      job->state = JobState::Running;
      job->started_ms = mros::platform::mros_millis();
      started = job->started_ms;
      copy_text(command, sizeof(command), job->command);
    }
  }

  std::string output;
  std::string error;
  const bool ok = execute_line_capture(command, &output, false, ShellTransport::System);

  mark_job_finished(job, ok ? JobState::Done : JobState::Failed, output.c_str(), error.c_str(), started);
  vTaskDelete(nullptr);
}

}  // namespace

const char* job_state_name(const JobState state) {
  switch (state) {
    case JobState::Queued: return "queued";
    case JobState::Running: return "running";
    case JobState::Done: return "done";
    case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
    case JobState::Empty:
    default: return "empty";
  }
}

bool job_start(
    const char* command,
    const uint32_t capability_mask,
    const char* owner,
    uint32_t* job_id,
    char* error,
    const size_t error_size) {
  if (command == nullptr || command[0] == '\0') {
    set_error(error, error_size, "INVALID_ARGUMENT");
    return false;
  }

  JobRecord* job = nullptr;
  {
    LockGuard guard(true);
    if (!guard.locked) {
      set_error(error, error_size, "INTERNAL_ERROR");
      return false;
    }
    job = allocate_job_locked();
    if (job == nullptr || job->state == JobState::Queued || job->state == JobState::Running) {
      g_drop_count++;
      set_error(error, error_size, "BUSY");
      return false;
    }
    *job = JobRecord();
    job->id = g_next_job_id++;
    if (job->id == 0U) job->id = g_next_job_id++;
    job->state = JobState::Queued;
    job->capability_mask = capability_mask;
    copy_text(job->owner, sizeof(job->owner), owner != nullptr ? owner : "shell");
    copy_text(job->command, sizeof(job->command), command);
    if (job_id != nullptr) *job_id = job->id;
  }

  audit_record("job-start", command);
  BaseType_t task_ok = xTaskCreate(job_task, "mshell_job", 8192, job, tskIDLE_PRIORITY + 1, nullptr);
  if (task_ok != pdPASS) {
    LockGuard guard;
    if (guard.locked && job != nullptr) {
      job->state = JobState::Failed;
      copy_text(job->error, sizeof(job->error), "TASK_CREATE_FAILED");
      job->ended_ms = mros::platform::mros_millis();
    }
    set_error(error, error_size, "TASK_CREATE_FAILED");
    return false;
  }
  set_error(error, error_size, "OK");
  return true;
}

bool job_cancel(const uint32_t job_id, char* error, const size_t error_size) {
  LockGuard guard(false);
  if (!guard.locked) {
    set_error(error, error_size, "INTERNAL_ERROR");
    return false;
  }
  JobRecord* job = find_job_locked(job_id);
  if (job == nullptr) {
    set_error(error, error_size, "NOT_FOUND");
    return false;
  }
  job->cancel_requested = true;
  if (job->state == JobState::Queued) {
    job->state = JobState::Cancelled;
    job->ended_ms = mros::platform::mros_millis();
  }
  set_error(error, error_size, "OK");
  audit_record("job-cancel", job->command);
  return true;
}

bool jobs_json(char* out, const size_t out_size) {
  if (out == nullptr || out_size == 0U) return false;
  std::string json = "{\"ok\":true,\"capacity\":";
  json += std::to_string(kJobSlots);
  json += ",\"active\":";
  json += std::to_string(job_active_count());
  json += ",\"completed\":";
  json += std::to_string(g_completed_count);
  json += ",\"drops\":";
  json += std::to_string(g_drop_count);
  json += ",\"storage_bytes\":";
  json += std::to_string(job_storage_bytes());
  json += ",\"storage_psram\":";
  json += job_storage_uses_psram() ? "true" : "false";
  json += ",\"storage_allocated\":";
  json += (g_jobs != nullptr) ? "true" : "false";
  json += ",\"jobs\":[";
  {
    LockGuard guard(false);
    if (!guard.locked) return false;
    bool first = true;
    if (g_jobs != nullptr) {
      for (size_t i = 0; i < kJobSlots; ++i) {
        const JobRecord& job = g_jobs[i];
        if (job.state == JobState::Empty) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"id\":";
        json += std::to_string(job.id);
        json += ",\"state\":\"";
        json += job_state_name(job.state);
        json += "\",\"owner\":\"";
        json += json_escape(job.owner);
        json += "\",\"command\":\"";
        json += json_escape(job.command);
        json += "\",\"duration_ms\":";
        json += std::to_string(job.duration_ms);
        json += ",\"truncated\":";
        json += job.truncated ? "true" : "false";
        json += "}";
      }
    }
  }
  json += "]}";
  if (json.size() >= out_size) return false;
  copy_text(out, out_size, json.c_str());
  return true;
}

bool job_log_json(const uint32_t job_id, char* out, const size_t out_size) {
  if (out == nullptr || out_size == 0U) return false;
  LockGuard guard(false);
  if (!guard.locked) return false;
  JobRecord* job = find_job_locked(job_id);
  if (job == nullptr) {
    copy_text(out, out_size, "{\"ok\":false,\"error_code\":\"NOT_FOUND\"}");
    return true;
  }
  std::string json = "{\"ok\":true,\"id\":";
  json += std::to_string(job->id);
  json += ",\"state\":\"";
  json += job_state_name(job->state);
  json += "\",\"truncated\":";
  json += job->truncated ? "true" : "false";
  json += ",\"output\":\"";
  json += json_escape(job->output);
  json += "\",\"error\":\"";
  json += json_escape(job->error);
  json += "\"}";
  if (json.size() >= out_size) return false;
  copy_text(out, out_size, json.c_str());
  return true;
}

uint32_t job_active_count() {
  LockGuard guard(false);
  if (!guard.locked) return 0U;
  uint32_t count = 0U;
  if (g_jobs != nullptr) {
    for (size_t i = 0; i < kJobSlots; ++i) {
      const JobRecord& job = g_jobs[i];
      if (job.state == JobState::Queued || job.state == JobState::Running) ++count;
    }
  }
  return count;
}

uint32_t job_capacity() { return static_cast<uint32_t>(kJobSlots); }
uint32_t job_completed_count() { return g_completed_count; }
uint32_t job_drop_count() { return g_drop_count; }
uint32_t job_storage_bytes() {
  return static_cast<uint32_t>(sizeof(JobRecord) * kJobSlots);
}
bool job_storage_allocated() { return g_jobs != nullptr; }
bool job_storage_uses_psram() { return g_jobs != nullptr && g_jobs_in_psram; }

bool tx_begin(const char* owner, uint32_t* tx_id, char* error, const size_t error_size) {
  LockGuard guard(false);
  if (!guard.locked) {
    set_error(error, error_size, "INTERNAL_ERROR");
    return false;
  }
  if (g_tx.active) {
    set_error(error, error_size, "BUSY");
    return false;
  }
  g_tx = TxRecord();
  g_tx.active = true;
  g_tx.id = g_next_tx_id++;
  if (g_tx.id == 0U) g_tx.id = g_next_tx_id++;
  g_tx.started_ms = mros::platform::mros_millis();
  copy_text(g_tx.owner, sizeof(g_tx.owner), owner != nullptr ? owner : "shell");
  if (tx_id != nullptr) *tx_id = g_tx.id;
  set_error(error, error_size, "OK");
  audit_record("tx-begin", g_tx.owner);
  return true;
}

bool tx_stage(const char* note, char* error, const size_t error_size) {
  LockGuard guard(false);
  if (!guard.locked || !g_tx.active) {
    set_error(error, error_size, "NO_ACTIVE_TX");
    return false;
  }
  g_tx.staged_count++;
  copy_text(g_tx.note, sizeof(g_tx.note), note != nullptr ? note : "");
  set_error(error, error_size, "OK");
  return true;
}

bool tx_commit(char* error, const size_t error_size) {
  LockGuard guard(false);
  if (!guard.locked || !g_tx.active) {
    set_error(error, error_size, "NO_ACTIVE_TX");
    return false;
  }
  audit_record("tx-commit", g_tx.owner);
  g_tx = TxRecord();
  set_error(error, error_size, "OK");
  return true;
}

bool tx_rollback(char* error, const size_t error_size) {
  LockGuard guard(false);
  if (!guard.locked || !g_tx.active) {
    set_error(error, error_size, "NO_ACTIVE_TX");
    return false;
  }
  audit_record("tx-rollback", g_tx.owner);
  g_tx = TxRecord();
  set_error(error, error_size, "OK");
  return true;
}

bool tx_json(char* out, const size_t out_size) {
  if (out == nullptr || out_size == 0U) return false;
  LockGuard guard(false);
  if (!guard.locked) return false;
  std::string json = "{\"ok\":true,\"active\":";
  json += g_tx.active ? "true" : "false";
  json += ",\"id\":";
  json += std::to_string(g_tx.active ? g_tx.id : 0U);
  json += ",\"owner\":\"";
  json += json_escape(g_tx.owner);
  json += "\",\"staged_count\":";
  json += std::to_string(g_tx.staged_count);
  json += ",\"last_note\":\"";
  json += json_escape(g_tx.note);
  json += "\"}";
  if (json.size() >= out_size) return false;
  copy_text(out, out_size, json.c_str());
  return true;
}

bool tx_active() {
  LockGuard guard(false);
  return guard.locked && g_tx.active;
}

}  // namespace mros::shell::runtime
