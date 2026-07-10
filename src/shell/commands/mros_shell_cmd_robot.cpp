#include "src/shell/mros_shell_internal.h"

#include "src/app/fw_kinematics.h"
#include "src/comm_interfaces/spi/spi_c3_master.h"
#include "src/comm_interfaces/spi/spi_t41_link.h"
#include "src/comm_interfaces/uart/uart_cobs.h"
#include "src/core/health/actuator_health.h"
#include "src/core/health/bearing_health.h"
#include "src/core/health/structural_health.h"
#include "src/drivers/i2c_pca9685.h"
#include "src/experimental/experimental_worker.h"
#include "src/kinematics/inverse/inv_kinematics.h"
#include "src/kinematics/robot_model.h"
#include "src/platform/mros_file.h"
#include "src/web/server/trajectory_handler.h"
#include "src/web/web_server.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mros::shell {
namespace {

constexpr const char* kRawJsonPrefix = "@@RAW_JSON@@";
constexpr float kRobotSpeedMin = 0.25f;
constexpr float kRobotSpeedMax = 3.0f;
constexpr float kRobotSpeedStep = 0.05f;
constexpr float kRobotMoveSpeedMin = 0.1f;
constexpr float kRobotMoveSpeedMax = 1.0f;
constexpr float kRobotMoveSpeedStep = 0.05f;

struct RobotPathPoint {
  ShellRobotVector3 pos {};
  float time_ms = 1000.0f;
  bool ee_auto = true;
  float roll_deg = 0.0f;
  float ee_pitch = 0.0f;
  float yaw_deg = 0.0f;
};

struct RobotMotionBlock {
  std::string type;
  std::string payload;
};

struct RobotFrameworkState {
  uint32_t revision = 0U;
  std::vector<RobotPathPoint> path_queue;
  std::vector<RobotMotionBlock> motion_blocks;
  std::vector<std::string> profiles = {"default", "safe", "fast", "precision"};
  std::vector<std::string> models = {"mros-7dof-v1", "mros-7dof-web"};
  std::vector<std::string> frames = {"base", "tool", "world", "camera"};
  std::vector<std::string> units = {"mm", "deg", "rad"};
  std::vector<std::string> solvers = {"dls", "qp", "svd-robust"};
  std::vector<std::string> jacobian_modes = {"numerical", "geometric", "spatial"};
  std::vector<std::string> nullspace_modes = {"joint_centering", "off"};
  std::vector<std::string> trajectory_modes = {"quintic", "heptic", "scurve", "time-optimal", "linear"};
  std::vector<std::string> seed_policies = {"current", "zero", "center", "multi"};
  std::vector<std::string> limits_profiles = {"default", "safe", "precision"};
  std::vector<std::string> path_height_modes = {"ground", "elevated"};
  std::vector<std::string> turret_modes = {"auto_shortest", "shortest", "stable"};
  std::string current_profile = "default";
  std::string current_model = "mros-7dof-v1";
  std::string current_frame = "base";
  std::string current_units = "mm";
  std::string current_solver = "dls";
  std::string current_jacobian = "numerical";
  std::string current_nullspace = "joint_centering";
  std::string current_trajectory = "quintic";
  std::string seed_policy = "current";
  std::string limits_profile = "default";
  std::string path_height_mode = "ground";
  std::string turret_mode = "auto_shortest";
  float pos_tol_mm = 0.5f;
  float ori_tol_deg = 0.5f;
  float singularity_threshold = 5.0f;
  float alpha_step = 0.5f;
  float null_gain = 0.1f;
  float lambda_max = 0.5f;
  float max_step_deg = 10.0f;
  float ground_z_mm = 0.0f;
  float cart_step_mm = 8.0f;
  float yaw_step_deg = 4.0f;
  float jump_revolute_deg = 18.0f;
  bool allow_negative_z_input = false;
  uint16_t max_iter = 500U;
};

RobotFrameworkState g_robot_state {};
bool g_robot_math_synced = false;

std::string lower_copy(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch >= 'A' && ch <= 'Z') out.push_back(static_cast<char>(ch - 'A' + 'a'));
    else out.push_back(ch);
  }
  return out;
}

std::string join_tokens(const std::vector<std::string>& args, size_t start_index) {
  std::string out;
  for (size_t i = start_index; i < args.size(); ++i) {
    if (!out.empty()) out.push_back(' ');
    out += args[i];
  }
  return out;
}

std::string json_escape(const std::string& text) {
  std::string escaped;
  escaped.reserve(text.size() + 16U);
  for (const char ch : text) {
    switch (ch) {
      case '\"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          char encoded[8] = {};
          std::snprintf(encoded, sizeof(encoded), "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(ch)));
          escaped += encoded;
        } else {
          escaped.push_back(ch);
        }
        break;
    }
  }
  return escaped;
}

class JsonObjectWriter {
 public:
  explicit JsonObjectWriter(size_t reserve_bytes = 256U) {
    json_.reserve(reserve_bytes);
    json_.push_back('{');
  }

  void add_raw(const char* key, const std::string& value) {
    add_key(key);
    json_ += value;
  }

  void add_bool(const char* key, bool value) {
    add_key(key);
    json_ += value ? "true" : "false";
  }

  std::string finish() {
    json_.push_back('}');
    return std::move(json_);
  }

 private:
  void add_key(const char* key) {
    if (!first_) {
      json_.push_back(',');
    }
    first_ = false;
    json_.push_back('"');
    json_ += key != nullptr ? key : "";
    json_ += "\":";
  }

  std::string json_;
  bool first_ = true;
};

void copy_field(char* dst, const size_t dst_size, const std::string& value) {
  if (dst == nullptr || dst_size == 0U) return;
  std::snprintf(dst, dst_size, "%s", value.c_str());
}

bool parse_float_text(const std::string& text, float* out) {
  if (out == nullptr || text.empty()) return false;
  char* end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || (end != nullptr && *end != '\0') || !std::isfinite(value)) return false;
  *out = value;
  return true;
}

bool parse_u32_text(const std::string& text, uint32_t* out) {
  if (out == nullptr || text.empty()) return false;
  char* end = nullptr;
  const unsigned long value = std::strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
  *out = static_cast<uint32_t>(value);
  return true;
}

bool parse_size_text(const std::string& text, size_t* out) {
  uint32_t value = 0U;
  if (!parse_u32_text(text, &value) || out == nullptr) return false;
  *out = static_cast<size_t>(value);
  return true;
}

bool parse_on_off(const std::string& text, bool* out) {
  if (out == nullptr) return false;
  const std::string key = lower_copy(text);
  if (key == "on" || key == "1" || key == "true") {
    *out = true;
    return true;
  }
  if (key == "off" || key == "0" || key == "false") {
    *out = false;
    return true;
  }
  return false;
}

std::string canonical_backend_mode(const std::string& text) {
  const std::string key = lower_copy(text);
  if (key == "auto") return "auto";
  if (key == "web") return "web";
  if (key == "onboard" || key == "onboard-s3" || key == "s3" || key == "device") return "onboard-s3";
  if (key == "t41" || key == "spi" || key == "t41-qspi" || key == "t41" ||
      key == "t41-qspi" || key == "teensy" || key == "teensy41") return "t41-qspi";
  if (key == "espnow" || key == "t41-espnow" || key == "T41-ESP-NOW" ||
      key == "t41-espnow" || key == "teensy-espnow") return "T41-ESP-NOW";
  if (key == "status") return "status";
  return "";
}

std::string canonical_units(const std::string& text) {
  const std::string key = lower_copy(text);
  if (key == "mm" || key == "deg" || key == "rad") return key;
  return "";
}

std::string canonical_solver(const std::string& text) {
  const std::string key = lower_copy(text);
  if (key == "dls" || key == "damped-least-squares") return "dls";
  if (key == "qp" || key == "qp-projected" || key == "qp_projected" || key == "box_qp") return "qp";
  if (key == "svd-robust" || key == "robust" || key == "svd") return "svd-robust";
  if (key == "numeric-web" || key == "t41-native" || key == "stub-local" || key == "web" || key == "t41" || key == "stub") {
    return "dls";
  }
  return "";
}

std::string canonical_jacobian_mode(const std::string& text) {
  const std::string key = lower_copy(text);
  if (key == "numerical" || key == "numeric") return "numerical";
  if (key == "geometric" || key == "analytic") return "geometric";
  if (key == "spatial" || key == "poe" || key == "spatial-poe") return "spatial";
  return "";
}

std::string worker_json_string(const String& text) {
  return std::string(text.c_str());
}

bool arg_present(const std::vector<std::string>& args, const char* needle) {
  return std::find(args.begin(), args.end(), needle) != args.end();
}

void fill_current_joint_seed(float seed[7]) {
  if (seed == nullptr) return;
  seed[0] = spi_s3_get_turret_deg();
  seed[1] = spi_s3_get_joint_deg(0);
  seed[2] = spi_s3_get_joint_deg(1);
  seed[3] = spi_s3_get_joint_deg(2);
  seed[4] = spi_s3_get_joint_deg(3);
  seed[5] = spi_s3_get_joint_deg(4);
  seed[6] = spi_s3_get_joint_deg(5);
}

std::string canonical_nullspace_mode(const std::string& text) {
  const std::string key = lower_copy(text);
  if (key == "joint_centering" || key == "joint-centering" || key == "centering") return "joint_centering";
  if (key == "off" || key == "none" || key == "disabled") return "off";
  return "";
}

std::string canonical_seed_policy(const std::string& text) {
  const std::string key = lower_copy(text);
  if (key == "current" || key == "zero" || key == "center" || key == "multi") return key;
  return "";
}

std::string canonical_limits_profile(const std::string& text) {
  const std::string key = lower_copy(text);
  if (key == "default" || key == "safe" || key == "precision") return key;
  return "";
}

std::string canonical_trajectory_mode(const std::string& text) {
  const std::string key = lower_copy(text);
  if (key == "quintic" || key == "heptic" || key == "linear") return key;
  if (key == "scurve" || key == "s-curve") return "scurve";
  if (key == "time-optimal" || key == "time_optimal" || key == "trap" || key == "trapezoidal") return "time-optimal";
  return "";
}

std::string canonical_path_height_mode(const std::string& text) {
  const std::string key = lower_copy(text);
  if (key == "ground" || key == "zeminde") return "ground";
  if (key == "elevated" || key == "yukseltide" || key == "yükseltide") return "elevated";
  return "";
}

std::string canonical_turret_mode(const std::string& text) {
  const std::string key = lower_copy(text);
  if (key == "auto_shortest" || key == "auto-shortest" || key == "auto" || key == "oto") return "auto_shortest";
  if (key == "shortest" || key == "en-kisa" || key == "en_kisa" || key == "enkisa") return "shortest";
  if (key == "stable" || key == "stabil") return "stable";
  return "";
}

