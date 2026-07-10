#pragma once

#include <stdint.h>

bool jacobian_geometric_deg(const float joint_angles_deg[7], float out_j_6x7[42]);
bool jacobian_numeric_position_deg(const float joint_angles_deg[7], float delta_rad, float out_j_3x7[21]);
float jacobian_min_sigma_position_deg(const float joint_angles_deg[7]);
