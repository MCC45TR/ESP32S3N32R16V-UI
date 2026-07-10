#pragma once

enum TrajectoryProfile {
  TRAJECTORY_PROFILE_LINEAR = 0,
  TRAJECTORY_PROFILE_QUINTIC,
  TRAJECTORY_PROFILE_HEPTIC,
  TRAJECTORY_PROFILE_SCURVE,
  TRAJECTORY_PROFILE_TIME_OPTIMAL,
};

float trajectory_lerp(float a, float b, float t);
float trajectory_clamp01(float t);
float trajectory_profile_blend(TrajectoryProfile profile, float t_s, float duration_s);
float trajectory_profile_sample(float start, float end, float t_s, float duration_s, TrajectoryProfile profile);
float trajectory_trapezoid_step(float current, float target, float max_vel, float max_accel,
                                float dt_s, float *vel_state);
