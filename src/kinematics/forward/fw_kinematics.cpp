#include "fw_kinematics.h"
#include <math.h>

// ============================================================
// MROS 7-DOF Forward Kinematics (ESP32S3 Implementation)
// Ported from MATLAB: get_DH_params.m, calc_forward_kinematics.m,
//                     extract_joint_positions.m
// ============================================================

static const float FK_J4_PHYS_OFFSET_MM = 48.00f;
static const float FK_J6_PHYS_OFFSET_MM = 156.00f;

// ---- 4x4 Matrix operations (row-major, flat array[16]) ----

// A row-major 4x4 matrix: M[row*4 + col]
// Element access: M[r][c] = flat[r*4 + c]

static void mat4_identity(float M[16]) {
  for (int i = 0; i < 16; i++)
    M[i] = 0.0f;
  M[0] = 1.0f;
  M[5] = 1.0f;
  M[10] = 1.0f;
  M[15] = 1.0f;
}

static void mat4_multiply(const float A[16], const float B[16], float R[16]) {
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 4; col++) {
      float sum = 0.0f;
      for (int k = 0; k < 4; k++) {
        sum += A[row * 4 + k] * B[k * 4 + col];
      }
      R[row * 4 + col] = sum;
    }
  }
}

// Standard DH transformation matrix
// A = [ct  -st*ca   st*sa   a*ct ]
//     [st   ct*ca  -ct*sa   a*st ]
//     [0    sa      ca      d    ]
//     [0    0       0       1    ]
static void dh_transform(float theta, float d, float a, float alpha,
                         float T[16]) {
  float ct = cosf(theta);
  float st = sinf(theta);
  float ca = cosf(alpha);
  float sa = sinf(alpha);

  T[0] = ct;
  T[1] = -st * ca;
  T[2] = st * sa;
  T[3] = a * ct;
  T[4] = st;
  T[5] = ct * ca;
  T[6] = -ct * sa;
  T[7] = a * st;
  T[8] = 0.0f;
  T[9] = sa;
  T[10] = ca;
  T[11] = d;
  T[12] = 0.0f;
  T[13] = 0.0f;
  T[14] = 0.0f;
  T[15] = 1.0f;
}

// ---- Forward Kinematics ----

struct FKWorkspace {
  float T_all[7][16];
  float T_temp[16];
  float T_current[16];
  float Ak[16];
};

static void compute_fk_internal(const float joint_angles_deg[7],
                                FKWorkspace &workspace) {
  mat4_identity(workspace.T_current);
  const auto& model = mros::kinematics::robot_model();

  for (int i = 0; i < 7; i++) {
    float theta = joint_angles_deg[i] * mros::kinematics::kDegToRad + model.theta_offset_rad[i];

    dh_transform(theta, model.d_mm[i], model.a_mm[i], model.alpha_rad[i], workspace.Ak);

    // T_current = T_current * Ak
    mat4_multiply(workspace.T_current, workspace.Ak, workspace.T_temp);

    // Copy temp to current and store
    for (int j = 0; j < 16; j++) {
      workspace.T_current[j] = workspace.T_temp[j];
      workspace.T_all[i][j] = workspace.T_temp[j];
    }
  }
}

void fk_compute_transforms(const float joint_angles_deg[7], float out_t_all[7][16]) {
  if (!joint_angles_deg || !out_t_all) return;
  FKWorkspace workspace {};
  compute_fk_internal(joint_angles_deg, workspace);
  for (int i = 0; i < 7; ++i) {
    for (int j = 0; j < 16; ++j) out_t_all[i][j] = workspace.T_all[i][j];
  }
}

