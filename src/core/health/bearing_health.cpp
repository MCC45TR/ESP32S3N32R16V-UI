#include "src/core/health/bearing_health.h"

#include "src/comm_interfaces/spi/spi_t41_link.h"
#include "src/core/rtos/task_manager.h"
#include "src/platform/mros_file.h"
#include "src/platform/mros_fs.h"
#include "src/platform/mros_time.h"

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace mros::health::bearing {
namespace {

constexpr const char* kConfigPath = "/ESPUSER/bearing_health/bearings_skf.json";
constexpr const char* kRuntimePath = "/ESPUSER/bearing_health/runtime.json";
constexpr const char* kRuntimeTmpPath = "/ESPUSER/bearing_health/runtime.tmp";
constexpr const char* kExamplePath = "/ESPUSER/bearing_health/bearings_skf.example.json";
constexpr uint32_t kUpdatePeriodMs = 20U;
constexpr uint32_t kPersistPeriodMs = 60000U;
constexpr uint32_t kRuntimeSchemaVersion = 1U;
constexpr float kMinimumEquivalentLoadN = 1.0f;
constexpr float kMinimumRpmForHourEstimate = 0.01f;
constexpr float kDamageRateEpsilon = 1.0e-12f;
constexpr uint32_t kDefaultRuntimeLoadTtlMs = 2000U;

struct BearingSpec {
  std::string id;
  std::string designation;
  std::string manufacturer = "SKF";
  std::string type;
  std::string source_url;
  std::string source_date;
  float bore_mm = 0.0f;
  float outer_mm = 0.0f;
  float width_mm = 0.0f;
  float dynamic_c_n = 0.0f;
  float static_c0_n = 0.0f;
  float fatigue_limit_pu_n = 0.0f;
  float reference_speed_rpm = 0.0f;
  float limiting_speed_rpm = 0.0f;
  float fatigue_exponent_p = 3.0f;
  float a_skf = 1.0f;
  float x_factor = 1.0f;
  float y_factor = 0.0f;
};

struct BearingPlacement {
  std::string id;
  std::string joint_id;
  std::string joint_name;
  std::string spec_id;
  int joint_index = 0;
  uint8_t count = 1;
  float rotation_ratio_to_joint = 1.0f;
  float load_share_factor = 1.0f;
  float nominal_radial_load_n = 0.0f;
  float nominal_axial_load_n = 0.0f;
  float equivalent_load_override_n = 0.0f;
  float x_factor_override = 0.0f;
  float y_factor_override = 0.0f;
};

struct BearingRuntime {
  std::string placement_id;
  bool position_valid = false;
  float last_position_deg = 0.0f;
  float rpm_ema = 0.0f;
  float damage_rate_ema_per_hour = 0.0f;
  double total_revolutions = 0.0;
  double damage_l10 = 0.0;
  double damage_modified = 0.0;
  float last_equivalent_load_n = 0.0f;
  float runtime_radial_load_n = 0.0f;
  float runtime_axial_load_n = 0.0f;
  float runtime_equivalent_load_n = 0.0f;
  uint32_t runtime_load_expires_ms = 0U;
  std::string runtime_load_source;
  uint32_t last_update_ms = 0U;
};

SemaphoreHandle_t g_mutex = nullptr;
std::vector<BearingSpec> g_specs;
std::vector<BearingPlacement> g_placements;
std::vector<BearingRuntime> g_runtime;
BearingHealthSnapshot g_snapshot {};
bool g_initialized = false;
bool g_runtime_loaded = false;
bool g_runtime_persist_ok = false;
uint32_t g_last_persist_ms = 0U;
uint32_t g_update_count = 0U;
std::string g_config_error;

void copy_cstr(char* dst, const size_t dst_size, const std::string& value) {
  if (dst == nullptr || dst_size == 0U) return;
  std::snprintf(dst, dst_size, "%s", value.c_str());
}

const char* json_string(const cJSON* obj, const char* key, const char* fallback = "") {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : fallback;
}

std::string json_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8U);
  for (const char ch : text) {
    switch (ch) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          char encoded[8] = {};
          std::snprintf(encoded, sizeof(encoded), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(ch)));
          out += encoded;
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

float json_float(const cJSON* obj, const char* key, const float fallback = 0.0f) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(item) ? static_cast<float>(item->valuedouble) : fallback;
}

int json_int(const cJSON* obj, const char* key, const int fallback = 0) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(item) ? item->valueint : fallback;
}

const BearingSpec* find_spec_locked(const std::string& spec_id) {
  for (const BearingSpec& spec : g_specs) {
    if (spec.id == spec_id) return &spec;
  }
  return nullptr;
}

bool is_skf_manufacturer(const std::string& manufacturer) {
  if (manufacturer.size() != 3U) return false;
  return (manufacturer[0] == 'S' || manufacturer[0] == 's') &&
         (manufacturer[1] == 'K' || manufacturer[1] == 'k') &&
         (manufacturer[2] == 'F' || manufacturer[2] == 'f');
}

bool is_skf_source_url(const std::string& source_url) {
  return source_url.find("skf.com") != std::string::npos ||
         source_url.find("www.skf.com") != std::string::npos;
}

