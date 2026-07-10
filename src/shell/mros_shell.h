#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mros::shell {

inline constexpr const char* kShellName = "mshell";
inline constexpr const char* kShellReleaseLine = "v0.2";
inline constexpr const char* kShellVersion = "v0.2.0-rc1";
inline constexpr const char* kShellReleaseStatus = "rc";
inline constexpr const char* kShellProtocolName = "mros-shell-v2";
inline constexpr const char* kShellBinaryFormat = "shell-bin-v1";
inline constexpr const char* kShellJsonFormat = "shell-json-v1";
inline constexpr const char* kShellCborFormat = "shell-cbor-v1";
inline constexpr const char* kShellStorageAlias = "/fs";

using ShellOutputCallback = void (*)(const char* text, void* user_data);
using ShellStreamCallback = void (*)(const char* text, void* user_data);
using ShellBoolCallback = bool (*)(void* user_data);
using ShellIntCallback = int (*)(void* user_data);
using ShellTextCallback = const char* (*)(void* user_data);
using ShellStorageActionCallback = bool (*)(char* message, size_t size, void* user_data);

enum class ShellSettingAction : uint8_t {
  Get = 0,
  Set = 1,
};

enum class ShellSystemAction : uint8_t {
  Reboot = 0,
  Poweroff = 1,
};

enum class ShellEspNowAction : uint8_t {
  Scan = 0,
  List = 1,
  Connect = 2,
};

enum class ShellRobotAction : uint8_t {
  Position = 0,
  Move = 1,
  Gripper = 2,
  TurretPosition = 3,
  Speed = 4,
  Emergency = 5,
  DefaultPosition = 6,
  CalcPreference = 7,
  PathAdd = 8,
  PathClear = 9,
  PathPreview = 10,
  PathRun = 11,
  PathStatus = 12,
  JointSet = 13,
  JointApply = 14,
  Hold = 15,
  Stop = 16,
  Park = 17,
};

enum class ShellRobotResource : uint8_t {
  Unknown = 0,
  Power,
  Safety,
  Status,
  Telemetry,
  Turret,
  Gripper,
  Joint,
  Cartesian,
  Move,
  Path,
  Motion,
  Profile,
  Model,
  Frame,
  Limits,
  Math,
  Calibration,
  Diagnostics,
};

enum class ShellRobotVerb : uint8_t {
  Unknown = 0,
  List,
  Status,
  Get,
  Set,
  Add,
  Remove,
  Clear,
  Preview,
  Apply,
  Run,
  Stop,
  Hold,
  Reset,
  Home,
  Park,
  Solve,
  Validate,
  Export,
  Import,
  Describe,
  Compile,
  Compare,
  Benchmark,
  Explain,
  Open,
  Close,
  Insert,
  Delete,
  Jog,
  Zero,
};

struct ShellRobotVector3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct ShellRobotRequest {
  ShellRobotAction action = ShellRobotAction::Position;
  ShellRobotResource resource = ShellRobotResource::Unknown;
  ShellRobotVerb verb = ShellRobotVerb::Unknown;
  ShellRobotVector3 position {};
  ShellRobotVector3 from {};
  ShellRobotVector3 to {};
  float value = 0.0f;
  bool enabled = false;
  float time_ms = 1000.0f;
  bool ee_auto = true;
  float roll_deg = 0.0f;
  float pitch_deg = 0.0f;
  float yaw_deg = 0.0f;
  float ee_pitch = 0.0f;
  bool apply = false;
  bool wait = false;
  uint32_t timeout_ms = 0U;
  char calc_mode[16] = {};
  char profile[24] = {};
  char frame[24] = {};
  char units[8] = {};
  char model[32] = {};
  char subject[24] = {};
  char object_name[32] = {};
  float joints[7] = {};
  size_t joint_count = 0;
};

enum class ShellC6UpdateSourceKind : uint8_t {
  Default = 0,
  File = 1,
};

enum class ShellC6UpdateState : uint8_t {
  Idle = 0,
  Queued,
  Preparing,
  Validating,
  Transferring,
  Finalizing,
  Activating,
  Reconnecting,
  Completed,
  NotRequired,
  Failed,
};

struct ShellC6UpdateRequest {
  ShellC6UpdateSourceKind source = ShellC6UpdateSourceKind::Default;
  const char* path = nullptr;
};

struct ShellC6UpdateSnapshot {
  uint32_t revision = 0U;
  bool busy = false;
  uint8_t progress = 0U;
  ShellC6UpdateState state = ShellC6UpdateState::Idle;
  char source_label[160] = {};
  char status_text[96] = {};
  char detail_text[192] = {};
  char selected_path[192] = {};
  char last_result[192] = {};
};

enum class ShellTaskSignal : uint8_t {
  Term = 15,
  Kill = 9,
};

enum class ShellTransport : uint8_t {
  Current = 0,
  SerialConsole,
  Web,
  Ssh,
  System,
};

enum ShellCapability : uint32_t {
  ShellCapabilityRead = 1UL << 0,
  ShellCapabilityWrite = 1UL << 1,
  ShellCapabilityRobot = 1UL << 2,
  ShellCapabilityNetwork = 1UL << 3,
  ShellCapabilityUpdate = 1UL << 4,
  ShellCapabilityDebug = 1UL << 5,
  ShellCapabilityRoot = 1UL << 6,
};

inline constexpr uint32_t kShellCapabilityUserDefault =
    ShellCapabilityRead | ShellCapabilityWrite;
