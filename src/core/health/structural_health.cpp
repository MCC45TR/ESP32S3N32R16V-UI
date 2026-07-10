#include "src/core/health/structural_health.h"

#include "src/platform/mros_time.h"

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace mros::health::structural {
namespace {

constexpr uint32_t kDefaultTtlMs = 5000U;

struct RuntimeStructuralCheck {
  std::string part_id;
  std::string link_frame_id;
  std::string material_id;
  std::string design_policy_id;
  std::string status;
  std::string source;
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
  uint32_t updated_ms = 0U;
  uint32_t expires_ms = 0U;
  uint32_t ttl_ms = 0U;
};

SemaphoreHandle_t g_mutex = nullptr;
RuntimeStructuralCheck g_rows[kMaxStructuralSnapshotRows] {};
size_t g_row_count = 0U;
uint32_t g_update_count = 0U;
std::string g_last_error;
std::string g_source;
std::string g_worst_status = "UNKNOWN";

void copy_cstr(char* dst, const size_t dst_size, const std::string& value) {
  if (dst == nullptr || dst_size == 0U) return;
  std::snprintf(dst, dst_size, "%s", value.c_str());
}

bool take_mutex(const TickType_t timeout = pdMS_TO_TICKS(250)) {
  if (g_mutex == nullptr) return false;
  return xSemaphoreTake(g_mutex, timeout) == pdTRUE;
}

void give_mutex() {
  if (g_mutex != nullptr) xSemaphoreGive(g_mutex);
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

const char* json_string(const cJSON* obj, const char* key, const char* fallback = "") {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : fallback;
}

float json_float(const cJSON* obj, const char* key, const float fallback = 0.0f) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(item) && std::isfinite(static_cast<float>(item->valuedouble))
             ? static_cast<float>(item->valuedouble)
             : fallback;
}

uint32_t json_u32(const cJSON* obj, const char* key, const uint32_t fallback = 0U) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(item) && item->valuedouble >= 0.0 ? static_cast<uint32_t>(item->valuedouble) : fallback;
}

int status_rank(const std::string& status) {
  if (status == "CRITICAL") return 6;
  if (status == "WATCH") return 5;
  if (status == "FATIGUE_WATCH") return 4;
  if (status == "LOW_MARGIN") return 3;
  if (status == "OK") return 2;
  if (status == "STALE") return 1;
  return 0;
}

bool row_active(const RuntimeStructuralCheck& row, const uint32_t now_ms) {
  return !row.part_id.empty() && row.expires_ms != 0U &&
         static_cast<int32_t>(row.expires_ms - now_ms) > 0;
}

RuntimeStructuralCheck* find_or_allocate_locked(const std::string& part_id) {
  for (size_t i = 0; i < g_row_count; ++i) {
    if (g_rows[i].part_id == part_id) return &g_rows[i];
  }
  if (g_row_count >= kMaxStructuralSnapshotRows) return nullptr;
  RuntimeStructuralCheck& row = g_rows[g_row_count++];
  row = RuntimeStructuralCheck {};
  row.part_id = part_id;
  return &row;
}

std::string current_worst_status_locked(const uint32_t now_ms) {
  std::string worst = g_row_count > 0U ? "OK" : "UNKNOWN";
  int worst_rank = status_rank(worst);
  for (size_t i = 0; i < g_row_count; ++i) {
    std::string status = row_active(g_rows[i], now_ms) ? g_rows[i].status : "STALE";
    const int rank = status_rank(status);
    if (rank > worst_rank) {
      worst = status;
      worst_rank = rank;
    }
  }
  return worst;
}

void fill_snapshot_locked(StructuralHealthSnapshot* snapshot) {
  if (snapshot == nullptr) return;
  *snapshot = StructuralHealthSnapshot {};
  const uint32_t now_ms = mros::platform::mros_millis();
  snapshot->configured = g_row_count > 0U;
  snapshot->row_count = static_cast<uint32_t>(std::min(g_row_count, kMaxStructuralSnapshotRows));
  snapshot->update_count = g_update_count;
  copy_cstr(snapshot->source, sizeof(snapshot->source), g_source.empty() ? "none" : g_source);
  copy_cstr(snapshot->last_error, sizeof(snapshot->last_error), g_last_error);
  const std::string worst = current_worst_status_locked(now_ms);
  copy_cstr(snapshot->worst_structural_status, sizeof(snapshot->worst_structural_status), worst);
  copy_cstr(snapshot->status, sizeof(snapshot->status), worst == "UNKNOWN" ? "NO_DATA" : worst);
  for (uint32_t i = 0; i < snapshot->row_count; ++i) {
    const RuntimeStructuralCheck& src = g_rows[i];
    StructuralHealthRow& dst = snapshot->rows[i];
    const bool active = row_active(src, now_ms);
    copy_cstr(dst.part_id, sizeof(dst.part_id), src.part_id);
    copy_cstr(dst.link_frame_id, sizeof(dst.link_frame_id), src.link_frame_id);
    copy_cstr(dst.material_id, sizeof(dst.material_id), src.material_id);
    copy_cstr(dst.design_policy_id, sizeof(dst.design_policy_id), src.design_policy_id);
    copy_cstr(dst.status, sizeof(dst.status), active ? src.status : "STALE");
    copy_cstr(dst.source, sizeof(dst.source), src.source);
    dst.force_n = src.force_n;
    dst.moment_nm = src.moment_nm;
    dst.von_mises_mpa = src.von_mises_mpa;
    dst.bending_stress_mpa = src.bending_stress_mpa;
    dst.shear_stress_mpa = src.shear_stress_mpa;
    dst.fatigue_stress_mpa = src.fatigue_stress_mpa;
    dst.design_yield_sf = src.design_yield_sf;
    dst.design_shear_sf = src.design_shear_sf;
    dst.design_ultimate_sf = src.design_ultimate_sf;
    dst.design_fatigue_sf = src.design_fatigue_sf;
    dst.structural_temperature_factor = src.structural_temperature_factor;
    dst.structural_reference_temp_c = src.structural_reference_temp_c;
    dst.ttl_ms = src.ttl_ms;
    dst.age_ms = src.updated_ms == 0U ? 0U : now_ms - src.updated_ms;
    if (src.updated_ms > snapshot->last_update_ms) snapshot->last_update_ms = src.updated_ms;
  }
}

bool parse_structural_check(const cJSON* item,
                            const uint32_t now_ms,
                            const uint32_t default_ttl_ms,
                            RuntimeStructuralCheck* out,
                            std::string* error) {
  if (!cJSON_IsObject(item) || out == nullptr) return false;
  const char* part = json_string(item, "part_id", json_string(item, "part", ""));
  if (part == nullptr || part[0] == '\0') {
    if (error != nullptr) *error = "part_id required";
    return false;
  }

  RuntimeStructuralCheck row {};
  row.part_id = part;
  row.link_frame_id = json_string(item, "link_frame_id", "");
  row.material_id = json_string(item, "material_id", "");
  row.design_policy_id = json_string(item, "design_policy_id", "");
  row.status = json_string(item, "status", "UNKNOWN");
  row.source = json_string(item, "source", "web_cad_force_snapshot");
  row.force_n = json_float(item, "force_n", 0.0f);
  row.moment_nm = json_float(item, "moment_about_link_origin_nm", 0.0f);
  row.von_mises_mpa = json_float(item, "von_mises_stress_mpa", 0.0f);
  row.bending_stress_mpa = json_float(item, "bending_stress_mpa", 0.0f);
  row.shear_stress_mpa = json_float(item, "shear_stress_mpa", 0.0f);
  row.fatigue_stress_mpa = json_float(item, "fatigue_alternating_stress_mpa", 0.0f);
  row.design_yield_sf = json_float(item, "design_yield_safety_factor", 0.0f);
  row.design_shear_sf = json_float(item, "design_shear_safety_factor", 0.0f);
  row.design_ultimate_sf = json_float(item, "design_ultimate_safety_factor", 0.0f);
  row.design_fatigue_sf = json_float(item, "design_fatigue_safety_factor", 0.0f);
  row.structural_temperature_factor = json_float(item, "structural_temperature_factor", 1.0f);
  row.structural_reference_temp_c = json_float(item, "structural_reference_temp_c", 25.0f);
  row.ttl_ms = json_u32(item, "ttl_ms", default_ttl_ms > 0U ? default_ttl_ms : kDefaultTtlMs);
  if (row.ttl_ms == 0U) row.ttl_ms = kDefaultTtlMs;
  row.updated_ms = now_ms;
  row.expires_ms = now_ms + row.ttl_ms;
  *out = row;
  return true;
}

}  // namespace