bool parse_vector_text(const std::string& text, ShellRobotVector3* out) {
  if (out == nullptr) return false;
  std::string normalized = text;
  for (char& ch : normalized) {
    if (ch == ',' || ch == ';') ch = ' ';
  }
  std::stringstream stream(normalized);
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  if (!(stream >> x >> y >> z)) return false;
  stream >> std::ws;
  if (!stream.eof()) return false;
  out->x = x;
  out->y = y;
  out->z = z;
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool parse_vector_arg(
    const std::vector<std::string>& args,
    const size_t start_index,
    ShellRobotVector3* out,
    size_t* next_index,
    std::string* error) {
  if (out == nullptr || next_index == nullptr) return false;
  if (start_index >= args.size()) {
    if (error != nullptr) *error = "missing vector argument";
    return false;
  }

  const std::string& token = args[start_index];
  if (token == "[" || (!token.empty() && token.front() == '[')) {
    std::string vector_text;
    bool closed = false;
    size_t i = start_index;
    for (; i < args.size(); ++i) {
      std::string part = args[i];
      if (i == start_index) {
        if (part == "[") part.clear();
        else if (!part.empty() && part.front() == '[') part.erase(part.begin());
      }
      const size_t close_pos = part.find(']');
      if (close_pos != std::string::npos) {
        const std::string before_close = part.substr(0U, close_pos);
        if (!before_close.empty()) {
          if (!vector_text.empty()) vector_text.push_back(' ');
          vector_text += before_close;
        }
        if (part.find(']', close_pos + 1U) != std::string::npos || (close_pos + 1U) != part.size()) {
          if (error != nullptr) *error = "invalid vector syntax";
          return false;
        }
        closed = true;
        ++i;
        *next_index = i;
        break;
      }
      if (!part.empty()) {
        if (!vector_text.empty()) vector_text.push_back(' ');
        vector_text += part;
      }
    }
    if (!closed) {
      if (error != nullptr) *error = "missing closing ']'";
      return false;
    }
    if (!parse_vector_text(vector_text, out)) {
      if (error != nullptr) *error = "vector must contain exactly three numeric values";
      return false;
    }
    return true;
  }

  if ((start_index + 2U) >= args.size()) {
    if (error != nullptr) *error = "vector requires x y z values";
    return false;
  }
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  if (!parse_float_text(args[start_index], &x) ||
      !parse_float_text(args[start_index + 1U], &y) ||
      !parse_float_text(args[start_index + 2U], &z)) {
    if (error != nullptr) *error = "vector expects numeric x y z values";
    return false;
  }
  out->x = x;
  out->y = y;
  out->z = z;
  *next_index = start_index + 3U;
  return true;
}

bool parse_joint_values(
    const std::vector<std::string>& args,
    const size_t start_index,
    ShellRobotRequest* request,
    size_t* next_index,
    std::string* error) {
  if (request == nullptr || next_index == nullptr) return false;
  if (start_index >= args.size()) {
    if (error != nullptr) *error = "joint set requires 7 angles";
    return false;
  }
  std::vector<float> values;
  if (args[start_index] == "[" || (!args[start_index].empty() && args[start_index].front() == '[')) {
    std::string text;
    bool closed = false;
    size_t i = start_index;
    for (; i < args.size(); ++i) {
      std::string part = args[i];
      if (i == start_index) {
        if (part == "[") part.clear();
        else if (!part.empty() && part.front() == '[') part.erase(part.begin());
      }
      const size_t close_pos = part.find(']');
      if (close_pos != std::string::npos) {
        const std::string before_close = part.substr(0U, close_pos);
        if (!before_close.empty()) {
          if (!text.empty()) text.push_back(' ');
          text += before_close;
        }
        closed = true;
        ++i;
        *next_index = i;
        break;
      }
      if (!part.empty()) {
        if (!text.empty()) text.push_back(' ');
        text += part;
      }
    }
    if (!closed) {
      if (error != nullptr) *error = "missing closing ']' in joint list";
      return false;
    }
    std::stringstream stream(text);
    float value = 0.0f;
    while (stream >> value) values.push_back(value);
  } else {
    if ((start_index + 6U) >= args.size()) {
      if (error != nullptr) *error = "joint set requires 7 numeric values";
      return false;
    }
    for (size_t i = 0U; i < 7U; ++i) {
      float value = 0.0f;
      if (!parse_float_text(args[start_index + i], &value)) {
        if (error != nullptr) *error = "joint values must be numeric";
        return false;
      }
      values.push_back(value);
    }
    *next_index = start_index + 7U;
  }
  if (values.size() != 7U) {
    if (error != nullptr) *error = "joint set requires exactly 7 values";
    return false;
  }
  for (size_t i = 0U; i < 7U; ++i) request->joints[i] = values[i];
  request->joint_count = 7U;
  return true;
}

float normalize_speed_up(const float raw) {
  float clamped = raw;
  if (clamped < kRobotSpeedMin) clamped = kRobotSpeedMin;
  if (clamped > kRobotSpeedMax) clamped = kRobotSpeedMax;
  const float stepped = std::ceil((clamped / kRobotSpeedStep) - 1e-5f) * kRobotSpeedStep;
  return std::max(kRobotSpeedMin, std::min(kRobotSpeedMax, stepped));
}

float normalize_move_speed(const float raw) {
  float clamped = raw;
  if (clamped < kRobotMoveSpeedMin) clamped = kRobotMoveSpeedMin;
  if (clamped > kRobotMoveSpeedMax) clamped = kRobotMoveSpeedMax;
  const float stepped = std::ceil((clamped / kRobotMoveSpeedStep) - 1e-5f) * kRobotMoveSpeedStep;
  return std::max(kRobotMoveSpeedMin, std::min(kRobotMoveSpeedMax, stepped));
}

void reset_request_defaults(ShellRobotRequest* request) {
  if (request == nullptr) return;
  request->time_ms = 1000.0f;
  request->ee_auto = true;
  request->roll_deg = 0.0f;
  request->pitch_deg = 0.0f;
  request->yaw_deg = 0.0f;
  request->ee_pitch = 0.0f;
  request->apply = false;
  request->wait = false;
  request->timeout_ms = 0U;
  copy_field(request->calc_mode, sizeof(request->calc_mode), "auto");
  copy_field(request->profile, sizeof(request->profile), g_robot_state.current_profile);
  copy_field(request->frame, sizeof(request->frame), g_robot_state.current_frame);
  copy_field(request->units, sizeof(request->units), g_robot_state.current_units);
  copy_field(request->model, sizeof(request->model), g_robot_state.current_model);
  request->joint_count = 0U;
}

bool parse_common_options(
    const std::vector<std::string>& args,
    size_t index,
    ShellRobotRequest* request,
    std::string* error,
    const bool allow_alpha,
    const bool allow_time) {
  if (request == nullptr) return false;
  while (index < args.size()) {
    const std::string key = lower_copy(args[index]);
    if (key == "--apply" || key == "apply") {
      request->apply = true;
      ++index;
      continue;
    }
    if (key == "--preview" || key == "preview") {
      request->apply = false;
      ++index;
      continue;
    }
    if (key == "--wait" || key == "wait") {
      request->wait = true;
      ++index;
      continue;
    }
    if (key == "--timeout" || key == "timeout") {
      if ((index + 1U) >= args.size() || !parse_u32_text(args[index + 1U], &request->timeout_ms)) {
        if (error != nullptr) *error = "timeout must be a numeric millisecond value";
        return false;
      }
      index += 2U;
      continue;
    }
    if (key == "--calc" || key == "calc" || key == "--mode" || key == "mode" || key == "--backend" || key == "backend") {
      if ((index + 1U) >= args.size()) {
        if (error != nullptr) *error = "missing backend mode";
        return false;
      }
      const std::string mode = canonical_backend_mode(args[index + 1U]);
      if (mode.empty() || mode == "status") {
        if (error != nullptr) *error = "backend must be auto, web, t41-qspi or t41-esp-now";
        return false;
      }
      copy_field(request->calc_mode, sizeof(request->calc_mode), mode);
      index += 2U;
      continue;
    }
    if (key == "--profile" || key == "profile") {
      if ((index + 1U) >= args.size()) {
        if (error != nullptr) *error = "missing profile name";
        return false;
      }
      copy_field(request->profile, sizeof(request->profile), args[index + 1U]);
      index += 2U;
      continue;
    }
    if (key == "--frame" || key == "frame") {
      if ((index + 1U) >= args.size()) {
        if (error != nullptr) *error = "missing frame name";
        return false;
      }
      copy_field(request->frame, sizeof(request->frame), lower_copy(args[index + 1U]));
      index += 2U;
      continue;
    }
    if (key == "--units" || key == "units") {
      if ((index + 1U) >= args.size()) {
        if (error != nullptr) *error = "missing units";
        return false;
      }
      const std::string units = canonical_units(args[index + 1U]);
      if (units.empty()) {
        if (error != nullptr) *error = "units must be mm, deg or rad";
        return false;
      }
      copy_field(request->units, sizeof(request->units), units);
      index += 2U;
      continue;
    }
    if (key == "--model" || key == "model") {
      if ((index + 1U) >= args.size()) {
        if (error != nullptr) *error = "missing model name";
        return false;
      }
      copy_field(request->model, sizeof(request->model), args[index + 1U]);
      index += 2U;
      continue;
    }
    if (allow_time && (key == "--time" || key == "-t" || key == "time")) {
      if ((index + 1U) >= args.size() || !parse_float_text(args[index + 1U], &request->time_ms) ||
          request->time_ms < 1.0f) {
        if (error != nullptr) *error = "time must be a positive millisecond value";
        return false;
      }
      index += 2U;
      continue;
    }
    if (allow_time && (key == "--speed" || key == "-s" || key == "speed")) {
      float speed = 0.0f;
      if ((index + 1U) >= args.size() || !parse_float_text(args[index + 1U], &speed) ||
          speed < kRobotMoveSpeedMin || speed > kRobotMoveSpeedMax) {
        if (error != nullptr) *error = "speed must be a numeric value from 0.1 to 1.0";
        return false;
      }
      request->value = normalize_move_speed(speed);
      index += 2U;
      continue;
    }
    if (allow_alpha && (key == "--roll" || key == "roll")) {
      if ((index + 1U) >= args.size() || !parse_float_text(args[index + 1U], &request->roll_deg)) {
        if (error != nullptr) *error = "roll must be numeric";
        return false;
      }
      request->ee_auto = false;
      index += 2U;
      continue;
    }
    if (allow_alpha && (key == "--yaw" || key == "yaw")) {
      if ((index + 1U) >= args.size() || !parse_float_text(args[index + 1U], &request->yaw_deg)) {
        if (error != nullptr) *error = "yaw must be numeric";
        return false;
      }
      request->ee_auto = false;
      index += 2U;
      continue;
    }
    if (allow_alpha && (key == "--alpha" || key == "alpha" || key == "--pitch" || key == "pitch")) {
      if ((index + 1U) >= args.size()) {
        if (error != nullptr) *error = "missing alpha value";
        return false;
      }
      if (lower_copy(args[index + 1U]) == "auto") {
        request->ee_auto = true;
        request->pitch_deg = 0.0f;
        request->ee_pitch = 0.0f;
      } else if (parse_float_text(args[index + 1U], &request->pitch_deg)) {
        request->ee_auto = false;
        request->ee_pitch = request->pitch_deg;
      } else {
        if (error != nullptr) *error = "alpha must be auto or numeric";
        return false;
      }
      index += 2U;
      continue;
    }
    if (error != nullptr) *error = "unknown option '" + args[index] + "'";
    return false;
  }
  return true;
}

bool is_common_option_key(const std::string& key) {
  return key == "--apply" || key == "apply" || key == "--preview" || key == "preview" ||
         key == "--wait" || key == "wait" || key == "--timeout" || key == "timeout" ||
         key == "--calc" || key == "calc" || key == "--mode" || key == "mode" ||
         key == "--backend" || key == "backend" || key == "--profile" || key == "profile" ||
         key == "--frame" || key == "frame" || key == "--units" || key == "units" ||
         key == "--model" || key == "model" || key == "--time" || key == "-t" ||
         key == "time" || key == "--speed" || key == "-s" || key == "speed" ||
         key == "--alpha" || key == "alpha" || key == "--pitch" || key == "pitch" ||
         key == "--roll" || key == "roll" || key == "--yaw" || key == "yaw";
}

bool parse_move_vectors(
    const std::vector<std::string>& args,
    const size_t start_index,
    ShellRobotRequest* request,
    size_t* next_index,
    std::string* error) {
  if (request == nullptr || next_index == nullptr) return false;
  bool has_from = false;
  bool has_to = false;
  size_t i = start_index;
  while (i < args.size()) {
    const std::string key = lower_copy(args[i]);
    if (is_common_option_key(key)) break;
    if (key == "--from" || key == "from") {
      size_t ni = 0U;
      if (!parse_vector_arg(args, i + 1U, &request->from, &ni, error)) return false;
      has_from = true;
      i = ni;
      continue;
    }
    if (key == "--to" || key == "to") {
      size_t ni = 0U;
      if (!parse_vector_arg(args, i + 1U, &request->to, &ni, error)) return false;
      has_to = true;
      i = ni;
      continue;
    }
    if (!has_from) {
      size_t ni = 0U;
      if (!parse_vector_arg(args, i, &request->from, &ni, error)) return false;
      has_from = true;
      i = ni;
      continue;
    }
    if (!has_to) {
      size_t ni = 0U;
      if (!parse_vector_arg(args, i, &request->to, &ni, error)) return false;
      has_to = true;
      i = ni;
      continue;
    }
    break;
  }
  if (!has_from || !has_to) {
    if (error != nullptr) *error = "move requires --from x y z --to x y z, or [x y z] [x y z]";
    return false;
  }
  *next_index = i;
  return true;
}

bool parse_move_target_only(
    const std::vector<std::string>& args,
    const size_t start_index,
    ShellRobotRequest* request,
    size_t* next_index,
    std::string* error) {
  if (request == nullptr || next_index == nullptr) return false;
  if (start_index >= args.size()) {
    if (error != nullptr) *error = "move target requires x y z values";
    return false;
  }
  const std::string key = lower_copy(args[start_index]);
  if (is_common_option_key(key) || key == "--from" || key == "from" || key == "--to" || key == "to") {
    if (error != nullptr) *error = "move target requires x y z values before options";
    return false;
  }
  size_t ni = 0U;
  if (!parse_vector_arg(args, start_index, &request->to, &ni, error)) return false;
  request->from.x = spi_s3_get_coord_x();
  request->from.y = spi_s3_get_coord_y();
  request->from.z = spi_s3_get_coord_z();
  *next_index = ni;
  return true;
}

std::string resource_name(const ShellRobotResource resource) {
  switch (resource) {
    case ShellRobotResource::Power: return "power";
    case ShellRobotResource::Safety: return "safety";
    case ShellRobotResource::Status: return "status";
    case ShellRobotResource::Telemetry: return "telemetry";
    case ShellRobotResource::Turret: return "turret";
    case ShellRobotResource::Gripper: return "gripper";
    case ShellRobotResource::Joint: return "joint";
    case ShellRobotResource::Cartesian: return "cartesian";
    case ShellRobotResource::Move: return "move";
    case ShellRobotResource::Path: return "path";
    case ShellRobotResource::Motion: return "motion";
    case ShellRobotResource::Profile: return "profile";
    case ShellRobotResource::Model: return "model";
    case ShellRobotResource::Frame: return "frame";
    case ShellRobotResource::Limits: return "limits";
    case ShellRobotResource::Math: return "math";
    case ShellRobotResource::Calibration: return "calibration";
    case ShellRobotResource::Diagnostics: return "diag";
    default: return "unknown";
  }
}

std::string verb_name(const ShellRobotVerb verb) {
  switch (verb) {
    case ShellRobotVerb::List: return "list";
    case ShellRobotVerb::Status: return "status";
    case ShellRobotVerb::Get: return "get";
    case ShellRobotVerb::Set: return "set";
    case ShellRobotVerb::Add: return "add";
    case ShellRobotVerb::Remove: return "remove";
    case ShellRobotVerb::Clear: return "clear";
    case ShellRobotVerb::Preview: return "preview";
    case ShellRobotVerb::Apply: return "apply";
    case ShellRobotVerb::Run: return "run";
    case ShellRobotVerb::Stop: return "stop";
    case ShellRobotVerb::Hold: return "hold";
    case ShellRobotVerb::Reset: return "reset";
    case ShellRobotVerb::Home: return "home";
    case ShellRobotVerb::Park: return "park";
    case ShellRobotVerb::Solve: return "solve";
    case ShellRobotVerb::Validate: return "validate";
    case ShellRobotVerb::Export: return "export";
    case ShellRobotVerb::Import: return "import";
    case ShellRobotVerb::Describe: return "describe";
    case ShellRobotVerb::Compile: return "compile";
    case ShellRobotVerb::Compare: return "compare";
    case ShellRobotVerb::Benchmark: return "benchmark";
    case ShellRobotVerb::Explain: return "explain";
    case ShellRobotVerb::Open: return "open";
    case ShellRobotVerb::Close: return "close";
    case ShellRobotVerb::Insert: return "insert";
    case ShellRobotVerb::Delete: return "delete";
    case ShellRobotVerb::Jog: return "jog";
    case ShellRobotVerb::Zero: return "zero";
    default: return "unknown";
  }
}

uint32_t next_revision() {
  g_robot_state.revision += 1U;
  if (g_robot_state.revision == 0U) g_robot_state.revision = 1U;
  return g_robot_state.revision;
}

void emit_robot_result(
    ShellContext& ctx,
    const bool ok,
    const ShellRobotRequest& request,
    const std::string& text,
    const std::string& result_json) {
  const uint32_t revision = next_revision();
  if (ctx.json_output) {
    std::string json = kRawJsonPrefix;
    json += "{";
    json += "\"ok\":";
    json += ok ? "true" : "false";
    json += ",\"resource\":\"" + json_escape(resource_name(request.resource)) + "\"";
    json += ",\"verb\":\"" + json_escape(verb_name(request.verb)) + "\"";
    json += ",\"mode\":{";
    json += "\"preview\":" + std::string(request.apply ? "false" : "true");
    json += ",\"apply\":" + std::string(request.apply ? "true" : "false");
    json += ",\"wait\":" + std::string(request.wait ? "true" : "false");
    json += ",\"timeout_ms\":" + std::to_string(request.timeout_ms);
    json += ",\"backend\":\"" + json_escape(request.calc_mode[0] != '\0' ? request.calc_mode : "auto") + "\"";
    json += ",\"profile\":\"" + json_escape(request.profile) + "\"";
    json += ",\"frame\":\"" + json_escape(request.frame) + "\"";
    json += ",\"units\":\"" + json_escape(request.units) + "\"";
    json += "}";
    json += ",\"source\":\"shell\"";
    json += ",\"revision\":" + std::to_string(revision);
    json += ",\"result\":";
    json += result_json.empty() ? "{\"message\":\"\"}" : result_json;
    json += "}";
    shell_write(ctx.state, json.c_str());
    return;
  }
  shell_write_line(ctx.state, text.c_str());
}

bool dispatch_robot_request(ShellContext& ctx, const ShellRobotRequest& request, std::string* out_text) {
  if (ctx.state.config.robot_action_callback == nullptr || out_text == nullptr) return false;
  char message[768] = {};
  const bool ok = ctx.state.config.robot_action_callback(request, message, sizeof(message), ctx.state.config.user_data);
  *out_text = message[0] != '\0' ? message : (ok ? "robot: command accepted" : "robot: command failed");
  return ok;
}

void sync_path_queue_to_web(ShellContext& ctx) {
  if (ctx.state.config.robot_action_callback == nullptr) return;
  ShellRobotRequest req {};
  reset_request_defaults(&req);
  req.action = ShellRobotAction::PathClear;
  req.resource = ShellRobotResource::Path;
  req.verb = ShellRobotVerb::Clear;
  char message[256] = {};
  ctx.state.config.robot_action_callback(req, message, sizeof(message), ctx.state.config.user_data);
  for (const RobotPathPoint& point : g_robot_state.path_queue) {
    ShellRobotRequest add {};
    reset_request_defaults(&add);
    add.action = ShellRobotAction::PathAdd;
    add.resource = ShellRobotResource::Path;
    add.verb = ShellRobotVerb::Add;
    add.position = point.pos;
    add.time_ms = point.time_ms;
    add.ee_auto = point.ee_auto;
    add.roll_deg = point.roll_deg;
    add.pitch_deg = point.ee_pitch;
    add.yaw_deg = point.yaw_deg;
    add.ee_pitch = point.ee_pitch;
    ctx.state.config.robot_action_callback(add, message, sizeof(message), ctx.state.config.user_data);
  }
}

std::string path_queue_result_json() {
  std::string json = "{";
  json.reserve(64U + g_robot_state.path_queue.size() * 160U);
  json += "\"count\":" + std::to_string(g_robot_state.path_queue.size()) + ",";
  json += "\"points\":[";
  for (size_t i = 0U; i < g_robot_state.path_queue.size(); ++i) {
    const RobotPathPoint& p = g_robot_state.path_queue[i];
    if (i > 0U) json += ",";
    json += "{";
    json += "\"x\":" + std::to_string(p.pos.x) + ",";
    json += "\"y\":" + std::to_string(p.pos.y) + ",";
    json += "\"z\":" + std::to_string(p.pos.z) + ",";
    json += "\"t_ms\":" + std::to_string(p.time_ms) + ",";
    json += "\"ee_auto\":" + std::string(p.ee_auto ? "true" : "false") + ",";
    json += "\"roll_deg\":" + std::to_string(p.roll_deg) + ",";
    json += "\"pitch_deg\":" + std::to_string(p.ee_pitch) + ",";
    json += "\"yaw_deg\":" + std::to_string(p.yaw_deg) + ",";
    json += "\"ee_pitch\":" + std::to_string(p.ee_pitch);
    json += "}";
  }
  json += "]";
  json += "}";
  return json;
}

std::string motion_blocks_json() {
  std::string json = "{";
  json.reserve(64U + g_robot_state.motion_blocks.size() * 96U);
  json += "\"count\":" + std::to_string(g_robot_state.motion_blocks.size()) + ",";
  json += "\"blocks\":[";
  for (size_t i = 0U; i < g_robot_state.motion_blocks.size(); ++i) {
    if (i > 0U) json += ",";
    json += "{\"type\":\"" + json_escape(g_robot_state.motion_blocks[i].type) + "\",";
    json += "\"payload\":\"" + json_escape(g_robot_state.motion_blocks[i].payload) + "\"}";
  }
  json += "]}";
  return json;
}

std::string json_string_array(const std::vector<std::string>& values) {
  std::string json = "[";
  json.reserve(2U + values.size() * 16U);
  for (size_t i = 0U; i < values.size(); ++i) {
    if (i > 0U) json += ",";
    json += "\"";
    json += json_escape(values[i]);
    json += "\"";
  }
  json += "]";
  return json;
}

std::string ik_warnings_json(const uint32_t warnings) {
  std::vector<std::string> values;
  if ((warnings & kIkWarningJointLimitClamp) != 0U) values.push_back("joint-limit-clamp");
  if ((warnings & kIkWarningSingularity) != 0U) values.push_back("singularity");
  if ((warnings & kIkWarningMaxIter) != 0U) values.push_back("max-iter");
  return json_string_array(values);
}

void sync_robot_math_from_web_once() {
  if (g_robot_math_synced) return;
  g_robot_math_synced = true;
  WebRobotMathState state {};
  web_server_get_robot_math_state(&state);
  if (state.solver[0] != '\0') g_robot_state.current_solver = canonical_solver(state.solver);
  if (state.jacobian[0] != '\0') g_robot_state.current_jacobian = canonical_jacobian_mode(state.jacobian);
  if (state.nullspace[0] != '\0') g_robot_state.current_nullspace = canonical_nullspace_mode(state.nullspace);
  if (state.trajectory[0] != '\0') g_robot_state.current_trajectory = canonical_trajectory_mode(state.trajectory);
  if (state.seed_policy[0] != '\0') {
    const std::string seed = canonical_seed_policy(state.seed_policy);
    if (!seed.empty()) g_robot_state.seed_policy = seed;
  }
  if (state.limits_profile[0] != '\0') {
    const std::string limits = canonical_limits_profile(state.limits_profile);
    if (!limits.empty()) g_robot_state.limits_profile = limits;
  }
  if (state.model[0] != '\0') g_robot_state.current_model = state.model;
  if (state.frame[0] != '\0') g_robot_state.current_frame = lower_copy(state.frame);
  if (state.units[0] != '\0') g_robot_state.current_units = canonical_units(state.units);
  if (state.pos_tol_mm > 0.0f) g_robot_state.pos_tol_mm = state.pos_tol_mm;
  if (state.ori_tol_deg > 0.0f) g_robot_state.ori_tol_deg = state.ori_tol_deg;
  if (state.singularity_threshold > 0.0f) g_robot_state.singularity_threshold = state.singularity_threshold;
  if (state.alpha_step > 0.0f) g_robot_state.alpha_step = state.alpha_step;
  if (state.null_gain >= 0.0f) g_robot_state.null_gain = state.null_gain;
  if (state.lambda_max >= 0.0f) g_robot_state.lambda_max = state.lambda_max;
  if (state.max_step_deg > 0.0f) g_robot_state.max_step_deg = state.max_step_deg;
  if (state.path_height_mode[0] != '\0') {
    const std::string mode = canonical_path_height_mode(state.path_height_mode);
    if (!mode.empty()) g_robot_state.path_height_mode = mode;
  }
  if (state.turret_mode[0] != '\0') {
    const std::string mode = canonical_turret_mode(state.turret_mode);
    if (!mode.empty()) g_robot_state.turret_mode = mode;
  }
  if (state.cart_step_mm > 0.0f) g_robot_state.cart_step_mm = state.cart_step_mm;
  if (state.yaw_step_deg > 0.0f) g_robot_state.yaw_step_deg = state.yaw_step_deg;
  if (state.jump_revolute_deg > 0.0f) g_robot_state.jump_revolute_deg = state.jump_revolute_deg;
  g_robot_state.ground_z_mm = state.ground_z_mm;
  g_robot_state.allow_negative_z_input = state.allow_negative_z_input;
  if (state.max_iter > 0U) g_robot_state.max_iter = state.max_iter;
  if (state.revision > 0U) g_robot_state.revision = state.revision;
  if (g_robot_state.current_solver.empty()) g_robot_state.current_solver = "dls";
  if (g_robot_state.current_jacobian.empty()) g_robot_state.current_jacobian = "numerical";
  if (g_robot_state.current_nullspace.empty()) g_robot_state.current_nullspace = "joint_centering";
  if (g_robot_state.current_trajectory.empty()) g_robot_state.current_trajectory = "quintic";
  if (g_robot_state.seed_policy.empty()) g_robot_state.seed_policy = "current";
  if (g_robot_state.limits_profile.empty()) g_robot_state.limits_profile = "default";
  if (g_robot_state.current_units.empty()) g_robot_state.current_units = "mm";
  if (g_robot_state.path_height_mode.empty()) g_robot_state.path_height_mode = "ground";
  if (g_robot_state.turret_mode.empty()) g_robot_state.turret_mode = "auto_shortest";
}

uint32_t publish_robot_math_state() {
  sync_robot_math_from_web_once();
  WebRobotMathState state {};
  copy_field(state.solver, sizeof(state.solver), g_robot_state.current_solver);
  copy_field(state.jacobian, sizeof(state.jacobian), g_robot_state.current_jacobian);
  copy_field(state.nullspace, sizeof(state.nullspace), g_robot_state.current_nullspace);
  copy_field(state.trajectory, sizeof(state.trajectory), g_robot_state.current_trajectory);
  copy_field(state.seed_policy, sizeof(state.seed_policy), g_robot_state.seed_policy);
  copy_field(state.limits_profile, sizeof(state.limits_profile), g_robot_state.limits_profile);
  copy_field(state.model, sizeof(state.model), g_robot_state.current_model);
  copy_field(state.frame, sizeof(state.frame), g_robot_state.current_frame);
  copy_field(state.units, sizeof(state.units), g_robot_state.current_units);
  state.pos_tol_mm = g_robot_state.pos_tol_mm;
  state.ori_tol_deg = g_robot_state.ori_tol_deg;
  state.singularity_threshold = g_robot_state.singularity_threshold;
  state.alpha_step = g_robot_state.alpha_step;
  state.null_gain = g_robot_state.null_gain;
  state.lambda_max = g_robot_state.lambda_max;
  state.max_step_deg = g_robot_state.max_step_deg;
  state.max_iter = g_robot_state.max_iter;
  copy_field(state.path_height_mode, sizeof(state.path_height_mode), g_robot_state.path_height_mode);
  state.ground_z_mm = g_robot_state.ground_z_mm;
  copy_field(state.turret_mode, sizeof(state.turret_mode), g_robot_state.turret_mode);
  state.cart_step_mm = g_robot_state.cart_step_mm;
  state.yaw_step_deg = g_robot_state.yaw_step_deg;
  state.jump_revolute_deg = g_robot_state.jump_revolute_deg;
  state.allow_negative_z_input = g_robot_state.allow_negative_z_input;
  const uint32_t revision = web_server_publish_robot_math_state(&state);
  if (revision > 0U) g_robot_state.revision = revision;
  return revision;
}

std::string math_registry_json() {
  sync_robot_math_from_web_once();
  const std::string backend = web_server_get_ik_compute_preference();
  std::string json = "{";
  json.reserve(1600U);
  json += "\"revision\":" + std::to_string(g_robot_state.revision) + ",";
  json += "\"model_revision\":\"" + json_escape(mros::kinematics::kRobotModelRevision) + "\",";
  json += "\"backend\":\"" + json_escape(backend) + "\",";
  json += "\"available_backends\":[\"auto\",\"web\",\"onboard-s3\",\"t41-qspi\",\"t41-esp-now\"],";
  json += "\"worker\":" + worker_json_string(mros::experimental::worker_status_json()) + ",";
  json += "\"solver\":\"" + json_escape(g_robot_state.current_solver) + "\",";
  json += "\"available_solvers\":" + json_string_array(g_robot_state.solvers) + ",";
  json += "\"jacobian\":\"" + json_escape(g_robot_state.current_jacobian) + "\",";
  json += "\"available_jacobians\":" + json_string_array(g_robot_state.jacobian_modes) + ",";
  json += "\"nullspace\":\"" + json_escape(g_robot_state.current_nullspace) + "\",";
  json += "\"available_nullspace\":" + json_string_array(g_robot_state.nullspace_modes) + ",";
  json += "\"units\":\"" + json_escape(g_robot_state.current_units) + "\",";
  json += "\"frame\":\"" + json_escape(g_robot_state.current_frame) + "\",";
  json += "\"model\":\"" + json_escape(g_robot_state.current_model) + "\",";
  json += "\"profile\":\"" + json_escape(g_robot_state.current_profile) + "\",";
  json += "\"trajectory\":\"" + json_escape(g_robot_state.current_trajectory) + "\",";
  json += "\"trajectory_mode\":\"" + json_escape(g_robot_state.current_trajectory) + "\",";
  json += "\"available_trajectory_modes\":" + json_string_array(g_robot_state.trajectory_modes) + ",";
  json += "\"seed_policy\":\"" + json_escape(g_robot_state.seed_policy) + "\",";
  json += "\"available_seed_policies\":" + json_string_array(g_robot_state.seed_policies) + ",";
  json += "\"limits_profile\":\"" + json_escape(g_robot_state.limits_profile) + "\",";
  json += "\"available_limits_profiles\":" + json_string_array(g_robot_state.limits_profiles) + ",";
  json += "\"path_height_mode\":\"" + json_escape(g_robot_state.path_height_mode) + "\",";
  json += "\"available_path_height_modes\":" + json_string_array(g_robot_state.path_height_modes) + ",";
  json += "\"ground_z_mm\":" + std::to_string(g_robot_state.ground_z_mm) + ",";
  json += "\"turret_mode\":\"" + json_escape(g_robot_state.turret_mode) + "\",";
  json += "\"available_turret_modes\":" + json_string_array(g_robot_state.turret_modes) + ",";
  json += "\"cart_step_mm\":" + std::to_string(g_robot_state.cart_step_mm) + ",";
  json += "\"yaw_step_deg\":" + std::to_string(g_robot_state.yaw_step_deg) + ",";
  json += "\"jump_revolute_deg\":" + std::to_string(g_robot_state.jump_revolute_deg) + ",";
  json += "\"allow_negative_z_input\":" + std::string(g_robot_state.allow_negative_z_input ? "true" : "false") + ",";
  json += "\"tuning\":{";
  json += "\"pos_tol_mm\":" + std::to_string(g_robot_state.pos_tol_mm) + ",";
  json += "\"ori_tol_deg\":" + std::to_string(g_robot_state.ori_tol_deg) + ",";
  json += "\"singularity_threshold\":" + std::to_string(g_robot_state.singularity_threshold) + ",";
  json += "\"alpha_step\":" + std::to_string(g_robot_state.alpha_step) + ",";
  json += "\"null_gain\":" + std::to_string(g_robot_state.null_gain) + ",";
  json += "\"lambda_max\":" + std::to_string(g_robot_state.lambda_max) + ",";
  json += "\"max_step_deg\":" + std::to_string(g_robot_state.max_step_deg) + ",";
  json += "\"max_iter\":" + std::to_string(g_robot_state.max_iter) + "}";
  json += "}";
  return json;
}

std::string summarize_robot_status() {
  char buffer[512] = {};
  std::snprintf(buffer,
                sizeof(buffer),
                "power motor=%u oe=%u | t41=%s c3=%s espnow=%s | turret=%.1f/%.1f | xyz=(%.1f, %.1f, %.1f) alpha=%.1f | gripper=%u",
                static_cast<unsigned>(spi_s3_get_motor_state()),
                static_cast<unsigned>(pca9685_get_output_enable() ? 1U : 0U),
                spi_s3_is_connected() ? "ok" : "down",
                spi_c3_is_connected() ? "ok" : "down",
                spi_c3_is_espnow_connected() ? "ok" : "down",
                spi_s3_get_turret_deg(),
                spi_s3_get_turret_actual_deg(),
                spi_s3_get_coord_x(),
                spi_s3_get_coord_y(),
                spi_s3_get_coord_z(),
                spi_s3_get_alpha(),
                static_cast<unsigned>(spi_s3_get_gripper()));
  return buffer;
}

std::string robot_status_json() {
  JsonObjectWriter writer(384U);
  writer.add_raw("motor_power", std::to_string(static_cast<unsigned>(spi_s3_get_motor_state())));
  writer.add_bool("oe", pca9685_get_output_enable());
  writer.add_bool("t41_connected", spi_s3_is_connected());
  writer.add_bool("c3_connected", spi_c3_is_connected());
  writer.add_bool("espnow_connected", spi_c3_is_espnow_connected());
  writer.add_raw("turret_target", std::to_string(spi_s3_get_turret_deg()));
  writer.add_raw("turret_actual", std::to_string(spi_s3_get_turret_actual_deg()));
  std::string coord;
  coord.reserve(128U);
  coord += "{\"x\":" + std::to_string(spi_s3_get_coord_x()) + ",";
  coord += "\"y\":" + std::to_string(spi_s3_get_coord_y()) + ",";
  coord += "\"z\":" + std::to_string(spi_s3_get_coord_z()) + ",";
  coord += "\"alpha\":" + std::to_string(spi_s3_get_alpha()) + "}";
  writer.add_raw("coord", coord);
  writer.add_raw("gripper", std::to_string(static_cast<unsigned>(spi_s3_get_gripper())));
  return writer.finish();
}

std::string joints_text_line() {
  char buffer[256] = {};
  std::snprintf(buffer,
                sizeof(buffer),
                "t=%.1f j1=%.1f j2=%.1f j3=%.1f j4=%.1f j5=%.1f j6=%.1f",
                spi_s3_get_turret_deg(),
                spi_s3_get_joint_deg(0),
                spi_s3_get_joint_deg(1),
                spi_s3_get_joint_deg(2),
                spi_s3_get_joint_deg(3),
                spi_s3_get_joint_deg(4),
                spi_s3_get_joint_deg(5));
  return buffer;
}

std::string joints_json() {
  std::string json = "[";
  json.reserve(96U);
  json += std::to_string(spi_s3_get_turret_deg());
  for (int i = 0; i < 6; ++i) {
    json += "," + std::to_string(spi_s3_get_joint_deg(i));
  }
  json += "]";
  return json;
}

std::string requested_joints_json(const ShellRobotRequest& request) {
  std::string json = "[";
  json.reserve(2U + request.joint_count * 16U);
  for (size_t i = 0U; i < request.joint_count; ++i) {
    if (i > 0U) json += ",";
    json += std::to_string(request.joints[i]);
  }
  json += "]";
  return json;
}

int handle_power(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Power;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "status" || verb == "get") {
    request.verb = (verb == "get") ? ShellRobotVerb::Get : ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request,
                      summarize_robot_status(),
                      robot_status_json());
    return 0;
  }
  if (verb == "set") {
    if (args.size() < 4U) {
      shell_write_line(ctx.state, "robot power set: on/off value required");
      return 1;
    }
    bool enabled = false;
    if (!parse_on_off(args[3], &enabled)) {
      shell_write_line(ctx.state, "robot power set: value must be on/off");
      return 1;
    }
    spi_s3_set_motor_power(enabled ? 1U : 0U);
    pca9685_set_output_enable(enabled);
    request.verb = ShellRobotVerb::Set;
    emit_robot_result(ctx, true, request,
                      std::string("robot power set: ") + (enabled ? "on" : "off"),
                      std::string("{\"motor_power\":") + (enabled ? "1" : "0") +
                          ",\"oe\":" + (enabled ? "true" : "false") + "}");
    return 0;
  }
  shell_write_line(ctx.state, "Usage: robot power status|get|set on|off");
  return 1;
}

