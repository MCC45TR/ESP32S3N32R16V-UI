#include "trajectory.h"
#include <cmath>
#include <math.h>

float trajectory_clamp01(float t) {
  if (t < 0.0f) return 0.0f;
  if (t > 1.0f) return 1.0f;
  return t;
}

float trajectory_lerp(float a, float b, float t) { return a + (b - a) * trajectory_clamp01(t); }

float trajectory_profile_blend(TrajectoryProfile profile, float t_s, float duration_s) {
  if (duration_s <= 0.0f || !std::isfinite(duration_s)) return 1.0f;
  const float u = trajectory_clamp01(t_s / duration_s);
  switch (profile) {
    case TRAJECTORY_PROFILE_QUINTIC:
      return (10.0f * u * u * u) - (15.0f * u * u * u * u) + (6.0f * u * u * u * u * u);
    case TRAJECTORY_PROFILE_HEPTIC: {
      const float u2 = u * u;
      const float u3 = u2 * u;
      const float u4 = u2 * u2;
      return (35.0f * u4) - (84.0f * u4 * u) + (70.0f * u4 * u2) - (20.0f * u4 * u3);
    }
    case TRAJECTORY_PROFILE_SCURVE: {
      const float half_t = 0.5f * duration_s;
      const float tj = 0.25f * duration_s;
      const float jerk = 32.0f / (duration_s * duration_s * duration_s);
      auto first_half = [tj, jerk](float t) -> float {
        if (t <= tj) return (jerk * t * t * t) / 6.0f;
        const float v1 = 0.5f * jerk * tj * tj;
        const float s1 = (jerk * tj * tj * tj) / 6.0f;
        const float tau = t - tj;
        return s1 + v1 * tau + 0.5f * jerk * tj * tau * tau - (jerk * tau * tau * tau) / 6.0f;
      };
      return (t_s <= half_t) ? first_half(std::max(0.0f, t_s))
                             : (1.0f - first_half(std::max(0.0f, duration_s - t_s)));
    }
    case TRAJECTORY_PROFILE_TIME_OPTIMAL:
      return u < 0.5f ? (2.0f * u * u) : (1.0f - 2.0f * (1.0f - u) * (1.0f - u));
    case TRAJECTORY_PROFILE_LINEAR:
    default:
      return u;
  }
}

float trajectory_profile_sample(float start, float end, float t_s, float duration_s, TrajectoryProfile profile) {
  return trajectory_lerp(start, end, trajectory_profile_blend(profile, t_s, duration_s));
}

float trajectory_trapezoid_step(float current, float target, float max_vel, float max_accel,
                                float dt_s, float *vel_state) {
  if (dt_s <= 0.0f || !std::isfinite(dt_s)) return current;
  if (!vel_state) return current;

  float vel = *vel_state;
  if (!std::isfinite(vel)) vel = 0.0f;
  max_vel = fabsf(max_vel);
  max_accel = fabsf(max_accel);
  if (max_vel <= 0.0f) max_vel = 1.0f;
  if (max_accel <= 0.0f) max_accel = 1.0f;

  const float err = target - current;
  const float dir = (err >= 0.0f) ? 1.0f : -1.0f;
  const float stop_dist = (vel * vel) / (2.0f * max_accel + 1e-6f);
  const float accel = (fabsf(err) > fabsf(stop_dist)) ? (dir * max_accel) : (-dir * max_accel);

  vel += accel * dt_s;
  if (vel > max_vel) vel = max_vel;
  if (vel < -max_vel) vel = -max_vel;

  float next = current + vel * dt_s;
  if ((dir > 0.0f && next > target) || (dir < 0.0f && next < target)) {
    next = target;
    vel = 0.0f;
  }
  *vel_state = vel;
  return next;
}
