#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "WString.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

enum class SpiS3EndpointMode : uint8_t {
  ClassicSpi1Bit = 0,
  QuadReady4Bit = 1,
};

void spi_slave_s3_init();
void spi_slave_s3_loop(unsigned long now);
void spi_s3_set_notify_task(TaskHandle_t task_handle);
void spi_s3_set_joint_task_handle(TaskHandle_t task_handle);
void spi_s3_service_comm(unsigned long now);
void spi_s3_service_joint_traj(unsigned long now);
void spi_s3_service_turret_pid(unsigned long now);
bool spi_s3_joint_traj_is_active();

// Used by Web API to write Teensy 4.1 command frames
void spi_s3_set_target_turret(float deg);
void spi_s3_set_target_joint(int index, float deg);
void spi_s3_set_target_gripper(float deg);
void spi_s3_set_target_cartesian(float x_mm, float y_mm, float z_mm,
                                 float roll_deg, float pitch_deg,
                                 float yaw_deg, bool use_orientation);
void spi_s3_set_joint_traj_time_scale(float scale);
float spi_s3_get_joint_traj_time_scale();
void spi_s3_set_motor_power(uint8_t power);
void spi_s3_set_reset_encoder();
void spi_s3_set_turret_pid(float kp, float ki, float kd, float i_limit);
void spi_s3_set_turret_dspc(float dspc);
void spi_s3_set_turret_output_lock(bool lock);
bool spi_s3_get_turret_output_lock();

// Used by Web API to read Teensy 4.1 telemetry state
float spi_s3_get_turret_deg();
float spi_s3_get_joint_deg(int index);
uint8_t spi_s3_get_gripper();
uint8_t spi_s3_get_motor_state();
uint16_t spi_s3_get_error_code();
uint16_t spi_s3_get_loop_ms();
int16_t spi_s3_get_device_status_code();

bool spi_s3_is_connected();

// FK End-Effector Coordinates (from Teensy 4.1 snapshot)
float spi_s3_get_coord_x();
float spi_s3_get_coord_y();
float spi_s3_get_coord_z();
float spi_s3_get_alpha();
float spi_s3_get_coord_roll();
float spi_s3_get_coord_pitch();
float spi_s3_get_coord_yaw();
float spi_s3_get_turret_actual_deg();
float spi_s3_get_turret_pid_output();
float spi_s3_get_turret_pid_error();
void spi_s3_get_turret_pid(float *kp, float *ki, float *kd, float *i_limit);
float spi_s3_get_turret_dspc();

// SPI Link Diagnostics (for web dashboard)
uint32_t spi_s3_get_total_transactions();
uint32_t spi_s3_get_crc_errors();
uint32_t spi_s3_get_marker_errors();
uint32_t spi_s3_get_ack_frames();
uint32_t spi_s3_get_nack_frames();
uint32_t spi_s3_get_retry_frames();
uint32_t spi_s3_get_ack_timeouts();
uint8_t spi_s3_get_last_rx_marker();
uint8_t spi_s3_get_last_rx_seq();
String spi_s3_get_error_log_json();
void spi_s3_reset_error_counters();
SpiS3EndpointMode spi_s3_endpoint_mode();
const char* spi_s3_endpoint_mode_name();
bool spi_s3_is_clock_prep_safe();