int handle_safety(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Safety;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "status") {
    request.verb = ShellRobotVerb::Status;
    const bool motor_on = spi_s3_get_motor_state() != 0U;
    const bool oe_on = pca9685_get_output_enable();
    const bool emg = !motor_on && !oe_on;
    emit_robot_result(ctx, true, request,
                      std::string("safety emg=") + (emg ? "on" : "off") + " hold=" + (motor_on && oe_on ? "on" : "off"),
                      std::string("{\"emg\":") + (emg ? "true" : "false") +
                          ",\"hold\":" + (motor_on && oe_on ? "true" : "false") + "}");
    return 0;
  }
  if (verb == "emg" && args.size() >= 4U) {
    bool enabled = false;
    if (!parse_on_off(args[3], &enabled)) {
      shell_write_line(ctx.state, "robot safety emg: on/off required");
      return 1;
    }
    request.action = ShellRobotAction::Emergency;
    request.verb = ShellRobotVerb::Set;
    request.enabled = enabled;
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  if (verb == "hold" && args.size() >= 4U) {
    bool enabled = false;
    if (!parse_on_off(args[3], &enabled)) {
      shell_write_line(ctx.state, "robot safety hold: on/off required");
      return 1;
    }
    request.action = ShellRobotAction::Hold;
    request.verb = ShellRobotVerb::Hold;
    request.enabled = enabled;
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  if (verb == "stop") {
    request.action = ShellRobotAction::Stop;
    request.verb = ShellRobotVerb::Stop;
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  if (verb == "reset") {
    spi_s3_set_motor_power(1U);
    pca9685_set_output_enable(true);
    request.verb = ShellRobotVerb::Reset;
    emit_robot_result(ctx, true, request,
                      "robot safety reset: motor-power=1 oe=1",
                      "{\"motor_power\":1,\"oe\":true}");
    return 0;
  }
  shell_write_line(ctx.state, "Usage: robot safety status|emg on|off|hold on|off|stop|reset");
  return 1;
}

int handle_status_resource(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Status;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "summary";
  request.verb = ShellRobotVerb::Status;
  if (verb == "summary" || verb == "status") {
    emit_robot_result(ctx, true, request, summarize_robot_status(), robot_status_json());
    return 0;
  }
  if (verb == "full") {
    std::string text = summarize_robot_status();
    text += "\n";
    text += "joints: " + joints_text_line();
    text += "\n";
    text += "path queue: " + std::to_string(g_robot_state.path_queue.size()) + " point(s)";
    emit_robot_result(ctx, true, request, text,
                      std::string("{\"summary\":") + robot_status_json() +
                          ",\"joints\":" + joints_json() +
                          ",\"path_count\":" + std::to_string(g_robot_state.path_queue.size()) + "}");
    return 0;
  }
  shell_write_line(ctx.state, "Usage: robot status [summary|full]");
  return 1;
}

int handle_telemetry(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Telemetry;
  request.verb = ShellRobotVerb::Status;
  if (args.size() >= 3U && lower_copy(args[2]) != "status") {
    shell_write_line(ctx.state, "Usage: robot telemetry status");
    return 1;
  }
  WebServerDiagSnapshot diag {};
  web_server_get_diag_snapshot(&diag);
  char text[256] = {};
  std::snprintf(text,
                sizeof(text),
                "telemetry pid_avg=%.2f pid_last=%lu pid_exec=%lu fk_avg=%.3f fk_last=%.3f web_feedback=%lu",
                diag.pid_cycle_avg_ms,
                static_cast<unsigned long>(diag.pid_cycle_last_ms),
                static_cast<unsigned long>(diag.pid_cycle_exec_ms),
                diag.fk_avg_ms,
                diag.fk_last_ms,
                static_cast<unsigned long>(diag.last_web_feedback_ms));
  std::string json = "{";
  json += "\"pid_avg_ms\":" + std::to_string(diag.pid_cycle_avg_ms) + ",";
  json += "\"pid_last_ms\":" + std::to_string(diag.pid_cycle_last_ms) + ",";
  json += "\"pid_exec_ms\":" + std::to_string(diag.pid_cycle_exec_ms) + ",";
  json += "\"fk_avg_ms\":" + std::to_string(diag.fk_avg_ms) + ",";
  json += "\"fk_last_ms\":" + std::to_string(diag.fk_last_ms) + ",";
  json += "\"web_feedback_ms\":" + std::to_string(diag.last_web_feedback_ms);
  json += "}";
  emit_robot_result(ctx, true, request, text, json);
  return 0;
}

int handle_turret(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Turret;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "status") {
    request.verb = ShellRobotVerb::Status;
    char text[192] = {};
    std::snprintf(text, sizeof(text), "turret target=%.2f actual=%.2f error=%.2f out=%.2f",
                  spi_s3_get_turret_deg(),
                  spi_s3_get_turret_actual_deg(),
                  spi_s3_get_turret_pid_error(),
                  spi_s3_get_turret_pid_output());
    std::string json = "{";
    json += "\"target_deg\":" + std::to_string(spi_s3_get_turret_deg()) + ",";
    json += "\"actual_deg\":" + std::to_string(spi_s3_get_turret_actual_deg()) + ",";
    json += "\"pid_error\":" + std::to_string(spi_s3_get_turret_pid_error()) + ",";
    json += "\"pid_output\":" + std::to_string(spi_s3_get_turret_pid_output()) + "}";
    emit_robot_result(ctx, true, request, text, json);
    return 0;
  }
  if (verb == "set" && args.size() >= 4U) {
    request.action = ShellRobotAction::TurretPosition;
    request.verb = ShellRobotVerb::Set;
    if (!parse_float_text(args[3], &request.value)) {
      shell_write_line(ctx.state, "robot turret set: degree must be numeric");
      return 1;
    }
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  if (verb == "zero") {
    request.action = ShellRobotAction::TurretPosition;
    request.verb = ShellRobotVerb::Zero;
    request.value = 0.0f;
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  if (verb == "pid" && args.size() >= 4U && lower_copy(args[3]) == "status") {
    request.verb = ShellRobotVerb::Status;
    float kp = 0.0f;
    float ki = 0.0f;
    float kd = 0.0f;
    float imax = 0.0f;
    spi_s3_get_turret_pid(&kp, &ki, &kd, &imax);
    char text[192] = {};
    std::snprintf(text, sizeof(text), "turret pid kp=%.4f ki=%.4f kd=%.4f imax=%.4f dspc=%.4f",
                  kp, ki, kd, imax, spi_s3_get_turret_dspc());
    std::string json = "{";
    json += "\"kp\":" + std::to_string(kp) + ",";
    json += "\"ki\":" + std::to_string(ki) + ",";
    json += "\"kd\":" + std::to_string(kd) + ",";
    json += "\"imax\":" + std::to_string(imax) + ",";
    json += "\"dspc\":" + std::to_string(spi_s3_get_turret_dspc()) + "}";
    emit_robot_result(ctx, true, request, text, json);
    return 0;
  }
  shell_write_line(ctx.state, "Usage: robot turret status|set <deg>|zero|pid status");
  return 1;
}

int handle_gripper(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Gripper;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "status") {
    request.verb = ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request,
                      "gripper=" + std::to_string(static_cast<unsigned>(spi_s3_get_gripper())),
                      std::string("{\"gripper\":") + std::to_string(static_cast<unsigned>(spi_s3_get_gripper())) + "}");
    return 0;
  }
  if (verb == "set" && args.size() >= 4U) {
    request.action = ShellRobotAction::Gripper;
    request.verb = ShellRobotVerb::Set;
    if (!parse_float_text(args[3], &request.value)) {
      shell_write_line(ctx.state, "robot gripper set: numeric value required");
      return 1;
    }
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  if (verb == "open" || verb == "close") {
    request.action = ShellRobotAction::Gripper;
    request.verb = (verb == "open") ? ShellRobotVerb::Open : ShellRobotVerb::Close;
    request.value = (verb == "open") ? 100.0f : 0.0f;
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  shell_write_line(ctx.state, "Usage: robot gripper status|set <pct>|open|close");
  return 1;
}

int handle_joint(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Joint;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "status") {
    request.verb = ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request, joints_text_line(), std::string("{\"joints\":") + joints_json() + "}");
    return 0;
  }
  if (verb == "list") {
    request.verb = ShellRobotVerb::List;
    std::string text = "joints ranges: t[-270,270] j1..j6[-90,90]";
    std::string json = "{\"names\":[\"t\",\"j1\",\"j2\",\"j3\",\"j4\",\"j5\",\"j6\"],\"ranges\":[[-270,270],[-90,90],[-90,90],[-90,90],[-90,90],[-90,90],[-90,90]]}";
    emit_robot_result(ctx, true, request, text, json);
    return 0;
  }
  if (verb == "set" || verb == "apply") {
    request.action = (verb == "set") ? ShellRobotAction::JointSet : ShellRobotAction::JointApply;
    request.verb = (verb == "set") ? ShellRobotVerb::Set : ShellRobotVerb::Apply;
    request.apply = (verb == "apply");
    size_t next_index = 0U;
    std::string error;
    if (args.size() < 4U || !parse_joint_values(args, 3U, &request, &next_index, &error)) {
      shell_printf(ctx.state, "robot joint %s: %s\n", verb.c_str(), error.empty() ? "invalid joint list" : error.c_str());
      return 1;
    }
    if (!parse_common_options(args, next_index, &request, &error, false, false)) {
      shell_printf(ctx.state, "robot joint %s: %s\n", verb.c_str(), error.c_str());
      return 1;
    }
    if (verb == "apply") request.apply = true;
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"requested\":") + requested_joints_json(request) + ",\"message\":\"" + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  if (verb == "home") {
    request.action = ShellRobotAction::DefaultPosition;
    request.verb = ShellRobotVerb::Home;
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  if (verb == "park") {
    request.action = ShellRobotAction::Park;
    request.verb = ShellRobotVerb::Park;
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  if (verb == "zero") {
    request.action = ShellRobotAction::JointSet;
    request.verb = ShellRobotVerb::Zero;
    for (size_t i = 0U; i < 7U; ++i) request.joints[i] = 0.0f;
    request.joint_count = 7U;
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  if (verb == "jog" && args.size() >= 5U) {
    request.action = ShellRobotAction::JointApply;
    request.verb = ShellRobotVerb::Jog;
    for (size_t i = 0U; i < 7U; ++i) request.joints[i] = 0.0f;
    request.joints[0] = spi_s3_get_turret_deg();
    for (int i = 0; i < 6; ++i) request.joints[i + 1U] = spi_s3_get_joint_deg(i);
    request.joint_count = 7U;
    const std::string axis = lower_copy(args[3]);
    float delta = 0.0f;
    if (!parse_float_text(args[4], &delta)) {
      shell_write_line(ctx.state, "robot joint jog: delta must be numeric");
      return 1;
    }
    if (axis == "t") request.joints[0] += delta;
    else if (axis == "j1") request.joints[1] += delta;
    else if (axis == "j2") request.joints[2] += delta;
    else if (axis == "j3") request.joints[3] += delta;
    else if (axis == "j4") request.joints[4] += delta;
    else if (axis == "j5") request.joints[5] += delta;
    else if (axis == "j6") request.joints[6] += delta;
    else {
      shell_write_line(ctx.state, "robot joint jog: axis must be t or j1..j6");
      return 1;
    }
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  shell_write_line(ctx.state, "Usage: robot joint status|list|set|apply|home|park|zero|jog");
  return 1;
}

bool parse_cartesian_target(
    const std::vector<std::string>& args,
    const size_t start_index,
    ShellRobotRequest* request,
    size_t* next_index,
    std::string* error) {
  if (!parse_vector_arg(args, start_index, &request->position, next_index, error)) return false;
  return parse_common_options(args, *next_index, request, error, true, true);
}

int handle_cartesian(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Cartesian;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "status") {
    request.verb = ShellRobotVerb::Status;
    char text[192] = {};
    std::snprintf(text, sizeof(text), "cartesian x=%.1f y=%.1f z=%.1f roll=%.1f pitch=%.1f yaw=%.1f backend=%s",
                  spi_s3_get_coord_x(), spi_s3_get_coord_y(), spi_s3_get_coord_z(),
                  spi_s3_get_coord_roll(), spi_s3_get_coord_pitch(), spi_s3_get_coord_yaw(),
                  web_server_get_ik_compute_preference());
    std::string json = "{";
    json += "\"x\":" + std::to_string(spi_s3_get_coord_x()) + ",";
    json += "\"y\":" + std::to_string(spi_s3_get_coord_y()) + ",";
    json += "\"z\":" + std::to_string(spi_s3_get_coord_z()) + ",";
    json += "\"roll_deg\":" + std::to_string(spi_s3_get_coord_roll()) + ",";
    json += "\"pitch_deg\":" + std::to_string(spi_s3_get_coord_pitch()) + ",";
    json += "\"yaw_deg\":" + std::to_string(spi_s3_get_coord_yaw()) + ",";
    json += "\"alpha\":" + std::to_string(spi_s3_get_alpha()) + ",";
    json += "\"backend\":\"" + json_escape(web_server_get_ik_compute_preference()) + "\"}";
    emit_robot_result(ctx, true, request, text, json);
    return 0;
  }
  if (verb == "set" || verb == "preview" || verb == "apply") {
    request.action = ShellRobotAction::Position;
    request.verb = (verb == "set") ? ShellRobotVerb::Set : (verb == "apply" ? ShellRobotVerb::Apply : ShellRobotVerb::Preview);
    if (verb == "apply") request.apply = true;
    size_t next_index = 0U;
    std::string error;
    if (args.size() < 4U || !parse_cartesian_target(args, 3U, &request, &next_index, &error)) {
      shell_printf(ctx.state, "robot cartesian %s: %s\n", verb.c_str(), error.empty() ? "invalid target" : error.c_str());
      return 1;
    }
    if (verb == "preview") request.apply = false;
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    std::string json = "{";
    json += "\"target\":{\"x\":" + std::to_string(request.position.x) + ",\"y\":" + std::to_string(request.position.y) + ",\"z\":" + std::to_string(request.position.z) + "},";
    json += "\"orientation\":{\"roll_deg\":" + std::to_string(request.roll_deg) + ",\"pitch_deg\":" + std::to_string(request.ee_pitch) + ",\"yaw_deg\":" + std::to_string(request.yaw_deg) + ",\"ee_auto\":" + std::string(request.ee_auto ? "true" : "false") + "},";
    json += "\"message\":\"" + json_escape(text) + "\"}";
    emit_robot_result(ctx, ok, request, text, json);
    return ok ? 0 : 1;
  }
  shell_write_line(ctx.state, "Usage: robot cartesian status|set|preview|apply <x y z> [--roll deg] [--pitch deg|--alpha deg|auto] [--yaw deg]");
  return 1;
}

int handle_move(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Move;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "preview";
  size_t vector_start = 2U;
  bool target_only = false;
  if (verb == "status") {
    request.verb = ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request,
                      "move parser ready: use x y z [--speed 0.1..1.0], --from/--to or [x y z] [x y z]",
                      "{\"supports\":[\"x y z --speed 0.1..1.0\",\"--from/--to\",\"[x y z] [x y z]\"]}");
    return 0;
  }
  if (verb == "preview" || verb == "apply") {
    request.verb = (verb == "apply") ? ShellRobotVerb::Apply : ShellRobotVerb::Preview;
    vector_start = 3U;
    request.apply = (verb == "apply");
  } else {
    request.verb = ShellRobotVerb::Apply;
    request.apply = true;
    target_only = true;
  }
  request.action = ShellRobotAction::Move;
  size_t next_index = 0U;
  std::string error;
  const bool parsed = target_only
      ? parse_move_target_only(args, vector_start, &request, &next_index, &error)
      : parse_move_vectors(args, vector_start, &request, &next_index, &error);
  if (!parsed) {
    shell_printf(ctx.state, "robot move: %s\n", error.c_str());
    return 1;
  }
  if (!parse_common_options(args, next_index, &request, &error, true, true)) {
    shell_printf(ctx.state, "robot move: %s\n", error.c_str());
    return 1;
  }
  if (request.value > 0.0f) {
    request.time_ms = std::max(1.0f, request.time_ms / request.value);
  }
  std::string text;
  const bool ok = dispatch_robot_request(ctx, request, &text);
  std::string json = "{";
  json += "\"from\":{\"x\":" + std::to_string(request.from.x) + ",\"y\":" + std::to_string(request.from.y) + ",\"z\":" + std::to_string(request.from.z) + "},";
  json += "\"to\":{\"x\":" + std::to_string(request.to.x) + ",\"y\":" + std::to_string(request.to.y) + ",\"z\":" + std::to_string(request.to.z) + "},";
  json += "\"orientation\":{\"roll_deg\":" + std::to_string(request.roll_deg) + ",\"pitch_deg\":" + std::to_string(request.ee_pitch) + ",\"yaw_deg\":" + std::to_string(request.yaw_deg) + ",\"ee_auto\":" + std::string(request.ee_auto ? "true" : "false") + "},";
  json += "\"speed\":" + std::to_string(request.value > 0.0f ? request.value : 1.0f) + ",";
  json += "\"time_ms\":" + std::to_string(request.time_ms) + ",";
  json += "\"message\":\"" + json_escape(text) + "\"}";
  emit_robot_result(ctx, ok, request, text, json);
  return ok ? 0 : 1;
}

std::string path_export_csv() {
  std::string csv;
  for (const RobotPathPoint& p : g_robot_state.path_queue) {
    csv += std::to_string(p.pos.x) + ",";
    csv += std::to_string(p.pos.y) + ",";
    csv += std::to_string(p.pos.z) + ",";
    csv += std::to_string(p.time_ms) + ",";
    csv += (p.ee_auto ? "1" : "0");
    csv += ",";
    csv += std::to_string(p.ee_pitch);
    csv += ",";
    csv += std::to_string(p.roll_deg);
    csv += ",";
    csv += std::to_string(p.yaw_deg);
    csv += "\n";
  }
  return csv;
}

bool parse_path_import_payload(const std::string& payload, std::vector<RobotPathPoint>* out_points) {
  if (out_points == nullptr) return false;
  out_points->clear();
  std::stringstream lines(payload);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    std::replace(line.begin(), line.end(), ';', '\n');
    std::stringstream semis(line);
    std::string row;
    while (std::getline(semis, row)) {
      if (row.empty()) continue;
      for (char& ch : row) if (ch == ',') ch = ' ';
      std::stringstream ss(row);
      RobotPathPoint point {};
      int ee_auto = 1;
      if (!(ss >> point.pos.x >> point.pos.y >> point.pos.z)) return false;
      if (!(ss >> point.time_ms)) point.time_ms = 1000.0f;
      if (!(ss >> ee_auto)) ee_auto = 1;
      if (!(ss >> point.ee_pitch)) point.ee_pitch = 0.0f;
      if (!(ss >> point.roll_deg)) point.roll_deg = 0.0f;
      if (!(ss >> point.yaw_deg)) point.yaw_deg = 0.0f;
      point.ee_auto = ee_auto != 0;
      out_points->push_back(point);
    }
  }
  return true;
}

int handle_path(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Path;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "status") {
    request.verb = ShellRobotVerb::Status;
    std::string text = "path queue points=" + std::to_string(g_robot_state.path_queue.size()) +
                       " device_buffer=" + std::to_string(static_cast<unsigned>(trajectory_handler_count()));
    emit_robot_result(ctx, true, request, text, path_queue_result_json());
    return 0;
  }
  if (verb == "list") {
    request.verb = ShellRobotVerb::List;
    std::string text = "path list";
    for (size_t i = 0U; i < g_robot_state.path_queue.size(); ++i) {
      char line[160] = {};
      std::snprintf(line, sizeof(line), "\nP%u: %.1f %.1f %.1f t=%.1f",
                    static_cast<unsigned>(i + 1U),
                    g_robot_state.path_queue[i].pos.x,
                    g_robot_state.path_queue[i].pos.y,
                    g_robot_state.path_queue[i].pos.z,
                    g_robot_state.path_queue[i].time_ms);
      text += line;
    }
    emit_robot_result(ctx, true, request, text, path_queue_result_json());
    return 0;
  }
  if (verb == "add" || verb == "insert") {
    request.verb = (verb == "add") ? ShellRobotVerb::Add : ShellRobotVerb::Insert;
    size_t point_index = 3U;
    size_t insert_index = g_robot_state.path_queue.size();
    if (verb == "insert") {
      if (args.size() < 4U || !parse_size_text(args[3], &insert_index)) {
        shell_write_line(ctx.state, "robot path insert: index required");
        return 1;
      }
      point_index = 4U;
    }
    size_t next_index = 0U;
    std::string error;
    if (!parse_vector_arg(args, point_index, &request.position, &next_index, &error)) {
      shell_printf(ctx.state, "robot path %s: %s\n", verb.c_str(), error.c_str());
      return 1;
    }
    if (!parse_common_options(args, next_index, &request, &error, true, true)) {
      shell_printf(ctx.state, "robot path %s: %s\n", verb.c_str(), error.c_str());
      return 1;
    }
    RobotPathPoint point {};
    point.pos = request.position;
    point.time_ms = request.time_ms;
    point.ee_auto = request.ee_auto;
    point.roll_deg = request.roll_deg;
    point.ee_pitch = request.ee_pitch;
    point.yaw_deg = request.yaw_deg;
    if (verb == "insert" && insert_index < g_robot_state.path_queue.size()) {
      g_robot_state.path_queue.insert(g_robot_state.path_queue.begin() + static_cast<long>(insert_index), point);
      sync_path_queue_to_web(ctx);
    } else {
      g_robot_state.path_queue.push_back(point);
      request.action = ShellRobotAction::PathAdd;
      std::string dummy;
      dispatch_robot_request(ctx, request, &dummy);
    }
    emit_robot_result(ctx, true, request,
                      "path point stored. total=" + std::to_string(g_robot_state.path_queue.size()),
                      path_queue_result_json());
    return 0;
  }
  if (verb == "remove") {
    if (args.size() < 4U) {
      shell_write_line(ctx.state, "robot path remove: index required");
      return 1;
    }
    size_t index = 0U;
    if (!parse_size_text(args[3], &index) || index >= g_robot_state.path_queue.size()) {
      shell_write_line(ctx.state, "robot path remove: invalid index");
      return 1;
    }
    request.verb = ShellRobotVerb::Remove;
    g_robot_state.path_queue.erase(g_robot_state.path_queue.begin() + static_cast<long>(index));
    sync_path_queue_to_web(ctx);
    emit_robot_result(ctx, true, request,
                      "path point removed. total=" + std::to_string(g_robot_state.path_queue.size()),
                      path_queue_result_json());
    return 0;
  }
  if (verb == "clear") {
    request.verb = ShellRobotVerb::Clear;
    g_robot_state.path_queue.clear();
    request.action = ShellRobotAction::PathClear;
    std::string dummy;
    dispatch_robot_request(ctx, request, &dummy);
    emit_robot_result(ctx, true, request, "path queue cleared", path_queue_result_json());
    return 0;
  }
  if (verb == "preview" || verb == "run") {
    request.verb = (verb == "run") ? ShellRobotVerb::Run : ShellRobotVerb::Preview;
    request.action = (verb == "run") ? ShellRobotAction::PathRun : ShellRobotAction::PathPreview;
    request.apply = (verb == "run");
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, path_queue_result_json());
    return ok ? 0 : 1;
  }
  if (verb == "export") {
    request.verb = ShellRobotVerb::Export;
    const std::string csv = path_export_csv();
    emit_robot_result(ctx, true, request, csv.empty() ? "path export: queue empty" : csv,
                      std::string("{\"csv\":\"") + json_escape(csv) + "\"}");
    return 0;
  }
  if (verb == "import") {
    request.verb = ShellRobotVerb::Import;
    const std::string payload = (ctx.stdin_buffer != nullptr && !ctx.stdin_buffer->empty())
        ? *ctx.stdin_buffer
        : join_tokens(args, 3U);
    if (payload.empty()) {
      shell_write_line(ctx.state, "robot path import: provide CSV rows or pipe content");
      return 1;
    }
    std::vector<RobotPathPoint> imported;
    if (!parse_path_import_payload(payload, &imported)) {
      shell_write_line(ctx.state, "robot path import: invalid payload");
      return 1;
    }
    g_robot_state.path_queue = imported;
    sync_path_queue_to_web(ctx);
    emit_robot_result(ctx, true, request,
                      "path import complete. total=" + std::to_string(g_robot_state.path_queue.size()),
                      path_queue_result_json());
    return 0;
  }
  shell_write_line(ctx.state, "Usage: robot path status|list|add|insert|remove|clear|preview|run|export|import");
  return 1;
}

