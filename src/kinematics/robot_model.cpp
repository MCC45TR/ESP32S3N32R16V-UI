#include "robot_model.h"

#include <algorithm>

namespace mros::kinematics {
namespace {

constexpr RobotModel kModel = {
    {0.0f, kPi / 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, -kPi / 2.0f},
    {210.40f, 0.0f, 0.0f, 202.25f, 0.0f, 272.00f, 0.0f},
    {0.0f, 240.00f, 90.00f, 0.0f, 0.0f, 0.0f, 160.00f},
    {kPi / 2.0f, 0.0f, kPi / 2.0f, -kPi / 2.0f, kPi / 2.0f, -kPi / 2.0f, 0.0f},
    {-270.0f, -90.0f, -90.0f, -90.0f, -90.0f, -90.0f, -90.0f},
    {270.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f},
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    {120.0f, 120.0f, 120.0f, 120.0f, 120.0f, 120.0f, 120.0f},
    {240.0f, 240.0f, 240.0f, 240.0f, 240.0f, 240.0f, 240.0f},
    0.02f,
    0.5f,
    kPi / 360.0f,
    500U,
};

}  // namespace

const RobotModel& robot_model() { return kModel; }

void robot_model_clamp_degrees(float q_deg[kRobotJointCount]) {
  if (q_deg == nullptr) return;
  const RobotModel& model = robot_model();
  for (size_t i = 0U; i < kRobotJointCount; ++i) {
    q_deg[i] = std::max(model.q_min_deg[i], std::min(model.q_max_deg[i], q_deg[i]));
  }
}

void robot_model_joint_margin_degrees(const float q_deg[kRobotJointCount], float margin[kRobotJointCount]) {
  if (q_deg == nullptr || margin == nullptr) return;
  const RobotModel& model = robot_model();
  for (size_t i = 0U; i < kRobotJointCount; ++i) {
    const float range_half = 0.5f * (model.q_max_deg[i] - model.q_min_deg[i]);
    if (range_half <= 0.0f) {
      margin[i] = 0.0f;
      continue;
    }
    const float d_min = q_deg[i] - model.q_min_deg[i];
    const float d_max = model.q_max_deg[i] - q_deg[i];
    margin[i] = std::max(0.0f, std::min(d_min, d_max) / range_half);
  }
}

}  // namespace mros::kinematics