float shortest_delta_deg(const float current_deg, const float previous_deg) {
  float delta = current_deg - previous_deg;
  while (delta > 180.0f) delta -= 360.0f;
  while (delta < -180.0f) delta += 360.0f;
  return delta;
}

BearingRuntime* find_runtime_locked(const std::string& placement_id) {
  for (BearingRuntime& rt : g_runtime) {
    if (rt.placement_id == placement_id) return &rt;
  }
  return nullptr;
}

bool spec_exists(const std::vector<BearingSpec>& specs, const std::string& spec_id) {
  for (const BearingSpec& spec : specs) {
    if (spec.id == spec_id) return true;
  }
  return false;
}

float telemetry_joint_position_deg(const int joint_index) {
  if (joint_index <= 0) return spi_s3_get_turret_deg();
  if (joint_index <= 6) return spi_s3_get_joint_deg(joint_index - 1);
  return 0.0f;
}

bool runtime_load_active(const BearingRuntime& rt, const uint32_t now_ms) {
  return rt.runtime_load_expires_ms != 0U &&
         static_cast<int32_t>(rt.runtime_load_expires_ms - now_ms) > 0;
}

float bearing_equivalent_load_n(const BearingSpec& spec,
                                const BearingPlacement& placement,
                                const BearingRuntime& rt,
                                const uint32_t now_ms) {
  if (runtime_load_active(rt, now_ms)) {
    float p = rt.runtime_equivalent_load_n;
    if (p <= 0.0f) {
      const float x = placement.x_factor_override > 0.0f ? placement.x_factor_override : spec.x_factor;
      const float y = placement.y_factor_override > 0.0f ? placement.y_factor_override : spec.y_factor;
      p = (x * rt.runtime_radial_load_n) + (y * rt.runtime_axial_load_n);
    }
    if (placement.count > 1U) {
      p /= static_cast<float>(placement.count);
    }
    p *= placement.load_share_factor > 0.0f ? placement.load_share_factor : 1.0f;
    if (!std::isfinite(p) || p < kMinimumEquivalentLoadN) p = kMinimumEquivalentLoadN;
    return p;
  }

  float p = placement.equivalent_load_override_n;
  if (p <= 0.0f) {
    const float x = placement.x_factor_override > 0.0f ? placement.x_factor_override : spec.x_factor;
    const float y = placement.y_factor_override > 0.0f ? placement.y_factor_override : spec.y_factor;
    p = (x * placement.nominal_radial_load_n) + (y * placement.nominal_axial_load_n);
  }
  if (placement.count > 1U) {
    p /= static_cast<float>(placement.count);
  }
  p *= placement.load_share_factor > 0.0f ? placement.load_share_factor : 1.0f;
  if (!std::isfinite(p) || p < kMinimumEquivalentLoadN) p = kMinimumEquivalentLoadN;
  return p;
}

const char* load_source_for_runtime(const BearingRuntime& rt, const uint32_t now_ms) {
  if (!runtime_load_active(rt, now_ms)) return "config";
  return rt.runtime_load_source.empty() ? "runtime" : rt.runtime_load_source.c_str();
}

double rating_life_revolutions(const BearingSpec& spec, const float equivalent_load_n) {
  if (spec.dynamic_c_n <= 0.0f || equivalent_load_n <= 0.0f) return 0.0;
  const float p = spec.fatigue_exponent_p > 0.0f ? spec.fatigue_exponent_p : 3.0f;
  return 1000000.0 * std::pow(static_cast<double>(spec.dynamic_c_n / equivalent_load_n),
                              static_cast<double>(p));
}

std::string row_status(const BearingSpec* spec, const float damage_percent, const float rpm_avg) {
  if (spec == nullptr || spec->dynamic_c_n <= 0.0f) return "DATA_MISSING";
  if (spec->limiting_speed_rpm > 0.0f && rpm_avg > spec->limiting_speed_rpm) return "OVERSPEED";
  if (damage_percent >= 95.0f) return "REPLACE";
  if (damage_percent >= 80.0f) return "SERVICE_SOON";
  if (damage_percent >= 60.0f) return "WATCH";
  return "OK";
}

bool ensure_storage_dirs() {
  if (!mros::platform::mros_fs_is_mounted()) return false;
  if (!mros::platform::mros_fs_mkdir("/ESPUSER")) return false;
  return mros::platform::mros_fs_mkdir("/ESPUSER/bearing_health");
}

