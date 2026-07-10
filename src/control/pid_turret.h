#pragma once

#include <stdbool.h>
#include <stdint.h>

struct PidTurretState {
  float kp;
  float ki;
  float kd;
  float i_limit;
  float out_min;
  float out_max;
  float integral;
  float prev_error;
  float prev_derivative;
  float last_output;
  float last_error;
  float last_dt_s;
  uint32_t saturation_count;
  uint32_t integral_clamp_count;
  uint32_t finite_fault_count;
  float max_abs_error;
  float derivative_alpha;
  float slew_rate_per_s;
  bool initialized;
};

struct PidTurretDiag {
  float last_error = 0.0f;
  float last_output = 0.0f;
  float integral = 0.0f;
  float last_dt_s = 0.0f;
  float max_abs_error = 0.0f;
  uint32_t saturation_count = 0;
  uint32_t integral_clamp_count = 0;
  uint32_t finite_fault_count = 0;
};

void pid_turret_init(PidTurretState *state, float kp, float ki, float kd, float i_limit,
                     float out_min, float out_max);
void pid_turret_reset(PidTurretState *state);
float pid_turret_step(PidTurretState *state, float target_deg, float measured_deg, float dt_s);
float pid_turret_step(float target_deg, float measured_deg, float dt_s);
void pid_turret_get_diag(const PidTurretState *state, PidTurretDiag *diag);
