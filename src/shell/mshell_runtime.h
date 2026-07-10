#pragma once

#include <cstddef>
#include <cstdint>

#include "src/shell/mros_shell.h"

namespace mros::shell::runtime {

enum class JobState : uint8_t {
  Empty = 0,
  Queued,
  Running,
  Done,
  Failed,
  Cancelled,
};

bool job_start(
    const char* command,
    uint32_t capability_mask,
    const char* owner,
    uint32_t* job_id,
    char* error,
    size_t error_size);

bool job_cancel(uint32_t job_id, char* error, size_t error_size);
bool jobs_json(char* out, size_t out_size);
bool job_log_json(uint32_t job_id, char* out, size_t out_size);
uint32_t job_active_count();
uint32_t job_capacity();
uint32_t job_completed_count();
uint32_t job_drop_count();
uint32_t job_storage_bytes();
bool job_storage_allocated();
bool job_storage_uses_psram();

bool tx_begin(const char* owner, uint32_t* tx_id, char* error, size_t error_size);
bool tx_stage(const char* note, char* error, size_t error_size);
bool tx_commit(char* error, size_t error_size);
bool tx_rollback(char* error, size_t error_size);
bool tx_json(char* out, size_t out_size);
bool tx_active();

const char* job_state_name(JobState state);

}  // namespace mros::shell::runtime
