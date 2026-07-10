#pragma once

#include <stdint.h>

#include "src/kinematics/robot_model.h"

/**
 * @brief Forward Kinematics result structure
 *        End-effector position (mm) and pitch angle (degrees)
 */
typedef struct {
    float x;           // End-effector X position (mm)
    float y;           // End-effector Y position (mm)
    float z;           // End-effector Z position (mm)
    float alpha_deg;   // End-effector pitch angle (degrees)
} FK_Result_t;

/**
 * @brief Joint positions for all 9 visualization points (mm)
 *        Base, J1, J2, J3_corner, J4_phys, J5, J6_phys, J7, EE
 */
typedef struct {
    float x[9];
    float y[9];
    float z[9];
} FK_JointPositions_t;

/**
 * @brief Compute forward kinematics from 7 joint angles (degrees)
 * 
 * DH parameters match MATLAB get_DH_params.m exactly:
 *   J1: d=210.40, a=0,      alpha=+pi/2,  theta=q1
 *   J2: d=0,      a=240.00, alpha=0,       theta=q2+pi/2
 *   J3: d=0,      a=90.00,  alpha=+pi/2,  theta=q3
 *   J4: d=202.25, a=0,      alpha=-pi/2,  theta=q4
 *   J5: d=0,      a=0,      alpha=+pi/2,  theta=q5
 *   J6: d=272.00, a=0,      alpha=-pi/2,  theta=q6
 *   J7: d=0,      a=160.00, alpha=0,       theta=q7-pi/2
 *
 * @param joint_angles_deg  Array of 7 joint angles in degrees
 * @param result            Output FK result (EE position + pitch)
 */
void fk_compute(const float joint_angles_deg[7], FK_Result_t *result);

void fk_compute_transforms(const float joint_angles_deg[7], float out_t_all[7][16]);

/**
 * @brief Compute forward kinematics with all joint positions
 * 
 * Returns 9 joint positions matching MATLAB extract_joint_positions.m:
 *   [Base, J1, J2, J3_corner, J4_phys, J5, J6_phys, J7, EE]
 *
 * @param joint_angles_deg  Array of 7 joint angles in degrees
 * @param positions         Output joint positions
 * @param ee_result         Output EE result (optional, can be NULL)
 */
void fk_compute_full(const float joint_angles_deg[7], FK_JointPositions_t *positions, FK_Result_t *ee_result);