void init() {
  if (g_mutex == nullptr) {
    g_mutex = xSemaphoreCreateMutex();
  }
}

bool get_snapshot(StructuralHealthSnapshot* snapshot) {
  if (snapshot == nullptr) return false;
  init();
  if (!take_mutex(pdMS_TO_TICKS(50))) return false;
  fill_snapshot_locked(snapshot);
  give_mutex();
  return true;
}

bool clear(std::string* error) {
  init();
  if (!take_mutex()) {
    if (error != nullptr) *error = "structural lock timeout";
    return false;
  }
  for (RuntimeStructuralCheck& row : g_rows) row = RuntimeStructuralCheck {};
  g_row_count = 0U;
  ++g_update_count;
  g_source = "cleared";
  g_worst_status = "UNKNOWN";
  g_last_error.clear();
  give_mutex();
  if (error != nullptr) error->clear();
  return true;
}

bool apply_structural_checks_json(const char* json_text, const uint32_t default_ttl_ms, std::string* error) {
  if (json_text == nullptr || json_text[0] == '\0') {
    if (error != nullptr) *error = "JSON text required";
    return false;
  }
  cJSON* root = cJSON_Parse(json_text);
  if (root == nullptr) {
    if (error != nullptr) *error = "invalid JSON";
    return false;
  }
  const cJSON* rows = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "structuralChecks");
  if (!cJSON_IsArray(rows)) {
    rows = cJSON_GetObjectItemCaseSensitive(root, "structural");
  }
  if (!cJSON_IsArray(rows)) {
    cJSON_Delete(root);
    if (error != nullptr) *error = "expected structuralChecks[] or structural[]";
    return false;
  }

  RuntimeStructuralCheck parsed[kMaxStructuralSnapshotRows] {};
  size_t parsed_count = 0U;
  uint32_t failed = 0U;
  const uint32_t now_ms = mros::platform::mros_millis();
  const cJSON* item = nullptr;
  cJSON_ArrayForEach(item, rows) {
    if (parsed_count >= kMaxStructuralSnapshotRows) break;
    RuntimeStructuralCheck row {};
    std::string row_error;
    if (parse_structural_check(item, now_ms, default_ttl_ms, &row, &row_error)) {
      parsed[parsed_count++] = row;
    } else {
      ++failed;
    }
  }
  const char* source = json_string(root, "source", "web_cad_force_snapshot");
  const char* worst = json_string(root, "worstStructuralStatus", "");
  cJSON_Delete(root);

  if (parsed_count == 0U) {
    if (error != nullptr) *error = failed > 0U ? "no valid structural rows" : "structuralChecks[] empty";
    return false;
  }

  init();
  if (!take_mutex()) {
    if (error != nullptr) *error = "structural lock timeout";
    return false;
  }
  uint32_t applied = 0U;
  for (size_t i = 0; i < parsed_count; ++i) {
    RuntimeStructuralCheck* slot = find_or_allocate_locked(parsed[i].part_id);
    if (slot == nullptr) {
      ++failed;
      continue;
    }
    *slot = parsed[i];
    ++applied;
  }
  ++g_update_count;
  g_source = source;
  g_worst_status = worst != nullptr && worst[0] != '\0' ? worst : current_worst_status_locked(now_ms);
  g_last_error = failed > 0U ? (std::to_string(failed) + " structural rows skipped") : "";
  const std::string result_error = g_last_error;
  give_mutex();

  if (error != nullptr) *error = result_error;
  return applied > 0U;
}