void write_example_config_if_missing() {
  if (!ensure_storage_dirs() || mros::platform::mros_fs_exists(kExamplePath)) return;
  constexpr std::string_view example =
      "{\n"
      "  \"schema\": \"mros.bearing_health.v1\",\n"
      "  \"source_policy\": \"Use only SKF catalog/product-page values for specs.\",\n"
      "  \"specs\": [\n"
      "    {\n"
      "      \"id\": \"skf-designation-here\",\n"
      "      \"manufacturer\": \"SKF\",\n"
      "      \"designation\": \"SKF designation here\",\n"
      "      \"type\": \"deep_groove_ball|angular_contact|tapered_roller|...\",\n"
      "      \"d_mm\": 0,\n"
      "      \"D_mm\": 0,\n"
      "      \"B_mm\": 0,\n"
      "      \"C_N\": 0,\n"
      "      \"C0_N\": 0,\n"
      "      \"Pu_N\": 0,\n"
      "      \"fatigue_exponent_p\": 3,\n"
      "      \"aSKF\": 1,\n"
      "      \"X\": 1,\n"
      "      \"Y\": 0,\n"
      "      \"reference_speed_rpm\": 0,\n"
      "      \"limiting_speed_rpm\": 0,\n"
      "      \"source_url\": \"https://www.skf.com/...\",\n"
      "      \"source_date\": \"YYYY-MM-DD\"\n"
      "    }\n"
      "  ],\n"
      "  \"placements\": [\n"
      "    {\n"
      "      \"id\": \"J1-BRG-A\",\n"
      "      \"joint_id\": \"J1\",\n"
      "      \"joint_name\": \"Base\",\n"
      "      \"joint_index\": 0,\n"
      "      \"spec_id\": \"skf-designation-here\",\n"
      "      \"count\": 1,\n"
      "      \"rotation_ratio_to_joint\": 1,\n"
      "      \"load_share_factor\": 1,\n"
      "      \"nominal_radial_load_N\": 0,\n"
      "      \"nominal_axial_load_N\": 0\n"
      "    }\n"
      "  ]\n"
      "}\n";
  (void)mros::platform::mros_file_write_all(kExamplePath, example);
}

bool parse_config_text(const std::string& raw,
                       std::vector<BearingSpec>* specs,
                       std::vector<BearingPlacement>* placements,
                       std::string* error) {
  if (specs == nullptr || placements == nullptr) return false;
  cJSON* root = cJSON_Parse(raw.c_str());
  if (root == nullptr) {
    if (error != nullptr) *error = "invalid JSON";
    return false;
  }

  std::vector<BearingSpec> next_specs;
  std::vector<BearingPlacement> next_placements;
  const cJSON* specs_json = cJSON_GetObjectItemCaseSensitive(root, "specs");
  const cJSON* placements_json = cJSON_GetObjectItemCaseSensitive(root, "placements");
  if (!cJSON_IsArray(specs_json) || !cJSON_IsArray(placements_json)) {
    cJSON_Delete(root);
    if (error != nullptr) *error = "expected specs[] and placements[]";
    return false;
  }

  const cJSON* item = nullptr;
  cJSON_ArrayForEach(item, specs_json) {
    if (!cJSON_IsObject(item) || next_specs.size() >= 64U) continue;
    BearingSpec spec {};
    spec.id = json_string(item, "id");
    spec.designation = json_string(item, "designation", spec.id.c_str());
    spec.manufacturer = json_string(item, "manufacturer", "SKF");
    spec.type = json_string(item, "type");
    spec.source_url = json_string(item, "source_url");
    spec.source_date = json_string(item, "source_date");
    spec.bore_mm = json_float(item, "d_mm");
    spec.outer_mm = json_float(item, "D_mm");
    spec.width_mm = json_float(item, "B_mm");
    spec.dynamic_c_n = json_float(item, "C_N");
    spec.static_c0_n = json_float(item, "C0_N");
    spec.fatigue_limit_pu_n = json_float(item, "Pu_N");
    spec.reference_speed_rpm = json_float(item, "reference_speed_rpm");
    spec.limiting_speed_rpm = json_float(item, "limiting_speed_rpm");
    spec.fatigue_exponent_p = json_float(item, "fatigue_exponent_p", 3.0f);
    spec.a_skf = json_float(item, "aSKF", 1.0f);
    spec.x_factor = json_float(item, "X", 1.0f);
    spec.y_factor = json_float(item, "Y", 0.0f);
    if (spec.id.empty() || spec.designation.empty()) continue;
    if (!is_skf_manufacturer(spec.manufacturer)) continue;
    if (!is_skf_source_url(spec.source_url)) continue;
    if (spec.dynamic_c_n <= 0.0f || spec.fatigue_exponent_p <= 0.0f) continue;
    if (!std::isfinite(spec.dynamic_c_n) || !std::isfinite(spec.fatigue_exponent_p)) continue;
    if (spec.a_skf <= 0.0f || !std::isfinite(spec.a_skf)) spec.a_skf = 1.0f;
    if (spec.x_factor <= 0.0f || !std::isfinite(spec.x_factor)) spec.x_factor = 1.0f;
    if (!std::isfinite(spec.y_factor)) spec.y_factor = 0.0f;
    {
      spec.manufacturer = "SKF";
      next_specs.push_back(spec);
    }
  }

  cJSON_ArrayForEach(item, placements_json) {
    if (!cJSON_IsObject(item) || next_placements.size() >= kMaxSnapshotRows) continue;
    BearingPlacement placement {};
    placement.id = json_string(item, "id");
    placement.joint_id = json_string(item, "joint_id");
    placement.joint_name = json_string(item, "joint_name", placement.joint_id.c_str());
    placement.spec_id = json_string(item, "spec_id");
    placement.joint_index = json_int(item, "joint_index", 0);
    placement.count = static_cast<uint8_t>(std::max(1, std::min(16, json_int(item, "count", 1))));
    placement.rotation_ratio_to_joint = json_float(item, "rotation_ratio_to_joint", 1.0f);
    placement.load_share_factor = json_float(item, "load_share_factor", 1.0f);
    placement.nominal_radial_load_n = json_float(item, "nominal_radial_load_N");
    placement.nominal_axial_load_n = json_float(item, "nominal_axial_load_N");
    placement.equivalent_load_override_n = json_float(item, "equivalent_load_N");
    placement.x_factor_override = json_float(item, "X");
    placement.y_factor_override = json_float(item, "Y");
    if (placement.joint_index < 0) placement.joint_index = 0;
    if (placement.joint_index > 6) placement.joint_index = 6;
    if (placement.rotation_ratio_to_joint == 0.0f ||
        !std::isfinite(placement.rotation_ratio_to_joint)) {
      placement.rotation_ratio_to_joint = 1.0f;
    }
    if (!std::isfinite(placement.load_share_factor) || placement.load_share_factor <= 0.0f) {
      placement.load_share_factor = 1.0f;
    }
    if (!placement.id.empty() && !placement.spec_id.empty() &&
        spec_exists(next_specs, placement.spec_id)) {
      next_placements.push_back(placement);
    }
  }

  cJSON_Delete(root);
  *specs = std::move(next_specs);
  *placements = std::move(next_placements);
  if (specs->empty()) {
    if (error != nullptr) *error = "no valid SKF specs; require manufacturer=SKF, skf.com source_url, C_N > 0";
    return false;
  }
  if (placements->empty()) {
    if (error != nullptr) *error = "no valid placements referencing loaded SKF specs";
    return false;
  }
  if (error != nullptr) error->clear();
  return true;
}

