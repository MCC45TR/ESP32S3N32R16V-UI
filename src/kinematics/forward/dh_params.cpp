#include "dh_params.h"

#include "src/kinematics/robot_model.h"

namespace {

DhParam g_params[mros::kinematics::kRobotJointCount] = {};
bool g_initialized = false;

void init_params_once() {
  if (g_initialized) return;
  const auto& model = mros::kinematics::robot_model();
  for (size_t i = 0U; i < mros::kinematics::kRobotJointCount; ++i) {
    g_params[i] = {
        model.a_mm[i],
        model.alpha_rad[i],
        model.d_mm[i],
        model.theta_offset_rad[i],
    };
  }
  g_initialized = true;
}

}  // namespace

size_t dh_params_count() { return mros::kinematics::kRobotJointCount; }

const DhParam *dh_params_get() {
  init_params_once();
  return g_params;
}
