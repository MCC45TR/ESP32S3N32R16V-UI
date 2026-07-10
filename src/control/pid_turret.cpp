#include "pid_turret.h"
#include <cmath>
#include <math.h>

namespace {
constexpr float kMinDtS = 0.0005f;
constexpr float kMaxDtS = 0.1f;

float clampf(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

uint32_t sat_inc(uint32_t value) {
  return value == 0xFFFFFFFFUL ? value : value + 1U;
}

PidTurretState g_default_state = {};
}

void pid_turret_init(PidTurretState *state, float kp, float ki, float kd, float i_limit,
                     float out_min, float out_max) {
  if (!state) return;
  state->kp = kp;
  state->ki = ki;
  state->kd = kd;
  state->i_limit = (i_limit > 0.0f) ? i_limit : 0.0f;
  state->out_min = out_min;
  state->out_max = out_max;
  state->integral = 0.0f;
  state->prev_error = 0.0f;
  state->prev_derivative = 0.0f;
  state->last_output = 0.0f;
  state->last_error = 0.0f;
  state->last_dt_s = 0.0f;
  state->saturation_count = 0;
  state->integral_clamp_count = 0;
  state->finite_fault_count = 0;
  state->max_abs_error = 0.0f;
  state->derivative_alpha = 0.25f;
  state->slew_rate_per_s = 0.0f;
  state->initialized = true;
}

void pid_turret_reset(PidTurretState *state) {
  if (!state) return;
  state->integral = 0.0f;
  state->prev_error = 0.0f;
  state->prev_derivative = 0.0f;
  state->last_output = 0.0f;
  state->last_error = 0.0f;
  state->last_dt_s = 0.0f;
  state->saturation_count = 0;
  state->integral_clamp_count = 0;
  state->finite_fault_count = 0;
  state->max_abs_error = 0.0f;
}

float pid_turret_step(PidTurretState *state, float target_deg, float measured_deg, float dt_s) {
  if (!state || !state->initialized) return 0.0f;
  if (!std::isfinite(target_deg) || !std::isfinite(measured_deg)) {
    state->finite_fault_count = sat_inc(state->finite_fault_count);
    return state->last_output;
  }
  if (!std::isfinite(dt_s) || dt_s < kMinDtS) dt_s = kMinDtS;
  if (dt_s > kMaxDtS) dt_s = kMaxDtS;

  const float err = target_deg - measured_deg;
  const float abs_err = fabsf(err);
  if (abs_err > state->max_abs_error) state->max_abs_error = abs_err;
  state->integral += err * dt_s;
  if (state->integral > state->i_limit) {
    state->integral = state->i_limit;
    state->integral_clamp_count = sat_inc(state->integral_clamp_count);
  }
  if (state->integral < -state->i_limit) {
    state->integral = -state->i_limit;
    state->integral_clamp_count = sat_inc(state->integral_clamp_count);
  }

  const float raw_derivative = (err - state->prev_error) / dt_s;
  const float alpha = clampf(state->derivative_alpha, 0.0f, 1.0f);
  const float derr = (state->prev_derivative * (1.0f - alpha)) + (raw_derivative * alpha);
  state->prev_derivative = derr;
  state->prev_error = err;
  float out = state->kp * err + state->ki * state->integral + state->kd * derr;
  if (!std::isfinite(out)) {
    state->finite_fault_count = sat_inc(state->finite_fault_count);
    return state->last_output;
  }
  if (state->slew_rate_per_s > 0.0f) {
    const float max_step = state->slew_rate_per_s * dt_s;
    out = clampf(out, state->last_output - max_step, state->last_output + max_step);
  }
  if (out > state->out_max) {
    out = state->out_max;
    state->saturation_count = sat_inc(state->saturation_count);
  }
  if (out < state->out_min) {
    out = state->out_min;
    state->saturation_count = sat_inc(state->saturation_count);
  }
  state->last_error = err;
  state->last_output = out;
  state->last_dt_s = dt_s;
  return out;
}

float pid_turret_step(float target_deg, float measured_deg, float dt_s) {
  if (!g_default_state.initialized) {
    pid_turret_init(&g_default_state, 1.2f, 0.02f, 0.01f, 30.0f, -100.0f, 100.0f);
  }
  return pid_turret_step(&g_default_state, target_deg, measured_deg, dt_s);
}

void pid_turret_get_diag(const PidTurretState *state, PidTurretDiag *diag) {
  if (diag == nullptr) return;
  *diag = PidTurretDiag {};
  if (state == nullptr) return;
  diag->last_error = state->last_error;
  diag->last_output = state->last_output;
  diag->integral = state->integral;
  diag->last_dt_s = state->last_dt_s;
  diag->max_abs_error = state->max_abs_error;
  diag->saturation_count = state->saturation_count;
  diag->integral_clamp_count = state->integral_clamp_count;
  diag->finite_fault_count = state->finite_fault_count;
}