void apply_runtime_file_locked(const std::string& raw) {
  cJSON* root = cJSON_Parse(raw.c_str());
  if (root == nullptr) return;
  const cJSON* placements_json = cJSON_GetObjectItemCaseSensitive(root, "placements");
  if (!cJSON_IsArray(placements_json)) {
    cJSON_Delete(root);
    return;
  }
  const cJSON* item = nullptr;
  cJSON_ArrayForEach(item, placements_json) {
    if (!cJSON_IsObject(item)) continue;
    const std::string id = json_string(item, "id");
    BearingRuntime* rt = find_runtime_locked(id);
    if (rt == nullptr) continue;
    rt->total_revolutions = json_float(item, "total_revolutions");
    rt->damage_l10 = json_float(item, "damage_l10");
    rt->damage_modified = json_float(item, "damage_modified");
  }
  g_runtime_loaded = true;
  cJSON_Delete(root);
}

std::string runtime_json_locked() {
  std::string out;
  out.reserve(256U + g_runtime.size() * 128U);
  out += "{\"schema\":\"mros.bearing_runtime.v1\",\"version\":";
  out += std::to_string(kRuntimeSchemaVersion);
  out += ",\"updated_ms\":";
  out += std::to_string(mros::platform::mros_millis());
  out += ",\"placements\":[";
  bool first = true;
  for (const BearingRuntime& rt : g_runtime) {
    if (!first) out.push_back(',');
    first = false;
    char item[256] = {};
    std::snprintf(item, sizeof(item),
                  "{\"id\":\"%s\",\"total_revolutions\":%.6f,"
                  "\"damage_l10\":%.9f,\"damage_modified\":%.9f}",
                  rt.placement_id.c_str(),
                  rt.total_revolutions,
                  rt.damage_l10,
                  rt.damage_modified);
    out += item;
  }
  out += "]}";
  return out;
}

