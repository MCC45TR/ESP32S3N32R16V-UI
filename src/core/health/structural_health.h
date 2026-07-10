#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mros::health::structural {

constexpr size_t kMaxStructuralSnapshotRows = 32;

struct StructuralHealthRow {
  char part_id[32] = {};
  char link_frame_id[24] = {};
  char material_id[32] = {};
  char design_policy_id[24] = {};
  char status[18] = {};
  char source[28] = {};
  float force_n = 0.0f;
  float moment_nm = 0.0f;
  float von_mises_mpa = 0.0f;
  float bending_stress_mpa = 0.0f;
  float shear_stress_mpa = 0.0f;
  float fatigue_stress_mpa = 0.0f;
  float design_yield_sf = 0.0f;
  float design_shear_sf = 0.0f;
  float design_ultimate_sf = 0.0f;
  float design_fatigue_sf = 0.0f;
  float structural_temperature_factor = 1.0f;
  float structural_reference_temp_c = 25.0f;
  uint32_t age_ms = 0U;
  uint32_t ttl_ms = 0U;
};

struct StructuralHealthSnapshot {
  bool configured = false;
  uint32_t row_count = 0U;
  uint32_t update_count = 0U;
  uint32_t last_update_ms = 0U;
  char status[18] = {};
  char worst_structural_status[18] = {};
  char source[28] = {};
  char last_error[96] = {};
  StructuralHealthRow rows[kMaxStructuralSnapshotRows] = {};
};

void init();
bool get_snapshot(StructuralHealthSnapshot* snapshot);
bool clear(std::string* error);
bool apply_structural_checks_json(const char* json_text, uint32_t default_ttl_ms, std::string* error);

std::string format_table();
std::string format_json();

}  // namespace mros::health::structural
