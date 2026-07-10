#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// SPI Master driver to read encoder data from ESP32-C3
// Uses SPI3_HOST with DMA on ESP32-S3

// Failsafe option values sent from S3 -> C3 over the validated 16-byte command frame.
enum C3FailsafeOption : int8_t {
  C3_FAILSAFE_T41_QSPI = 0,              // Main: IK on t41, transfer via t41-S3 SPI
  C3_FAILSAFE_C3SPI_T41_ESPNOW = 1,     // Alternate: C3-S3 SPI + t41-C3 ESP-NOW
  C3_FAILSAFE_WEB = 2,                 // Alternate: solve on WEB side
  C3_FAILSAFE_RESERVED = 3             // Reserved for future option
};

// C3 command flags in the validated S3 -> C3 command frame.
enum C3CommandFlag : uint8_t {
  C3_CMD_FLAG_NONE = 0U,
  C3_CMD_FLAG_FK_REQUEST = 1U << 0,
  C3_CMD_FLAG_IK_REQUEST = 1U << 1,
  C3_CMD_FLAG_TRAJECTORY_REQUEST = 1U << 2
};

enum C3StatusFlag : uint8_t {
  C3_STATUS_ESPNOW_ACTIVE = 1U << 0,
  C3_STATUS_ESPNOW_CONNECTED = 1U << 1,
  C3_STATUS_ESPNOW_RECENT_CMD = 1U << 2
};

typedef struct {
  uint32_t effective_period_ms;
  uint32_t transactions_queued;
  uint32_t transactions_completed;
  uint32_t transaction_timeout_count;
  uint32_t transaction_fail_count;
  uint32_t crc_error_count;
  uint32_t marker_error_count;
  uint32_t total_rx_count;
  uint32_t last_good_rx_ms;
  bool connected;
  bool alive_ok;
  bool link_synced;
} C3SpiDiagSnapshot;

void spi_c3_master_init();
void spi_c3_set_task_handle(TaskHandle_t task_handle);
void spi_c3_service_cycle(unsigned long now_ms);
void spi_c3_get_diag_snapshot(C3SpiDiagSnapshot *snapshot);
uint32_t spi_c3_get_effective_period_ms();

// Service C3 SPI state machine (call from main loop)
void spi_c3_master_poll();
void spi_c3_request_encoder_reset();
void spi_c3_set_failsafe_option(int8_t option);
int8_t spi_c3_get_failsafe_option();
void spi_c3_set_cmd_flags(uint8_t flags);
uint8_t spi_c3_get_cmd_flags();
void spi_c3_set_pid_setpoint_x100(int32_t setpoint_x100);
int32_t spi_c3_get_pid_setpoint_x100();

// Get latest decoded encoder values (fixed-point -> float)
float spi_c3_get_position_deg();
float spi_c3_get_speed_deg_s();
float spi_c3_get_accel_deg_s2();
uint8_t spi_c3_get_loop_hz();

// Connection status
bool spi_c3_is_connected();     // Valid C3 packet seen recently (1s timeout)
bool spi_c3_is_espnow_active();
bool spi_c3_is_espnow_connected();
bool spi_c3_has_recent_espnow_cmd();
uint32_t spi_c3_get_crc_errors();
uint32_t spi_c3_get_marker_errors();
uint32_t spi_c3_get_total_rx();
uint32_t spi_c3_get_last_good_rx_ms();
void spi_c3_reset_error_counters();