void rebuild_snapshot_locked() {
  BearingHealthSnapshot snap {};
  snap.configured = !g_specs.empty() && !g_placements.empty();
  snap.runtime_loaded = g_runtime_loaded;
  snap.runtime_persist_ok = g_runtime_persist_ok;
  snap.storage_available = mros::platform::mros_fs_is_mounted();
  snap.update_hz = 1000U / kUpdatePeriodMs;
  snap.spec_count = static_cast<uint32_t>(g_specs.size());
  snap.placement_count = static_cast<uint32_t>(g_placements.size());
  snap.last_persist_ms = g_last_persist_ms;
  snap.update_count = g_update_count;
  copy_cstr(snap.config_path, sizeof(snap.config_path), kConfigPath);
  copy_cstr(snap.runtime_path, sizeof(snap.runtime_path), kRuntimePath);
  copy_cstr(snap.config_error, sizeof(snap.config_error), g_config_error);
  const uint32_t now_ms = mros::platform::mros_millis();

  float worst_damage = 0.0f;
  bool data_missing = false;
  for (size_t i = 0; i < g_placements.size() && snap.row_count < kMaxSnapshotRows; ++i) {
    const BearingPlacement& placement = g_placements[i];
    const BearingSpec* spec = find_spec_locked(placement.spec_id);
    const BearingRuntime* rt = i < g_runtime.size() ? &g_runtime[i] : nullptr;
    BearingHealthRow& row = snap.rows[snap.row_count++];
    copy_cstr(row.placement_id, sizeof(row.placement_id), placement.id);
    copy_cstr(row.joint_id, sizeof(row.joint_id), placement.joint_id);
    copy_cstr(row.joint_name, sizeof(row.joint_name), placement.joint_name);
    copy_cstr(row.designation, sizeof(row.designation), spec != nullptr ? spec->designation : placement.spec_id);
    copy_cstr(row.manufacturer, sizeof(row.manufacturer), spec != nullptr ? spec->manufacturer : "");
    row.count = placement.count;
    if (rt != nullptr) {
      row.rpm_avg = rt->rpm_ema;
      row.total_revolutions = rt->total_revolutions;
      row.equivalent_load_n = rt->last_equivalent_load_n;
      if (runtime_load_active(*rt, now_ms)) {
        row.runtime_radial_load_n = rt->runtime_radial_load_n;
        row.runtime_axial_load_n = rt->runtime_axial_load_n;
      }
      copy_cstr(row.load_source, sizeof(row.load_source), load_source_for_runtime(*rt, now_ms));
      row.damage_percent = static_cast<float>(std::min(999.0, rt->damage_modified * 100.0));
      snap.last_update_ms = std::max(snap.last_update_ms, rt->last_update_ms);
    }
    if (spec != nullptr && rt != nullptr) {
      if (row.equivalent_load_n <= 0.0f) {
        row.equivalent_load_n = bearing_equivalent_load_n(*spec, placement, *rt, now_ms);
      }
      row.load_ratio = spec->dynamic_c_n > 0.0f ? row.equivalent_load_n / spec->dynamic_c_n : 0.0f;
      const double l10_rev = rating_life_revolutions(*spec, row.equivalent_load_n);
      const double mod_rev = l10_rev * std::max(0.01f, spec->a_skf);
      const double remaining_l10 = std::max(0.0, 1.0 - rt->damage_l10);
      const double remaining_mod = std::max(0.0, 1.0 - rt->damage_modified);
      const float rpm = std::max(kMinimumRpmForHourEstimate, std::fabs(rt->rpm_ema));
      row.remaining_l10_hours =
          l10_rev > 0.0 ? static_cast<float>((remaining_l10 * l10_rev) / (60.0 * rpm)) : 0.0f;
      row.remaining_modified_hours =
          mod_rev > 0.0 ? static_cast<float>((remaining_mod * mod_rev) / (60.0 * rpm)) : 0.0f;
      if (rt->damage_rate_ema_per_hour > kDamageRateEpsilon) {
        row.remaining_modified_hours = static_cast<float>(remaining_mod / rt->damage_rate_ema_per_hour);
      }
    } else {
      data_missing = true;
    }
    const std::string status = row_status(spec, row.damage_percent, std::fabs(row.rpm_avg));
    copy_cstr(row.status, sizeof(row.status), status);
    worst_damage = std::max(worst_damage, row.damage_percent);
  }

  if (!snap.configured) copy_cstr(snap.status, sizeof(snap.status), "UNCONFIGURED");
  else if (data_missing) copy_cstr(snap.status, sizeof(snap.status), "DATA_MISSING");
  else if (worst_damage >= 95.0f) copy_cstr(snap.status, sizeof(snap.status), "REPLACE");
  else if (worst_damage >= 80.0f) copy_cstr(snap.status, sizeof(snap.status), "SERVICE_SOON");
  else if (worst_damage >= 60.0f) copy_cstr(snap.status, sizeof(snap.status), "WATCH");
  else copy_cstr(snap.status, sizeof(snap.status), "OK");

  g_snapshot = snap;
}

bool take_mutex(const TickType_t timeout = pdMS_TO_TICKS(250)) {
  if (g_mutex == nullptr) return false;
  return xSemaphoreTake(g_mutex, timeout) == pdTRUE;
}

void give_mutex() {
  if (g_mutex != nullptr) xSemaphoreGive(g_mutex);
}

void update_once() {
  const uint32_t now = mros::platform::mros_millis();
  if (!take_mutex(pdMS_TO_TICKS(5))) return;
  for (size_t i = 0; i < g_placements.size() && i < g_runtime.size(); ++i) {
    const BearingPlacement& placement = g_placements[i];
    const BearingSpec* spec = find_spec_locked(placement.spec_id);
    BearingRuntime& rt = g_runtime[i];
    const float position_deg = telemetry_joint_position_deg(placement.joint_index);
    if (!std::isfinite(position_deg)) continue;
    if (!rt.position_valid) {
      rt.position_valid = true;
      rt.last_position_deg = position_deg;
      rt.last_update_ms = now;
      continue;
    }
    const uint32_t dt_ms = now - rt.last_update_ms;
    if (dt_ms == 0U) continue;
    const float delta_deg = shortest_delta_deg(position_deg, rt.last_position_deg);
    const double joint_rev = std::fabs(static_cast<double>(delta_deg) / 360.0);
    const double bearing_rev = joint_rev * std::fabs(static_cast<double>(placement.rotation_ratio_to_joint));
    const float rpm = static_cast<float>((joint_rev * 60000.0) / static_cast<double>(dt_ms));
    rt.rpm_ema = (rt.rpm_ema * 0.90f) + (rpm * 0.10f);
    rt.total_revolutions += bearing_rev;
    rt.last_position_deg = position_deg;
    rt.last_update_ms = now;

    if (spec != nullptr) {
      const float eq_load = bearing_equivalent_load_n(*spec, placement, rt, now);
      const double l10_rev = rating_life_revolutions(*spec, eq_load);
      const double mod_rev = l10_rev * std::max(0.01f, spec->a_skf);
      rt.last_equivalent_load_n = eq_load;
      double modified_damage_delta = 0.0;
      if (l10_rev > 0.0) {
        rt.damage_l10 += bearing_rev / l10_rev;
      }
      if (mod_rev > 0.0) {
        modified_damage_delta = bearing_rev / mod_rev;
        rt.damage_modified += modified_damage_delta;
      }
      const float hours = static_cast<float>(dt_ms) / 3600000.0f;
      const float rate = hours > 0.0f ? static_cast<float>(modified_damage_delta / hours) : 0.0f;
      rt.damage_rate_ema_per_hour = (rt.damage_rate_ema_per_hour * 0.95f) + (rate * 0.05f);
    }
  }
  ++g_update_count;
  rebuild_snapshot_locked();
  const bool should_persist = now - g_last_persist_ms >= kPersistPeriodMs;
  give_mutex();
  if (should_persist) {
    (void)persist_runtime_now();
  }
}

}  // namespace