int handle_motion(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Motion;
  if (args.size() < 3U) {
    shell_write_line(ctx.state, "Usage: robot motion block list|add|clear|preview|compile|apply");
    return 1;
  }
  const std::string subject = lower_copy(args[2]);
  if (subject != "block") {
    shell_write_line(ctx.state, "robot motion: only 'block' resource is supported in v1");
    return 1;
  }
  const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "list";
  copy_field(request.subject, sizeof(request.subject), "block");
  if (verb == "list") {
    request.verb = ShellRobotVerb::List;
    emit_robot_result(ctx, true, request, "motion blocks=" + std::to_string(g_robot_state.motion_blocks.size()), motion_blocks_json());
    return 0;
  }
  if (verb == "add") {
    if (args.size() < 5U) {
      shell_write_line(ctx.state, "robot motion block add: type required");
      return 1;
    }
    request.verb = ShellRobotVerb::Add;
    RobotMotionBlock block {};
    block.type = lower_copy(args[4]);
    block.payload = join_tokens(args, 5U);
    g_robot_state.motion_blocks.push_back(block);
    emit_robot_result(ctx, true, request,
                      "motion block added. total=" + std::to_string(g_robot_state.motion_blocks.size()),
                      motion_blocks_json());
    return 0;
  }
  if (verb == "clear") {
    request.verb = ShellRobotVerb::Clear;
    g_robot_state.motion_blocks.clear();
    emit_robot_result(ctx, true, request, "motion blocks cleared", motion_blocks_json());
    return 0;
  }
  if (verb == "preview" || verb == "compile" || verb == "apply") {
    request.verb = (verb == "preview") ? ShellRobotVerb::Preview :
                   (verb == "compile") ? ShellRobotVerb::Compile : ShellRobotVerb::Apply;
    std::string text = "motion blocks=" + std::to_string(g_robot_state.motion_blocks.size()) +
                       " path_points=" + std::to_string(g_robot_state.path_queue.size());
    if (verb == "preview" && ctx.state.config.robot_action_callback != nullptr) {
      ShellRobotRequest op {};
      reset_request_defaults(&op);
      op.action = ShellRobotAction::PathPreview;
      char message[256] = {};
      ctx.state.config.robot_action_callback(op, message, sizeof(message), ctx.state.config.user_data);
    } else if (verb == "apply" && ctx.state.config.robot_action_callback != nullptr) {
      ShellRobotRequest op {};
      reset_request_defaults(&op);
      op.action = ShellRobotAction::PathRun;
      op.apply = true;
      char message[256] = {};
      ctx.state.config.robot_action_callback(op, message, sizeof(message), ctx.state.config.user_data);
    }
    emit_robot_result(ctx, true, request, text, motion_blocks_json());
    return 0;
  }
  shell_write_line(ctx.state, "Usage: robot motion block list|add|clear|preview|compile|apply");
  return 1;
}

