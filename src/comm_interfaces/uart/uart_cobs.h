#pragma once

#include <stdint.h>
#include "WString.h"
#include <driver/uart.h>
#include <freertos/queue.h>

typedef struct {
  size_t log_size;
  size_t log_capacity;
  size_t base_offset;
  size_t total_written;
  uint32_t revision;
  uint32_t append_count;
  uint32_t full_copy_count;
  uint32_t since_copy_count;
  uint32_t snapshot_string_count;
  uint32_t lock_fail_count;
  bool storage_allocated;
  bool storage_static;
  bool storage_init_failed;
} UartLogDiagSnapshot;

enum class UartLogNoiseMode : uint8_t {
  Quiet = 0,
  Normal = 1,
  Verbose = 2,
};

void uart1_cobs_init();
bool uart1_cobs_install_driver(QueueHandle_t* out_event_queue);
void uart1_cobs_handle_uart_event(const uart_event_t& event, unsigned long now_ms);
void uart1_cobs_periodic(unsigned long now_ms);
void uart1_cobs_loop(unsigned long now);
void uart1_cobs_get_diag_snapshot(UartLogDiagSnapshot* snapshot);
void uart1_cobs_set_log_noise_mode(UartLogNoiseMode mode);
UartLogNoiseMode uart1_cobs_get_log_noise_mode();
const char* uart1_cobs_log_noise_mode_name(UartLogNoiseMode mode);
uint32_t uart1_cobs_get_log_version();
size_t uart1_cobs_get_system_logs_size();
size_t uart1_cobs_get_system_logs_base_offset();
bool uart1_cobs_copy_system_logs(char* dst, size_t capacity, size_t* out_len = nullptr);
bool uart1_cobs_copy_system_logs_since(char* dst,
                                       size_t capacity,
                                       size_t offset,
                                       size_t max_bytes,
                                       size_t* out_len = nullptr,
                                       size_t* out_next_offset = nullptr,
                                       size_t* out_base_offset = nullptr,
                                       bool* out_truncated = nullptr);
String uart1_cobs_get_system_logs_snapshot();
void uart1_cobs_clear_system_logs();
bool uart1_cobs_send_text_line(const char* text);
bool uart1_cobs_send_binary_frame(const uint8_t* data, size_t len);

void appendSystemLog(const char* source, String line);