const char* config_path() { return kConfigPath; }
const char* runtime_path() { return kRuntimePath; }

void init() {
  if (g_mutex == nullptr) {
    g_mutex = xSemaphoreCreateMutex();
  }
  if (g_initialized) return;
  g_initialized = true;
  write_example_config_if_missing();
  (void)reload_config();
}

void task(void* /*arg*/) {
  init();
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t previous_start_ms = mros::platform::mros_millis();
  while (true) {
    const uint32_t start_ms = mros::platform::mros_millis();
    update_once();
    const uint32_t end_ms = mros::platform::mros_millis();
    const uint32_t actual_period_ms = start_ms - previous_start_ms;
    previous_start_ms = start_ms;
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::BearingHealth,
                               kUpdatePeriodMs,
                               actual_period_ms,
                               end_ms - start_ms);
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kUpdatePeriodMs));
  }
}

bool reload_config() {
  if (g_mutex == nullptr) {
    g_mutex = xSemaphoreCreateMutex();
  }
  write_example_config_if_missing();

  std::string raw;
  std::vector<BearingSpec> next_specs;
  std::vector<BearingPlacement> next_placements;
  const bool read_ok = mros::platform::mros_file_read_all(kConfigPath, &raw);
  std::string error;
  const bool parse_ok = read_ok && parse_config_text(raw, &next_specs, &next_placements, &error);
  if (!read_ok) error = "config file not found";

  if (!take_mutex()) return false;
  g_specs = parse_ok ? std::move(next_specs) : std::vector<BearingSpec> {};
  g_placements = parse_ok ? std::move(next_placements) : std::vector<BearingPlacement> {};
  g_config_error = parse_ok ? std::string() : error;
  g_runtime.clear();
  g_runtime.reserve(g_placements.size());
  for (const BearingPlacement& placement : g_placements) {
    BearingRuntime rt {};
    rt.placement_id = placement.id;
    g_runtime.push_back(rt);
  }
  g_runtime_loaded = false;
  std::string runtime_raw;
  if (mros::platform::mros_file_read_all(kRuntimePath, &runtime_raw)) {
    apply_runtime_file_locked(runtime_raw);
  }
  rebuild_snapshot_locked();
  give_mutex();
  return parse_ok;
}

bool import_config_from_path(const char* source_path, std::string* error) {
  if (source_path == nullptr || source_path[0] == '\0') {
    if (error != nullptr) *error = "source path required";
    return false;
  }
  if (!ensure_storage_dirs()) {
    if (error != nullptr) *error = "storage not mounted";
    return false;
  }
  std::string raw;
  if (!mros::platform::mros_file_read_all(source_path, &raw)) {
    if (error != nullptr) *error = "cannot read source file";
    return false;
  }
  std::vector<BearingSpec> specs;
  std::vector<BearingPlacement> placements;
  std::string parse_error;
  if (!parse_config_text(raw, &specs, &placements, &parse_error)) {
    if (error != nullptr) *error = parse_error;
    return false;
  }
  if (!mros::platform::mros_file_write_all_atomic(kConfigPath, "/ESPUSER/bearing_health/bearings_skf.tmp", raw)) {
    if (error != nullptr) *error = "cannot write active config";
    return false;
  }
  const bool ok = reload_config();
  if (!ok && error != nullptr) {
    BearingHealthSnapshot snap {};
    if (get_snapshot(&snap)) *error = snap.config_error;
  }
  return ok;
}

bool reset_runtime() {
  if (!take_mutex()) return false;
  for (BearingRuntime& rt : g_runtime) {
    rt.position_valid = false;
    rt.last_position_deg = 0.0f;
    rt.rpm_ema = 0.0f;
    rt.damage_rate_ema_per_hour = 0.0f;
    rt.total_revolutions = 0.0;
    rt.damage_l10 = 0.0;
    rt.damage_modified = 0.0;
    rt.last_equivalent_load_n = 0.0f;
    rt.runtime_radial_load_n = 0.0f;
    rt.runtime_axial_load_n = 0.0f;
    rt.runtime_equivalent_load_n = 0.0f;
    rt.runtime_load_expires_ms = 0U;
    rt.runtime_load_source.clear();
    rt.last_update_ms = 0U;
  }
  rebuild_snapshot_locked();
  give_mutex();
  return persist_runtime_now();
}