inline constexpr uint32_t kShellCapabilityAdmin =
    ShellCapabilityRead | ShellCapabilityWrite | ShellCapabilityRobot |
    ShellCapabilityNetwork | ShellCapabilityUpdate | ShellCapabilityDebug;
inline constexpr uint32_t kShellCapabilityRoot =
    kShellCapabilityAdmin | ShellCapabilityRoot;

struct ShellSession;

using ShellSettingActionCallback =
    bool (*)(ShellSettingAction action, const char* name, const char* value, char* message, size_t size, void* user_data);
using ShellSystemActionCallback = bool (*)(ShellSystemAction action, void* user_data);
using ShellDiagnosticsCallback = bool (*)(char* message, size_t size, void* user_data);
using ShellStatusCallback = bool (*)(char* message, size_t size, void* user_data);
using ShellLogReadCallback = bool (*)(size_t line_count, char* message, size_t size, void* user_data);
using ShellClearCallback = bool (*)(void* user_data);
using ShellEspNowActionCallback =
    bool (*)(ShellEspNowAction action, const char* argument, char* message, size_t size, void* user_data);
using ShellRobotActionCallback =
    bool (*)(const ShellRobotRequest& request, char* message, size_t size, void* user_data);
using ShellTaskSignalCallback =
    bool (*)(ShellTaskSignal signal, const char* target, char* message, size_t size, void* user_data);
using ShellC6UpdateStartCallback =
    bool (*)(const ShellC6UpdateRequest& request, char* message, size_t size, void* user_data);
using ShellC6UpdateSnapshotCallback = bool (*)(ShellC6UpdateSnapshot* snapshot, void* user_data);

struct ShellConfig {
  const char* hostname = "mros";
  const char* firmware_name = "MROS Firmware";
  const char* board_name = "M5Stack Tab5";
  const char* storage_mount_point = "/littlefs";
  ShellOutputCallback output_callback = nullptr;
  ShellBoolCallback is_storage_mounted_callback = nullptr;
  ShellStorageActionCallback mount_storage_callback = nullptr;
  ShellSettingActionCallback setting_action_callback = nullptr;
  ShellSystemActionCallback system_action_callback = nullptr;
  ShellDiagnosticsCallback diagnostics_callback = nullptr;
  ShellStatusCallback status_callback = nullptr;
  ShellLogReadCallback log_read_callback = nullptr;
  ShellClearCallback clear_callback = nullptr;
  ShellEspNowActionCallback espnow_action_callback = nullptr;
  ShellRobotActionCallback robot_action_callback = nullptr;
  ShellTaskSignalCallback task_signal_callback = nullptr;
  ShellC6UpdateStartCallback c6_update_start_callback = nullptr;
  ShellC6UpdateSnapshotCallback c6_update_snapshot_callback = nullptr;
  ShellBoolCallback is_spi_connected_callback = nullptr;
  ShellBoolCallback is_espnow_connected_callback = nullptr;
  ShellIntCallback battery_percent_callback = nullptr;
  ShellTextCallback active_page_callback = nullptr;
  void* user_data = nullptr;
};

void init(const ShellConfig& config);
bool execute_line(const char* line, bool echo_command = true, ShellTransport transport = ShellTransport::Current);
bool execute_line_capture(
    const char* line,
    std::string* output,
    bool echo_command = true,
    ShellTransport transport = ShellTransport::Current);
bool execute_line_capture_stream(
    const char* line,
    std::string* output,
    ShellStreamCallback stream_callback,
    void* stream_user_data,
    bool echo_command = true,
    ShellTransport transport = ShellTransport::Current);
bool execute_line_capture_as_user(
    const char* line,
    const char* username,
    std::string* output,
    bool echo_command = true,
    ShellTransport transport = ShellTransport::Current);
ShellSession* create_session(ShellTransport transport);
void destroy_session(ShellSession* session);
bool session_apply_user(ShellSession* session, const char* username);
bool execute_session_line(
    ShellSession* session,
    const char* line,
    std::string* output,
    bool echo_command = true);
bool execute_session_line_capture_stream(
    ShellSession* session,
    const char* line,
    std::string* output,
    ShellStreamCallback stream_callback,
    void* stream_user_data,
    bool echo_command = true);
void set_transport_terminal_size(ShellTransport transport, uint16_t columns, uint16_t rows);
void session_set_terminal_size(ShellSession* session, uint16_t columns, uint16_t rows);
std::string prompt_for_transport(ShellTransport transport);
const char* session_prompt(ShellSession* session);
bool session_close_requested(const ShellSession* session);
void session_request_close(ShellSession* session);
bool session_is_root(const ShellSession* session);
uint32_t session_id(const ShellSession* session);
uint32_t active_session_count();
uint32_t active_root_session_count();
uint32_t session_capacity();
uint32_t capability_mask_for_user(const char* username);
const char* capabilities_text(uint32_t capabilities);
void audit_record(const char* event, const char* detail);
std::string audit_report();
void audit_clear();
void poll_serial();
const char* current_working_directory();
bool complete_input(
    const char* input,
    char* completed_line,
    size_t completed_line_size,
    char* suggestions,
    size_t suggestions_size);
bool suggest_input(const char* input, char* suggestions, size_t suggestions_size);
bool serial_has_pending_input();
bool serial_recently_active(uint32_t within_ms = 3000U);
bool has_partial_input();
bool serial_auth_required();
bool set_serial_auth_required(bool required);
const char* serial_auth_mode_text();

}  // namespace mros::shell
