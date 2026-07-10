#include "jacobian.h"

#include "src/kinematics/forward/fw_kinematics.h"
#include "src/kinematics/robot_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

void mat4_pos(const float t[16], float out[3]) {
  out[0] = t[3];
  out[1] = t[7];
  out[2] = t[11];
}

void mat4_axis_z(const float t[16], float out[3]) {
  out[0] = t[2];
  out[1] = t[6];
  out[2] = t[10];
}

void cross3(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

float smallest_eigenvalue_symmetric3(float a[3][3]) {
  for (int iter = 0; iter < 24; ++iter) {
    int p = 0;
    int q = 1;
    float max_val = fabsf(a[0][1]);
    const float a02 = fabsf(a[0][2]);
    const float a12 = fabsf(a[1][2]);
    if (a02 > max_val) {
      max_val = a02;
      p = 0;
      q = 2;
    }
    if (a12 > max_val) {
      max_val = a12;
      p = 1;
      q = 2;
    }
    if (max_val < 1e-5f) break;

    const float app = a[p][p];
    const float aqq = a[q][q];
    const float apq = a[p][q];
    const float phi = 0.5f * atan2f(2.0f * apq, aqq - app);
    const float c = cosf(phi);
    const float s = sinf(phi);

    for (int k = 0; k < 3; ++k) {
      if (k == p || k == q) continue;
      const float akp = a[k][p];
      const float akq = a[k][q];
      a[k][p] = c * akp - s * akq;
      a[p][k] = a[k][p];
      a[k][q] = s * akp + c * akq;
      a[q][k] = a[k][q];
    }
    a[p][p] = c * c * app - 2.0f * s * c * apq + s * s * aqq;
    a[q][q] = s * s * app + 2.0f * s * c * apq + c * c * aqq;
    a[p][q] = 0.0f;
    a[q][p] = 0.0f;
  }
  return std::min(a[0][0], std::min(a[1][1], a[2][2]));
}

float min_sigma_from_position_rows(const float j_6x7[42]) {
  float jjt[3][3] = {};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      float sum = 0.0f;
      for (int k = 0; k < 7; ++k) sum += j_6x7[r * 7 + k] * j_6x7[c * 7 + k];
      jjt[r][c] = sum;
    }
  }
  const float min_eig = smallest_eigenvalue_symmetric3(jjt);
  return sqrtf(std::max(0.0f, min_eig));
}

}  // namespace

bool jacobian_geometric_deg(const float joint_angles_deg[7], float out_j_6x7[42]) {
  if (!joint_angles_deg || !out_j_6x7) return false;

  float t_all[7][16] = {};
  fk_compute_transforms(joint_angles_deg, t_all);
  std::memset(out_j_6x7, 0, sizeof(float) * 42U);

  float p_ee[3] = {};
  mat4_pos(t_all[6], p_ee);

  for (int i = 0; i < 7; ++i) {
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float axis[3] = {0.0f, 0.0f, 1.0f};
    if (i > 0) {
      mat4_pos(t_all[i - 1], origin);
      mat4_axis_z(t_all[i - 1], axis);
    }

    float delta[3] = {p_ee[0] - origin[0], p_ee[1] - origin[1], p_ee[2] - origin[2]};
    float jv[3] = {};
    cross3(axis, delta, jv);
    out_j_6x7[0 * 7 + i] = jv[0];
    out_j_6x7[1 * 7 + i] = jv[1];
    out_j_6x7[2 * 7 + i] = jv[2];
    out_j_6x7[3 * 7 + i] = axis[0];
    out_j_6x7[4 * 7 + i] = axis[1];
    out_j_6x7[5 * 7 + i] = axis[2];
  }
  return true;
}

bool jacobian_numeric_position_deg(const float joint_angles_deg[7], float delta_rad, float out_j_3x7[21]) {
  if (!joint_angles_deg || !out_j_3x7) return false;
  if (!(delta_rad > 0.0f) || !std::isfinite(delta_rad)) delta_rad = 1e-4f;
  const float delta_deg = delta_rad * mros::kinematics::kRadToDeg;

  for (int i = 0; i < 7; ++i) {
    float qp[7] = {};
    float qm[7] = {};
    for (int k = 0; k < 7; ++k) {
      qp[k] = joint_angles_deg[k];
      qm[k] = joint_angles_deg[k];
    }
    qp[i] += delta_deg;
    qm[i] -= delta_deg;
    mros::kinematics::robot_model_clamp_degrees(qp);
    mros::kinematics::robot_model_clamp_degrees(qm);

    FK_Result_t fp {};
    FK_Result_t fm {};
    fk_compute(qp, &fp);
    fk_compute(qm, &fm);
    out_j_3x7[0 * 7 + i] = (fp.x - fm.x) / (2.0f * delta_rad);
    out_j_3x7[1 * 7 + i] = (fp.y - fm.y) / (2.0f * delta_rad);
    out_j_3x7[2 * 7 + i] = (fp.z - fm.z) / (2.0f * delta_rad);
  }
  return true;
}

float jacobian_min_sigma_position_deg(const float joint_angles_deg[7]) {
  float j[42] = {};
  if (!jacobian_geometric_deg(joint_angles_deg, j)) return 0.0f;
  return min_sigma_from_position_rows(j);
}