bool persist_runtime_now() {
  if (!ensure_storage_dirs()) {
    g_runtime_persist_ok = false;
    return false;
  }
  std::string json;
  if (!take_mutex()) return false;
  json = runtime_json_locked();
  give_mutex();
  const bool ok = mros::platform::mros_file_write_all_atomic(kRuntimePath, kRuntimeTmpPath, json);
  if (take_mutex()) {
    g_runtime_persist_ok = ok;
    if (ok) g_last_persist_ms = mros::platform::mros_millis();
    rebuild_snapshot_locked();
    give_mutex();
  }
  return ok;
}

bool get_snapshot(BearingHealthSnapshot* snapshot) {
  if (snapshot == nullptr || !take_mutex(pdMS_TO_TICKS(50))) return false;
  *snapshot = g_snapshot;
  give_mutex();
  return true;
}

bool set_runtime_load(const char* placement_id,
                      const float radial_load_n,
                      const float axial_load_n,
                      const float equivalent_load_n,
                      const char* source,
                      const uint32_t ttl_ms,
                      std::string* error) {
  if (placement_id == nullptr || placement_id[0] == '\0') {
    if (error != nullptr) *error = "placement id required";
    return false;
  }
  if ((!std::isfinite(radial_load_n) || radial_load_n < 0.0f) ||
      (!std::isfinite(axial_load_n) || axial_load_n < 0.0f) ||
      (!std::isfinite(equivalent_load_n) || equivalent_load_n < 0.0f)) {
    if (error != nullptr) *error = "loads must be finite and non-negative";
    return false;
  }
  if (!take_mutex()) {
    if (error != nullptr) *error = "bearing lock timeout";
    return false;
  }
  BearingRuntime* rt = find_runtime_locked(placement_id);
  if (rt == nullptr) {
    give_mutex();
    if (error != nullptr) *error = "placement not configured";
    return false;
  }
  rt->runtime_radial_load_n = radial_load_n;
  rt->runtime_axial_load_n = axial_load_n;
  rt->runtime_equivalent_load_n = equivalent_load_n;
  rt->runtime_load_expires_ms =
      mros::platform::mros_millis() + (ttl_ms > 0U ? ttl_ms : kDefaultRuntimeLoadTtlMs);
  rt->runtime_load_source = source != nullptr && source[0] != '\0' ? source : "runtime";
  if (rt->runtime_load_source.size() > 23U) rt->runtime_load_source.resize(23U);
  rebuild_snapshot_locked();
  give_mutex();
  if (error != nullptr) error->clear();
  return true;
}

bool clear_runtime_load(const char* placement_id, std::string* error) {
  if (placement_id == nullptr || placement_id[0] == '\0') {
    if (error != nullptr) *error = "placement id required";
    return false;
  }
  if (!take_mutex()) {
    if (error != nullptr) *error = "bearing lock timeout";
    return false;
  }
  BearingRuntime* rt = find_runtime_locked(placement_id);
  if (rt == nullptr) {
    give_mutex();
    if (error != nullptr) *error = "placement not configured";
    return false;
  }
  rt->runtime_radial_load_n = 0.0f;
  rt->runtime_axial_load_n = 0.0f;
  rt->runtime_equivalent_load_n = 0.0f;
  rt->runtime_load_expires_ms = 0U;
  rt->runtime_load_source.clear();
  rebuild_snapshot_locked();
  give_mutex();
  if (error != nullptr) error->clear();
  return true;
}

bool apply_loads_json(const char* json_text, std::string* error) {
  if (json_text == nullptr || json_text[0] == '\0') {
    if (error != nullptr) *error = "JSON text required";
    return false;
  }
  cJSON* root = cJSON_Parse(json_text);
  if (root == nullptr) {
    if (error != nullptr) *error = "invalid JSON";
    return false;
  }
  const cJSON* rows = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "rows");
  if (!cJSON_IsArray(rows)) {
    rows = cJSON_GetObjectItemCaseSensitive(root, "bearingLoads");
  }
  if (!cJSON_IsArray(rows)) {
    cJSON_Delete(root);
    if (error != nullptr) *error = "expected rows[] or bearingLoads[]";
    return false;
  }

  uint32_t applied = 0U;
  uint32_t failed = 0U;
  const cJSON* item = nullptr;
  cJSON_ArrayForEach(item, rows) {
    if (!cJSON_IsObject(item)) continue;
    const char* placement =
        json_string(item, "placement", json_string(item, "placement_id",
                    json_string(item, "bearing_mount_id", json_string(item, "id"))));
    const float radial = json_float(item, "radial_load_N",
                         json_float(item, "runtime_radial_load_N",
                         json_float(item, "estimated_static_load_n", 0.0f)));
    const float axial = json_float(item, "axial_load_N",
                        json_float(item, "runtime_axial_load_N", 0.0f));
    const float equivalent = json_float(item, "equivalent_load_N",
                             json_float(item, "estimated_equivalent_load_N", 0.0f));
    const uint32_t ttl_ms = static_cast<uint32_t>(
        std::max(0, json_int(item, "ttl_ms", static_cast<int>(kDefaultRuntimeLoadTtlMs))));
    std::string row_error;
    if (set_runtime_load(placement,
                         radial,
                         axial,
                         equivalent,
                         json_string(item, "source", "json_load"),
                         ttl_ms,
                         &row_error)) {
      ++applied;
    } else {
      ++failed;
    }
  }
  cJSON_Delete(root);
  if (applied == 0U) {
    if (error != nullptr) *error = failed > 0U ? "no load rows applied" : "empty load rows";
    return false;
  }
  if (error != nullptr) {
    *error = "applied=" + std::to_string(applied) + " failed=" + std::to_string(failed);
  }
  return true;
}

