#include "inv_kinematics.h"

#include "jacobian.h"
#include "src/kinematics/forward/fw_kinematics.h"
#include "src/kinematics/robot_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

bool invert3(const float a[3][3], float inv[3][3]) {
  const float det =
      a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
      a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
      a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (fabsf(det) < 1e-9f || !std::isfinite(det)) return false;
  const float id = 1.0f / det;
  inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * id;
  inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * id;
  inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id;
  inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * id;
  inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * id;
  inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * id;
  inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * id;
  inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * id;
  inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
  return true;
}

void mat3_vec(const float a[3][3], const float v[3], float out[3]) {
  for (int r = 0; r < 3; ++r) out[r] = a[r][0] * v[0] + a[r][1] * v[1] + a[r][2] * v[2];
}

float norm3(const float v[3]) {
  return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void fill_default_options(InvKinematicsOptions *out) {
  const auto& model = mros::kinematics::robot_model();
  out->max_iter = static_cast<uint16_t>(model.ik_max_iter);
  out->pos_tol_mm = model.ik_pos_tol_mm;
  out->max_step_deg = 10.0f;
  out->null_gain = 0.1f;
}

}  // namespace

bool inv_kinematics_solve(float x, float y, float z, float out_joints_deg[7]) {
  InvKinematicsResult result {};
  const bool ok = inv_kinematics_solve_with_seed(x, y, z, nullptr, nullptr, &result);
  if (out_joints_deg) {
    for (int i = 0; i < 7; ++i) out_joints_deg[i] = result.joints_deg[i];
  }
  return ok;
}

bool inv_kinematics_solve_with_seed(
    float x,
    float y,
    float z,
    const float seed_joints_deg[7],
    const InvKinematicsOptions *options,
    InvKinematicsResult *result) {
  if (!result || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;

  const auto& model = mros::kinematics::robot_model();
  InvKinematicsOptions opts {};
  fill_default_options(&opts);
  if (options) {
    if (options->max_iter > 0U) opts.max_iter = options->max_iter;
    if (options->pos_tol_mm > 0.0f) opts.pos_tol_mm = options->pos_tol_mm;
    if (options->max_step_deg > 0.0f) opts.max_step_deg = options->max_step_deg;
    if (options->null_gain >= 0.0f) opts.null_gain = options->null_gain;
  }

  std::memset(result, 0, sizeof(*result));
  float q_deg[7] = {};
  for (int i = 0; i < 7; ++i) q_deg[i] = seed_joints_deg ? seed_joints_deg[i] : model.q_center_deg[i];
  mros::kinematics::robot_model_clamp_degrees(q_deg);

  float best_q[7] = {};
  for (int i = 0; i < 7; ++i) best_q[i] = q_deg[i];
  float best_err = INFINITY;
  const float target[3] = {x, y, z};
  const float max_step_rad = opts.max_step_deg * mros::kinematics::kDegToRad;

  for (uint16_t iter = 1U; iter <= opts.max_iter; ++iter) {
    FK_Result_t fk {};
    fk_compute(q_deg, &fk);
    const float err_vec[3] = {target[0] - fk.x, target[1] - fk.y, target[2] - fk.z};
    const float err = norm3(err_vec);
    if (err < best_err) {
      best_err = err;
      for (int i = 0; i < 7; ++i) best_q[i] = q_deg[i];
    }

    float j[42] = {};
    if (!jacobian_geometric_deg(q_deg, j)) break;
    const float sigma = jacobian_min_sigma_position_deg(q_deg);
    if (sigma < 1.0f) result->warnings |= kIkWarningSingularity;
    result->sigma_min = sigma;

    if (err <= opts.pos_tol_mm) {
      result->success = true;
      result->iterations = iter;
      result->pos_err_mm = err;
      for (int i = 0; i < 7; ++i) result->joints_deg[i] = q_deg[i];
      return true;
    }

    float lambda = 5.0f;
    if (err < 10.0f) lambda = 0.5f;
    else if (err < 50.0f) lambda = 2.0f;

    float a[3][3] = {};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 7; ++k) a[r][c] += j[r * 7 + k] * j[c * 7 + k];
      }
      a[r][r] += lambda * lambda;
    }

    float inv_a[3][3] = {};
    if (!invert3(a, inv_a)) {
      result->warnings |= kIkWarningSingularity;
      break;
    }

    float y_vec[3] = {};
    mat3_vec(inv_a, err_vec, y_vec);
    float dq_primary[7] = {};
    for (int k = 0; k < 7; ++k) {
      for (int r = 0; r < 3; ++r) dq_primary[k] += j[r * 7 + k] * y_vec[r];
    }

    float q_bias[7] = {};
    for (int k = 0; k < 7; ++k) {
      const float q_rad = q_deg[k] * mros::kinematics::kDegToRad;
      const float center_rad = model.q_center_deg[k] * mros::kinematics::kDegToRad;
      q_bias[k] = opts.null_gain * (center_rad - q_rad);
    }

    float j_bias[3] = {};
    for (int r = 0; r < 3; ++r) {
      for (int k = 0; k < 7; ++k) j_bias[r] += j[r * 7 + k] * q_bias[k];
    }
    float y_bias[3] = {};
    mat3_vec(inv_a, j_bias, y_bias);
    float null_step[7] = {};
    for (int k = 0; k < 7; ++k) {
      float correction = 0.0f;
      for (int r = 0; r < 3; ++r) correction += j[r * 7 + k] * y_bias[r];
      null_step[k] = q_bias[k] - correction;
    }

    float max_abs_step = 0.0f;
    float dq[7] = {};
    for (int k = 0; k < 7; ++k) {
      dq[k] = 0.5f * dq_primary[k] + null_step[k];
      max_abs_step = std::max(max_abs_step, fabsf(dq[k]));
    }
    if (max_abs_step > max_step_rad && max_abs_step > 0.0f) {
      const float scale = max_step_rad / max_abs_step;
      for (int k = 0; k < 7; ++k) dq[k] *= scale;
    }

    for (int k = 0; k < 7; ++k) q_deg[k] += dq[k] * mros::kinematics::kRadToDeg;
    float before_clamp[7] = {};
    for (int k = 0; k < 7; ++k) before_clamp[k] = q_deg[k];
    mros::kinematics::robot_model_clamp_degrees(q_deg);
    for (int k = 0; k < 7; ++k) {
      if (fabsf(before_clamp[k] - q_deg[k]) > 1e-5f) result->warnings |= kIkWarningJointLimitClamp;
    }
    result->iterations = iter;
  }

  result->success = false;
  result->warnings |= kIkWarningMaxIter;
  result->pos_err_mm = best_err;
  for (int i = 0; i < 7; ++i) result->joints_deg[i] = best_q[i];
  result->sigma_min = jacobian_min_sigma_position_deg(best_q);
  return false;
}