int handle_profile(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Profile;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "list") {
    request.verb = ShellRobotVerb::List;
    std::string text = "profiles:";
    std::string json = "{\"profiles\":[";
    for (size_t i = 0U; i < g_robot_state.profiles.size(); ++i) {
      text += (i == 0U ? " " : ", ") + g_robot_state.profiles[i];
      if (i > 0U) json += ",";
      json += "\"" + json_escape(g_robot_state.profiles[i]) + "\"";
    }
    json += "],\"current\":\"" + json_escape(g_robot_state.current_profile) + "\"}";
    emit_robot_result(ctx, true, request, text, json);
    return 0;
  }
  if (verb == "status") {
    request.verb = ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request,
                      "profile=" + g_robot_state.current_profile,
                      std::string("{\"current\":\"") + json_escape(g_robot_state.current_profile) + "\"}");
    return 0;
  }
  if ((verb == "set" || verb == "save" || verb == "delete") && args.size() >= 4U) {
    request.verb = (verb == "set") ? ShellRobotVerb::Set : (verb == "save" ? ShellRobotVerb::Add : ShellRobotVerb::Delete);
    const std::string name = args[3];
    if (verb == "set") {
      g_robot_state.current_profile = name;
    } else if (verb == "save") {
      if (std::find(g_robot_state.profiles.begin(), g_robot_state.profiles.end(), name) == g_robot_state.profiles.end()) {
        g_robot_state.profiles.push_back(name);
      }
      g_robot_state.current_profile = name;
    } else {
      g_robot_state.profiles.erase(std::remove(g_robot_state.profiles.begin(), g_robot_state.profiles.end(), name),
                                   g_robot_state.profiles.end());
      if (g_robot_state.current_profile == name) g_robot_state.current_profile = "default";
    }
    emit_robot_result(ctx, true, request,
                      "profile " + verb + ": " + name,
                      std::string("{\"current\":\"") + json_escape(g_robot_state.current_profile) + "\"}");
    return 0;
  }
  shell_write_line(ctx.state, "Usage: robot profile list|status|set <name>|save <name>|delete <name>");
  return 1;
}

int handle_model(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Model;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "list") {
    request.verb = ShellRobotVerb::List;
    std::string text = "models:";
    std::string json = "{\"models\":[";
    for (size_t i = 0U; i < g_robot_state.models.size(); ++i) {
      text += (i == 0U ? " " : ", ") + g_robot_state.models[i];
      if (i > 0U) json += ",";
      json += "\"" + json_escape(g_robot_state.models[i]) + "\"";
    }
    json += "],\"current\":\"" + json_escape(g_robot_state.current_model) + "\"}";
    emit_robot_result(ctx, true, request, text, json);
    return 0;
  }
  if (verb == "status") {
    request.verb = ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request,
                      "model=" + g_robot_state.current_model,
                      std::string("{\"current\":\"") + json_escape(g_robot_state.current_model) + "\"}");
    return 0;
  }
  if (verb == "set" && args.size() >= 4U) {
    request.verb = ShellRobotVerb::Set;
    g_robot_state.current_model = args[3];
    emit_robot_result(ctx, true, request,
                      "model set: " + g_robot_state.current_model,
                      std::string("{\"current\":\"") + json_escape(g_robot_state.current_model) + "\"}");
    return 0;
  }
  if (verb == "describe" && args.size() >= 4U) {
    request.verb = ShellRobotVerb::Describe;
    const std::string name = args[3];
    const std::string text = "model " + name + ": 7DOF manipulator, web+shell+ai command framework target";
    emit_robot_result(ctx, true, request, text,
                      std::string("{\"name\":\"") + json_escape(name) + "\",\"dof\":7}");
    return 0;
  }
  shell_write_line(ctx.state, "Usage: robot model list|status|set <name>|describe <name>");
  return 1;
}

int handle_frame(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Frame;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "list") {
    request.verb = ShellRobotVerb::List;
    std::string text = "frames:";
    std::string json = "{\"frames\":[";
    for (size_t i = 0U; i < g_robot_state.frames.size(); ++i) {
      text += (i == 0U ? " " : ", ") + g_robot_state.frames[i];
      if (i > 0U) json += ",";
      json += "\"" + json_escape(g_robot_state.frames[i]) + "\"";
    }
    json += "],\"current\":\"" + json_escape(g_robot_state.current_frame) + "\"}";
    emit_robot_result(ctx, true, request, text, json);
    return 0;
  }
  if (verb == "status") {
    request.verb = ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request,
                      "frame=" + g_robot_state.current_frame,
                      std::string("{\"current\":\"") + json_escape(g_robot_state.current_frame) + "\"}");
    return 0;
  }
  if (verb == "set" && args.size() >= 4U) {
    request.verb = ShellRobotVerb::Set;
    g_robot_state.current_frame = lower_copy(args[3]);
    if (std::find(g_robot_state.frames.begin(), g_robot_state.frames.end(), g_robot_state.current_frame) ==
        g_robot_state.frames.end()) {
      g_robot_state.frames.push_back(g_robot_state.current_frame);
    }
    emit_robot_result(ctx, true, request,
                      "frame set: " + g_robot_state.current_frame,
                      std::string("{\"current\":\"") + json_escape(g_robot_state.current_frame) + "\"}");
    return 0;
  }
  if (verb == "define" && args.size() >= 4U) {
    request.verb = ShellRobotVerb::Add;
    const std::string name = lower_copy(args[3]);
    if (std::find(g_robot_state.frames.begin(), g_robot_state.frames.end(), name) == g_robot_state.frames.end()) {
      g_robot_state.frames.push_back(name);
    }
    emit_robot_result(ctx, true, request,
                      "frame defined: " + name,
                      std::string("{\"defined\":\"") + json_escape(name) + "\"}");
    return 0;
  }
  shell_write_line(ctx.state, "Usage: robot frame list|status|set <name>|define <name>");
  return 1;
}

int handle_limits(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Limits;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "list" || verb == "status") {
    request.verb = (verb == "list") ? ShellRobotVerb::List : ShellRobotVerb::Status;
    std::string text = "limits profile=" + g_robot_state.limits_profile + " joint=t[-270,270] j1..j6[-90,90]";
    std::string json = "{\"limits_profile\":\"" + json_escape(g_robot_state.limits_profile) + "\",\"joint_ranges\":[[-270,270],[-90,90],[-90,90],[-90,90],[-90,90],[-90,90],[-90,90]]}";
    emit_robot_result(ctx, true, request, text, json);
    return 0;
  }
  if (verb == "set" && args.size() >= 4U) {
    request.verb = ShellRobotVerb::Set;
    g_robot_state.limits_profile = lower_copy(args[3]);
    emit_robot_result(ctx, true, request,
                      "limits profile set: " + g_robot_state.limits_profile,
                      std::string("{\"limits_profile\":\"") + json_escape(g_robot_state.limits_profile) + "\"}");
    return 0;
  }
  if (verb == "reset") {
    request.verb = ShellRobotVerb::Reset;
    g_robot_state.limits_profile = "default";
    emit_robot_result(ctx, true, request,
                      "limits reset: default",
                      "{\"limits_profile\":\"default\"}");
    return 0;
  }
  shell_write_line(ctx.state, "Usage: robot limits list|status|set <profile>|reset");
  return 1;
}