std::string format_config_help() {
  std::string out;
  out += "bearing health: no SKF bearing config loaded\n";
  out += "config path   : ";
  out += kConfigPath;
  out += "\n";
  out += "example path  : ";
  out += kExamplePath;
  out += "\n";
  out += "policy        : fill specs only from SKF catalog/product pages, then run 'robot bearing reload'\n";
  BearingHealthSnapshot snap {};
  if (get_snapshot(&snap) && snap.config_error[0] != '\0') {
    out += "last error    : ";
    out += snap.config_error;
    out += "\n";
  }
  return out;
}

std::string format_table() {
  BearingHealthSnapshot snap {};
  if (!get_snapshot(&snap) || !snap.configured) {
    return format_config_help();
  }
  std::string out;
  char line[192] = {};
  std::snprintf(line, sizeof(line),
                "Bearing health: %s specs=%lu placements=%lu update=%luHz runtime=%s persist=%s\n",
                snap.status,
                static_cast<unsigned long>(snap.spec_count),
                static_cast<unsigned long>(snap.placement_count),
                static_cast<unsigned long>(snap.update_hz),
                snap.runtime_loaded ? "loaded" : "new",
                snap.runtime_persist_ok ? "ok" : "pending");
  out += line;
  out += "Joint      Placement        SKF designation          P/C    P(N)     Src       rpm     Used rev     Damage%  Rem SKFh   Status\n";
  out += "---------  ---------------  -----------------------  -----  -------  --------  ------  -----------  -------  ---------  ------------\n";
  for (uint32_t i = 0; i < snap.row_count; ++i) {
    const BearingHealthRow& row = snap.rows[i];
    std::snprintf(line, sizeof(line),
                  "%-9s  %-15s  %-23s  %5.3f  %7.1f  %-8s  %6.1f  %11.0f  %7.2f  %9.1f  %s\n",
                  row.joint_id,
                  row.placement_id,
                  row.designation,
                  row.load_ratio,
                  row.equivalent_load_n,
                  row.load_source,
                  row.rpm_avg,
                  row.total_revolutions,
                  row.damage_percent,
                  row.remaining_modified_hours,
                  row.status);
    out += line;
  }
  return out;
}

std::string format_json() {
  BearingHealthSnapshot snap {};
  if (!get_snapshot(&snap)) return "{\"ok\":false}";
  std::string out;
  out.reserve(384U + snap.row_count * 256U);
  out += "{\"ok\":true,\"configured\":";
  out += snap.configured ? "true" : "false";
  out += ",\"status\":\"";
  out += json_escape(snap.status);
  out += "\",\"update_hz\":";
  out += std::to_string(snap.update_hz);
  out += ",\"config_path\":\"";
  out += json_escape(snap.config_path);
  out += "\",\"runtime_path\":\"";
  out += json_escape(snap.runtime_path);
  out += "\",\"config_error\":\"";
  out += json_escape(snap.config_error);
  out += "\",\"rows\":[";
  for (uint32_t i = 0; i < snap.row_count; ++i) {
    if (i > 0U) out.push_back(',');
    const BearingHealthRow& row = snap.rows[i];
    char item[384] = {};
    std::snprintf(item, sizeof(item),
                  "{\"joint\":\"%s\",\"placement\":\"%s\",\"designation\":\"%s\","
                  "\"count\":%u,\"rpm_avg\":%.3f,\"used_revolutions\":%.3f,"
                  "\"equivalent_load_N\":%.3f,\"load_ratio\":%.6f,"
                  "\"runtime_radial_load_N\":%.3f,\"runtime_axial_load_N\":%.3f,"
                  "\"load_source\":\"%s\","
                  "\"damage_percent\":%.6f,\"remaining_l10_hours\":%.3f,"
                  "\"remaining_modified_hours\":%.3f,\"status\":\"%s\"}",
                  json_escape(row.joint_id).c_str(),
                  json_escape(row.placement_id).c_str(),
                  json_escape(row.designation).c_str(),
                  static_cast<unsigned>(row.count),
                  row.rpm_avg,
                  row.total_revolutions,
                  row.equivalent_load_n,
                  row.load_ratio,
                  row.runtime_radial_load_n,
                  row.runtime_axial_load_n,
                  json_escape(row.load_source).c_str(),
                  row.damage_percent,
                  row.remaining_l10_hours,
                  row.remaining_modified_hours,
                  json_escape(row.status).c_str());
    out += item;
  }
  out += "]}";
  return out;
}

}  // namespace mros::health::bearing
