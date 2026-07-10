#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mros::health::bearing {

constexpr size_t kMaxSnapshotRows = 48;

struct BearingHealthRow {
  char placement_id[24] = {};
  char joint_id[16] = {};
  char joint_name[24] = {};
  char designation[32] = {};
  char manufacturer[8] = {};
  char status[18] = {};
  uint8_t count = 0;
  float rpm_avg = 0.0f;
  double total_revolutions = 0.0;
  float equivalent_load_n = 0.0f;
  float load_ratio = 0.0f;
  float runtime_radial_load_n = 0.0f;
  float runtime_axial_load_n = 0.0f;
  char load_source[24] = {};
  float damage_percent = 0.0f;
  float remaining_l10_hours = 0.0f;
  float remaining_modified_hours = 0.0f;
};

struct BearingHealthSnapshot {
  bool configured = false;
  bool runtime_loaded = false;
  bool runtime_persist_ok = false;
  bool storage_available = false;
  uint32_t update_hz = 0;
  uint32_t spec_count = 0;
  uint32_t placement_count = 0;
  uint32_t row_count = 0;
  uint32_t last_update_ms = 0;
  uint32_t last_persist_ms = 0;
  uint32_t update_count = 0;
  char config_path[64] = {};
  char runtime_path[64] = {};
  char status[18] = {};
  char config_error[96] = {};
  BearingHealthRow rows[kMaxSnapshotRows] = {};
};

void init();
void task(void* arg);

bool reload_config();
bool import_config_from_path(const char* source_path, std::string* error);
bool reset_runtime();
bool persist_runtime_now();
bool get_snapshot(BearingHealthSnapshot* snapshot);
bool set_runtime_load(const char* placement_id,
                      float radial_load_n,
                      float axial_load_n,
                      float equivalent_load_n,
                      const char* source,
                      uint32_t ttl_ms,
                      std::string* error);
bool clear_runtime_load(const char* placement_id, std::string* error);
bool apply_loads_json(const char* json_text, std::string* error);

std::string format_table();
std::string format_json();
std::string format_config_help();

const char* config_path();
const char* runtime_path();

}  // namespace mros::health::bearing
