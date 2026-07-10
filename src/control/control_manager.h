#pragma once

#include <stdint.h>

struct ControlInput {
  float target_turret_deg;
  float measured_turret_deg;
  float target_joints_deg[6];
  float measured_joints_deg[6];
  float dt_s;
};

struct ControlOutput {
  float turret_cmd;
  float joints_cmd[6];
};

struct ControlManagerDiag {
  float turret_last_error = 0.0f;
  float turret_last_output = 0.0f;
  float turret_integral = 0.0f;
  float turret_last_dt_s = 0.0f;
  float turret_max_abs_error = 0.0f;
  uint32_t turret_saturation_count = 0;
  uint32_t turret_integral_clamp_count = 0;
  uint32_t turret_finite_fault_count = 0;
};

void control_manager_init();
void control_manager_loop(unsigned long now_ms);
void control_manager_compute(const ControlInput &in, ControlOutput *out);
void control_manager_set_turret_gains(float kp, float ki, float kd);
void control_manager_get_diag(ControlManagerDiag *diag);
