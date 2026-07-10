#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// PCA9685 I2C Driver for ESP32-S3
// Communicates with PCA9685 16-channel PWM servo driver

#define PCA_TOTAL_CHANNELS 16

// Per-channel calibration data
typedef struct {
  float angle_min_us;    // Pulse width for 0°   (default: 500 µs)
  float angle_max_us;    // Pulse width for 180° (default: 2500 µs)
  float speed_min_us;    // Pulse width for -100 speed (default: 500 µs)
  float speed_center_us; // Pulse width for 0 speed / stop (default: 1500 µs)
  float speed_max_us;    // Pulse width for +100 speed (default: 2500 µs)
} PCA9685_ChannelCal_t;

typedef struct {
  uint32_t queue_depth;
  uint32_t queue_capacity;
  uint32_t queue_high_watermark;
  uint32_t enqueue_count;
  uint32_t process_count;
  uint32_t duplicate_skip_count;
  uint32_t coalesced_count;
  uint32_t drop_oldest_count;
  uint32_t drop_count;
  uint32_t shadow_update_count;
  uint32_t shadow_coalesce_count;
  uint32_t shadow_flush_count;
  bool shadow_valid;
  uint8_t shadow_type;
  uint8_t shadow_start_channel;
  uint8_t shadow_count;
} PCA9685_DiagSnapshot_t;

// Initialize I2C bus and PCA9685 device (50 Hz servo mode)
bool pca9685_init();
void pca9685_set_task_handle(TaskHandle_t task_handle);
void pca9685_process_queue();
void pca9685_get_diag_snapshot(PCA9685_DiagSnapshot_t *snapshot);

// Set single channel PWM (on/off ticks, 0-4095)
bool pca9685_set_pwm(uint8_t channel, uint16_t on, uint16_t off);

// Set contiguous channel PWM block (on assumed 0 for each channel)
// off_ticks[i] applies to (start_channel + i)
bool pca9685_set_pwm_group(uint8_t start_channel, const uint16_t *off_ticks,
                           uint8_t count);

// Set servo angle (0.0 - 180.0 degrees) using per-channel calibration
bool pca9685_set_servo_angle(uint8_t channel, float angle);

// Set servo speed for continuous rotation (-100 to +100) using per-channel
// calibration
bool pca9685_set_servo_speed(uint8_t channel, int speed);

// Set same speed to a contiguous channel group (e.g. channels 1..3)
bool pca9685_set_servo_speed_group(uint8_t start_channel, uint8_t count,
                                   int speed);

// Set angle list to contiguous channels (angles[i] -> start_channel+i)
bool pca9685_set_servo_angle_group(uint8_t start_channel, const float *angles,
                                   uint8_t count);

// Set PWM frequency in Hz (requires sleep/wake cycle)
bool pca9685_set_frequency(float freq_hz);

// Get currently set PWM frequency (Hz)
float pca9685_get_frequency();

// Oscillator calibration: Set/Get the actual oscillator frequency (Hz)
// Default is 25000000 (25 MHz)
void pca9685_set_osc_freq(uint32_t freq_hz);
uint32_t pca9685_get_osc_freq();

// Output Enable control (OE pin)
// enable = true  -> OE LOW  -> PWM outputs active (motors ON)
// enable = false -> OE HIGH -> PWM outputs disabled (motors OFF)
void pca9685_set_output_enable(bool enable);

// Returns true if PCA9685 was initialized successfully
bool pca9685_is_ready();

// Returns current OE state (true = outputs enabled)
bool pca9685_get_output_enable();

// --- Calibration API ---

// Get pointer to calibration data for a channel (1-16), returns NULL if invalid
PCA9685_ChannelCal_t *pca9685_get_cal(uint8_t channel);

// Set calibration data for a channel (1-16)
void pca9685_set_cal(uint8_t channel, const PCA9685_ChannelCal_t *cal);

// Save all calibration data to flash (Preferences)
void pca9685_save_cal();

// Load calibration from flash (called automatically in init)
void pca9685_load_cal();

// Reset all calibration to factory defaults
void pca9685_reset_cal();