int handle_math(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Math;
  sync_robot_math_from_web_once();
  if (args.size() < 3U) {
    request.verb = ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request, "math status", math_registry_json());
    return 0;
  }
  const std::string subject = lower_copy(args[2]);
  if (subject == "list" || subject == "status") {
    request.verb = (subject == "list") ? ShellRobotVerb::List : ShellRobotVerb::Status;
    std::string text = "math backend=" + std::string(web_server_get_ik_compute_preference()) +
                       " solver=" + g_robot_state.current_solver +
                       " jac=" + g_robot_state.current_jacobian +
                       " null=" + g_robot_state.current_nullspace +
                       " units=" + g_robot_state.current_units +
                       " frame=" + g_robot_state.current_frame +
                       " model=" + g_robot_state.current_model +
                       " profile=" + g_robot_state.current_profile +
                       " trajectory=" + g_robot_state.current_trajectory +
                       " seed=" + g_robot_state.seed_policy +
                       " limits=" + g_robot_state.limits_profile +
                       " path=" + g_robot_state.path_height_mode +
                       " turret=" + g_robot_state.turret_mode;
    emit_robot_result(ctx, true, request, text, math_registry_json());
    return 0;
  }
  if (subject == "planner") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list" || verb == "status") {
      request.verb = (verb == "list") ? ShellRobotVerb::List : ShellRobotVerb::Status;
      std::string text = "planner path=" + g_robot_state.path_height_mode +
                         " ground_z=" + std::to_string(g_robot_state.ground_z_mm) +
                         " turret=" + g_robot_state.turret_mode +
                         " cart_step=" + std::to_string(g_robot_state.cart_step_mm) +
                         " yaw_step=" + std::to_string(g_robot_state.yaw_step_deg) +
                         " jump=" + std::to_string(g_robot_state.jump_revolute_deg);
      emit_robot_result(ctx, true, request, text, math_registry_json());
      return 0;
    }
    if (verb == "set") {
      if (args.size() < 6U) {
        shell_write_line(ctx.state, "Usage: robot math planner set path ground|elevated | turret auto_shortest|shortest|stable | ground-z <mm> | cart-step <mm> | yaw-step <deg> | jump <deg> | negative-z on|off");
        return 1;
      }
      const std::string key = lower_copy(args[4]);
      if (key == "path" || key == "height" || key == "path-height") {
        const std::string mode = canonical_path_height_mode(args[5]);
        if (mode.empty()) {
          shell_write_line(ctx.state, "robot math planner set path: use ground or elevated");
          return 1;
        }
        g_robot_state.path_height_mode = mode;
      } else if (key == "turret") {
        const std::string mode = canonical_turret_mode(args[5]);
        if (mode.empty()) {
          shell_write_line(ctx.state, "robot math planner set turret: use auto_shortest, shortest or stable");
          return 1;
        }
        g_robot_state.turret_mode = mode;
      } else if (key == "ground-z" || key == "ground_z" || key == "zmin" || key == "z-min") {
        if (!parse_float_text(args[5], &g_robot_state.ground_z_mm)) {
          shell_write_line(ctx.state, "robot math planner set ground-z: numeric mm value required");
          return 1;
        }
      } else if (key == "cart-step" || key == "cart_step") {
        if (!parse_float_text(args[5], &g_robot_state.cart_step_mm) || g_robot_state.cart_step_mm < 1.0f) {
          shell_write_line(ctx.state, "robot math planner set cart-step: value must be >= 1");
          return 1;
        }
      } else if (key == "yaw-step" || key == "yaw_step") {
        if (!parse_float_text(args[5], &g_robot_state.yaw_step_deg) || g_robot_state.yaw_step_deg < 0.1f) {
          shell_write_line(ctx.state, "robot math planner set yaw-step: value must be >= 0.1");
          return 1;
        }
      } else if (key == "jump" || key == "jump-thr" || key == "jump_revolute_deg") {
        if (!parse_float_text(args[5], &g_robot_state.jump_revolute_deg) || g_robot_state.jump_revolute_deg < 1.0f) {
          shell_write_line(ctx.state, "robot math planner set jump: value must be >= 1");
          return 1;
        }
      } else if (key == "negative-z" || key == "negative_z") {
        bool enabled = false;
        if (!parse_on_off(args[5], &enabled)) {
          shell_write_line(ctx.state, "robot math planner set negative-z: use on or off");
          return 1;
        }
        g_robot_state.allow_negative_z_input = enabled;
      } else {
        shell_write_line(ctx.state, "robot math planner set: unknown key");
        return 1;
      }
      request.verb = ShellRobotVerb::Set;
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "planner updated", math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math planner list|status|set ...");
    return 1;
  }
  if (subject == "onboard") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    request.resource = ShellRobotResource::Math;
    if (verb == "status" || verb == "stats") {
      request.verb = ShellRobotVerb::Status;
      mros::experimental::WorkerSnapshot snapshot {};
      mros::experimental::worker_get_snapshot(&snapshot);
      std::string text = "onboard-s3 ";
      text += snapshot.enabled ? "enabled" : "disabled";
      text += snapshot.active ? " active" : " sleeping";
      text += " wake_count=" + std::to_string(snapshot.wake_count);
      emit_robot_result(ctx, true, request, text,
                        worker_json_string(mros::experimental::worker_status_json()));
      return 0;
    }
    if (verb == "enable" || verb == "disable") {
      request.verb = ShellRobotVerb::Set;
      const bool enabled = (verb == "enable");
      const bool saved = mros::experimental::worker_set_enabled(enabled);
      emit_robot_result(ctx, saved, request,
                        std::string("onboard-s3 ") + (enabled ? "enabled" : "disabled"),
                        worker_json_string(mros::experimental::worker_status_json()));
      return saved ? 0 : 1;
    }
    if (verb == "clear") {
      request.verb = ShellRobotVerb::Clear;
      mros::experimental::worker_clear_stats();
      emit_robot_result(ctx, true, request, "onboard-s3 stats cleared",
                        worker_json_string(mros::experimental::worker_status_json()));
      return 0;
    }
    if (verb == "cancel") {
      request.verb = ShellRobotVerb::Stop;
      mros::experimental::worker_cancel();
      emit_robot_result(ctx, true, request, "onboard-s3 cancel requested",
                        worker_json_string(mros::experimental::worker_status_json()));
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math onboard status|enable|disable|stats|clear|cancel");
    return 1;
  }
  if (subject == "backend") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list") {
      request.verb = ShellRobotVerb::List;
      emit_robot_result(ctx, true, request,
                        "backends: auto, web, onboard-s3, t41-qspi, t41-esp-now",
                        "{\"backends\":[\"auto\",\"web\",\"onboard-s3\",\"t41-qspi\",\"t41-esp-now\"]}");
      return 0;
    }
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request,
                        "backend: " + std::string(web_server_get_ik_compute_preference()),
                        math_registry_json());
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      const std::string mode = canonical_backend_mode(args[4]);
      if (mode.empty() || mode == "status") {
        shell_write_line(ctx.state, "robot math backend set: invalid backend");
        return 1;
      }
      request.action = ShellRobotAction::CalcPreference;
      copy_field(request.calc_mode, sizeof(request.calc_mode), mode);
      std::string text;
      const bool ok = dispatch_robot_request(ctx, request, &text);
      emit_robot_result(ctx, ok, request, text, math_registry_json());
      return ok ? 0 : 1;
    }
    shell_write_line(ctx.state, "Usage: robot math backend list|status|set <auto|web|onboard-s3|t41-qspi|t41-esp-now>");
    return 1;
  }
  if (subject == "solver") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list") {
      request.verb = ShellRobotVerb::List;
      emit_robot_result(ctx, true, request,
                        "solvers: dls, qp, svd-robust",
                        math_registry_json());
      return 0;
    }
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request,
                        "solver: " + g_robot_state.current_solver,
                        math_registry_json());
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      const std::string solver = canonical_solver(args[4]);
      if (solver.empty()) {
        shell_write_line(ctx.state, "robot math solver set: invalid solver");
        return 1;
      }
      g_robot_state.current_solver = solver;
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "solver set: " + solver, math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math solver list|set <solver>");
    return 1;
  }
  if (subject == "jacobian") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list") {
      request.verb = ShellRobotVerb::List;
      emit_robot_result(ctx, true, request,
                        "jacobians: numerical, geometric, spatial",
                        math_registry_json());
      return 0;
    }
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request,
                        "jacobian: " + g_robot_state.current_jacobian,
                        math_registry_json());
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      const std::string mode = canonical_jacobian_mode(args[4]);
      if (mode.empty()) {
        shell_write_line(ctx.state, "robot math jacobian set: invalid mode");
        return 1;
      }
      g_robot_state.current_jacobian = mode;
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "jacobian set: " + mode, math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math jacobian list|status|set numerical|geometric|spatial");
    return 1;
  }
  if (subject == "nullspace") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list") {
      request.verb = ShellRobotVerb::List;
      emit_robot_result(ctx, true, request,
                        "nullspace modes: joint_centering, off",
                        math_registry_json());
      return 0;
    }
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request,
                        "nullspace: " + g_robot_state.current_nullspace,
                        math_registry_json());
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      const std::string mode = canonical_nullspace_mode(args[4]);
      if (mode.empty()) {
        shell_write_line(ctx.state, "robot math nullspace set: invalid mode");
        return 1;
      }
      g_robot_state.current_nullspace = mode;
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "nullspace set: " + mode, math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math nullspace list|status|set joint_centering|off");
    return 1;
  }
  if (subject == "units") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request, "units: " + g_robot_state.current_units, math_registry_json());
      return 0;
    }
    if (verb == "list") {
      request.verb = ShellRobotVerb::List;
      emit_robot_result(ctx, true, request,
                        "units: mm, deg, rad",
                        std::string("{\"units\":") + json_string_array(g_robot_state.units) + "}");
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      const std::string units = canonical_units(args[4]);
      if (units.empty()) {
        shell_write_line(ctx.state, "robot math units set: invalid units");
        return 1;
      }
      g_robot_state.current_units = units;
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "units set: " + units, math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math units list|status|set mm|deg|rad");
    return 1;
  }
  if (subject == "frame") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list") {
      request.verb = ShellRobotVerb::List;
      emit_robot_result(ctx, true, request,
                        "frames: base, tool, world, camera",
                        std::string("{\"frames\":") + json_string_array(g_robot_state.frames) + "}");
      return 0;
    }
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request, "math frame: " + g_robot_state.current_frame, math_registry_json());
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      g_robot_state.current_frame = lower_copy(args[4]);
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "math frame set: " + g_robot_state.current_frame, math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math frame list|status|set <name>");
    return 1;
  }
  if (subject == "model") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list") {
      request.verb = ShellRobotVerb::List;
      emit_robot_result(ctx, true, request,
                        "models: mros-7dof-v1, mros-7dof-web",
                        std::string("{\"models\":") + json_string_array(g_robot_state.models) + "}");
      return 0;
    }
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request, "model: " + g_robot_state.current_model, math_registry_json());
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      g_robot_state.current_model = args[4];
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "model set: " + g_robot_state.current_model, math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math model list|status|set <name>");
    return 1;
  }
  if (subject == "profile") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list") {
      request.verb = ShellRobotVerb::List;
      emit_robot_result(ctx, true, request,
                        "profiles: default, safe, fast, precision",
                        std::string("{\"profiles\":") + json_string_array(g_robot_state.profiles) + "}");
      return 0;
    }
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request, "profile: " + g_robot_state.current_profile, math_registry_json());
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      g_robot_state.current_profile = args[4];
      emit_robot_result(ctx, true, request, "profile set: " + g_robot_state.current_profile, math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math profile list|status|set <name>");
    return 1;
  }
  if (subject == "seed") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list") {
      request.verb = ShellRobotVerb::List;
      emit_robot_result(ctx, true, request,
                        "seed policies: current, zero, center, multi",
                        std::string("{\"seed_policies\":") + json_string_array(g_robot_state.seed_policies) + "}");
      return 0;
    }
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request, "seed policy: " + g_robot_state.seed_policy, math_registry_json());
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      const std::string seed = canonical_seed_policy(args[4]);
      if (seed.empty()) {
        shell_write_line(ctx.state, "robot math seed set: invalid seed policy");
        return 1;
      }
      g_robot_state.seed_policy = seed;
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "seed policy set: " + g_robot_state.seed_policy, math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math seed list|status|set current|zero|center|multi");
    return 1;
  }
  if (subject == "limits") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list") {
      request.verb = ShellRobotVerb::List;
      emit_robot_result(ctx, true, request,
                        "limits profiles: default, safe, precision",
                        std::string("{\"limits_profiles\":") + json_string_array(g_robot_state.limits_profiles) + "}");
      return 0;
    }
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request, "limits profile: " + g_robot_state.limits_profile, math_registry_json());
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      const std::string limits = canonical_limits_profile(args[4]);
      if (limits.empty()) {
        shell_write_line(ctx.state, "robot math limits set: invalid limits profile");
        return 1;
      }
      g_robot_state.limits_profile = limits;
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "limits profile set: " + g_robot_state.limits_profile, math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math limits list|status|set default|safe|precision");
    return 1;
  }
  if (subject == "singularity") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request,
                        "singularity threshold: " + std::to_string(g_robot_state.singularity_threshold),
                        math_registry_json());
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      float value = 0.0f;
      if (!parse_float_text(args[4], &value) || value <= 0.0f) {
        shell_write_line(ctx.state, "robot math singularity set: positive numeric value required");
        return 1;
      }
      g_robot_state.singularity_threshold = value;
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "singularity threshold set", math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math singularity status|set <threshold>");
    return 1;
  }
  if (subject == "tuning") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request,
                        "tuning pos_tol=" + std::to_string(g_robot_state.pos_tol_mm) +
                            "mm ori_tol=" + std::to_string(g_robot_state.ori_tol_deg) +
                            "deg alpha=" + std::to_string(g_robot_state.alpha_step) +
                            " null_gain=" + std::to_string(g_robot_state.null_gain) +
                            " lambda=" + std::to_string(g_robot_state.lambda_max) +
                            " max_step=" + std::to_string(g_robot_state.max_step_deg) +
                            " max_iter=" + std::to_string(g_robot_state.max_iter),
                        math_registry_json());
      return 0;
    }
    if (verb == "set" && args.size() >= 6U) {
      request.verb = ShellRobotVerb::Set;
      const std::string field = lower_copy(args[4]);
      float value = 0.0f;
      uint32_t int_value = 0U;
      bool ok = false;
      if (field == "max_iter") {
        ok = parse_u32_text(args[5], &int_value) && int_value > 0U && int_value <= 2000U;
        if (ok) g_robot_state.max_iter = static_cast<uint16_t>(int_value);
      } else {
        ok = parse_float_text(args[5], &value);
        if (ok) {
          if (field == "pos_tol_mm" && value > 0.0f) g_robot_state.pos_tol_mm = value;
          else if ((field == "ori_tol_deg" || field == "ori_tol_rad") && value > 0.0f) {
            g_robot_state.ori_tol_deg = (field == "ori_tol_rad") ? (value * 57.2957795f) : value;
          } else if (field == "alpha_step" && value > 0.0f) g_robot_state.alpha_step = value;
          else if (field == "null_gain" && value >= 0.0f) g_robot_state.null_gain = value;
          else if (field == "lambda_max" && value >= 0.0f) g_robot_state.lambda_max = value;
          else if (field == "max_step_deg" && value > 0.0f) g_robot_state.max_step_deg = value;
          else if (field == "singularity_threshold" && value > 0.0f) g_robot_state.singularity_threshold = value;
          else ok = false;
        }
      }
      if (!ok) {
        shell_write_line(ctx.state, "robot math tuning set: invalid field/value");
        return 1;
      }
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "math tuning updated", math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math tuning status|set <pos_tol_mm|ori_tol_deg|ori_tol_rad|alpha_step|null_gain|lambda_max|max_step_deg|max_iter|singularity_threshold> <value>");
    return 1;
  }
  if (subject == "validate") {
    request.verb = ShellRobotVerb::Validate;
    const bool ok = spi_s3_is_connected() || spi_c3_is_espnow_connected() ||
                    std::strcmp(web_server_get_ik_compute_preference(), "web") == 0 ||
                    (std::strcmp(web_server_get_ik_compute_preference(), "onboard-s3") == 0 &&
                     mros::experimental::worker_is_enabled());
    emit_robot_result(ctx, ok, request,
                      ok ? "math validate: backend/solver/frame configuration is usable" : "math validate: no active compute path",
                      std::string("{\"valid\":") + (ok ? "true" : "false") +
                          ",\"worker\":" + worker_json_string(mros::experimental::worker_status_json()) + "}");
    return ok ? 0 : 1;
  }
  if (subject == "benchmark") {
    request.verb = ShellRobotVerb::Benchmark;
    WebServerDiagSnapshot diag {};
    web_server_get_diag_snapshot(&diag);
    char text[256] = {};
    std::snprintf(text, sizeof(text), "benchmark fk_avg=%.3f fk_max=%.3f pid_avg=%.2f cpu=%luMHz",
                  diag.fk_avg_ms, diag.fk_max_ms, diag.pid_cycle_avg_ms,
                  static_cast<unsigned long>(diag.cpu_freq_applied_mhz));
    std::string json = "{";
    json += "\"fk_avg_ms\":" + std::to_string(diag.fk_avg_ms) + ",";
    json += "\"fk_max_ms\":" + std::to_string(diag.fk_max_ms) + ",";
    json += "\"pid_avg_ms\":" + std::to_string(diag.pid_cycle_avg_ms) + ",";
    json += "\"cpu_mhz\":" + std::to_string(diag.cpu_freq_applied_mhz) + ",";
    json += "\"worker\":" + worker_json_string(mros::experimental::worker_status_json()) + "}";
    emit_robot_result(ctx, true, request, text, json);
    return 0;
  }
  if (subject == "explain") {
    request.verb = ShellRobotVerb::Explain;
    const std::string text =
        "math stack: backend selects execution path, solver selects IK engine, jacobian selects differential model, "
        "nullspace shapes redundancy, frame and units define interpretation, trajectory selects interpolation mode, "
        "limits/model/profile/tuning shape safety and convergence.";
    emit_robot_result(ctx, true, request, text, math_registry_json());
    return 0;
  }
  if (subject == "fk") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      float joints[7] = {};
      fill_current_joint_seed(joints);
      if (std::strcmp(web_server_get_ik_compute_preference(), "onboard-s3") == 0) {
        mros::experimental::WorkerRequest worker_req {};
        worker_req.type = mros::experimental::WorkerJobType::Fk;
        std::snprintf(worker_req.source, sizeof(worker_req.source), "shell");
        for (uint8_t i = 0; i < 7U; ++i) worker_req.joints_deg[i] = joints[i];
        mros::experimental::WorkerResult worker_result {};
        const bool ok = mros::experimental::worker_submit_sync(worker_req, 5000U, &worker_result);
        emit_robot_result(ctx, ok, request, std::string(worker_result.message),
                          worker_json_string(mros::experimental::worker_result_json(worker_result)));
        return ok ? 0 : 1;
      }
      FK_Result_t fk {};
      fk_compute(joints, &fk);
      char text[192] = {};
      std::snprintf(text, sizeof(text), "fk status x=%.1f y=%.1f z=%.1f alpha=%.1f", fk.x, fk.y, fk.z, fk.alpha_deg);
      std::string json = "{";
      json += "\"x\":" + std::to_string(fk.x) + ",";
      json += "\"y\":" + std::to_string(fk.y) + ",";
      json += "\"z\":" + std::to_string(fk.z) + ",";
      json += "\"alpha\":" + std::to_string(fk.alpha_deg) + "}";
      emit_robot_result(ctx, true, request, text, json);
      return 0;
    }
    if ((verb == "solve" || verb == "preview") && args.size() >= 11U) {
      request.verb = (verb == "solve") ? ShellRobotVerb::Solve : ShellRobotVerb::Preview;
      size_t next_index = 0U;
      std::string error;
      if (!parse_joint_values(args, 4U, &request, &next_index, &error)) {
        shell_printf(ctx.state, "robot math fk %s: %s\n", verb.c_str(), error.c_str());
        return 1;
      }
      if (std::strcmp(web_server_get_ik_compute_preference(), "onboard-s3") == 0) {
        mros::experimental::WorkerRequest worker_req {};
        worker_req.type = mros::experimental::WorkerJobType::Fk;
        std::snprintf(worker_req.source, sizeof(worker_req.source), "shell");
        for (uint8_t i = 0; i < 7U; ++i) worker_req.joints_deg[i] = request.joints[i];
        mros::experimental::WorkerResult worker_result {};
        const bool ok = mros::experimental::worker_submit_sync(worker_req, 5000U, &worker_result);
        emit_robot_result(ctx, ok, request, std::string(worker_result.message),
                          worker_json_string(mros::experimental::worker_result_json(worker_result)));
        return ok ? 0 : 1;
      }
      FK_Result_t fk {};
      fk_compute(request.joints, &fk);
      char text[192] = {};
      std::snprintf(text, sizeof(text), "fk %s x=%.1f y=%.1f z=%.1f alpha=%.1f", verb.c_str(), fk.x, fk.y, fk.z, fk.alpha_deg);
      std::string json = "{";
      json += "\"x\":" + std::to_string(fk.x) + ",";
      json += "\"y\":" + std::to_string(fk.y) + ",";
      json += "\"z\":" + std::to_string(fk.z) + ",";
      json += "\"alpha\":" + std::to_string(fk.alpha_deg) + "}";
      emit_robot_result(ctx, true, request, text, json);
      return 0;
    }
    if (verb == "compare") {
      request.verb = ShellRobotVerb::Compare;
      FK_Result_t fk {};
      float joints[7] = {
          spi_s3_get_turret_deg(),
          spi_s3_get_joint_deg(0),
          spi_s3_get_joint_deg(1),
          spi_s3_get_joint_deg(2),
          spi_s3_get_joint_deg(3),
          spi_s3_get_joint_deg(4),
          spi_s3_get_joint_deg(5)};
      fk_compute(joints, &fk);
      char text[256] = {};
      std::snprintf(text, sizeof(text), "fk compare local=(%.1f %.1f %.1f %.1f) t41=(%.1f %.1f %.1f %.1f)",
                    fk.x, fk.y, fk.z, fk.alpha_deg,
                    spi_s3_get_coord_x(), spi_s3_get_coord_y(), spi_s3_get_coord_z(), spi_s3_get_alpha());
      std::string json = "{";
      json += "\"local\":{\"x\":" + std::to_string(fk.x) + ",\"y\":" + std::to_string(fk.y) + ",\"z\":" + std::to_string(fk.z) + ",\"alpha\":" + std::to_string(fk.alpha_deg) + "},";
      json += "\"t41\":{\"x\":" + std::to_string(spi_s3_get_coord_x()) + ",\"y\":" + std::to_string(spi_s3_get_coord_y()) + ",\"z\":" + std::to_string(spi_s3_get_coord_z()) + ",\"alpha\":" + std::to_string(spi_s3_get_alpha()) + "}}";
      emit_robot_result(ctx, true, request, text, json);
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math fk status|solve <7 joints>|preview <7 joints>|compare");
    return 1;
  }
  if (subject == "ik") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "status") {
      request.verb = ShellRobotVerb::Status;
      emit_robot_result(ctx, true, request,
                        "ik backend=" + std::string(web_server_get_ik_compute_preference()) +
                            " solver=" + g_robot_state.current_solver +
                            " jac=" + g_robot_state.current_jacobian +
                            " null=" + g_robot_state.current_nullspace +
                            " seed=" + g_robot_state.seed_policy,
                        math_registry_json());
      return 0;
    }
    if ((verb == "solve" || verb == "preview" || verb == "apply") && args.size() >= 7U) {
      request.resource = ShellRobotResource::Cartesian;
      request.action = ShellRobotAction::Position;
      request.verb = (verb == "solve") ? ShellRobotVerb::Solve :
                     (verb == "apply") ? ShellRobotVerb::Apply : ShellRobotVerb::Preview;
      size_t next_index = 0U;
      std::string error;
      if (!parse_cartesian_target(args, 4U, &request, &next_index, &error)) {
        shell_printf(ctx.state, "robot math ik %s: %s\n", verb.c_str(), error.c_str());
        return 1;
      }
      if (verb == "apply") request.apply = true;
      if (verb == "preview" || verb == "solve") request.apply = false;
      float seed[7] = {};
      fill_current_joint_seed(seed);
      if (std::strcmp(web_server_get_ik_compute_preference(), "onboard-s3") == 0) {
        mros::experimental::WorkerRequest worker_req {};
        worker_req.type = mros::experimental::WorkerJobType::Ik;
        std::snprintf(worker_req.source, sizeof(worker_req.source), "shell");
        worker_req.x = request.position.x;
        worker_req.y = request.position.y;
        worker_req.z = request.position.z;
        for (uint8_t i = 0; i < 7U; ++i) worker_req.joints_deg[i] = seed[i];
        mros::experimental::WorkerResult worker_result {};
        const bool ok = mros::experimental::worker_submit_sync(worker_req, 7000U, &worker_result);
        emit_robot_result(ctx, ok, request, std::string(worker_result.message),
                          worker_json_string(mros::experimental::worker_result_json(worker_result)));
        return ok ? 0 : 1;
      }
      InvKinematicsOptions ik_opts {};
      ik_opts.max_iter = g_robot_state.max_iter;
      ik_opts.pos_tol_mm = g_robot_state.pos_tol_mm;
      ik_opts.max_step_deg = g_robot_state.max_step_deg;
      ik_opts.null_gain = g_robot_state.null_gain;
      InvKinematicsResult local_result {};
      const bool local_ok = inv_kinematics_solve_with_seed(
          request.position.x,
          request.position.y,
          request.position.z,
          seed,
          &ik_opts,
          &local_result);
      std::string text;
      const bool ok = dispatch_robot_request(ctx, request, &text);
      std::string json = "{";
      json += "\"target\":{\"x\":" + std::to_string(request.position.x) + ",\"y\":" + std::to_string(request.position.y) + ",\"z\":" + std::to_string(request.position.z) + "},";
      json += "\"model_revision\":\"" + json_escape(mros::kinematics::kRobotModelRevision) + "\",";
      json += "\"local_ok\":" + std::string(local_ok ? "true" : "false") + ",";
      json += "\"solver\":\"" + json_escape(g_robot_state.current_solver) + "\",";
      json += "\"jacobian\":\"" + json_escape(g_robot_state.current_jacobian) + "\",";
      json += "\"sigma_min\":" + std::to_string(local_result.sigma_min) + ",";
      json += "\"pos_err_mm\":" + std::to_string(local_result.pos_err_mm) + ",";
      json += "\"iterations\":" + std::to_string(local_result.iterations) + ",";
      json += "\"limit_warnings\":" + ik_warnings_json(local_result.warnings) + ",";
      json += "\"joints_deg\":[";
      for (int i = 0; i < 7; ++i) {
        if (i > 0) json += ",";
        json += std::to_string(local_result.joints_deg[i]);
      }
      json += "],";
      json += "\"message\":\"" + json_escape(text) + "\"}";
      emit_robot_result(ctx, ok, request, text, json);
      return ok ? 0 : 1;
    }
    if (verb == "seed") {
      if (args.size() == 4U || (args.size() >= 5U && lower_copy(args[4]) == "status")) {
        request.verb = ShellRobotVerb::Status;
        emit_robot_result(ctx, true, request, "ik seed policy: " + g_robot_state.seed_policy, math_registry_json());
        return 0;
      }
      if (args.size() >= 5U && lower_copy(args[4]) == "list") {
        request.verb = ShellRobotVerb::List;
        emit_robot_result(ctx, true, request,
                          "ik seed policies: current, zero, center, multi",
                          std::string("{\"seed_policies\":") + json_string_array(g_robot_state.seed_policies) + "}");
        return 0;
      }
      const size_t seed_index = (args.size() >= 6U && lower_copy(args[4]) == "set") ? 5U : 4U;
      if (args.size() > seed_index) {
        request.verb = ShellRobotVerb::Set;
        const std::string seed = canonical_seed_policy(args[seed_index]);
        if (seed.empty()) {
          shell_write_line(ctx.state, "robot math ik seed: invalid seed policy");
          return 1;
        }
        g_robot_state.seed_policy = seed;
        publish_robot_math_state();
        emit_robot_result(ctx, true, request,
                          "ik seed policy set: " + g_robot_state.seed_policy,
                          math_registry_json());
        return 0;
      }
    }
    shell_write_line(ctx.state, "Usage: robot math ik status|solve|preview|apply <x y z> [options] | seed list|status|set <name>");
    return 1;
  }
  if (subject == "pid") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list" || verb == "status") {
      request.verb = (verb == "list") ? ShellRobotVerb::List : ShellRobotVerb::Status;
      float kp = 0.0f;
      float ki = 0.0f;
      float kd = 0.0f;
      float imax = 0.0f;
      spi_s3_get_turret_pid(&kp, &ki, &kd, &imax);
      char text[192] = {};
      std::snprintf(text, sizeof(text), "pid kp=%.4f ki=%.4f kd=%.4f imax=%.4f dspc=%.4f",
                    kp, ki, kd, imax, spi_s3_get_turret_dspc());
      std::string json = "{";
      json += "\"kp\":" + std::to_string(kp) + ",";
      json += "\"ki\":" + std::to_string(ki) + ",";
      json += "\"kd\":" + std::to_string(kd) + ",";
      json += "\"imax\":" + std::to_string(imax) + ",";
      json += "\"dspc\":" + std::to_string(spi_s3_get_turret_dspc()) + "}";
      emit_robot_result(ctx, true, request, text, json);
      return 0;
    }
    if (verb == "set" && args.size() >= 8U) {
      request.verb = ShellRobotVerb::Set;
      float kp = 0.0f;
      float ki = 0.0f;
      float kd = 0.0f;
      float imax = 0.0f;
      float dspc = 0.0f;
      if (!parse_float_text(args[4], &kp) || !parse_float_text(args[5], &ki) ||
          !parse_float_text(args[6], &kd) || !parse_float_text(args[7], &imax)) {
        shell_write_line(ctx.state, "robot math pid set: kp ki kd imax required");
        return 1;
      }
      if (args.size() >= 9U && !parse_float_text(args[8], &dspc)) {
        shell_write_line(ctx.state, "robot math pid set: dspc must be numeric");
        return 1;
      }
      spi_s3_set_turret_pid(kp, ki, kd, imax);
      if (args.size() >= 9U) spi_s3_set_turret_dspc(dspc);
      emit_robot_result(ctx, true, request, "pid updated", "{\"updated\":true}");
      return 0;
    }
    if (verb == "reset") {
      request.verb = ShellRobotVerb::Reset;
      spi_s3_set_turret_pid(0.60f, 0.60f, 0.05f, 0.02f);
      spi_s3_set_turret_dspc(0.0f);
      emit_robot_result(ctx, true, request, "pid reset to defaults", "{\"updated\":true}");
      return 0;
    }
    if (verb == "profile" && args.size() >= 6U && lower_copy(args[4]) == "set") {
      request.verb = ShellRobotVerb::Set;
      g_robot_state.current_profile = args[5];
      emit_robot_result(ctx, true, request, "pid profile set: " + g_robot_state.current_profile, math_registry_json());
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot math pid list|status|set kp ki kd imax [dspc]|profile set <name>|reset");
    return 1;
  }
  if (subject == "trajectory") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "list" || verb == "status") {
      request.verb = (verb == "list") ? ShellRobotVerb::List : ShellRobotVerb::Status;
      std::string text = "trajectory mode=" + g_robot_state.current_trajectory + " scale=" +
                         std::to_string(spi_s3_get_joint_traj_time_scale());
      std::string json = "{";
      json += "\"mode\":\"" + json_escape(g_robot_state.current_trajectory) + "\",";
      json += "\"scale\":" + std::to_string(spi_s3_get_joint_traj_time_scale()) + "}";
      emit_robot_result(ctx, true, request, text, json);
      return 0;
    }
    if (verb == "set" && args.size() >= 5U) {
      request.verb = ShellRobotVerb::Set;
      const std::string mode = canonical_trajectory_mode(args[4]);
      if (mode.empty()) {
        shell_write_line(ctx.state, "robot math trajectory set: invalid mode");
        return 1;
      }
      g_robot_state.current_trajectory = mode;
      publish_robot_math_state();
      emit_robot_result(ctx, true, request, "trajectory mode set: " + mode, math_registry_json());
      return 0;
    }
    if (verb == "sample") {
      request.verb = ShellRobotVerb::Get;
      emit_robot_result(ctx, true, request,
                        "trajectory sample: queue_points=" + std::to_string(g_robot_state.path_queue.size()),
                        path_queue_result_json());
      return 0;
    }
    if (verb == "preview") {
      request.verb = ShellRobotVerb::Preview;
      ShellRobotRequest op {};
      reset_request_defaults(&op);
      op.action = ShellRobotAction::PathPreview;
      std::string text;
      const bool ok = dispatch_robot_request(ctx, op, &text);
      emit_robot_result(ctx, ok, request, text, path_queue_result_json());
      return ok ? 0 : 1;
    }
    shell_write_line(ctx.state, "Usage: robot math trajectory list|status|set quintic|heptic|scurve|time-optimal|linear|sample|preview");
    return 1;
  }
  shell_write_line(ctx.state, "Usage: robot math list|status|planner|backend|solver|jacobian|nullspace|units|frame|model|profile|seed|limits|singularity|tuning|validate|benchmark|explain|fk|ik|pid|trajectory");
  return 1;
}

