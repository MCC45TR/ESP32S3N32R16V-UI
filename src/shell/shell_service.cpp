#include "src/shell/shell_service.h"

#include "src/security/ssh_identity.h"

#include "src/shell/mros_shell.h"
#include "src/shell/mshell_remote.h"

#include <esp_ota_ops.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "src/comm_interfaces/spi/spi_c3_master.h"
#include "src/comm_interfaces/spi/spi_t41_link.h"
#include "src/comm_interfaces/uart/uart_cobs.h"
#include "src/drivers/i2c_pca9685.h"
#include "src/drivers/storage/logger_driver.h"
#include "src/drivers/utils/mros_console.h"
#include "src/platform/mros_system.h"
#include "src/platform/mros_time.h"
#include "src/utils/mros_json_writer.h"
#include "src/web/server/trajectory_handler.h"
#include "src/web/server/wifi_manager.h"
#include "src/web/web_server.h"

namespace mros::shell::service {
namespace {

constexpr const char* kShellHostname = "deuscara-s3v";
constexpr const char* kShellFirmwareName = "MROS DEUSCARA Bridge";
constexpr const char* kShellBoardName = "ESP32-S3V Bridge";
constexpr const char* kShellMountPoint = "/littlefs";
constexpr size_t kShellPayloadSize = 256U;
constexpr size_t kShellJsonCapacity = 16384U;
constexpr size_t kShellStreamChunkBytes = 768U;
constexpr size_t kShellFinalOutputMaxBytes = 10000U;
constexpr size_t kShellResponsePoolSlots = 16U;
constexpr size_t kShellProtocolSlots = 16U;
constexpr size_t kShellClientIdentitySlots = 16U;
constexpr size_t kShellClientUsernameBytes = 33U;
constexpr size_t kShellBinHeaderLen = 32U;
// Protocol format names: shell-bin-v1, shell-json-v1.
constexpr const char* kShellBinMagic = "MSH1";
constexpr uint32_t kShellInitialCreditBytes = 64U * 1024U;
constexpr UBaseType_t kShellRequestQueueLength = 8U;
constexpr UBaseType_t kShellResponseQueueLength = 64U;
constexpr size_t kShellWebPaneSlots = 16U;
constexpr uint8_t kShellMaxPanesPerClient = 4U;

enum ShellBinFrameType : uint8_t {
  SHELL_BIN_FRAME_START = 1U,
  SHELL_BIN_FRAME_STDOUT = 2U,
  SHELL_BIN_FRAME_STDERR_LOGICAL = 3U,
  SHELL_BIN_FRAME_FINAL = 4U,
  SHELL_BIN_FRAME_PROMPT = 5U,
  SHELL_BIN_FRAME_STATE = 6U,
  SHELL_BIN_FRAME_COMPLETE = 7U,
  SHELL_BIN_FRAME_ERROR = 8U,
  SHELL_BIN_FRAME_ACK = 9U,
  SHELL_BIN_FRAME_METRIC = 10U,
};

constexpr uint8_t SHELL_BIN_FLAG_UTF8 = 1U << 0;
constexpr uint8_t SHELL_BIN_FLAG_ANSI = 1U << 1;
constexpr uint8_t SHELL_BIN_FLAG_TRUNCATED = 1U << 2;
constexpr uint8_t SHELL_BIN_FLAG_MORE = 1U << 3;

char* alloc_shell_text_buffer(const size_t bytes) {
  if (bytes == 0U) {
    return nullptr;
  }
  char* ptr = static_cast<char*>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (ptr != nullptr) {
    return ptr;
  }
  return static_cast<char*>(
      heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

TaskStatus_t* alloc_task_status_buffer(const size_t count) {
  if (count == 0U) {
    return nullptr;
  }
  return static_cast<TaskStatus_t*>(
      heap_caps_malloc(sizeof(TaskStatus_t) * count,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

struct ShellWebRequest {
  WebRequestType type = WebRequestType::State;
  uint32_t client_id = 0U;
  uint8_t pane_id = 0U;
  uint16_t command_id = 0U;
  char payload[kShellPayloadSize] = {};
};

struct ShellWebResponse {
  uint32_t client_id = 0U;
  ShellWebOutboundKind kind = ShellWebOutboundKind::TextJson;
  char* data = nullptr;
  size_t len = 0U;
  bool pooled = false;
};

struct ShellResponsePoolSlot {
  char* data = nullptr;
  bool in_use = false;
};

struct ShellWebStreamContext {
  uint32_t client_id = 0U;
  uint8_t pane_id = 0U;
  uint16_t command_id = 0U;
  uint32_t session_id = 0U;
  bool dropped = false;
  bool sent_any = false;
  uint32_t chunk_count = 0U;
  uint32_t byte_count = 0U;
  char pending[kShellStreamChunkBytes + 1U] = {};
  size_t pending_len = 0U;
};

struct ShellWebPane {
  bool in_use = false;
  uint32_t client_id = 0U;
  uint8_t pane_id = 0U;
  ShellSession* session = nullptr;
};

struct ShellClientIdentity {
  bool in_use = false;
  uint32_t client_id = 0U;
  char username[kShellClientUsernameBytes] = {};
};

struct ShellClientProtocolState {
  bool in_use = false;
  uint32_t client_id = 0U;
  bool binary_stream = false;
  uint32_t next_seq = 1U;
  uint32_t last_ack = 0U;
  uint32_t credit = kShellInitialCreditBytes;
};

QueueHandle_t g_request_queue = nullptr;
QueueHandle_t g_response_queue = nullptr;
TaskHandle_t g_task_handle = nullptr;
bool g_initialized = false;
SemaphoreHandle_t g_response_pool_mutex = nullptr;
ShellResponsePoolSlot g_response_pool[kShellResponsePoolSlots];
ShellWebPane g_web_panes[kShellWebPaneSlots];
ShellClientIdentity g_client_identities[kShellClientIdentitySlots];
ShellClientProtocolState g_protocol_states[kShellProtocolSlots];
ShellServiceMetrics g_metrics {};

bool ensure_response_pool_mutex() {
  if (g_response_pool_mutex != nullptr) {
    return true;
  }
  g_response_pool_mutex = xSemaphoreCreateMutex();
  return g_response_pool_mutex != nullptr;
}

char* acquire_response_pool_buffer(const size_t bytes) {
  if (bytes == 0U || bytes > kShellJsonCapacity || !ensure_response_pool_mutex()) {
    return nullptr;
  }
  if (xSemaphoreTake(g_response_pool_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
    return nullptr;
  }
  for (ShellResponsePoolSlot& slot : g_response_pool) {
    if (slot.in_use) {
      continue;
    }
    if (slot.data == nullptr) {
      slot.data = alloc_shell_text_buffer(kShellJsonCapacity);
      if (slot.data == nullptr) {
        continue;
      }
      g_metrics.response_pool_allocated++;
    }
    slot.in_use = true;
    xSemaphoreGive(g_response_pool_mutex);
    return slot.data;
  }
  xSemaphoreGive(g_response_pool_mutex);
  return nullptr;
}

char* acquire_response_buffer(const size_t bytes, bool* pooled) {
  if (pooled != nullptr) *pooled = false;
  char* buffer = acquire_response_pool_buffer(bytes);
  if (buffer != nullptr) {
    if (pooled != nullptr) *pooled = true;
    return buffer;
  }
  g_metrics.response_pool_miss++;
  g_metrics.response_fallback_alloc++;
  return alloc_shell_text_buffer(bytes);
}

void release_response_buffer(char* buffer, const bool pooled) {
  if (buffer == nullptr) {
    return;
  }
  if (!pooled) {
    heap_caps_free(buffer);
    return;
  }
  if (!ensure_response_pool_mutex() ||
      xSemaphoreTake(g_response_pool_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }
  for (ShellResponsePoolSlot& slot : g_response_pool) {
    if (slot.data == buffer) {
      slot.in_use = false;
      break;
    }
  }
  xSemaphoreGive(g_response_pool_mutex);
}

void bin_put_u16(uint8_t* out, const size_t offset, const uint16_t value) {
  out[offset + 0U] = static_cast<uint8_t>(value & 0xFFU);
  out[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

void bin_put_u32(uint8_t* out, const size_t offset, const uint32_t value) {
  out[offset + 0U] = static_cast<uint8_t>(value & 0xFFU);
  out[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  out[offset + 2U] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  out[offset + 3U] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

ShellClientProtocolState* find_protocol_state(const uint32_t client_id) {
  for (ShellClientProtocolState& state : g_protocol_states) {
    if (state.in_use && state.client_id == client_id) {
      return &state;
    }
  }
  return nullptr;
}

ShellClientProtocolState* ensure_protocol_state(const uint32_t client_id) {
  if (client_id == 0U) {
    return nullptr;
  }
  if (ShellClientProtocolState* existing = find_protocol_state(client_id)) {
    return existing;
  }
  for (ShellClientProtocolState& state : g_protocol_states) {
    if (state.in_use) {
      continue;
    }
    state = ShellClientProtocolState {};
    state.in_use = true;
    state.client_id = client_id;
    state.credit = kShellInitialCreditBytes;
    return &state;
  }
  return nullptr;
}

bool client_binary_stream_enabled(const uint32_t client_id) {
  ShellClientProtocolState* state = find_protocol_state(client_id);
  return state != nullptr && state->binary_stream;
}

void close_protocol_state(const uint32_t client_id) {
  for (ShellClientProtocolState& state : g_protocol_states) {
    if (state.in_use && state.client_id == client_id) {
      state = ShellClientProtocolState {};
      return;
    }
  }
}

void copy_text(char* dst, const size_t dst_size, const char* src) {
  if (dst == nullptr || dst_size == 0U) {
    return;
  }
  std::snprintf(dst, dst_size, "%s", src != nullptr ? src : "");
}

const char* strip_web_command_id_prefix(const char* payload, uint16_t* command_id) {
  if (payload == nullptr || payload[0] != '@') {
    return payload != nullptr ? payload : "";
  }

  uint32_t parsed_id = 0U;
  size_t i = 1U;
  bool has_digit = false;
  while (payload[i] >= '0' && payload[i] <= '9') {
    has_digit = true;
    parsed_id = (parsed_id * 10U) + static_cast<uint32_t>(payload[i] - '0');
    if (parsed_id > 0xFFFFU) {
      return payload;
    }
    ++i;
  }
  if (!has_digit || payload[i] != ':') {
    return payload;
  }
  if (command_id != nullptr && *command_id == 0U) {
    *command_id = static_cast<uint16_t>(parsed_id);
  }
  return payload + i + 1U;
}

void append_format(char* buffer, const size_t size, size_t* used, const char* format, ...) {
  if (buffer == nullptr || used == nullptr || format == nullptr || *used >= (size - 1U)) {
    return;
  }

  va_list args;
  va_start(args, format);
  const int written = std::vsnprintf(buffer + *used, size - *used, format, args);
  va_end(args);
  if (written <= 0) {
    return;
  }
  *used = std::min(size - 1U, *used + static_cast<size_t>(written));
}

bool truncate_shell_output(std::string* output) {
  if (output == nullptr || output->size() <= kShellFinalOutputMaxBytes) {
    return false;
  }
  output->resize(kShellFinalOutputMaxBytes);
  output->append("\n[mshell: output truncated for web response]\n");
  return true;
}

void format_reply(char* buffer, const size_t size, const char* format, ...) {
  if (buffer == nullptr || size == 0U || format == nullptr) {
    return;
  }

  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, size, format, args);
  va_end(args);
  buffer[size - 1U] = '\0';
}

std::string trim_copy(const char* text) {
  if (text == nullptr) {
    return {};
  }

  const char* start = text;
  while (*start != '\0' && std::isspace(static_cast<unsigned char>(*start)) != 0) {
    ++start;
  }

  const char* end = start + std::strlen(start);
  while (end > start && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0) {
    --end;
  }

  return std::string(start, static_cast<size_t>(end - start));
}

std::string normalize_setting_key(const char* text) {
  std::string normalized;
  for (const char ch : trim_copy(text)) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-') {
      normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
  }
  return normalized;
}

std::string shell_prompt_text() {
  return mros::shell::prompt_for_transport(mros::shell::ShellTransport::Web);
}

ShellWebPane* find_web_pane(const uint32_t client_id, const uint8_t pane_id) {
  for (ShellWebPane& pane : g_web_panes) {
    if (pane.in_use && pane.client_id == client_id && pane.pane_id == pane_id) {
      return &pane;
    }
  }
  return nullptr;
}

ShellClientIdentity* find_client_identity(const uint32_t client_id) {
  for (ShellClientIdentity& identity : g_client_identities) {
    if (identity.in_use && identity.client_id == client_id) {
      return &identity;
    }
  }
  return nullptr;
}

const char* username_for_client(const uint32_t client_id) {
  ShellClientIdentity* identity = find_client_identity(client_id);
  return identity != nullptr && identity->username[0] != '\0' ? identity->username : nullptr;
}

uint8_t active_panes_for_client(const uint32_t client_id) {
  uint8_t count = 0U;
  for (const ShellWebPane& pane : g_web_panes) {
    if (pane.in_use && pane.client_id == client_id) {
      ++count;
    }
  }
  return count;
}

ShellWebPane* ensure_web_pane(const uint32_t client_id, const uint8_t raw_pane_id) {
  const uint8_t pane_id = raw_pane_id < kShellMaxPanesPerClient ? raw_pane_id : 0U;
  ShellWebPane* existing = find_web_pane(client_id, pane_id);
  if (existing != nullptr) {
    return existing;
  }
  if (active_panes_for_client(client_id) >= kShellMaxPanesPerClient) {
    return nullptr;
  }
  for (ShellWebPane& pane : g_web_panes) {
    if (pane.in_use) {
      continue;
    }
    pane.session = mros::shell::create_session(mros::shell::ShellTransport::Web);
    if (pane.session == nullptr) {
      return nullptr;
    }
    const char* username = username_for_client(client_id);
    if (username != nullptr) {
      (void)mros::shell::session_apply_user(pane.session, username);
    }
    pane.in_use = true;
    pane.client_id = client_id;
    pane.pane_id = pane_id;
    return &pane;
  }
  return nullptr;
}

const char* pane_prompt(ShellWebPane* pane) {
  if (pane == nullptr || pane->session == nullptr) {
    return "mros@DEUSCARA-S3V:/$ ";
  }
  const char* prompt = mros::shell::session_prompt(pane->session);
  return prompt != nullptr ? prompt : "mros@DEUSCARA-S3V:/$ ";
}

void append_session_meta(mros::utils::FixedJsonWriter& writer, const ShellWebRequest& request, ShellWebPane* pane) {
  writer.append_raw(",\"pane_id\":");
  writer.u32(request.pane_id);
  writer.append_raw(",\"session_id\":");
  writer.u32(pane != nullptr ? mros::shell::session_id(pane->session) : 0U);
  writer.append_raw(",\"target\":\"");
  writer.append_escaped("s3");
  writer.append_raw("\",\"capabilities\":\"");
  writer.append_escaped(pane != nullptr && pane->session != nullptr && mros::shell::session_is_root(pane->session)
                            ? "root"
                            : "user");
  writer.append_raw("\"");
}

bool parse_bool_value(const char* value, bool* out_value) {
  if (value == nullptr || out_value == nullptr) {
    return false;
  }

  std::string normalized;
  for (const char ch : std::string(value)) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-') {
      normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
  }

  if (normalized == "1" || normalized == "on" || normalized == "true" || normalized == "enable" ||
      normalized == "enabled") {
    *out_value = true;
    return true;
  }
  if (normalized == "0" || normalized == "off" || normalized == "false" || normalized == "disable" ||
      normalized == "disabled") {
    *out_value = false;
    return true;
  }
  return false;
}

bool parse_int32_value(const char* value, int32_t* out_value) {
  if (value == nullptr || out_value == nullptr) {
    return false;
  }

  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out_value = static_cast<int32_t>(parsed);
  return true;
}

bool parse_uint32_value(const std::string& value, uint32_t* out_value) {
  if (out_value == nullptr || value.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (end == value.c_str() || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out_value = static_cast<uint32_t>(parsed);
  return true;
}

bool parse_float_value(const char* value, float* out_value) {
  if (value == nullptr || out_value == nullptr) {
    return false;
  }

  char* end = nullptr;
  const float parsed = std::strtof(value, &end);
  if (end == value || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out_value = parsed;
  return true;
}

bool logger_mount_status_message(char* message, const size_t size) {
  if (logger_storage_ready()) {
    uint64_t total_bytes = 0U;
    uint64_t used_bytes = 0U;
    (void)logger_storage_info(&total_bytes, &used_bytes);
    std::snprintf(
        message,
        size,
        "LittleFS ready at /fs (%luK total, %luK used)",
        static_cast<unsigned long>(total_bytes / 1024U),
        static_cast<unsigned long>(used_bytes / 1024U));
    return true;
  }

  copy_text(message, size, "LittleFS is not mounted");
  return false;
}

void shell_output_callback(const char* text, void* user_data) {
  (void)user_data;
  if (text == nullptr || text[0] == '\0') {
    return;
  }
  mros_console.print(text);
}

bool shell_is_storage_mounted(void* user_data) {
  (void)user_data;
  return logger_storage_ready();
}

bool shell_mount_storage(char* message, size_t size, void* user_data) {
  (void)user_data;
  if (!logger_storage_ready()) {
    logger_init();
  }
  return logger_mount_status_message(message, size);
}

bool shell_clear_console(void* user_data) {
  (void)user_data;
  uart1_cobs_clear_system_logs();
  return true;
}

bool shell_execute_system_action(const mros::shell::ShellSystemAction action, void* user_data) {
  (void)user_data;
  switch (action) {
    case mros::shell::ShellSystemAction::Reboot:
      mros_console.println("[SHELL] Reboot requested.");
      mros::platform::mros_delay_ms(20);
      mros::platform::mros_system_restart();
      return true;
    case mros::shell::ShellSystemAction::Poweroff:
      mros_console.println("[SHELL] Poweroff requested.");
      mros::platform::mros_delay_ms(20);
      esp_deep_sleep_start();
      return true;
    default:
      return false;
  }
}

float shell_robot_speed_to_traj_scale(const float speed_scale) {
  const float clamped = std::max(0.25f, std::min(3.0f, speed_scale));
  const float ratio = (clamped - 0.25f) / (3.0f - 0.25f);
  return 3.0f - (ratio * 2.0f);
}

void shell_apply_robot_joint_targets(const float joints_deg[6]) {
  for (int i = 0; i < 6; ++i) {
    spi_s3_set_target_joint(i, joints_deg[i]);
  }
}

bool shell_publish_robot_ui_target(
    const mros::shell::ShellRobotVector3& target,
    const mros::shell::ShellRobotRequest& request,
    const char* action_name,
    char* message,
    size_t size,
    const char* op = "point",
    const mros::shell::ShellRobotVector3* from = nullptr) {
  WebRobotUiCommand command {};
  std::snprintf(command.op, sizeof(command.op), "%s", op != nullptr ? op : "point");
  command.x = target.x;
  command.y = target.y;
  command.z = target.z;
  if (from != nullptr) {
    command.has_from = true;
    command.from_x = from->x;
    command.from_y = from->y;
    command.from_z = from->z;
  }
  command.t_ms = request.time_ms > 0.0f ? request.time_ms : 1000.0f;
  command.ee_auto = request.ee_auto;
  command.roll_deg = request.roll_deg;
  command.pitch_deg = request.ee_auto ? 0.0f : ((request.pitch_deg != 0.0f) ? request.pitch_deg : request.ee_pitch);
  command.yaw_deg = request.yaw_deg;
  command.ee_pitch = command.pitch_deg;
  command.apply = request.apply;
  std::snprintf(command.calc_mode,
                sizeof(command.calc_mode),
                "%s",
                request.calc_mode[0] != '\0' ? request.calc_mode : web_server_get_ik_compute_preference());
  std::snprintf(command.source, sizeof(command.source), "%s", "shell");

  const uint32_t rev = web_server_publish_robot_ui_target(&command);
  if (rev == 0U) {
    format_reply(message, size, "robot: %s target publish failed", action_name);
    return false;
  }

  format_reply(
      message,
      size,
      "robot: %s target published to web UI rev=%lu x=%.1f y=%.1f z=%.1f calc=%s apply=%s%s",
      action_name,
      static_cast<unsigned long>(rev),
      command.x,
      command.y,
      command.z,
      command.calc_mode,
      command.apply ? "yes" : "no",
      (command.apply && std::strcmp(command.calc_mode, "web") == 0)
          ? " (WEB apply requires an open web client)"
          : "");
  return true;
}

bool shell_publish_robot_ui_op(
    const char* op,
    const mros::shell::ShellRobotRequest& request,
    const char* action_name,
    char* message,
    size_t size) {
  WebRobotUiCommand command {};
  std::snprintf(command.op, sizeof(command.op), "%s", op != nullptr ? op : "point");
  command.t_ms = request.time_ms > 0.0f ? request.time_ms : 1000.0f;
  command.ee_auto = request.ee_auto;
  command.roll_deg = request.roll_deg;
  command.pitch_deg = request.ee_auto ? 0.0f : ((request.pitch_deg != 0.0f) ? request.pitch_deg : request.ee_pitch);
  command.yaw_deg = request.yaw_deg;
  command.ee_pitch = command.pitch_deg;
  command.apply = request.apply;
  command.joint_count = static_cast<uint8_t>(std::min<size_t>(request.joint_count, 7U));
  for (uint8_t i = 0; i < command.joint_count; ++i) {
    command.joints[i] = request.joints[i];
  }
  std::snprintf(command.calc_mode,
                sizeof(command.calc_mode),
                "%s",
                request.calc_mode[0] != '\0' ? request.calc_mode : web_server_get_ik_compute_preference());
  std::snprintf(command.source, sizeof(command.source), "%s", "shell");

  const uint32_t rev = web_server_publish_robot_ui_target(&command);
  if (rev == 0U) {
    format_reply(message, size, "robot: %s command publish failed", action_name);
    return false;
  }
  format_reply(
      message,
      size,
      "robot: %s command published to web UI rev=%lu calc=%s apply=%s",
      action_name,
      static_cast<unsigned long>(rev),
      command.calc_mode,
      command.apply ? "yes" : "no");
  return true;
}

bool shell_execute_robot_action(
    const mros::shell::ShellRobotRequest& request,
    char* message,
    size_t size,
    void* user_data) {
  (void)user_data;
  if (message != nullptr && size > 0U) {
    message[0] = '\0';
  }

  switch (request.action) {
    case mros::shell::ShellRobotAction::DefaultPosition: {
      static const float kDefaultJointsDeg[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
      spi_s3_set_target_turret(0.0f);
      shell_apply_robot_joint_targets(kDefaultJointsDeg);
      spi_s3_set_target_gripper(72.0f);
      spi_s3_set_joint_traj_time_scale(1.5f);
      pca9685_set_output_enable(true);
      spi_s3_set_motor_power(1U);
      format_reply(message, size, "robot: default pose queued (turret=0, joints=0, gripper=40%%)");
      return true;
    }
    case mros::shell::ShellRobotAction::Position:
      return shell_publish_robot_ui_target(request.position, request, "pos", message, size, "point");
    case mros::shell::ShellRobotAction::Move:
      return shell_publish_robot_ui_target(request.to, request, "mov", message, size, "move", &request.from);
    case mros::shell::ShellRobotAction::Gripper: {
      const float percent = std::max(0.0f, std::min(100.0f, request.value));
      const float target_deg = (percent / 100.0f) * 180.0f;
      spi_s3_set_target_gripper(target_deg);
      format_reply(message, size, "robot: gripper set to %.0f%% (%.1f deg)", percent, target_deg);
      return true;
    }
    case mros::shell::ShellRobotAction::TurretPosition:
      spi_s3_set_target_turret(request.value);
      format_reply(message, size, "robot: turret target set to %.1f deg", request.value);
      return true;
    case mros::shell::ShellRobotAction::Speed: {
      const float traj_scale = shell_robot_speed_to_traj_scale(request.value);
      spi_s3_set_joint_traj_time_scale(traj_scale);
      format_reply(message, size, "robot: speed scale %.2f mapped to trajectory scale %.2f", request.value, traj_scale);
      return true;
    }
    case mros::shell::ShellRobotAction::Emergency:
      if (request.enabled) {
        spi_s3_set_motor_power(0U);
        pca9685_set_output_enable(false);
        format_reply(message, size, "robot: emergency stop engaged (motor-power=0, oe=0)");
      } else {
        pca9685_set_output_enable(true);
        spi_s3_set_motor_power(1U);
        format_reply(message, size, "robot: emergency stop released (motor-power=1, oe=1)");
      }
      return true;
    case mros::shell::ShellRobotAction::PathAdd:
      return shell_publish_robot_ui_target(request.position, request, "path add", message, size, "path-add");
    case mros::shell::ShellRobotAction::PathClear:
      return shell_publish_robot_ui_op("path-clear", request, "path clear", message, size);
    case mros::shell::ShellRobotAction::PathPreview:
      return shell_publish_robot_ui_op("path-preview", request, "path preview", message, size);
    case mros::shell::ShellRobotAction::PathRun:
      return shell_publish_robot_ui_op("path-run", request, "path run", message, size);
    case mros::shell::ShellRobotAction::PathStatus:
      format_reply(
          message,
          size,
          "robot: device trajectory buffer points=%u (web UI queue may include unsynced browser points)",
          static_cast<unsigned>(trajectory_handler_count()));
      return true;
    case mros::shell::ShellRobotAction::JointSet:
      return shell_publish_robot_ui_op("joint-set", request, "joint set", message, size);
    case mros::shell::ShellRobotAction::JointApply:
      return shell_publish_robot_ui_op("joint-apply", request, "joint apply", message, size);
    case mros::shell::ShellRobotAction::Hold:
      pca9685_set_output_enable(request.enabled);
      spi_s3_set_motor_power(request.enabled ? 1U : 0U);
      format_reply(message, size, "robot: hold %s (motor-power=%u, oe=%u)",
                   request.enabled ? "on" : "off",
                   request.enabled ? 1U : 0U,
                   request.enabled ? 1U : 0U);
      return true;
    case mros::shell::ShellRobotAction::Stop:
      spi_s3_set_motor_power(0U);
      pca9685_set_output_enable(false);
      format_reply(message, size, "robot: stop requested (motor-power=0, oe=0)");
      return true;
    case mros::shell::ShellRobotAction::Park: {
      static const float kParkJointsDeg[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
      spi_s3_set_target_turret(0.0f);
      shell_apply_robot_joint_targets(kParkJointsDeg);
      spi_s3_set_target_gripper(72.0f);
      spi_s3_set_joint_traj_time_scale(1.8f);
      pca9685_set_output_enable(true);
      spi_s3_set_motor_power(1U);
      format_reply(message, size, "robot: park pose queued (turret=0, joints=0, gripper=40%%)");
      return true;
    }
    case mros::shell::ShellRobotAction::CalcPreference:
      if (std::strcmp(request.calc_mode, "status") == 0 || request.calc_mode[0] == '\0') {
        format_reply(message, size, "robot: calc preference=%s", web_server_get_ik_compute_preference());
        return true;
      }
      if (!web_server_set_ik_compute_preference(request.calc_mode)) {
        format_reply(message, size, "robot: unsupported calc preference '%s'", request.calc_mode);
        return false;
      }
      format_reply(message, size, "robot: calc preference set to %s", web_server_get_ik_compute_preference());
      return true;
    default:
      format_reply(message, size, "robot: unsupported action");
      return false;
  }
}

std::string wifi_mode_text() {
  WifiManagerSnapshot snapshot = {};
  wifi_manager_get_snapshot(&snapshot);
  if (!wifi_manager_is_enabled()) {
    return "OFF";
  }
  if (snapshot.state.ap_active && snapshot.state.sta_connected) {
    return "AP+STA";
  }
  if (snapshot.state.ap_active) {
    return "AP";
  }
  return "STA";
}

bool shell_format_status(char* message, size_t size, void* user_data) {
  (void)user_data;
  if (message == nullptr || size == 0U) {
    return false;
  }

  float kp = 0.0f;
  float ki = 0.0f;
  float kd = 0.0f;
  float imax = 0.0f;
  spi_s3_get_turret_pid(&kp, &ki, &kd, &imax);
  WebServerDiagSnapshot diag {};
  web_server_get_diag_snapshot(&diag);
  WifiManagerSnapshot wifi = {};
  wifi_manager_get_snapshot(&wifi);

  const bool wifi_connected = wifi.state.sta_connected;
  const bool wifi_ap = wifi.state.ap_active;
  uint64_t fs_total = 0U;
  uint64_t fs_used = 0U;
  if (logger_storage_ready()) {
    (void)logger_storage_info(&fs_total, &fs_used);
  }
  const uint64_t fs_free = fs_total >= fs_used ? (fs_total - fs_used) : 0U;

  std::snprintf(
      message,
      size,
      "wifi_mode=%s sta=%s ip=%s ap=%s ap_ip=%s\n"
      "links t41_qspi=%s c3_spi=%s espnow=%s pca=%s oe=%s motor=%u failsafe=%d flags=%u\n"
      "turret target=%.2f actual=%.2f pid_out=%.2f pid_err=%.2f gripper=%u\n"
      "joints j0=%.1f j1=%.1f j2=%.1f j3=%.1f j4=%.1f j5=%.1f\n"
      "coord x=%.1f y=%.1f z=%.1f a=%.1f\n"
      "pid kp=%.3f ki=%.3f kd=%.3f imax=%.3f dspc=%.3f out_lock=%s\n"
      "loop t41=%ums c3=%uHz pid_avg=%.2fms pid_last=%ums pid_exec=%ums pid_peak=%ums\n"
      "cpu applied=%uMHz target=%uMHz core0=%uMHz core1=%uMHz uptime=%lus\n"
      "heap free=%lu min=%lu psram_free=%lu psram_total=%lu\n"
      "littlefs total=%lu used=%lu free=%lu console_rev=%lu",
      wifi_mode_text().c_str(),
      wifi_connected ? "connected" : "disconnected",
      wifi_connected && wifi.ip.length() > 0 ? wifi.ip.c_str() : "-",
      wifi_ap ? "on" : "off",
      wifi_ap && wifi.ap_ip.length() > 0 ? wifi.ap_ip.c_str() : "-",
      spi_s3_is_connected() ? "ok" : "down",
      spi_c3_is_connected() ? "ok" : "down",
      spi_c3_is_espnow_connected() ? "ok" : "down",
      pca9685_is_ready() ? "ready" : "down",
      pca9685_get_output_enable() ? "on" : "off",
      static_cast<unsigned>(spi_s3_get_motor_state()),
      static_cast<int>(spi_c3_get_failsafe_option()),
      static_cast<unsigned>(spi_c3_get_cmd_flags()),
      spi_s3_get_turret_deg(),
      spi_s3_get_turret_actual_deg(),
      spi_s3_get_turret_pid_output(),
      spi_s3_get_turret_pid_error(),
      static_cast<unsigned>(spi_s3_get_gripper()),
      spi_s3_get_joint_deg(0),
      spi_s3_get_joint_deg(1),
      spi_s3_get_joint_deg(2),
      spi_s3_get_joint_deg(3),
      spi_s3_get_joint_deg(4),
      spi_s3_get_joint_deg(5),
      spi_s3_get_coord_x(),
      spi_s3_get_coord_y(),
      spi_s3_get_coord_z(),
      spi_s3_get_alpha(),
      kp,
      ki,
      kd,
      imax,
      spi_s3_get_turret_dspc(),
      spi_s3_get_turret_output_lock() ? "on" : "off",
      static_cast<unsigned>(spi_s3_get_loop_ms()),
      static_cast<unsigned>(spi_c3_get_loop_hz()),
      diag.pid_cycle_avg_ms,
      static_cast<unsigned>(diag.pid_cycle_last_ms),
      static_cast<unsigned>(diag.pid_cycle_exec_ms),
      static_cast<unsigned>(diag.pid_cycle_peak_ms),
      static_cast<unsigned>(diag.cpu_freq_applied_mhz),
      static_cast<unsigned>(diag.cpu_freq_target_mhz),
      static_cast<unsigned>(diag.cpu_freq_core0_mhz),
      static_cast<unsigned>(diag.cpu_freq_core1_mhz),
      static_cast<unsigned long>(mros::platform::mros_millis() / 1000UL),
      static_cast<unsigned long>(mros::platform::mros_system_heap_free()),
      static_cast<unsigned long>(mros::platform::mros_system_heap_min_free()),
      static_cast<unsigned long>(mros::platform::mros_system_psram_free()),
      static_cast<unsigned long>(mros::platform::mros_system_psram_total()),
      static_cast<unsigned long>(fs_total),
      static_cast<unsigned long>(fs_used),
      static_cast<unsigned long>(fs_free),
      static_cast<unsigned long>(uart1_cobs_get_log_version()));
  return true;
}

bool shell_format_diagnostics(char* message, size_t size, void* user_data) {
  (void)user_data;
  if (message == nullptr || size == 0U) {
    return false;
  }

  const UBaseType_t task_count = uxTaskGetNumberOfTasks();
  TaskStatus_t* tasks = alloc_task_status_buffer(task_count + 8U);
  if (tasks == nullptr) {
    copy_text(message, size, "mtop: task snapshot buffer allocation failed");
    return false;
  }
  uint32_t total_runtime = 0U;
  const UBaseType_t actual_count =
      uxTaskGetSystemState(tasks, task_count + 8U, &total_runtime);

  size_t used = 0U;
  PCA9685_DiagSnapshot_t pca_diag {};
  LoggerDiagSnapshot storage_diag {};
  MrosConsoleDiagSnapshot console_diag {};
  UartLogDiagSnapshot uart_diag {};
  C3SpiDiagSnapshot c3_diag {};
  pca9685_get_diag_snapshot(&pca_diag);
  logger_get_diag_snapshot(&storage_diag);
  mros_console_get_diag_snapshot(&console_diag);
  uart1_cobs_get_diag_snapshot(&uart_diag);
  spi_c3_get_diag_snapshot(&c3_diag);
  append_format(
      message,
      size,
      &used,
      "shell req_q=%lu/%lu resp_q=%lu/%lu task=%s\n",
      static_cast<unsigned long>(g_request_queue != nullptr ? uxQueueMessagesWaiting(g_request_queue) : 0U),
      static_cast<unsigned long>(kShellRequestQueueLength),
      static_cast<unsigned long>(g_response_queue != nullptr ? uxQueueMessagesWaiting(g_response_queue) : 0U),
      static_cast<unsigned long>(kShellResponseQueueLength),
      g_task_handle != nullptr ? "up" : "down");
  append_format(
      message,
      size,
      &used,
      "queues pca=%lu/%lu storage=%lu/%lu console=%lu/%lu shell_req=%lu/%lu shell_resp=%lu/%lu\n",
      static_cast<unsigned long>(pca_diag.queue_depth),
      static_cast<unsigned long>(pca_diag.queue_capacity),
      static_cast<unsigned long>(storage_diag.queue_depth),
      static_cast<unsigned long>(storage_diag.queue_capacity),
      static_cast<unsigned long>(console_diag.queue_depth),
      static_cast<unsigned long>(console_diag.queue_capacity),
      static_cast<unsigned long>(g_request_queue != nullptr ? uxQueueMessagesWaiting(g_request_queue) : 0U),
      static_cast<unsigned long>(kShellRequestQueueLength),
      static_cast<unsigned long>(g_response_queue != nullptr ? uxQueueMessagesWaiting(g_response_queue) : 0U),
      static_cast<unsigned long>(kShellResponseQueueLength));
  append_format(
      message,
      size,
      &used,
      "drops pca=%lu pca_old=%lu storage=%lu console=%lu c3_timeout=%lu c3_fail=%lu c3_period=%lums log_full=%lu log_since=%lu\n",
      static_cast<unsigned long>(pca_diag.drop_count),
      static_cast<unsigned long>(pca_diag.drop_oldest_count),
      static_cast<unsigned long>(storage_diag.drop_count),
      static_cast<unsigned long>(console_diag.dropped_bytes),
      static_cast<unsigned long>(c3_diag.transaction_timeout_count),
      static_cast<unsigned long>(c3_diag.transaction_fail_count),
      static_cast<unsigned long>(c3_diag.effective_period_ms),
      static_cast<unsigned long>(uart_diag.full_copy_count),
      static_cast<unsigned long>(uart_diag.since_copy_count));
  append_format(message, size, &used, "%-16s %-4s %-4s %-7s %-10s\n", "task", "pri", "cpu", "stk_hw", "runtime");

  for (UBaseType_t i = 0; i < actual_count; ++i) {
    char cpu_text[8] = "-";
#if defined(INCLUDE_xTaskGetCoreID) && (INCLUDE_xTaskGetCoreID == 1)
    const BaseType_t core_id = xTaskGetCoreID(tasks[i].xHandle);
    if (core_id >= 0) {
      std::snprintf(cpu_text, sizeof(cpu_text), "%ld", static_cast<long>(core_id));
    }
#endif
    append_format(
        message,
        size,
        &used,
        "%-16s %-4lu %-4s %-7lu %-10lu\n",
        tasks[i].pcTaskName != nullptr ? tasks[i].pcTaskName : "(task)",
        static_cast<unsigned long>(tasks[i].uxCurrentPriority),
        cpu_text,
        static_cast<unsigned long>(tasks[i].usStackHighWaterMark),
        static_cast<unsigned long>(tasks[i].ulRunTimeCounter));
  }

  if (used < size) {
    message[used] = '\0';
  }
  heap_caps_free(tasks);
  return true;
}

std::string extract_last_lines(const std::string& input, const size_t line_count) {
  if (line_count == 0U || input.empty()) {
    return {};
  }

  size_t found = 0U;
  size_t start = input.size();
  while (start > 0U) {
    --start;
    if (input[start] == '\n') {
      ++found;
      if (found >= line_count) {
        ++start;
        break;
      }
    }
  }
  if (found < line_count) {
    start = 0U;
  }
  return input.substr(start);
}

bool shell_read_recent_logs(size_t line_count, char* message, size_t size, void* user_data) {
  (void)user_data;
  if (message == nullptr || size == 0U) {
    return false;
  }
  const size_t snapshot_size = uart1_cobs_get_system_logs_size();
  if (snapshot_size == 0U) {
    message[0] = '\0';
    return true;
  }

  constexpr size_t kShellLogTailCopyMax = 64U * 1024U;
  size_t copy_size = snapshot_size;
  if (copy_size > kShellLogTailCopyMax) {
    copy_size = kShellLogTailCopyMax;
  }
  const size_t base_offset = uart1_cobs_get_system_logs_base_offset();
  const size_t start_offset = base_offset + snapshot_size - copy_size;
  char* snapshot = alloc_shell_text_buffer(copy_size + 1U);
  if (snapshot == nullptr) {
    copy_text(message, size, "log: system log tail allocation failed");
    return false;
  }
  size_t copied = 0U;
  const bool ok = uart1_cobs_copy_system_logs_since(
      snapshot, copy_size + 1U, start_offset, copy_size, &copied);
  if (!ok) {
    heap_caps_free(snapshot);
    copy_text(message, size, "log: system log tail read failed");
    return false;
  }
  const std::string logs(snapshot, copied);
  heap_caps_free(snapshot);
  const std::string tail = extract_last_lines(logs, line_count);
  copy_text(message, size, tail.c_str());
  return true;
}

bool shell_is_spi_connected(void* user_data) {
  (void)user_data;
  return spi_s3_is_connected();
}

bool shell_is_espnow_connected(void* user_data) {
  (void)user_data;
  return spi_c3_is_espnow_connected();
}

bool persist_pid_config() {
  float kp = 0.0f;
  float ki = 0.0f;
  float kd = 0.0f;
  float imax = 0.0f;
  spi_s3_get_turret_pid(&kp, &ki, &kd, &imax);
  prefs_save_pid(kp, ki, kd, imax, spi_s3_get_turret_dspc());
  return true;
}

bool shell_handle_setting_action(
    const mros::shell::ShellSettingAction action,
    const char* name,
    const char* value,
    char* message,
    size_t size,
    void* user_data) {
  (void)user_data;
  const std::string normalized_key = normalize_setting_key(name);
  if (normalized_key.empty()) {
    copy_text(message, size, "set: missing setting name");
    return false;
  }

  const bool is_get = action == mros::shell::ShellSettingAction::Get;
  if (normalized_key == "motor-power") {
    if (is_get) {
      format_reply(message, size, "motor-power=%u", static_cast<unsigned>(spi_s3_get_motor_state()));
      return true;
    }
    bool enabled = false;
    if (!parse_bool_value(value, &enabled)) {
      copy_text(message, size, "motor-power expects on/off");
      return false;
    }
    spi_s3_set_motor_power(enabled ? 1U : 0U);
    format_reply(message, size, "motor-power=%u", enabled ? 1U : 0U);
    return true;
  }
  if (normalized_key == "oe") {
    if (is_get) {
      format_reply(message, size, "oe=%u", pca9685_get_output_enable() ? 1U : 0U);
      return true;
    }
    bool enabled = false;
    if (!parse_bool_value(value, &enabled)) {
      copy_text(message, size, "oe expects on/off");
      return false;
    }
    pca9685_set_output_enable(enabled);
    format_reply(message, size, "oe=%u", enabled ? 1U : 0U);
    return true;
  }
  if (normalized_key == "failsafe") {
    if (is_get) {
      format_reply(message, size, "failsafe=%d", static_cast<int>(spi_c3_get_failsafe_option()));
      return true;
    }
    int32_t parsed = 0;
    if (!parse_int32_value(value, &parsed)) {
      copy_text(message, size, "failsafe expects integer 0..3");
      return false;
    }
    if (parsed < C3_FAILSAFE_T41_QSPI) parsed = C3_FAILSAFE_T41_QSPI;
    if (parsed > C3_FAILSAFE_RESERVED) parsed = C3_FAILSAFE_RESERVED;
    spi_c3_set_failsafe_option(static_cast<int8_t>(parsed));
    format_reply(message, size, "failsafe=%ld", static_cast<long>(parsed));
    return true;
  }
  if (normalized_key == "c3-flags") {
    if (is_get) {
      format_reply(message, size, "c3-flags=%u", static_cast<unsigned>(spi_c3_get_cmd_flags()));
      return true;
    }
    int32_t parsed = 0;
    if (!parse_int32_value(value, &parsed)) {
      copy_text(message, size, "c3-flags expects integer 0..255");
      return false;
    }
    if (parsed < 0) parsed = 0;
    if (parsed > 255) parsed = 255;
    spi_c3_set_cmd_flags(static_cast<uint8_t>(parsed));
    format_reply(message, size, "c3-flags=%ld", static_cast<long>(parsed));
    return true;
  }
  if (normalized_key == "c3-setpoint") {
    if (is_get) {
      format_reply(message, size, "c3-setpoint=%ld", static_cast<long>(spi_c3_get_pid_setpoint_x100()));
      return true;
    }
    int32_t parsed = 0;
    if (!parse_int32_value(value, &parsed)) {
      copy_text(message, size, "c3-setpoint expects integer value");
      return false;
    }
    spi_c3_set_pid_setpoint_x100(parsed);
    format_reply(message, size, "c3-setpoint=%ld", static_cast<long>(parsed));
    return true;
  }
  if (normalized_key == "pid-kp" || normalized_key == "pid-ki" || normalized_key == "pid-kd" || normalized_key == "pid-imax") {
    float kp = 0.0f;
    float ki = 0.0f;
    float kd = 0.0f;
    float imax = 0.0f;
    spi_s3_get_turret_pid(&kp, &ki, &kd, &imax);
    if (is_get) {
      if (normalized_key == "pid-kp") format_reply(message, size, "pid-kp=%.4f", kp);
      if (normalized_key == "pid-ki") format_reply(message, size, "pid-ki=%.4f", ki);
      if (normalized_key == "pid-kd") format_reply(message, size, "pid-kd=%.4f", kd);
      if (normalized_key == "pid-imax") format_reply(message, size, "pid-imax=%.4f", imax);
      return true;
    }
    float parsed = 0.0f;
    if (!parse_float_value(value, &parsed)) {
      copy_text(message, size, "PID gain expects numeric value");
      return false;
    }
    if (normalized_key == "pid-kp") kp = parsed;
    if (normalized_key == "pid-ki") ki = parsed;
    if (normalized_key == "pid-kd") kd = parsed;
    if (normalized_key == "pid-imax") imax = parsed;
    spi_s3_set_turret_pid(kp, ki, kd, imax);
    persist_pid_config();
    format_reply(message, size, "%s=%.4f", normalized_key.c_str(), parsed);
    return true;
  }
  if (normalized_key == "pid-dspc") {
    if (is_get) {
      format_reply(message, size, "pid-dspc=%.4f", spi_s3_get_turret_dspc());
      return true;
    }
    float parsed = 0.0f;
    if (!parse_float_value(value, &parsed)) {
      copy_text(message, size, "pid-dspc expects numeric value");
      return false;
    }
    spi_s3_set_turret_dspc(parsed);
    persist_pid_config();
    format_reply(message, size, "pid-dspc=%.4f", parsed);
    return true;
  }
  if (normalized_key == "output-lock") {
    if (is_get) {
      format_reply(message, size, "output-lock=%u", spi_s3_get_turret_output_lock() ? 1U : 0U);
      return true;
    }
    bool enabled = false;
    if (!parse_bool_value(value, &enabled)) {
      copy_text(message, size, "output-lock expects on/off");
      return false;
    }
    spi_s3_set_turret_output_lock(enabled);
    format_reply(message, size, "output-lock=%u", enabled ? 1U : 0U);
    return true;
  }

  copy_text(message, size, "unsupported setting");
  return false;
}

bool push_response_buffer(const uint32_t client_id,
                          char* data,
                          const size_t len,
                          const bool pooled,
                          const ShellWebOutboundKind kind) {
  if (g_response_queue == nullptr || data == nullptr) {
    release_response_buffer(data, pooled);
    g_metrics.response_drop++;
    return false;
  }
  ShellWebResponse response {};
  response.client_id = client_id;
  response.kind = kind;
  response.data = data;
  response.len = len;
  response.pooled = pooled;
  if (xQueueSend(g_response_queue, &response, 0) != pdTRUE) {
    release_response_buffer(response.data, response.pooled);
    g_metrics.response_drop++;
    return false;
  }
  web_server_notify_runtime_task();
  return true;
}

char* start_direct_response(bool* pooled) {
  return acquire_response_buffer(kShellJsonCapacity, pooled);
}

bool finish_direct_response(const uint32_t client_id, char* buffer, const bool pooled,
                            const mros::utils::FixedJsonWriter& writer) {
  if (writer.overflow()) {
    release_response_buffer(buffer, pooled);
    g_metrics.response_drop++;
    return false;
  }
  if (writer.length() > g_metrics.max_response_bytes) {
    g_metrics.max_response_bytes =
        static_cast<uint32_t>(std::min<size_t>(writer.length(), UINT32_MAX));
  }
  const bool ok = push_response_buffer(
      client_id, buffer, writer.length(), pooled, ShellWebOutboundKind::TextJson);
  if (ok) {
    g_metrics.shell_json_frames++;
    g_metrics.shell_json_bytes +=
        static_cast<uint32_t>(std::min<size_t>(writer.length(), UINT32_MAX));
  }
  return ok;
}

bool push_shell_binary_frame(const uint32_t client_id,
                             const uint8_t frame_type,
                             const uint8_t flags,
                             const uint8_t pane_id,
                             const uint16_t command_id,
                             const uint32_t session_id,
                             const char* payload,
                             const size_t payload_len) {
  if (!client_binary_stream_enabled(client_id)) {
    return false;
  }
  if (payload == nullptr && payload_len > 0U) {
    return false;
  }
  if (payload_len > kShellStreamChunkBytes) {
    return false;
  }
  ShellClientProtocolState* protocol = ensure_protocol_state(client_id);
  if (protocol == nullptr) {
    return false;
  }
  const size_t frame_len = kShellBinHeaderLen + payload_len;
  if (protocol->credit < frame_len) {
    g_metrics.shell_credit_exhausted++;
    return false;
  }

  bool pooled = false;
  char* raw = acquire_response_buffer(frame_len, &pooled);
  if (raw == nullptr) {
    g_metrics.response_drop++;
    return false;
  }
  uint8_t* out = reinterpret_cast<uint8_t*>(raw);
  std::memset(out, 0, frame_len);
  out[0] = static_cast<uint8_t>(kShellBinMagic[0]);
  out[1] = static_cast<uint8_t>(kShellBinMagic[1]);
  out[2] = static_cast<uint8_t>(kShellBinMagic[2]);
  out[3] = static_cast<uint8_t>(kShellBinMagic[3]);
  out[4] = 1U;
  out[5] = static_cast<uint8_t>(kShellBinHeaderLen);
  out[6] = frame_type;
  out[7] = flags;
  const uint32_t seq = protocol->next_seq++;
  bin_put_u32(out, 8U, seq);
  bin_put_u32(out, 12U, protocol->last_ack);
  bin_put_u32(out, 16U, protocol->credit);
  bin_put_u32(out, 20U, session_id);
  bin_put_u16(out, 24U, pane_id);
  bin_put_u16(out, 26U, command_id);
  bin_put_u32(out, 28U, static_cast<uint32_t>(payload_len));
  if (payload_len > 0U) {
    std::memcpy(out + kShellBinHeaderLen, payload, payload_len);
  }
  const bool ok = push_response_buffer(
      client_id, raw, frame_len, pooled, ShellWebOutboundKind::BinaryFrame);
  if (ok) {
    protocol->credit -=
        static_cast<uint32_t>(std::min<size_t>(frame_len, UINT32_MAX));
    g_metrics.shell_bin_frames++;
    g_metrics.shell_bin_bytes +=
        static_cast<uint32_t>(std::min<size_t>(frame_len, UINT32_MAX));
  }
  return ok;
}

bool push_shell_stream_response_chunk(const uint32_t client_id,
                                      const char* text,
                                      const size_t len,
                                      ShellWebStreamContext* context) {
  if (client_binary_stream_enabled(client_id)) {
    const uint8_t pane_id = context != nullptr ? context->pane_id : 0U;
    const uint16_t command_id = context != nullptr ? context->command_id : 0U;
    const uint32_t session_id = context != nullptr ? context->session_id : 0U;
    const bool ok = push_shell_binary_frame(
        client_id,
        SHELL_BIN_FRAME_STDOUT,
        SHELL_BIN_FLAG_UTF8 | SHELL_BIN_FLAG_ANSI,
        pane_id,
        command_id,
        session_id,
        text,
        len);
    if (ok) {
      g_metrics.stream_chunk_count++;
      g_metrics.stream_byte_count += static_cast<uint32_t>(std::min<size_t>(len, UINT32_MAX));
      if (context != nullptr) {
        context->chunk_count++;
        context->byte_count += static_cast<uint32_t>(std::min<size_t>(len, UINT32_MAX));
      }
    } else {
      g_metrics.stream_drop_count++;
    }
    return ok;
  }

  bool pooled = false;
  char* buffer = start_direct_response(&pooled);
  if (buffer == nullptr) {
    g_metrics.response_drop++;
    g_metrics.stream_drop_count++;
    return false;
  }
  mros::utils::FixedJsonWriter writer(buffer, kShellJsonCapacity);
  writer.append_raw("{\"shell\":{\"type\":\"stream\",\"output\":\"");
  writer.append_escaped(text, len);
  writer.append_raw("\"");
  if (context != nullptr) {
    writer.append_raw(",\"pane_id\":");
    writer.u32(context->pane_id);
    writer.append_raw(",\"session_id\":");
    writer.u32(context->session_id);
    writer.append_raw(",\"target\":\"s3\"");
  }
  writer.append_raw("}}");
  const bool ok = finish_direct_response(client_id, buffer, pooled, writer);
  if (ok) {
    g_metrics.stream_chunk_count++;
    g_metrics.stream_byte_count += static_cast<uint32_t>(std::min<size_t>(len, UINT32_MAX));
    if (context != nullptr) {
      context->chunk_count++;
      context->byte_count += static_cast<uint32_t>(std::min<size_t>(len, UINT32_MAX));
    }
  } else {
    g_metrics.stream_drop_count++;
  }
  return ok;
}

void push_shell_stream_response(const uint32_t client_id,
                                const char* text,
                                bool* dropped,
                                ShellWebStreamContext* context) {
  if (text == nullptr || text[0] == '\0') {
    return;
  }
  const size_t len = std::strlen(text);
  size_t offset = 0U;
  while (offset < len) {
    const size_t chunk_len = std::min(kShellStreamChunkBytes, len - offset);
    if (!push_shell_stream_response_chunk(client_id, text + offset, chunk_len, context)) {
      if (dropped != nullptr) {
        *dropped = true;
      }
      return;
    }
    offset += chunk_len;
  }
}

void flush_shell_stream_context(ShellWebStreamContext* context) {
  if (context == nullptr || context->pending_len == 0U) {
    return;
  }
  context->pending[context->pending_len] = '\0';
  push_shell_stream_response(
      context->client_id,
      context->pending,
      &context->dropped,
      context);
  context->pending_len = 0U;
  context->pending[0] = '\0';
}

void shell_stream_callback(const char* text, void* user_data) {
  ShellWebStreamContext* context = static_cast<ShellWebStreamContext*>(user_data);
  if (context == nullptr || text == nullptr || text[0] == '\0') {
    return;
  }
  context->sent_any = true;
  const size_t len = std::strlen(text);
  size_t offset = 0U;
  while (offset < len) {
    const size_t room = kShellStreamChunkBytes - context->pending_len;
    if (room == 0U) {
      flush_shell_stream_context(context);
      continue;
    }
    const size_t copy_len = std::min(room, len - offset);
    std::memcpy(context->pending + context->pending_len, text + offset, copy_len);
    context->pending_len += copy_len;
    context->pending[context->pending_len] = '\0';
    offset += copy_len;
    if (context->pending_len >= kShellStreamChunkBytes) {
      flush_shell_stream_context(context);
    }
  }
}

void push_error_response(const uint32_t client_id, const std::string& message) {
  const std::string prompt = shell_prompt_text();
  const char* cwd = mros::shell::current_working_directory();
  bool pooled = false;
  char* buffer = start_direct_response(&pooled);
  if (buffer == nullptr) {
    g_metrics.response_drop++;
    return;
  }
  mros::utils::FixedJsonWriter writer(buffer, kShellJsonCapacity);
  writer.append_raw("{\"shell\":{\"type\":\"error\",\"message\":\"");
  writer.append_escaped(message.c_str());
  writer.append_raw("\",\"prompt\":\"");
  writer.append_escaped(prompt.c_str());
  writer.append_raw("\",\"cwd\":\"");
  writer.append_escaped(cwd != nullptr ? cwd : "/");
  writer.append_raw("\"}}");
  (void)finish_direct_response(client_id, buffer, pooled, writer);
}

bool is_clear_command(const char* line) {
  const std::string trimmed = trim_copy(line);
  return trimmed == "clear";
}

void process_resize_request(const ShellWebRequest& request) {
  ShellWebPane* pane = ensure_web_pane(request.client_id, request.pane_id);
  if (pane == nullptr || pane->session == nullptr) {
    push_error_response(request.client_id, "shell pane/session limit reached");
    return;
  }
  const std::string payload = trim_copy(request.payload);
  const size_t separator = payload.find(':');
  if (separator == std::string::npos) {
    push_error_response(request.client_id, "invalid terminal resize payload");
    return;
  }

  const std::string columns_text = payload.substr(0U, separator);
  const std::string rows_text = payload.substr(separator + 1U);
  uint32_t columns = 0U;
  uint32_t rows = 0U;
  if (!parse_uint32_value(columns_text, &columns) || !parse_uint32_value(rows_text, &rows)) {
    push_error_response(request.client_id, "invalid terminal size");
    return;
  }

  mros::shell::session_set_terminal_size(
      pane->session,
      static_cast<uint16_t>(columns),
      static_cast<uint16_t>(rows));
}

void process_state_request(const ShellWebRequest& request) {
  ShellWebPane* pane = ensure_web_pane(request.client_id, request.pane_id);
  if (pane == nullptr || pane->session == nullptr) {
    push_error_response(request.client_id, "shell pane/session limit reached");
    return;
  }
  const char* prompt = pane_prompt(pane);
  const char* cwd = mros::shell::current_working_directory();
  bool pooled = false;
  char* buffer = start_direct_response(&pooled);
  if (buffer == nullptr) {
    g_metrics.response_drop++;
    return;
  }
  mros::utils::FixedJsonWriter writer(buffer, kShellJsonCapacity);
  writer.append_raw("{\"shell\":{\"type\":\"state\",\"prompt\":\"");
  writer.append_escaped(prompt);
  writer.append_raw("\",\"cwd\":\"");
  writer.append_escaped(cwd != nullptr ? cwd : "/");
  writer.append_raw("\"");
  append_session_meta(writer, request, pane);
  writer.append_raw("}}");
  (void)finish_direct_response(request.client_id, buffer, pooled, writer);
}

void process_exec_request(const ShellWebRequest& request) {
  ShellWebPane* pane = ensure_web_pane(request.client_id, request.pane_id);
  if (pane == nullptr || pane->session == nullptr) {
    push_error_response(request.client_id, "shell pane/session limit reached");
    return;
  }
  const std::string raw_line = request.payload;
  const std::string trimmed = trim_copy(request.payload);
  const char* prompt_after_before = pane_prompt(pane);

  if (trimmed.empty()) {
    const char* cwd = mros::shell::current_working_directory();
    bool pooled = false;
    char* buffer = start_direct_response(&pooled);
    if (buffer == nullptr) {
      g_metrics.response_drop++;
      return;
    }
    mros::utils::FixedJsonWriter writer(buffer, kShellJsonCapacity);
    writer.append_raw("{\"shell\":{\"type\":\"exec\",\"ok\":true,\"output\":\"\\n");
    writer.append_escaped(prompt_after_before);
    writer.append_raw("\",\"prompt\":\"");
    writer.append_escaped(prompt_after_before);
    writer.append_raw("\",\"cwd\":\"");
    writer.append_escaped(cwd != nullptr ? cwd : "/");
    writer.append_raw("\",\"cleared\":false");
    append_session_meta(writer, request, pane);
    writer.append_raw("}}");
    (void)finish_direct_response(request.client_id, buffer, pooled, writer);
    return;
  }

  const char* cwd_before = mros::shell::current_working_directory();
  {
    bool pooled = false;
    char* buffer = start_direct_response(&pooled);
    if (buffer == nullptr) {
      g_metrics.response_drop++;
      return;
    }
    mros::utils::FixedJsonWriter writer(buffer, kShellJsonCapacity);
    writer.append_raw("{\"shell\":{\"type\":\"start\",\"line\":\"");
    writer.append_escaped(raw_line.c_str());
    writer.append_raw("\",\"output\":\"");
    writer.append_escaped(prompt_after_before);
    writer.append_escaped(raw_line.c_str());
    writer.append_raw("\\n\",\"prompt\":\"");
    writer.append_escaped(prompt_after_before);
    writer.append_raw("\",\"cwd\":\"");
    writer.append_escaped(cwd_before != nullptr ? cwd_before : "/");
    writer.append_raw("\"");
    append_session_meta(writer, request, pane);
    writer.append_raw("}}");
    (void)finish_direct_response(request.client_id, buffer, pooled, writer);
  }

  std::string captured_output;
  ShellWebStreamContext stream_context {};
  stream_context.client_id = request.client_id;
  stream_context.pane_id = request.pane_id;
  stream_context.command_id = request.command_id;
  stream_context.session_id = mros::shell::session_id(pane->session);
  const uint32_t started_ms = mros::platform::mros_millis();
  const bool ok = mros::shell::execute_session_line_capture_stream(
      pane->session,
      raw_line.c_str(),
      &captured_output,
      shell_stream_callback,
      &stream_context,
      false);
  const uint32_t duration_ms = mros::platform::mros_millis() - started_ms;
  flush_shell_stream_context(&stream_context);
  const char* prompt_after = pane_prompt(pane);
  const bool cleared = ok && is_clear_command(raw_line.c_str());
  const char* cwd = mros::shell::current_working_directory();

  std::string output;
  output = stream_context.sent_any
               ? (stream_context.dropped ? "\n[mshell: stream queue overflow]\n" : "")
               : captured_output;
  if (stream_context.sent_any && !stream_context.dropped && !cleared) {
    g_metrics.stream_final_suppressed++;
  }
  if (cleared) {
    output = prompt_after;
  }
  const bool truncated = truncate_shell_output(&output);
  if (truncated) {
    g_metrics.final_truncation_count++;
  }
  bool pooled = false;
  char* buffer = start_direct_response(&pooled);
  if (buffer == nullptr) {
    g_metrics.response_drop++;
    return;
  }
  mros::utils::FixedJsonWriter writer(buffer, kShellJsonCapacity);
  writer.append_raw("{\"shell\":{\"type\":\"exec\",\"ok\":");
  writer.append_raw(ok ? "true" : "false");
  writer.append_raw(",\"rc\":");
  writer.u32(ok ? 0U : 1U);
  writer.append_raw(",\"duration_ms\":");
  writer.u32(duration_ms);
  writer.append_raw(",\"output\":\"");
  writer.append_escaped(output.c_str());
  writer.append_raw("\",\"prompt\":\"");
  writer.append_escaped(prompt_after);
  writer.append_raw("\",\"cwd\":\"");
  writer.append_escaped(cwd != nullptr ? cwd : "/");
  writer.append_raw("\",\"cleared\":");
  writer.append_raw(cleared ? "true" : "false");
  writer.append_raw(",\"truncated\":");
  writer.append_raw(truncated ? "true" : "false");
  writer.append_raw(",\"streamed\":");
  writer.append_raw(stream_context.sent_any ? "true" : "false");
  writer.append_raw(",\"stream_dropped\":");
  writer.append_raw(stream_context.dropped ? "true" : "false");
  writer.append_raw(",\"stream_bytes\":");
  writer.u32(stream_context.byte_count);
  writer.append_raw(",\"stream_chunks\":");
  writer.u32(stream_context.chunk_count);
  append_session_meta(writer, request, pane);
  writer.append_raw("}}");
  if (!finish_direct_response(request.client_id, buffer, pooled, writer)) {
    push_error_response(request.client_id, "shell response overflow");
  }
}

void process_complete_request(const ShellWebRequest& request, const bool suggest_only) {
  char completed[kShellPayloadSize] = {};
  char suggestions[1024] = {};
  if (suggest_only) {
    std::snprintf(completed, sizeof(completed), "%s", request.payload);
    mros::shell::suggest_input(request.payload, suggestions, sizeof(suggestions));
  } else {
    mros::shell::complete_input(request.payload, completed, sizeof(completed), suggestions, sizeof(suggestions));
  }

  bool pooled = false;
  char* buffer = start_direct_response(&pooled);
  if (buffer == nullptr) {
    g_metrics.response_drop++;
    return;
  }
  mros::utils::FixedJsonWriter writer(buffer, kShellJsonCapacity);
  writer.append_raw("{\"shell\":{\"type\":\"complete\",\"line\":\"");
  writer.append_escaped(completed);
  writer.append_raw("\",\"suggestions\":[");
  bool first = true;
  const char* start = suggestions;
  while (start != nullptr && *start != '\0') {
    const char* end = std::strchr(start, '\n');
    const size_t len = end != nullptr ? static_cast<size_t>(end - start)
                                      : std::strlen(start);
    if (len > 0U) {
      if (!first) writer.append_raw(",");
      first = false;
      writer.append_raw("\"");
      writer.append_escaped(start, len);
      writer.append_raw("\"");
    }
    if (end == nullptr) break;
    start = end + 1;
  }
  writer.append_raw("]");
  writer.append_raw(",\"completion_kind\":\"");
  writer.append_escaped(suggest_only ? "suggest" : "complete");
  writer.append_raw("\",\"rank\":");
  writer.u32(1U);
  writer.append_raw(",\"suggestions_meta\":[");
  bool first_meta = true;
  uint32_t rank = 1U;
  start = suggestions;
  while (start != nullptr && *start != '\0') {
    const char* end = std::strchr(start, '\n');
    const size_t len = end != nullptr ? static_cast<size_t>(end - start)
                                      : std::strlen(start);
    if (len > 0U) {
      if (!first_meta) writer.append_raw(",");
      first_meta = false;
      writer.append_raw("{\"value\":\"");
      writer.append_escaped(start, len);
      writer.append_raw("\",\"rank\":");
      writer.u32(rank++);
      writer.append_raw(",\"completion_kind\":\"");
      writer.append_escaped(suggest_only ? "history-or-command" : "command-path-option");
      writer.append_raw("\"}");
    }
    if (end == nullptr) break;
    start = end + 1;
  }
  writer.append_raw("]");
  ShellWebPane* pane = ensure_web_pane(request.client_id, request.pane_id);
  append_session_meta(writer, request, pane);
  writer.append_raw("}}");
  (void)finish_direct_response(request.client_id, buffer, pooled, writer);
}

}  // namespace

void init() {
  if (g_initialized) {
    return;
  }

  if (g_request_queue == nullptr) {
    g_request_queue = xQueueCreate(kShellRequestQueueLength, sizeof(ShellWebRequest));
  }
  if (g_response_queue == nullptr) {
    g_response_queue = xQueueCreate(kShellResponseQueueLength, sizeof(ShellWebResponse));
  }

  mros::shell::init({
      .hostname = kShellHostname,
      .firmware_name = kShellFirmwareName,
      .board_name = kShellBoardName,
      .storage_mount_point = kShellMountPoint,
      .output_callback = shell_output_callback,
      .is_storage_mounted_callback = shell_is_storage_mounted,
      .mount_storage_callback = shell_mount_storage,
      .setting_action_callback = shell_handle_setting_action,
      .system_action_callback = shell_execute_system_action,
      .diagnostics_callback = shell_format_diagnostics,
      .status_callback = shell_format_status,
      .log_read_callback = shell_read_recent_logs,
      .clear_callback = shell_clear_console,
      .robot_action_callback = shell_execute_robot_action,
      .is_spi_connected_callback = shell_is_spi_connected,
      .is_espnow_connected_callback = shell_is_espnow_connected,
      .user_data = nullptr,
  });
  g_initialized = true;
}

void set_task_handle(TaskHandle_t task_handle) {
  g_task_handle = task_handle;
}

void notify_task() {
  if (g_task_handle != nullptr) {
    xTaskNotifyGive(g_task_handle);
  }
}

void process_pending_requests() {
  if (!g_initialized) {
    init();
  }
  mros::shell::poll_serial();

  ShellWebRequest request {};
  while (g_request_queue != nullptr && xQueueReceive(g_request_queue, &request, 0) == pdTRUE) {
    switch (request.type) {
      case WebRequestType::State:
        process_state_request(request);
        break;
      case WebRequestType::Execute:
        process_exec_request(request);
        break;
      case WebRequestType::Complete:
        process_complete_request(request, false);
        break;
      case WebRequestType::Suggest:
        process_complete_request(request, true);
        break;
      case WebRequestType::Resize:
        process_resize_request(request);
        break;
      default:
        push_error_response(request.client_id, "unsupported shell request");
        break;
    }
  }
}

bool enqueue_web_request(
    const WebRequestType type,
    const uint32_t client_id,
    const char* payload,
    char* error,
    const size_t error_size,
    const uint8_t pane_id,
    const uint16_t command_id) {
  if (!g_initialized) {
    init();
  }
  if (g_request_queue == nullptr) {
    copy_text(error, error_size, "shell request queue unavailable");
    return false;
  }

  ShellWebRequest request {};
  request.type = type;
  request.client_id = client_id;
  request.pane_id = pane_id < kShellMaxPanesPerClient ? pane_id : 0U;
  request.command_id = command_id;
  const char* safe_payload = payload != nullptr ? payload : "";
  if (type == WebRequestType::Execute) {
    safe_payload = strip_web_command_id_prefix(safe_payload, &request.command_id);
  }
  copy_text(request.payload, sizeof(request.payload), safe_payload);

  if (xQueueSend(g_request_queue, &request, 0) != pdTRUE) {
    copy_text(error, error_size, "shell queue is full");
    return false;
  }

  notify_task();
  return true;
}

void close_client_sessions(const uint32_t client_id) {
  close_protocol_state(client_id);
  for (ShellClientIdentity& identity : g_client_identities) {
    if (identity.in_use && identity.client_id == client_id) {
      identity = ShellClientIdentity {};
    }
  }
  for (ShellWebPane& pane : g_web_panes) {
    if (!pane.in_use || pane.client_id != client_id) {
      continue;
    }
    if (pane.session != nullptr) {
      mros::shell::destroy_session(pane.session);
    }
    pane = ShellWebPane {};
  }
}

bool close_session_id(const uint32_t session_id_value) {
  if (session_id_value == 0U) {
    return false;
  }
  for (ShellWebPane& pane : g_web_panes) {
    if (!pane.in_use || pane.session == nullptr ||
        mros::shell::session_id(pane.session) != session_id_value) {
      continue;
    }
    mros::shell::destroy_session(pane.session);
    pane = ShellWebPane {};
    return true;
  }
  return false;
}

bool sessions_json(char* json, const size_t json_size) {
  if (json == nullptr || json_size == 0U) {
    return false;
  }
  mros::utils::FixedJsonWriter writer(json, json_size);
  writer.append_raw("{\"ok\":true,\"max_sessions\":");
  writer.u32(mros::shell::session_capacity());
  writer.append_raw(",\"active_sessions\":");
  writer.u32(mros::shell::active_session_count());
  writer.append_raw(",\"root_sessions\":");
  writer.u32(mros::shell::active_root_session_count());
  writer.append_raw(",\"max_panes_per_client\":");
  writer.u32(kShellMaxPanesPerClient);
  writer.append_raw(",\"sessions\":[");
  bool first = true;
  for (const ShellWebPane& pane : g_web_panes) {
    if (!pane.in_use || pane.session == nullptr) {
      continue;
    }
    if (!first) {
      writer.append_raw(",");
    }
    first = false;
    writer.append_raw("{\"client_id\":");
    writer.u32(pane.client_id);
    writer.append_raw(",\"pane_id\":");
    writer.u32(pane.pane_id);
    writer.append_raw(",\"session_id\":");
    writer.u32(mros::shell::session_id(pane.session));
    writer.append_raw(",\"root\":");
    writer.append_raw(mros::shell::session_is_root(pane.session) ? "true" : "false");
    writer.append_raw("}");
  }
  writer.append_raw("]}");
  return !writer.overflow();
}

void set_client_user(const uint32_t client_id, const char* username) {
  if (client_id == 0U || username == nullptr || username[0] == '\0') {
    return;
  }
  ShellClientIdentity* slot = find_client_identity(client_id);
  if (slot == nullptr) {
    for (ShellClientIdentity& candidate : g_client_identities) {
      if (!candidate.in_use) {
        slot = &candidate;
        break;
      }
    }
  }
  if (slot == nullptr) {
    slot = &g_client_identities[0];
  }
  slot->in_use = true;
  slot->client_id = client_id;
  std::snprintf(slot->username, sizeof(slot->username), "%s", username);

  for (ShellWebPane& pane : g_web_panes) {
    if (pane.in_use && pane.client_id == client_id && pane.session != nullptr) {
      (void)mros::shell::session_apply_user(pane.session, slot->username);
    }
  }
}

bool dequeue_web_response(uint32_t* client_id, char* json, const size_t json_size) {
  if (client_id == nullptr || json == nullptr || json_size == 0U || g_response_queue == nullptr) {
    return false;
  }

  ShellWebResponse response {};
  if (xQueueReceive(g_response_queue, &response, 0) != pdTRUE) {
    return false;
  }

  if (response.kind != ShellWebOutboundKind::TextJson) {
    release_response_buffer(response.data, response.pooled);
    return false;
  }
  *client_id = response.client_id;
  copy_text(json, json_size, response.data != nullptr ? response.data : "{}");
  release_response_buffer(response.data, response.pooled);
  return true;
}

bool dequeue_web_outbound(uint32_t* client_id,
                          ShellWebOutboundKind* kind,
                          uint8_t* payload,
                          const size_t payload_size,
                          size_t* payload_len) {
  if (client_id == nullptr || kind == nullptr || payload == nullptr ||
      payload_size == 0U || payload_len == nullptr || g_response_queue == nullptr) {
    return false;
  }

  ShellWebResponse response {};
  if (xQueueReceive(g_response_queue, &response, 0) != pdTRUE) {
    return false;
  }

  const size_t len = std::min(payload_size - 1U, response.len);
  if (len > 0U && response.data != nullptr) {
    std::memcpy(payload, response.data, len);
  }
  payload[len] = 0U;
  *client_id = response.client_id;
  *kind = response.kind;
  *payload_len = len;
  release_response_buffer(response.data, response.pooled);
  return true;
}

void get_metrics(ShellServiceMetrics* out_metrics) {
  if (out_metrics == nullptr) return;
  *out_metrics = g_metrics;
  out_metrics->response_pool_capacity =
      static_cast<uint32_t>(kShellResponsePoolSlots);
  if (ensure_response_pool_mutex() &&
      xSemaphoreTake(g_response_pool_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    uint32_t active = 0;
    uint32_t allocated = 0;
    for (const ShellResponsePoolSlot& slot : g_response_pool) {
      if (slot.data != nullptr) allocated++;
      if (slot.in_use) active++;
    }
    out_metrics->response_pool_active = active;
    out_metrics->response_pool_allocated = allocated;
    xSemaphoreGive(g_response_pool_mutex);
  }
  out_metrics->request_queue_depth =
      g_request_queue != nullptr
          ? static_cast<uint32_t>(uxQueueMessagesWaiting(g_request_queue))
          : 0U;
  out_metrics->request_queue_capacity =
      static_cast<uint32_t>(kShellRequestQueueLength);
  out_metrics->response_queue_depth =
      g_response_queue != nullptr
          ? static_cast<uint32_t>(uxQueueMessagesWaiting(g_response_queue))
          : 0U;
  out_metrics->response_queue_capacity =
      static_cast<uint32_t>(kShellResponseQueueLength);
}

void set_client_binary_stream(const uint32_t client_id, const bool enabled) {
  ShellClientProtocolState* state = ensure_protocol_state(client_id);
  if (state == nullptr) {
    return;
  }
  state->binary_stream = enabled;
  state->credit = enabled ? kShellInitialCreditBytes : 0U;
}

void note_client_ack_credit(const uint32_t client_id,
                            const uint32_t ack,
                            const uint32_t credit) {
  ShellClientProtocolState* state = ensure_protocol_state(client_id);
  if (state == nullptr) {
    return;
  }
  state->last_ack = ack;
  if (credit > 0U) {
    state->credit = credit;
    g_metrics.shell_credit_rx++;
  }
  g_metrics.shell_ack_rx++;
}

void note_client_binary_decode_error(const uint32_t client_id) {
  (void)client_id;
  g_metrics.shell_bin_decode_errors++;
}

void note_client_json_fallback(const uint32_t client_id) {
  set_client_binary_stream(client_id, false);
  g_metrics.shell_json_fallbacks++;
}

}  // namespace mros::shell::service
