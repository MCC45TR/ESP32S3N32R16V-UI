#include "src/core/health/actuator_health.h"

#include "src/platform/mros_time.h"

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace mros::health::actuator {
namespace {

constexpr uint32_t kDefaultTtlMs = 2000U;

struct RuntimeJointLoad {
  std::string joint_id;
  std::string link_frame_id;
  std::string status;
  std::string source;
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
  uint32_t updated_ms = 0U;
  uint32_t expires_ms = 0U;
  uint32_t ttl_ms = 0U;
};

SemaphoreHandle_t g_mutex = nullptr;
RuntimeJointLoad g_rows[kMaxActuatorSnapshotRows] {};
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

float nested_float(const cJSON* obj,
                   const char* object_key,
                   const char* value_key,
                   const float fallback = 0.0f) {
  const cJSON* child = cJSON_GetObjectItemCaseSensitive(obj, object_key);
  return cJSON_IsObject(child) ? json_float(child, value_key, fallback) : fallback;
}

int status_rank(const std::string& status) {
  if (status == "OVER_PEAK") return 5;
  if (status == "OVER_CONT") return 4;
  if (status == "WATCH") return 3;
  if (status == "OK") return 2;
  if (status == "STALE") return 1;
  return 0;
}

bool row_active(const RuntimeJointLoad& row, const uint32_t now_ms) {
  return !row.joint_id.empty() && row.expires_ms != 0U &&
         static_cast<int32_t>(row.expires_ms - now_ms) > 0;
}

RuntimeJointLoad* find_or_allocate_locked(const std::string& joint_id) {
  for (size_t i = 0; i < g_row_count; ++i) {
    if (g_rows[i].joint_id == joint_id) return &g_rows[i];
  }
  if (g_row_count >= kMaxActuatorSnapshotRows) return nullptr;
  RuntimeJointLoad& row = g_rows[g_row_count++];
  row = RuntimeJointLoad {};
  row.joint_id = joint_id;
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

void fill_snapshot_locked(ActuatorHealthSnapshot* snapshot) {
  if (snapshot == nullptr) return;
  *snapshot = ActuatorHealthSnapshot {};
  const uint32_t now_ms = mros::platform::mros_millis();
  snapshot->configured = g_row_count > 0U;
  snapshot->row_count = static_cast<uint32_t>(std::min(g_row_count, kMaxActuatorSnapshotRows));
  snapshot->update_count = g_update_count;
  copy_cstr(snapshot->source, sizeof(snapshot->source), g_source.empty() ? "none" : g_source);
  copy_cstr(snapshot->last_error, sizeof(snapshot->last_error), g_last_error);
  const std::string worst = current_worst_status_locked(now_ms);
  copy_cstr(snapshot->worst_joint_status, sizeof(snapshot->worst_joint_status), worst);
  copy_cstr(snapshot->status, sizeof(snapshot->status), worst == "UNKNOWN" ? "NO_DATA" : worst);
  for (uint32_t i = 0; i < snapshot->row_count; ++i) {
    const RuntimeJointLoad& src = g_rows[i];
    ActuatorHealthRow& dst = snapshot->rows[i];
    const bool active = row_active(src, now_ms);
    copy_cstr(dst.joint_id, sizeof(dst.joint_id), src.joint_id);
    copy_cstr(dst.link_frame_id, sizeof(dst.link_frame_id), src.link_frame_id);
    copy_cstr(dst.status, sizeof(dst.status), active ? src.status : "STALE");
    copy_cstr(dst.source, sizeof(dst.source), src.source);
    dst.axis_torque_nm = src.axis_torque_nm;
    dst.abs_axis_torque_nm = src.abs_axis_torque_nm;
    dst.continuous_limit_nm = src.continuous_limit_nm;
    dst.peak_limit_nm = src.peak_limit_nm;
    dst.nominal_continuous_limit_nm = src.nominal_continuous_limit_nm;
    dst.nominal_peak_limit_nm = src.nominal_peak_limit_nm;
    dst.current_limited_continuous_nm = src.current_limited_continuous_nm;
    dst.current_limited_peak_nm = src.current_limited_peak_nm;
    dst.continuous_margin_ratio = src.continuous_margin_ratio;
    dst.peak_margin_ratio = src.peak_margin_ratio;
    dst.total_derating_factor = src.total_derating_factor;
    dst.thermal_factor = src.thermal_factor;
    dst.voltage_factor = src.voltage_factor;
    dst.motor_temp_c = src.motor_temp_c;
    dst.controller_temp_c = src.controller_temp_c;
    dst.bus_voltage_v = src.bus_voltage_v;
    dst.ttl_ms = src.ttl_ms;
    dst.age_ms = src.updated_ms == 0U ? 0U : now_ms - src.updated_ms;
    if (src.updated_ms > snapshot->last_update_ms) snapshot->last_update_ms = src.updated_ms;
  }
}

bool parse_joint_load(const cJSON* item,
                      const uint32_t now_ms,
                      const uint32_t default_ttl_ms,
                      RuntimeJointLoad* out,
                      std::string* error) {
  if (!cJSON_IsObject(item) || out == nullptr) return false;
  const char* joint = json_string(item, "joint_id", json_string(item, "joint", ""));
  if (joint == nullptr || joint[0] == '\0') {
    if (error != nullptr) *error = "joint_id required";
    return false;
  }

  RuntimeJointLoad row {};
  row.joint_id = joint;
  row.link_frame_id = json_string(item, "link_frame_id", "");
  row.status = json_string(item, "status", "UNKNOWN");
  row.source = json_string(item, "source", "web_cad_force_snapshot");
  row.axis_torque_nm = json_float(item, "axis_torque_nm", 0.0f);
  row.abs_axis_torque_nm = json_float(item, "abs_axis_torque_nm", std::fabs(row.axis_torque_nm));
  row.continuous_limit_nm = json_float(item, "continuous_limit_nm", 0.0f);
  row.peak_limit_nm = json_float(item, "peak_limit_nm", 0.0f);
  row.nominal_continuous_limit_nm = json_float(item, "nominal_continuous_limit_nm", row.continuous_limit_nm);
  row.nominal_peak_limit_nm = json_float(item, "nominal_peak_limit_nm", row.peak_limit_nm);
  row.current_limited_continuous_nm = json_float(item, "current_limited_continuous_nm", row.continuous_limit_nm);
  row.current_limited_peak_nm = json_float(item, "current_limited_peak_nm", row.peak_limit_nm);
  row.continuous_margin_ratio = json_float(item, "continuous_margin_ratio",
      row.continuous_limit_nm > 0.0f ? row.abs_axis_torque_nm / row.continuous_limit_nm : 0.0f);
  row.peak_margin_ratio = json_float(item, "peak_margin_ratio",
      row.peak_limit_nm > 0.0f ? row.abs_axis_torque_nm / row.peak_limit_nm : 0.0f);
  row.total_derating_factor = nested_float(item, "actuator_derating", "total_derating_factor", 1.0f);
  row.thermal_factor = nested_float(item, "actuator_derating", "thermal_factor", 1.0f);
  row.voltage_factor = nested_float(item, "actuator_derating", "voltage_factor", 1.0f);
  row.motor_temp_c = nested_float(item, "actuator_derating", "motor_temp_c", 0.0f);
  row.controller_temp_c = nested_float(item, "actuator_derating", "controller_temp_c", 0.0f);
  row.bus_voltage_v = nested_float(item, "actuator_derating", "bus_voltage_v", 0.0f);
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

bool get_snapshot(ActuatorHealthSnapshot* snapshot) {
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
    if (error != nullptr) *error = "actuator lock timeout";
    return false;
  }
  for (RuntimeJointLoad& row : g_rows) row = RuntimeJointLoad {};
  g_row_count = 0U;
  ++g_update_count;
  g_source = "cleared";
  g_worst_status = "UNKNOWN";
  g_last_error.clear();
  give_mutex();
  if (error != nullptr) error->clear();
  return true;
}

bool apply_joint_loads_json(const char* json_text, const uint32_t default_ttl_ms, std::string* error) {
  if (json_text == nullptr || json_text[0] == '\0') {
    if (error != nullptr) *error = "JSON text required";
    return false;
  }
  cJSON* root = cJSON_Parse(json_text);
  if (root == nullptr) {
    if (error != nullptr) *error = "invalid JSON";
    return false;
  }
  const cJSON* rows = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "jointLoads");
  if (!cJSON_IsArray(rows)) {
    rows = cJSON_GetObjectItemCaseSensitive(root, "actuatorLoads");
  }
  if (!cJSON_IsArray(rows)) {
    cJSON_Delete(root);
    if (error != nullptr) *error = "expected jointLoads[] or actuatorLoads[]";
    return false;
  }

  RuntimeJointLoad parsed[kMaxActuatorSnapshotRows] {};
  size_t parsed_count = 0U;
  uint32_t failed = 0U;
  const uint32_t now_ms = mros::platform::mros_millis();
  const cJSON* item = nullptr;
  cJSON_ArrayForEach(item, rows) {
    if (parsed_count >= kMaxActuatorSnapshotRows) break;
    RuntimeJointLoad row {};
    std::string row_error;
    if (parse_joint_load(item, now_ms, default_ttl_ms, &row, &row_error)) {
      parsed[parsed_count++] = row;
    } else {
      ++failed;
    }
  }
  const char* source = json_string(root, "source", "web_cad_force_snapshot");
  const char* worst = json_string(root, "worstJointStatus", "");
  cJSON_Delete(root);

  if (parsed_count == 0U) {
    if (error != nullptr) *error = failed > 0U ? "no valid joint load rows" : "jointLoads[] empty";
    return false;
  }

  init();
  if (!take_mutex()) {
    if (error != nullptr) *error = "actuator lock timeout";
    return false;
  }
  uint32_t applied = 0U;
  for (size_t i = 0; i < parsed_count; ++i) {
    RuntimeJointLoad* slot = find_or_allocate_locked(parsed[i].joint_id);
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
  g_last_error = failed > 0U ? (std::to_string(failed) + " joint rows skipped") : "";
  const std::string result_error = g_last_error;
  give_mutex();

  if (error != nullptr) {
    *error = result_error;
  }
  return applied > 0U;
}

std::string format_table() {
  ActuatorHealthSnapshot snap {};
  if (!get_snapshot(&snap) || !snap.configured) {
    return "Actuator health: NO_DATA. Import web/CAD jointLoads with 'robot actuator load-json <path>'.\n";
  }
  std::string out;
  char line[224] = {};
  std::snprintf(line, sizeof(line),
                "Actuator health: %s joints=%lu worst=%s source=%s updates=%lu\n",
                snap.status,
                static_cast<unsigned long>(snap.row_count),
                snap.worst_joint_status,
                snap.source,
                static_cast<unsigned long>(snap.update_count));
  out += line;
  out += "Joint  Frame        TorqueNm  ContNm  PeakNm  Cont%  Peak%  Derate  Therm  Volt   Age  Src       Status\n";
  out += "-----  -----------  --------  ------  ------  -----  -----  ------  -----  -----  ---  --------  ------------\n";
  for (uint32_t i = 0; i < snap.row_count; ++i) {
    const ActuatorHealthRow& row = snap.rows[i];
    std::snprintf(line, sizeof(line),
                  "%-5s  %-11s  %8.2f  %6.1f  %6.1f  %5.1f  %5.1f  %6.2f  %5.2f  %5.2f  %3lu  %-8s  %s\n",
                  row.joint_id,
                  row.link_frame_id,
                  row.axis_torque_nm,
                  row.continuous_limit_nm,
                  row.peak_limit_nm,
                  row.continuous_margin_ratio * 100.0f,
                  row.peak_margin_ratio * 100.0f,
                  row.total_derating_factor,
                  row.thermal_factor,
                  row.voltage_factor,
                  static_cast<unsigned long>(row.age_ms / 1000U),
                  row.source,
                  row.status);
    out += line;
  }
  return out;
}

std::string format_json() {
  ActuatorHealthSnapshot snap {};
  if (!get_snapshot(&snap)) return "{\"ok\":false}";
  std::string out;
  out.reserve(320U + snap.row_count * 384U);
  out += "{\"ok\":true,\"configured\":";
  out += snap.configured ? "true" : "false";
  out += ",\"status\":\"";
  out += json_escape(snap.status);
  out += "\",\"worst_joint_status\":\"";
  out += json_escape(snap.worst_joint_status);
  out += "\",\"source\":\"";
  out += json_escape(snap.source);
  out += "\",\"last_error\":\"";
  out += json_escape(snap.last_error);
  out += "\",\"rows\":[";
  for (uint32_t i = 0; i < snap.row_count; ++i) {
    if (i > 0U) out.push_back(',');
    const ActuatorHealthRow& row = snap.rows[i];
    char item[640] = {};
    std::snprintf(item, sizeof(item),
                  "{\"joint\":\"%s\",\"frame\":\"%s\",\"axis_torque_Nm\":%.4f,"
                  "\"abs_axis_torque_Nm\":%.4f,\"continuous_limit_Nm\":%.4f,"
                  "\"peak_limit_Nm\":%.4f,\"nominal_continuous_limit_Nm\":%.4f,"
                  "\"nominal_peak_limit_Nm\":%.4f,\"current_limited_continuous_Nm\":%.4f,"
                  "\"current_limited_peak_Nm\":%.4f,\"continuous_margin_ratio\":%.6f,"
                  "\"peak_margin_ratio\":%.6f,\"total_derating_factor\":%.6f,"
                  "\"thermal_factor\":%.6f,\"voltage_factor\":%.6f,"
                  "\"motor_temp_C\":%.3f,\"controller_temp_C\":%.3f,"
                  "\"bus_voltage_V\":%.3f,\"age_ms\":%lu,\"ttl_ms\":%lu,"
                  "\"source\":\"%s\",\"status\":\"%s\"}",
                  json_escape(row.joint_id).c_str(),
                  json_escape(row.link_frame_id).c_str(),
                  row.axis_torque_nm,
                  row.abs_axis_torque_nm,
                  row.continuous_limit_nm,
                  row.peak_limit_nm,
                  row.nominal_continuous_limit_nm,
                  row.nominal_peak_limit_nm,
                  row.current_limited_continuous_nm,
                  row.current_limited_peak_nm,
                  row.continuous_margin_ratio,
                  row.peak_margin_ratio,
                  row.total_derating_factor,
                  row.thermal_factor,
                  row.voltage_factor,
                  row.motor_temp_c,
                  row.controller_temp_c,
                  row.bus_voltage_v,
                  static_cast<unsigned long>(row.age_ms),
                  static_cast<unsigned long>(row.ttl_ms),
                  json_escape(row.source).c_str(),
                  json_escape(row.status).c_str());
    out += item;
  }
  out += "]}";
  return out;
}

}  // namespace mros::health::actuator