int handle_calibration(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Calibration;
  if (args.size() < 3U) {
    shell_write_line(ctx.state, "Usage: robot calibration servo|encoder|pca ...");
    return 1;
  }
  const std::string subject = lower_copy(args[2]);
  copy_field(request.subject, sizeof(request.subject), subject);
  if (subject == "servo") {
    const std::string verb = args.size() > 3U ? lower_copy(args[3]) : "status";
    if (verb == "status" || verb == "list") {
      request.verb = (verb == "list") ? ShellRobotVerb::List : ShellRobotVerb::Status;
      std::string text = "servo calibration";
      std::string json = "{\"channels\":[";
      bool first = true;
      for (uint8_t ch = 1U; ch <= PCA_TOTAL_CHANNELS; ++ch) {
        PCA9685_ChannelCal_t* cal = pca9685_get_cal(ch);
        if (cal == nullptr) continue;
        char line[160] = {};
        std::snprintf(line, sizeof(line), "\nch%u angle=%.0f..%.0f speed=%.0f/%.0f/%.0f",
                      static_cast<unsigned>(ch),
                      cal->angle_min_us, cal->angle_max_us,
                      cal->speed_min_us, cal->speed_center_us, cal->speed_max_us);
        text += line;
        if (!first) json += ",";
        first = false;
        json += "{";
        json += "\"channel\":" + std::to_string(ch) + ",";
        json += "\"angle_min_us\":" + std::to_string(cal->angle_min_us) + ",";
        json += "\"angle_max_us\":" + std::to_string(cal->angle_max_us) + ",";
        json += "\"speed_min_us\":" + std::to_string(cal->speed_min_us) + ",";
        json += "\"speed_center_us\":" + std::to_string(cal->speed_center_us) + ",";
        json += "\"speed_max_us\":" + std::to_string(cal->speed_max_us) + "}";
      }
      json += "]}";
      emit_robot_result(ctx, true, request, text, json);
      return 0;
    }
    if (verb == "set" && args.size() >= 10U) {
      request.verb = ShellRobotVerb::Set;
      uint32_t channel = 0U;
      if (!parse_u32_text(args[4], &channel) || channel < 1U || channel > PCA_TOTAL_CHANNELS) {
        shell_write_line(ctx.state, "robot calibration servo set: channel must be 1..16");
        return 1;
      }
      PCA9685_ChannelCal_t cal {};
      if (!parse_float_text(args[5], &cal.angle_min_us) ||
          !parse_float_text(args[6], &cal.angle_max_us) ||
          !parse_float_text(args[7], &cal.speed_min_us) ||
          !parse_float_text(args[8], &cal.speed_center_us) ||
          !parse_float_text(args[9], &cal.speed_max_us)) {
        shell_write_line(ctx.state, "robot calibration servo set: five numeric calibration values required");
        return 1;
      }
      pca9685_set_cal(static_cast<uint8_t>(channel), &cal);
      pca9685_save_cal();
      emit_robot_result(ctx, true, request, "servo calibration updated", "{\"updated\":true}");
      return 0;
    }
    shell_write_line(ctx.state, "Usage: robot calibration servo status|list|set <ch> angle_min angle_max speed_min speed_center speed_max");
    return 1;
  }
  if (subject == "encoder" && args.size() >= 4U && lower_copy(args[3]) == "reset") {
    request.verb = ShellRobotVerb::Reset;
    spi_s3_set_reset_encoder();
    emit_robot_result(ctx, true, request, "encoder reset requested", "{\"reset\":true}");
    return 0;
  }
  if (subject == "pca" && args.size() >= 6U && lower_copy(args[3]) == "test") {
    request.verb = ShellRobotVerb::Set;
    uint32_t ch = 0U;
    float us = 0.0f;
    if (!parse_u32_text(args[4], &ch) || !parse_float_text(args[5], &us)) {
      shell_write_line(ctx.state, "robot calibration pca test: <ch> <us> required");
      return 1;
    }
    uint16_t tick = static_cast<uint16_t>(((us * 4096.0f * pca9685_get_frequency()) / 1000000.0f) + 0.5f);
    if (tick > 4095U) tick = 4095U;
    const bool ok = pca9685_set_pwm(static_cast<uint8_t>(ch), 0U, tick);
    emit_robot_result(ctx, ok, request,
                      ok ? "pca test applied" : "pca test failed",
                      std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"tick\":" + std::to_string(tick) + "}");
    return ok ? 0 : 1;
  }
  shell_write_line(ctx.state, "Usage: robot calibration servo ... | encoder reset | pca test <ch> <us>");
  return 1;
}

int handle_diag(ShellContext& ctx, const std::vector<std::string>& args, ShellRobotRequest request) {
  request.resource = ShellRobotResource::Diagnostics;
  const std::string verb = args.size() > 2U ? lower_copy(args[2]) : "status";
  if (verb == "status") {
    request.verb = ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request, summarize_robot_status(), robot_status_json());
    return 0;
  }
  if (verb == "errors") {
    request.verb = ShellRobotVerb::Status;
    char text[256] = {};
    std::snprintf(text, sizeof(text), "t41 crc=%lu marker=%lu | c3(disabled) crc=%lu marker=%lu | err_code=%u",
                  static_cast<unsigned long>(spi_s3_get_crc_errors()),
                  static_cast<unsigned long>(spi_s3_get_marker_errors()),
                  static_cast<unsigned long>(spi_c3_get_crc_errors()),
                  static_cast<unsigned long>(spi_c3_get_marker_errors()),
                  static_cast<unsigned>(spi_s3_get_error_code()));
    std::string json = "{";
    json += "\"t41_qspi_crc\":" + std::to_string(spi_s3_get_crc_errors()) + ",";
    json += "\"t41_qspi_marker\":" + std::to_string(spi_s3_get_marker_errors()) + ",";
    json += "\"c3_crc\":" + std::to_string(spi_c3_get_crc_errors()) + ",";
    json += "\"c3_marker\":" + std::to_string(spi_c3_get_marker_errors()) + ",";
    json += "\"t41_error_code\":" + std::to_string(spi_s3_get_error_code()) + "}";
    emit_robot_result(ctx, true, request, text, json);
    return 0;
  }
  if (verb == "links") {
    request.verb = ShellRobotVerb::Status;
    char text[192] = {};
    std::snprintf(text, sizeof(text), "links t41=%s c3=disabled espnow=%s pca=%s",
                  spi_s3_is_connected() ? "ok" : "down",
                  spi_c3_is_espnow_connected() ? "ok" : "down",
                  pca9685_is_ready() ? "ready" : "down");
    std::string json = "{";
    json += "\"t41\":" + std::string(spi_s3_is_connected() ? "true" : "false") + ",";
    json += "\"c3_disabled\":true,";
    json += "\"espnow\":" + std::string(spi_c3_is_espnow_connected() ? "true" : "false") + ",";
    json += "\"pca\":" + std::string(pca9685_is_ready() ? "true" : "false") + "}";
    emit_robot_result(ctx, true, request, text, json);
    return 0;
  }
  if (verb == "console") {
    request.verb = ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request,
                      "console_rev=" + std::to_string(static_cast<unsigned long>(uart1_cobs_get_log_version())),
                      std::string("{\"console_rev\":") + std::to_string(static_cast<unsigned long>(uart1_cobs_get_log_version())) + "}");
    return 0;
  }
  if (verb == "c3") {
    shell_write_line(ctx.state, "robot diag c3: c3 bu sistemde devre disi.");
    return 1;
  }
  if (verb == "t41") {
    request.verb = ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request,
                      "t41 loop_ms=" + std::to_string(spi_s3_get_loop_ms()) + " devstat=" + std::to_string(spi_s3_get_device_status_code()),
                      std::string("{\"loop_ms\":") + std::to_string(spi_s3_get_loop_ms()) +
                          ",\"device_status\":" + std::to_string(spi_s3_get_device_status_code()) + "}");
    return 0;
  }
  if (verb == "pca") {
    request.verb = ShellRobotVerb::Status;
    emit_robot_result(ctx, true, request,
                      "pca ready=" + std::string(pca9685_is_ready() ? "yes" : "no") + " freq=" + std::to_string(pca9685_get_frequency()),
                      std::string("{\"ready\":") + (pca9685_is_ready() ? "true" : "false") +
                          ",\"freq_hz\":" + std::to_string(pca9685_get_frequency()) + "}");
    return 0;
  }
  if (verb == "linktest") {
    request.verb = ShellRobotVerb::Status;
    const std::string target = args.size() >= 4U ? lower_copy(args[3]) : "all";
    uint32_t duration_s = 10U;
    for (size_t i = 4U; i + 1U < args.size(); ++i) {
      if (args[i] == "--duration") {
        duration_s = static_cast<uint32_t>(std::strtoul(args[i + 1U].c_str(), nullptr, 10));
      }
    }
    duration_s = std::max<uint32_t>(1U, std::min<uint32_t>(duration_s, 120U));
    const bool json = std::find(args.begin(), args.end(), "--json") != args.end();
    if (target == "qspi" || target == "p4" || target == "t41") {
      std::vector<std::string> sub_args = {"spi", "test", "--duration", std::to_string(duration_s)};
      if (json) sub_args.push_back("--json");
      ShellContext sub {ctx.state, sub_args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
      return shell_cmd_spi(sub);
    }
    if (target == "c3") {
    shell_write_line(ctx.state, "robot diag linktest c3: c3 bu sistemde devre disi. 'uart' veya 's3-uart' kullanin.");
      return 1;
    }
    if (target == "uart" || target == "s3-uart") {
      std::vector<std::string> sub_args = {"uart", "test", "--duration", std::to_string(duration_s)};
      if (json) sub_args.push_back("--json");
      ShellContext sub {ctx.state, sub_args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
      return shell_cmd_uart(sub);
    }
    if (target == "all") {
      std::vector<std::string> spi_args = {"spi", "test", "--duration", std::to_string(duration_s)};
      if (json) spi_args.push_back("--json");
      ShellContext spi_ctx {ctx.state, spi_args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
      const int spi_rc = shell_cmd_spi(spi_ctx);
      std::vector<std::string> uart_args = {"uart", "test", "--duration", std::to_string(duration_s)};
      if (json) uart_args.push_back("--json");
      ShellContext uart_ctx {ctx.state, uart_args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
      const int uart_rc = shell_cmd_uart(uart_ctx);
      return (spi_rc == 0 && uart_rc == 0) ? 0 : 1;
    }
    shell_write_line(ctx.state, "robot diag linktest: target must be uart|s3-uart|qspi|all");
    return 1;
  }
  shell_write_line(ctx.state, "Usage: robot diag status|errors|links|console|t41|pca|linktest [uart|s3-uart|qspi|all] [--duration 10] [--json]");
  return 1;
}

int handle_alias_command(ShellContext& ctx, const std::string& sub, const std::vector<std::string>& args) {
  ShellRobotRequest request {};
  reset_request_defaults(&request);

  if (sub == "pos") {
    request.resource = ShellRobotResource::Cartesian;
    request.verb = ShellRobotVerb::Set;
    if (args.size() >= 3U && lower_copy(args[2]) == "default") {
      request.action = ShellRobotAction::DefaultPosition;
      request.verb = ShellRobotVerb::Home;
      std::string text;
      const bool ok = dispatch_robot_request(ctx, request, &text);
      emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
      return ok ? 0 : 1;
    }
    request.action = ShellRobotAction::Position;
    size_t next_index = 0U;
    std::string error;
    if (!parse_cartesian_target(args, 2U, &request, &next_index, &error)) {
      shell_printf(ctx.state, "robot pos: %s\n", error.c_str());
      return 1;
    }
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"message\":\"") + json_escape(text) + "\"}");
    return ok ? 0 : 1;
  }
  if (sub == "mov") {
    std::vector<std::string> canonical = {"robot", "move"};
    canonical.insert(canonical.end(), args.begin() + 2, args.end());
    return handle_move(ctx, canonical, request);
  }
  if (sub == "calc" || sub == "change") {
    request.resource = ShellRobotResource::Math;
    request.verb = ShellRobotVerb::Set;
    request.action = ShellRobotAction::CalcPreference;
    if (args.size() < 3U) {
      copy_field(request.calc_mode, sizeof(request.calc_mode), "status");
    } else {
      const std::string mode = canonical_backend_mode(args[2]);
      if (mode.empty()) {
        shell_write_line(ctx.state, "robot calc/change: backend must be auto, web, t41-qspi, t41-esp-now or status");
        return 1;
      }
      copy_field(request.calc_mode, sizeof(request.calc_mode), mode);
    }
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, math_registry_json());
    return ok ? 0 : 1;
  }
  if (sub == "speed") {
    request.resource = ShellRobotResource::Profile;
    request.verb = ShellRobotVerb::Set;
    request.action = ShellRobotAction::Speed;
    if (args.size() != 3U || !parse_float_text(args[2], &request.value)) {
      shell_write_line(ctx.state, "robot speed: numeric value required");
      return 1;
    }
    request.value = normalize_speed_up(request.value);
    std::string text;
    const bool ok = dispatch_robot_request(ctx, request, &text);
    emit_robot_result(ctx, ok, request, text, std::string("{\"speed_scale\":") + std::to_string(request.value) + "}");
    return ok ? 0 : 1;
  }
  if (sub == "emg") {
    std::vector<std::string> canonical = {"robot", "safety", "emg"};
    canonical.insert(canonical.end(), args.begin() + 2, args.end());
    return handle_safety(ctx, canonical, request);
  }
  if (sub == "home") {
    std::vector<std::string> canonical = {"robot", "joint", "home"};
    return handle_joint(ctx, canonical, request);
  }
  if (sub == "park") {
    std::vector<std::string> canonical = {"robot", "joint", "park"};
    return handle_joint(ctx, canonical, request);
  }
  if (sub == "hold") {
    std::vector<std::string> canonical = {"robot", "safety", "hold"};
    canonical.insert(canonical.end(), args.begin() + 2, args.end());
    return handle_safety(ctx, canonical, request);
  }
  if (sub == "stop") {
    std::vector<std::string> canonical = {"robot", "safety", "stop"};
    return handle_safety(ctx, canonical, request);
  }
  return -1;
}

