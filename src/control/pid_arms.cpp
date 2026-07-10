#include "pid_arms.h"
#include <cmath>
#include <math.h>

namespace {
PidArmsState g_default = {};
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

void init_defaults(PidArmsState *s) {
  if (!s) return;
  for (int i = 0; i < 6; ++i) {
    s->kp[i] = 1.0f;
    s->ki[i] = 0.01f;
    s->kd[i] = 0.005f;
    s->integ[i] = 0.0f;
    s->prev_err[i] = 0.0f;
    s->prev_derivative[i] = 0.0f;
    s->last_output[i] = 0.0f;
    s->out_min[i] = -100.0f;
    s->out_max[i] = 100.0f;
    s->i_limit[i] = 30.0f;
    s->saturation_count[i] = 0U;
    s->integral_clamp_count[i] = 0U;
    s->finite_fault_count[i] = 0U;
  }
  s->derivative_alpha = 0.25f;
  s->slew_rate_per_s = 0.0f;
  s->initialized = true;
}
}  // namespace

void pid_arms_init(PidArmsState *state, float kp, float ki, float kd) {
  if (!state) return;
  init_defaults(state);
  for (int i = 0; i < 6; ++i) {
    state->kp[i] = kp;
    state->ki[i] = ki;
    state->kd[i] = kd;
  }
}

void pid_arms_reset(PidArmsState *state) {
  if (!state) return;
  for (int i = 0; i < 6; ++i) {
    state->integ[i] = 0.0f;
    state->prev_err[i] = 0.0f;
    state->prev_derivative[i] = 0.0f;
    state->last_output[i] = 0.0f;
    state->saturation_count[i] = 0U;
    state->integral_clamp_count[i] = 0U;
    state->finite_fault_count[i] = 0U;
  }
}

void pid_arms_step(PidArmsState *state, const float target_deg[6], const float measured_deg[6],
                   float dt_s, float out_cmd[6]) {
  if (!state || !target_deg || !measured_deg || !out_cmd) return;
  if (!state->initialized) init_defaults(state);
  if (!std::isfinite(dt_s) || dt_s < kMinDtS) dt_s = kMinDtS;
  if (dt_s > kMaxDtS) dt_s = kMaxDtS;
  const float alpha = clampf(state->derivative_alpha, 0.0f, 1.0f);

  for (int i = 0; i < 6; ++i) {
    float err = target_deg[i] - measured_deg[i];
    if (!std::isfinite(err)) {
      state->finite_fault_count[i] = sat_inc(state->finite_fault_count[i]);
      out_cmd[i] = state->last_output[i];
      continue;
    }
    state->integ[i] += err * dt_s;
    if (state->integ[i] > state->i_limit[i]) {
      state->integ[i] = state->i_limit[i];
      state->integral_clamp_count[i] = sat_inc(state->integral_clamp_count[i]);
    }
    if (state->integ[i] < -state->i_limit[i]) {
      state->integ[i] = -state->i_limit[i];
      state->integral_clamp_count[i] = sat_inc(state->integral_clamp_count[i]);
    }
    const float raw_derivative = (err - state->prev_err[i]) / dt_s;
    const float derr = (state->prev_derivative[i] * (1.0f - alpha)) + (raw_derivative * alpha);
    state->prev_derivative[i] = derr;
    state->prev_err[i] = err;
    float out = state->kp[i] * err + state->ki[i] * state->integ[i] + state->kd[i] * derr;
    if (!std::isfinite(out)) {
      state->finite_fault_count[i] = sat_inc(state->finite_fault_count[i]);
      out = state->last_output[i];
    }
    if (state->slew_rate_per_s > 0.0f) {
      const float max_step = state->slew_rate_per_s * dt_s;
      out = clampf(out, state->last_output[i] - max_step, state->last_output[i] + max_step);
    }
    if (out > state->out_max[i]) {
      out = state->out_max[i];
      state->saturation_count[i] = sat_inc(state->saturation_count[i]);
    }
    if (out < state->out_min[i]) {
      out = state->out_min[i];
      state->saturation_count[i] = sat_inc(state->saturation_count[i]);
    }
    state->last_output[i] = out;
    out_cmd[i] = out;
  }
}

void pid_arms_step(const float target_deg[6], const float measured_deg[6], float out_cmd[6]) {
  if (!g_default.initialized) init_defaults(&g_default);
  pid_arms_step(&g_default, target_deg, measured_deg, 0.01f, out_cmd);
}