std::string format_table() {
  StructuralHealthSnapshot snap {};
  if (!get_snapshot(&snap) || !snap.configured) {
    return "Structural health: NO_DATA. Import web/CAD structuralChecks with 'robot structural load-json <path>'.\n";
  }
  std::string out;
  char line[256] = {};
  std::snprintf(line, sizeof(line),
                "Structural health: %s parts=%lu worst=%s source=%s updates=%lu\n",
                snap.status,
                static_cast<unsigned long>(snap.row_count),
                snap.worst_structural_status,
                snap.source,
                static_cast<unsigned long>(snap.update_count));
  out += line;
  out += "Part              Material          VM(MPa)  YldSF  FatSF  TempF  Age  Policy      Status\n";
  out += "----------------  ----------------  -------  -----  -----  -----  ---  ----------  ------------\n";
  for (uint32_t i = 0; i < snap.row_count; ++i) {
    const StructuralHealthRow& row = snap.rows[i];
    std::snprintf(line, sizeof(line),
                  "%-16s  %-16s  %7.2f  %5.2f  %5.2f  %5.2f  %3lu  %-10s  %s\n",
                  row.part_id,
                  row.material_id,
                  row.von_mises_mpa,
                  row.design_yield_sf,
                  row.design_fatigue_sf,
                  row.structural_temperature_factor,
                  static_cast<unsigned long>(row.age_ms / 1000U),
                  row.design_policy_id,
                  row.status);
    out += line;
  }
  return out;
}

