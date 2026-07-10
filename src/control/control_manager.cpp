#include "control_manager.h"
#include "pid_arms.h"
#include "pid_turret.h"
#include "src/drivers/utils/mros_console.h"

namespace {

PidTurretState g_turret_pid = {};
PidArmsState g_arms_pid = {};
unsigned long g_last_diag_ms = 0;
ControlOutput g_last_output = {};

}  // namespace

void control_manager_init() {
  pid_turret_init(&g_turret_pid, 1.2f, 0.02f, 0.01f, 30.0f, -100.0f, 100.0f);
  pid_arms_init(&g_arms_pid, 1.0f, 0.01f, 0.005f);
}

void control_manager_compute(const ControlInput &in, ControlOutput *out) {
  if (!out) return;
  out->turret_cmd =
      pid_turret_step(&g_turret_pid, in.target_turret_deg, in.measured_turret_deg, in.dt_s);
  pid_arms_step(&g_arms_pid, in.target_joints_deg, in.measured_joints_deg, in.dt_s, out->joints_cmd);
  g_last_output = *out;
}

void control_manager_loop(unsigned long now_ms) {
  // This module currently exposes compute API.
  // Loop keeps lightweight diagnostics for scheduler visibility.
  if ((now_ms - g_last_diag_ms) < 5000UL) return;
  g_last_diag_ms = now_ms;
  mros_console.printf("[CTRL] turret_cmd=%.2f j0_cmd=%.2f\n", g_last_output.turret_cmd,
                      g_last_output.joints_cmd[0]);
}

void control_manager_set_turret_gains(float kp, float ki, float kd) {
  pid_turret_init(&g_turret_pid, kp, ki, kd, g_turret_pid.i_limit, g_turret_pid.out_min,
                  g_turret_pid.out_max);
}

void control_manager_get_diag(ControlManagerDiag *diag) {
  if (diag == nullptr) return;
  PidTurretDiag turret {};
  pid_turret_get_diag(&g_turret_pid, &turret);
  diag->turret_last_error = turret.last_error;
  diag->turret_last_output = turret.last_output;
  diag->turret_integral = turret.integral;
  diag->turret_last_dt_s = turret.last_dt_s;
  diag->turret_max_abs_error = turret.max_abs_error;
  diag->turret_saturation_count = turret.saturation_count;
  diag->turret_integral_clamp_count = turret.integral_clamp_count;
  diag->turret_finite_fault_count = turret.finite_fault_count;
}
