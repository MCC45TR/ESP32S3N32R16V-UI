#pragma once

#include <stdint.h>

enum InvKinematicsWarning : uint32_t {
  kIkWarningNone = 0U,
  kIkWarningJointLimitClamp = 1U << 0U,
  kIkWarningSingularity = 1U << 1U,
  kIkWarningMaxIter = 1U << 2U,
};

struct InvKinematicsOptions {
  uint16_t max_iter;
  float pos_tol_mm;
  float max_step_deg;
  float null_gain;
};

struct InvKinematicsResult {
  bool success;
  uint16_t iterations;
  float pos_err_mm;
  float sigma_min;
  uint32_t warnings;
  float joints_deg[7];
};

bool inv_kinematics_solve(float x, float y, float z, float out_joints_deg[7]);
bool inv_kinematics_solve_with_seed(
    float x,
    float y,
    float z,
    const float seed_joints_deg[7],
    const InvKinematicsOptions *options,
    InvKinematicsResult *result);
