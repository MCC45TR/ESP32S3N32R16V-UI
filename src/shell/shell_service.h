#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace mros::shell::service {

enum class WebRequestType : uint8_t {
  State = 0,
  Execute,
  Complete,
  Suggest,
  Resize,
};

struct ShellServiceMetrics {
  uint32_t response_pool_miss = 0;
  uint32_t response_pool_active = 0;
  uint32_t response_pool_capacity = 0;
  uint32_t response_pool_allocated = 0;
  uint32_t response_fallback_alloc = 0;
  uint32_t response_drop = 0;
  uint32_t stream_chunk_count = 0;
  uint32_t stream_byte_count = 0;
  uint32_t stream_drop_count = 0;
  uint32_t final_truncation_count = 0;
  uint32_t stream_final_suppressed = 0;
  uint32_t max_response_bytes = 0;
  uint32_t shell_bin_frames = 0;
  uint32_t shell_bin_bytes = 0;
  uint32_t shell_json_frames = 0;
  uint32_t shell_json_bytes = 0;
  uint32_t shell_bin_decode_errors = 0;
  uint32_t shell_json_fallbacks = 0;
  uint32_t shell_ack_rx = 0;
  uint32_t shell_credit_rx = 0;
  uint32_t shell_credit_exhausted = 0;
  uint32_t request_queue_depth = 0;
  uint32_t request_queue_capacity = 0;
  uint32_t response_queue_depth = 0;
  uint32_t response_queue_capacity = 0;
};

enum class ShellWebOutboundKind : uint8_t {
  TextJson = 0,
  BinaryFrame = 1,
};

void init();
void set_task_handle(TaskHandle_t task_handle);
void notify_task();
void process_pending_requests();

bool enqueue_web_request(
    WebRequestType type,
    uint32_t client_id,
    const char* payload,
    char* error,
    size_t error_size,
    uint8_t pane_id = 0U,
    uint16_t command_id = 0U);

bool dequeue_web_response(uint32_t* client_id, char* json, size_t json_size);
bool dequeue_web_outbound(uint32_t* client_id,
                          ShellWebOutboundKind* kind,
                          uint8_t* payload,
                          size_t payload_size,
                          size_t* payload_len);
void get_metrics(ShellServiceMetrics* out_metrics);
void close_client_sessions(uint32_t client_id);
bool close_session_id(uint32_t session_id);
bool sessions_json(char* json, size_t json_size);
void set_client_user(uint32_t client_id, const char* username);
void set_client_binary_stream(uint32_t client_id, bool enabled);
void note_client_ack_credit(uint32_t client_id, uint32_t ack, uint32_t credit);
void note_client_binary_decode_error(uint32_t client_id);
void note_client_json_fallback(uint32_t client_id);

}  // namespace mros::shell::service