std::string format_json() {
  StructuralHealthSnapshot snap {};
  if (!get_snapshot(&snap)) return "{\"ok\":false}";
  std::string out;
  out.reserve(320U + snap.row_count * 480U);
  out += "{\"ok\":true,\"configured\":";
  out += snap.configured ? "true" : "false";
  out += ",\"status\":\"";
  out += json_escape(snap.status);
  out += "\",\"worst_structural_status\":\"";
  out += json_escape(snap.worst_structural_status);
  out += "\",\"source\":\"";
  out += json_escape(snap.source);
  out += "\",\"last_error\":\"";
  out += json_escape(snap.last_error);
  out += "\",\"rows\":[";
  for (uint32_t i = 0; i < snap.row_count; ++i) {
    if (i > 0U) out.push_back(',');
    const StructuralHealthRow& row = snap.rows[i];
    char item[768] = {};
    std::snprintf(item, sizeof(item),
                  "{\"part\":\"%s\",\"frame\":\"%s\",\"material\":\"%s\",\"policy\":\"%s\","
                  "\"force_N\":%.4f,\"moment_Nm\":%.4f,\"von_mises_MPa\":%.4f,"
                  "\"bending_MPa\":%.4f,\"shear_MPa\":%.4f,\"fatigue_MPa\":%.4f,"
                  "\"design_yield_sf\":%.6f,\"design_shear_sf\":%.6f,"
                  "\"design_ultimate_sf\":%.6f,\"design_fatigue_sf\":%.6f,"
                  "\"structural_temperature_factor\":%.6f,\"structural_reference_temp_C\":%.3f,"
                  "\"age_ms\":%lu,\"ttl_ms\":%lu,\"source\":\"%s\",\"status\":\"%s\"}",
                  json_escape(row.part_id).c_str(),
                  json_escape(row.link_frame_id).c_str(),
                  json_escape(row.material_id).c_str(),
                  json_escape(row.design_policy_id).c_str(),
                  row.force_n,
                  row.moment_nm,
                  row.von_mises_mpa,
                  row.bending_stress_mpa,
                  row.shear_stress_mpa,
                  row.fatigue_stress_mpa,
                  row.design_yield_sf,
                  row.design_shear_sf,
                  row.design_ultimate_sf,
                  row.design_fatigue_sf,
                  row.structural_temperature_factor,
                  row.structural_reference_temp_c,
                  static_cast<unsigned long>(row.age_ms),
                  static_cast<unsigned long>(row.ttl_ms),
                  json_escape(row.source).c_str(),
                  json_escape(row.status).c_str());
    out += item;
  }
  out += "]}";
  return out;
}

}  // namespace mros::health::structural
