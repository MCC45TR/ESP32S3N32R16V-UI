#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct PidArmsState {
  float kp[6];
  float ki[6];
  float kd[6];
  float integ[6];
  float prev_err[6];
  float prev_derivative[6];
  float last_output[6];
  float out_min[6];
  float out_max[6];
  float i_limit[6];
  uint32_t saturation_count[6];
  uint32_t integral_clamp_count[6];
  uint32_t finite_fault_count[6];
  float derivative_alpha;
  float slew_rate_per_s;
  bool initialized;
};

void pid_arms_init(PidArmsState *state, float kp, float ki, float kd);
void pid_arms_reset(PidArmsState *state);
void pid_arms_step(PidArmsState *state, const float target_deg[6], const float measured_deg[6],
                   float dt_s, float out_cmd[6]);
void pid_arms_step(const float target_deg[6], const float measured_deg[6], float out_cmd[6]);
