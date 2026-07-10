#pragma once

#include <stddef.h>

namespace mros::kinematics {

constexpr size_t kRobotJointCount = 7U;
constexpr size_t kRobotPointCount = 9U;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr const char* kRobotModelName = "mros-7dof-v1";
constexpr const char* kRobotModelRevision = "matlab-mdl_robot_model-2026-04-29";

struct RobotModel {
  float theta_offset_rad[kRobotJointCount];
  float d_mm[kRobotJointCount];
  float a_mm[kRobotJointCount];
  float alpha_rad[kRobotJointCount];
  float q_min_deg[kRobotJointCount];
  float q_max_deg[kRobotJointCount];
  float q_center_deg[kRobotJointCount];
  float v_max_deg_s[kRobotJointCount];
  float a_max_deg_s2[kRobotJointCount];
  float ctrl_dt_s;
  float ik_pos_tol_mm;
  float ik_ori_tol_rad;
  unsigned ik_max_iter;
};

const RobotModel& robot_model();
void robot_model_clamp_degrees(float q_deg[kRobotJointCount]);
void robot_model_joint_margin_degrees(const float q_deg[kRobotJointCount], float margin[kRobotJointCount]);

}  // namespace mros::kinematics