int handle_health(ShellContext& ctx, const std::vector<std::string>& args) {
  const std::string sub = args.size() >= 3U ? lower_copy(args[2]) : "";
  if (sub == "load-json" || sub == "import") {
    if (args.size() < 4U) {
      shell_write_line(ctx.state, "Usage: robot health load-json <path> [ttl_ms]");
      return 1;
    }
    uint32_t ttl_ms = 5000U;
    if (args.size() >= 5U && !parse_u32_text(args[4], &ttl_ms)) {
      shell_write_line(ctx.state, "robot health load-json: ttl_ms must be an integer");
      return 1;
    }
    std::string raw;
    if (!mros::platform::mros_file_read_all(args[3].c_str(), &raw)) {
      shell_printf(ctx.state, "robot health load-json: cannot read %s\n", args[3].c_str());
      return 1;
    }

    std::string bearing_error;
    std::string actuator_error;
    std::string structural_error;
    const bool bearing_ok = mros::health::bearing::apply_loads_json(raw.c_str(), &bearing_error);
    const bool actuator_ok = mros::health::actuator::apply_joint_loads_json(raw.c_str(), ttl_ms, &actuator_error);
    const bool structural_ok = mros::health::structural::apply_structural_checks_json(raw.c_str(), ttl_ms, &structural_error);
    const bool any_ok = bearing_ok || actuator_ok || structural_ok;

    shell_printf(ctx.state,
                 "robot health: load-json %s %s ttl=%lums\n",
                 any_ok ? "applied" : "failed",
                 args[3].c_str(),
                 static_cast<unsigned long>(ttl_ms));
    shell_printf(ctx.state, "  bearing   : %s", bearing_ok ? "applied" : "failed");
    if (!bearing_error.empty()) shell_printf(ctx.state, " (%s)", bearing_error.c_str());
    shell_write_line(ctx.state, "");
    shell_printf(ctx.state, "  actuator  : %s", actuator_ok ? "applied" : "failed");
    if (!actuator_error.empty()) shell_printf(ctx.state, " (%s)", actuator_error.c_str());
    shell_write_line(ctx.state, "");
    shell_printf(ctx.state, "  structural: %s", structural_ok ? "applied" : "failed");
    if (!structural_error.empty()) shell_printf(ctx.state, " (%s)", structural_error.c_str());
    shell_write_line(ctx.state, "");
    return any_ok ? 0 : 1;
  }

  const bool json = ctx.json_output || arg_present(args, "--json");
  if (json) {
    const std::string bearing_json = mros::health::bearing::format_json();
    const std::string actuator_json = mros::health::actuator::format_json();
    const std::string structural_json = mros::health::structural::format_json();
    shell_write(ctx.state, kRawJsonPrefix);
    shell_write(ctx.state, "{\"bearing\":");
    shell_write(ctx.state, bearing_json.c_str());
    shell_write(ctx.state, ",\"actuator\":");
    shell_write(ctx.state, actuator_json.c_str());
    shell_write(ctx.state, ",\"structural\":");
    shell_write(ctx.state, structural_json.c_str());
    shell_write(ctx.state, "}");
    shell_write_line(ctx.state, "");
    return 0;
  }
  shell_write(ctx.state, mros::health::bearing::format_table().c_str());
  shell_write_line(ctx.state, "");
  shell_write(ctx.state, mros::health::actuator::format_table().c_str());
  shell_write_line(ctx.state, "");
  shell_write(ctx.state, mros::health::structural::format_table().c_str());
  return 0;
}

int handle_actuator(ShellContext& ctx, const std::vector<std::string>& args) {
  const std::string sub = args.size() >= 3U ? lower_copy(args[2]) : "table";
  if (sub == "table" || sub == "status" || sub == "health") {
    shell_write(ctx.state, mros::health::actuator::format_table().c_str());
    return 0;
  }
  if (sub == "json") {
    shell_write(ctx.state, mros::health::actuator::format_json().c_str());
    shell_write_line(ctx.state, "");
    return 0;
  }
  if (sub == "clear") {
    std::string error;
    const bool ok = mros::health::actuator::clear(&error);
    shell_printf(ctx.state, "robot actuator: runtime margins %s\n", ok ? "cleared" : "failed");
    if (!ok && !error.empty()) shell_printf(ctx.state, "robot actuator: %s\n", error.c_str());
    return ok ? 0 : 1;
  }
  if (sub == "load-json") {
    if (args.size() < 4U) {
      shell_write_line(ctx.state, "Usage: robot actuator load-json <path> [ttl_ms]");
      return 1;
    }
    uint32_t ttl_ms = 2000U;
    if (args.size() >= 5U && !parse_u32_text(args[4], &ttl_ms)) {
      shell_write_line(ctx.state, "robot actuator load-json: ttl_ms must be an integer");
      return 1;
    }
    std::string raw;
    if (!mros::platform::mros_file_read_all(args[3].c_str(), &raw)) {
      shell_printf(ctx.state, "robot actuator load-json: cannot read %s\n", args[3].c_str());
      return 1;
    }
    std::string error;
    const bool ok = mros::health::actuator::apply_joint_loads_json(raw.c_str(), ttl_ms, &error);
    shell_printf(ctx.state, "robot actuator: load-json %s %s\n",
                 ok ? "applied" : "failed",
                 args[3].c_str());
    if (!error.empty()) shell_printf(ctx.state, "robot actuator: %s\n", error.c_str());
    return ok ? 0 : 1;
  }
  shell_write_line(ctx.state, "Usage: robot actuator table|json|clear|load-json <path> [ttl_ms]");
  return 1;
}

int handle_structural(ShellContext& ctx, const std::vector<std::string>& args) {
  const std::string sub = args.size() >= 3U ? lower_copy(args[2]) : "table";
  if (sub == "table" || sub == "status" || sub == "health") {
    shell_write(ctx.state, mros::health::structural::format_table().c_str());
    return 0;
  }
  if (sub == "json") {
    shell_write(ctx.state, mros::health::structural::format_json().c_str());
    shell_write_line(ctx.state, "");
    return 0;
  }
  if (sub == "clear") {
    std::string error;
    const bool ok = mros::health::structural::clear(&error);
    shell_printf(ctx.state, "robot structural: runtime checks %s\n", ok ? "cleared" : "failed");
    if (!ok && !error.empty()) shell_printf(ctx.state, "robot structural: %s\n", error.c_str());
    return ok ? 0 : 1;
  }
  if (sub == "load-json") {
    if (args.size() < 4U) {
      shell_write_line(ctx.state, "Usage: robot structural load-json <path> [ttl_ms]");
      return 1;
    }
    uint32_t ttl_ms = 5000U;
    if (args.size() >= 5U && !parse_u32_text(args[4], &ttl_ms)) {
      shell_write_line(ctx.state, "robot structural load-json: ttl_ms must be an integer");
      return 1;
    }
    std::string raw;
    if (!mros::platform::mros_file_read_all(args[3].c_str(), &raw)) {
      shell_printf(ctx.state, "robot structural load-json: cannot read %s\n", args[3].c_str());
      return 1;
    }
    std::string error;
    const bool ok = mros::health::structural::apply_structural_checks_json(raw.c_str(), ttl_ms, &error);
    shell_printf(ctx.state, "robot structural: load-json %s %s\n",
                 ok ? "applied" : "failed",
                 args[3].c_str());
    if (!error.empty()) shell_printf(ctx.state, "robot structural: %s\n", error.c_str());
    return ok ? 0 : 1;
  }
  shell_write_line(ctx.state, "Usage: robot structural table|json|clear|load-json <path> [ttl_ms]");
  return 1;
}

int handle_bearing(ShellContext& ctx, const std::vector<std::string>& args) {
  const std::string sub = args.size() >= 3U ? lower_copy(args[2]) : "table";
  if (sub == "table" || sub == "status" || sub == "health") {
    shell_write(ctx.state, mros::health::bearing::format_table().c_str());
    return 0;
  }
  if (sub == "json") {
    shell_write(ctx.state, mros::health::bearing::format_json().c_str());
    shell_write_line(ctx.state, "");
    return 0;
  }
  if (sub == "reload") {
    const bool ok = mros::health::bearing::reload_config();
    shell_printf(ctx.state, "robot bearing: config %s (%s)\n",
                 ok ? "loaded" : "not loaded",
                 mros::health::bearing::config_path());
    if (!ok) {
      shell_write(ctx.state, mros::health::bearing::format_config_help().c_str());
    }
    return ok ? 0 : 1;
  }
  if (sub == "import") {
    if (args.size() < 4U) {
      shell_write_line(ctx.state, "robot bearing import: source JSON path required");
      shell_printf(ctx.state, "active config: %s\n", mros::health::bearing::config_path());
      return 1;
    }
    std::string error;
    const bool ok = mros::health::bearing::import_config_from_path(args[3].c_str(), &error);
    shell_printf(ctx.state, "robot bearing: import %s %s -> %s\n",
                 ok ? "OK" : "failed",
                 args[3].c_str(),
                 mros::health::bearing::config_path());
    if (!ok && !error.empty()) {
      shell_printf(ctx.state, "robot bearing: %s\n", error.c_str());
    }
    return ok ? 0 : 1;
  }
  if (sub == "reset-runtime") {
    const bool ok = mros::health::bearing::reset_runtime();
    shell_printf(ctx.state, "robot bearing: runtime reset %s (%s)\n",
                 ok ? "saved" : "pending",
                 mros::health::bearing::runtime_path());
    return ok ? 0 : 1;
  }
  if (sub == "load") {
    if (args.size() < 6U) {
      shell_write_line(ctx.state,
                       "Usage: robot bearing load <placement> <radial_N> <axial_N> [equivalent_N] [source] [ttl_ms]");
      return 1;
    }
    float radial = 0.0f;
    float axial = 0.0f;
    float equivalent = 0.0f;
    uint32_t ttl_ms = 2000U;
    if (!parse_float_text(args[4], &radial) || !parse_float_text(args[5], &axial)) {
      shell_write_line(ctx.state, "robot bearing load: radial_N and axial_N must be numbers");
      return 1;
    }
    if (args.size() >= 7U && !parse_float_text(args[6], &equivalent)) {
      shell_write_line(ctx.state, "robot bearing load: equivalent_N must be a number");
      return 1;
    }
    const char* source = args.size() >= 8U ? args[7].c_str() : "shell";
    if (args.size() >= 9U && !parse_u32_text(args[8], &ttl_ms)) {
      shell_write_line(ctx.state, "robot bearing load: ttl_ms must be an integer");
      return 1;
    }
    std::string error;
    const bool ok = mros::health::bearing::set_runtime_load(
        args[3].c_str(), radial, axial, equivalent, source, ttl_ms, &error);
    shell_printf(ctx.state,
                 "robot bearing: runtime load %s placement=%s radial=%.1fN axial=%.1fN equiv=%.1fN source=%s ttl=%lums\n",
                 ok ? "set" : "failed",
                 args[3].c_str(),
                 radial,
                 axial,
                 equivalent,
                 source,
                 static_cast<unsigned long>(ttl_ms));
    if (!ok && !error.empty()) shell_printf(ctx.state, "robot bearing: %s\n", error.c_str());
    return ok ? 0 : 1;
  }
  if (sub == "clear-load") {
    if (args.size() < 4U) {
      shell_write_line(ctx.state, "Usage: robot bearing clear-load <placement>");
      return 1;
    }
    std::string error;
    const bool ok = mros::health::bearing::clear_runtime_load(args[3].c_str(), &error);
    shell_printf(ctx.state, "robot bearing: runtime load %s placement=%s\n",
                 ok ? "cleared" : "failed",
                 args[3].c_str());
    if (!ok && !error.empty()) shell_printf(ctx.state, "robot bearing: %s\n", error.c_str());
    return ok ? 0 : 1;
  }
  if (sub == "load-json") {
    if (args.size() < 4U) {
      shell_write_line(ctx.state, "Usage: robot bearing load-json <path>");
      return 1;
    }
    std::string raw;
    if (!mros::platform::mros_file_read_all(args[3].c_str(), &raw)) {
      shell_printf(ctx.state, "robot bearing load-json: cannot read %s\n", args[3].c_str());
      return 1;
    }
    std::string error;
    const bool ok = mros::health::bearing::apply_loads_json(raw.c_str(), &error);
    shell_printf(ctx.state, "robot bearing: load-json %s %s\n",
                 ok ? "applied" : "failed",
                 args[3].c_str());
    if (!error.empty()) shell_printf(ctx.state, "robot bearing: %s\n", error.c_str());
    return ok ? 0 : 1;
  }
  if (sub == "config") {
    shell_write(ctx.state, mros::health::bearing::format_config_help().c_str());
    return 0;
  }
  shell_write_line(ctx.state, "Usage: robot bearing table|json|reload|import <path>|reset-runtime|load|clear-load|load-json|config");
  return 1;
}

}  // namespace

void shell_help_robot(ShellState& state) {
  shell_write_line(state, "Usage: robot <resource> <verb> [args] [--json] [--preview|--apply]");
  shell_write_line(state, "Canonical robot framework:");
  shell_write_line(state, "  robot power status|get|set on|off");
  shell_write_line(state, "  robot safety status|emg on|off|hold on|off|stop|reset");
  shell_write_line(state, "  robot health [--json]|load-json <path> [ttl_ms]");
  shell_write_line(state, "  robot actuator table|json|clear|load-json <path> [ttl_ms]");
  shell_write_line(state, "  robot structural table|json|clear|load-json <path> [ttl_ms]");
  shell_write_line(state, "  robot bearing table|json|reload|import <path>|reset-runtime|load|clear-load|load-json|config");
  shell_write_line(state, "  robot status [summary|full]");
  shell_write_line(state, "  robot telemetry status");
  shell_write_line(state, "  robot turret status|set <deg>|zero|pid status");
  shell_write_line(state, "  robot gripper status|set <pct>|open|close");
  shell_write_line(state, "  robot joint status|list|set|apply|home|park|zero|jog");
  shell_write_line(state, "  robot cartesian status|set|preview|apply <x y z> [--roll deg] [--pitch auto|deg|--alpha auto|deg] [--yaw deg]");
  shell_write_line(state, "  robot move <x y z> [--speed 0.1..1.0] [--time ms] [--roll deg] [--pitch auto|deg|--alpha auto|deg] [--yaw deg]");
  shell_write_line(state, "  robot move preview|apply --from x y z --to x y z [--speed 0.1..1.0]");
  shell_write_line(state, "  robot path status|list|add|insert|remove|clear|preview|run|export|import");
  shell_write_line(state, "  robot motion block list|add|clear|preview|compile|apply");
  shell_write_line(state, "  robot profile list|status|set|save|delete");
  shell_write_line(state, "  robot model list|status|set|describe");
  shell_write_line(state, "  robot frame list|status|set|define");
  shell_write_line(state, "  robot limits list|status|set|reset");
  shell_write_line(state, "  robot math list|status|planner|backend|solver|jacobian|nullspace|units|frame|model|profile|seed|limits|singularity|tuning|validate|benchmark|explain|fk|ik|pid|trajectory");
  shell_write_line(state, "  robot calibration servo|encoder|pca ...");
  shell_write_line(state, "  robot diag status|errors|links|console|t41|pca|linktest [uart|s3-uart|qspi|all] [--duration 10] [--json]");
  shell_write_line(state, "Alias commands:");
  shell_write_line(state, "  robot pos ... | mov ... | calc ... | change ... | speed ... | emg ... | home | park");
}

int shell_cmd_robot(ShellContext& ctx) {
  if (ctx.args.size() <= 1U) {
    shell_help_robot(ctx.state);
    return 1;
  }
  if (ctx.args[1] == "--help" || ctx.args[1] == "-h") {
    shell_help_robot(ctx.state);
    return 0;
  }
  const std::string sub = lower_copy(ctx.args[1]);
  const int alias_result = handle_alias_command(ctx, sub, ctx.args);
  if (alias_result >= 0) return alias_result;

  ShellRobotRequest request {};
  reset_request_defaults(&request);

  if (sub == "power") return handle_power(ctx, ctx.args, request);
  if (sub == "safety") return handle_safety(ctx, ctx.args, request);
  if (sub == "health") return handle_health(ctx, ctx.args);
  if (sub == "actuator") return handle_actuator(ctx, ctx.args);
  if (sub == "structural") return handle_structural(ctx, ctx.args);
  if (sub == "bearing") return handle_bearing(ctx, ctx.args);
  if (sub == "status") return handle_status_resource(ctx, ctx.args, request);
  if (sub == "telemetry") return handle_telemetry(ctx, ctx.args, request);
  if (sub == "turret") return handle_turret(ctx, ctx.args, request);
  if (sub == "gripper") return handle_gripper(ctx, ctx.args, request);
  if (sub == "joint") return handle_joint(ctx, ctx.args, request);
  if (sub == "cartesian") return handle_cartesian(ctx, ctx.args, request);
  if (sub == "move") return handle_move(ctx, ctx.args, request);
  if (sub == "path") return handle_path(ctx, ctx.args, request);
  if (sub == "motion") return handle_motion(ctx, ctx.args, request);
  if (sub == "profile") return handle_profile(ctx, ctx.args, request);
  if (sub == "model") return handle_model(ctx, ctx.args, request);
  if (sub == "frame") return handle_frame(ctx, ctx.args, request);
  if (sub == "limits") return handle_limits(ctx, ctx.args, request);
  if (sub == "math") return handle_math(ctx, ctx.args, request);
  if (sub == "calibration") return handle_calibration(ctx, ctx.args, request);
  if (sub == "diag") return handle_diag(ctx, ctx.args, request);

  shell_printf(ctx.state, "robot: unknown resource '%s'\n", ctx.args[1].c_str());
  shell_help_robot(ctx.state);
  return 1;
}

}  // namespace mros::shell
