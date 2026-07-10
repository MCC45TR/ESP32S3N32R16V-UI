#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mros::health::actuator {

constexpr size_t kMaxActuatorSnapshotRows = 12;

struct ActuatorHealthRow {
  char joint_id[12] = {};
  char link_frame_id[24] = {};
  char status[18] = {};
  char source[28] = {};
  float axis_torque_nm = 0.0f;
  float abs_axis_torque_nm = 0.0f;
  float continuous_limit_nm = 0.0f;
  float peak_limit_nm = 0.0f;
  float nominal_continuous_limit_nm = 0.0f;
  float nominal_peak_limit_nm = 0.0f;
  float current_limited_continuous_nm = 0.0f;
  float current_limited_peak_nm = 0.0f;
  float continuous_margin_ratio = 0.0f;
  float peak_margin_ratio = 0.0f;
  float total_derating_factor = 1.0f;
  float thermal_factor = 1.0f;
  float voltage_factor = 1.0f;
  float motor_temp_c = 0.0f;
  float controller_temp_c = 0.0f;
  float bus_voltage_v = 0.0f;
  uint32_t age_ms = 0U;
  uint32_t ttl_ms = 0U;
};

struct ActuatorHealthSnapshot {
  bool configured = false;
  uint32_t row_count = 0U;
  uint32_t update_count = 0U;
  uint32_t last_update_ms = 0U;
  char status[18] = {};
  char worst_joint_status[18] = {};
  char source[28] = {};
  char last_error[96] = {};
  ActuatorHealthRow rows[kMaxActuatorSnapshotRows] = {};
};

void init();
bool get_snapshot(ActuatorHealthSnapshot* snapshot);
bool clear(std::string* error);
bool apply_joint_loads_json(const char* json_text, uint32_t default_ttl_ms, std::string* error);

std::string format_table();
std::string format_json();

}  // namespace mros::health::actuator