void fk_compute(const float joint_angles_deg[7], FK_Result_t *result) {
  if (!result)
    return;

  FKWorkspace workspace {};
  compute_fk_internal(joint_angles_deg, workspace);

  // End-effector position from T_all[6] (last joint transform)
  // Position is in column 3 (indices 3, 7, 11)
  result->x = workspace.T_all[6][3];
  result->y = workspace.T_all[6][7];
  result->z = workspace.T_all[6][11];

  // Pitch angle (alpha): atan2 of the approach vector
  // Using the Z-axis of end-effector frame projected onto the XZ plane
  // alpha = atan2(R31, sqrt(R11^2 + R21^2)) in standard convention
  float r20 = workspace.T_all[6][8]; // Row 2, Col 0
  float r00 = workspace.T_all[6][0]; // Row 0, Col 0
  float r10 = workspace.T_all[6][4]; // Row 1, Col 0
  float denom = sqrtf(r00 * r00 + r10 * r10);
  result->alpha_deg = atan2f(-r20, denom) * mros::kinematics::kRadToDeg;
}

void fk_compute_full(const float joint_angles_deg[7],
                     FK_JointPositions_t *positions, FK_Result_t *ee_result) {
  FKWorkspace workspace {};
  compute_fk_internal(joint_angles_deg, workspace);

  if (positions) {
    // Base origin
    positions->x[0] = 0.0f;
    positions->y[0] = 0.0f;
    positions->z[0] = 0.0f;

    // J1 output (DH O1): T_all[0]
    positions->x[1] = workspace.T_all[0][3];
    positions->y[1] = workspace.T_all[0][7];
    positions->z[1] = workspace.T_all[0][11];

    // J2 output (DH O2): T_all[1]
    positions->x[2] = workspace.T_all[1][3];
    positions->y[2] = workspace.T_all[1][7];
    positions->z[2] = workspace.T_all[1][11];

    // J3 corner (DH O3): T_all[2]
    positions->x[3] = workspace.T_all[2][3];
    positions->y[3] = workspace.T_all[2][7];
    positions->z[3] = workspace.T_all[2][11];

    // J4 physical location = DH_O3 + FK_J4_PHYS_OFFSET_MM * Z3
    // Z3 is column 2 of T_all[2] (indices 2, 6, 10)
    float z3_x = workspace.T_all[2][2];
    float z3_y = workspace.T_all[2][6];
    float z3_z = workspace.T_all[2][10];
    positions->x[4] = workspace.T_all[2][3] + FK_J4_PHYS_OFFSET_MM * z3_x;
    positions->y[4] = workspace.T_all[2][7] + FK_J4_PHYS_OFFSET_MM * z3_y;
    positions->z[4] = workspace.T_all[2][11] + FK_J4_PHYS_OFFSET_MM * z3_z;

    // J5 (DH O4): T_all[3]
    positions->x[5] = workspace.T_all[3][3];
    positions->y[5] = workspace.T_all[3][7];
    positions->z[5] = workspace.T_all[3][11];

    // J6 physical location = DH_O5 + FK_J6_PHYS_OFFSET_MM * Z5
    float z5_x = workspace.T_all[4][2];
    float z5_y = workspace.T_all[4][6];
    float z5_z = workspace.T_all[4][10];
    positions->x[6] = workspace.T_all[4][3] + FK_J6_PHYS_OFFSET_MM * z5_x;
    positions->y[6] = workspace.T_all[4][7] + FK_J6_PHYS_OFFSET_MM * z5_y;
    positions->z[6] = workspace.T_all[4][11] + FK_J6_PHYS_OFFSET_MM * z5_z;

    // J7 (DH O6): T_all[5]
    positions->x[7] = workspace.T_all[5][3];
    positions->y[7] = workspace.T_all[5][7];
    positions->z[7] = workspace.T_all[5][11];

    // End-effector (DH O7): T_all[6]
    positions->x[8] = workspace.T_all[6][3];
    positions->y[8] = workspace.T_all[6][7];
    positions->z[8] = workspace.T_all[6][11];
  }

  if (ee_result) {
    ee_result->x = workspace.T_all[6][3];
    ee_result->y = workspace.T_all[6][7];
    ee_result->z = workspace.T_all[6][11];

    float r20 = workspace.T_all[6][8];
    float r00 = workspace.T_all[6][0];
    float r10 = workspace.T_all[6][4];
    float denom = sqrtf(r00 * r00 + r10 * r10);
    ee_result->alpha_deg = atan2f(-r20, denom) * mros::kinematics::kRadToDeg;
  }
}
