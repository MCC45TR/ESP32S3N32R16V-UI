#include "src/shell/mros_shell_internal.h"
#include "src/shell/mshell_remote.h"

#include "src/security/ssh_identity.h"
#include "src/drivers/utils/mros_console.h"
#include "src/platform/mros_nvs.h"
#include "src/platform/mros_time.h"
#include "src/platform/mros_uart.h"

#if defined(CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED) && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>

namespace mros::shell {

struct ShellSession {
  ShellState state {};
  std::string prompt_cache;
  bool in_use = false;
  uint32_t id = 0U;
  uint32_t created_ms = 0U;
};

namespace {

#ifndef MROS_ALLOW_INSECURE_SERIAL_SHELL
#define MROS_ALLOW_INSECURE_SERIAL_SHELL 0
#endif

struct ShellRedirect {
  bool enabled = false;
  bool append = false;
  std::string target;
  bool input_enabled = false;
  bool heredoc = false;
  std::string input_target;
};

struct StageExecutionOptions {
  std::vector<std::string> args;
  bool json_output = false;
  bool json_option_explicit = false;
  uint32_t timeout_ms = 0U;
};

struct ShellTokenSpan {
  size_t start = 0U;
  size_t len = 0U;
  std::string owned;

  bool is_owned() const { return !owned.empty() || (start == std::string::npos && len == 0U); }
};

constexpr uint16_t kDefaultTerminalColumns = 80U;
constexpr uint16_t kDefaultTerminalRows = 24U;
constexpr uint16_t kDefaultSshColumns = 120U;
constexpr uint16_t kDefaultSshRows = 32U;
constexpr uart_port_t kShellSerialPort = UART_NUM_0;
constexpr size_t kShellSessionPoolSize = 5U;
constexpr size_t kShellAuditRingSize = 16U;

struct ShellAuditRecord {
  uint32_t seq = 0U;
  uint32_t ms = 0U;
  char event[24] = {};
  char detail[96] = {};
};

enum class SerialAuthStage : uint8_t {
  None = 0,
  AwaitUser,
  AwaitPassword,
};

enum class SerialInputSource : uint8_t {
  Uart0 = 0,
  UsbJtag = 1,
};

constexpr size_t kSerialInputSourceCount = 2U;
constexpr size_t kSerialInputBufferSize = 256U;

bool g_serial_auth_loaded = false;
bool g_serial_auth_required = true;
SerialAuthStage g_serial_auth_stage = SerialAuthStage::None;
std::string g_serial_auth_username;
char g_serial_input_buffer[kSerialInputSourceCount][kSerialInputBufferSize] = {};
size_t g_serial_input_length[kSerialInputSourceCount] = {};
ShellSession g_session_pool[kShellSessionPoolSize];
uint32_t g_next_shell_session_id = 1U;
ShellAuditRecord g_audit_ring[kShellAuditRingSize];
uint32_t g_audit_seq = 0U;
size_t g_audit_pos = 0U;

size_t serial_source_index(const SerialInputSource source) {
  return source == SerialInputSource::UsbJtag ? 1U : 0U;
}

char* serial_input_buffer_for_source(const SerialInputSource source) {
  return g_serial_input_buffer[serial_source_index(source)];
}

size_t& serial_input_length_for_source(const SerialInputSource source) {
  return g_serial_input_length[serial_source_index(source)];
}

void clear_serial_input_buffer(const SerialInputSource source) {
  char* buffer = serial_input_buffer_for_source(source);
  size_t& length = serial_input_length_for_source(source);
  length = 0U;
  buffer[0] = '\0';
}

void clear_all_serial_input_buffers() {
  for (size_t i = 0U; i < kSerialInputSourceCount; ++i) {
    g_serial_input_length[i] = 0U;
    g_serial_input_buffer[i][0] = '\0';
  }
}

bool any_serial_input_buffer_has_data() {
  for (size_t i = 0U; i < kSerialInputSourceCount; ++i) {
    if (g_serial_input_length[i] > 0U) {
      return true;
    }
  }
  return false;
}

bool ensure_serial_console_ready() {
  if (mros::platform::mros_uart_is_ready(kShellSerialPort)) {
    return true;
  }
  mros::platform::UartConfig config = {};
  config.port = kShellSerialPort;
  config.tx_pin = UART_PIN_NO_CHANGE;
  config.rx_pin = UART_PIN_NO_CHANGE;
  config.baud_rate = 115200;
  config.rx_buffer_size = 1024;
  config.tx_buffer_size = 1024;
  config.queue_size = 0;
  return mros::platform::mros_uart_init(config);
}

bool ensure_usb_serial_console_ready() {
#if defined(CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED) && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
  if (usb_serial_jtag_is_driver_installed()) {
    return true;
  }
  usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  config.tx_buffer_size = 512;
  config.rx_buffer_size = 512;
  return usb_serial_jtag_driver_install(&config) == ESP_OK;
#else
  return false;
#endif
}

void usb_serial_console_write(const void* data, const size_t size) {
#if defined(CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED) && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
  if (data != nullptr && size > 0U && ensure_usb_serial_console_ready() &&
      usb_serial_jtag_is_connected()) {
    (void)usb_serial_jtag_write_bytes(data, size, 0);
  }
#else
  (void)data;
  (void)size;
#endif
}

void serial_console_write_text(const char* text) {
  if (text == nullptr || text[0] == '\0') {
    return;
  }
  const size_t len = std::strlen(text);
  if (ensure_serial_console_ready()) {
    (void)mros::platform::mros_uart_write(kShellSerialPort, text, len);
  }
  usb_serial_console_write(text, len);
}

void serial_console_write_line(const char* text = "") {
  serial_console_write_text(text);
  serial_console_write_text("\n");
}

bool serial_shell_line_edit_active() {
  // Keep line editing/echo active for both UART and USB serial inputs so users can
  // reliably type commands even while runtime logs are mirrored.
  return true;
}

bool serial_shell_password_entry_active() {
  return g_serial_auth_stage == SerialAuthStage::AwaitPassword &&
         !mros_console_is_serial_mirror_suppressed();
}

void serial_console_echo_char(const char ch, const SerialInputSource source) {
  if (!serial_shell_line_edit_active()) {
    return;
  }
  const char echo = serial_shell_password_entry_active() ? '*' : ch;
  if (source == SerialInputSource::Uart0) {
    if (ensure_serial_console_ready()) {
      (void)mros::platform::mros_uart_write(kShellSerialPort, &echo, 1U);
    }
    return;
  }
  usb_serial_console_write(&echo, 1U);
}

void serial_console_echo_backspace(const SerialInputSource source) {
  if (!serial_shell_line_edit_active()) {
    return;
  }
  static constexpr const char kBackspace[] = "\b \b";
  if (source == SerialInputSource::Uart0) {
    if (ensure_serial_console_ready()) {
      (void)mros::platform::mros_uart_write(kShellSerialPort, kBackspace, sizeof(kBackspace) - 1U);
    }
    return;
  }
  usb_serial_console_write(kBackspace, sizeof(kBackspace) - 1U);
}

struct TransportTerminalSize {
  uint16_t columns = kDefaultTerminalColumns;
  uint16_t rows = kDefaultTerminalRows;
};

bool is_operator_token(const std::string& token) {
  return token == "|" || token == ">" || token == ">>" || token == "<" || token == "<<" ||
         token == ";" || token == "&&" || token == "||";
}

bool starts_with_text(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() && value.compare(0U, prefix.size(), prefix) == 0;
}

std::string shared_prefix(const std::vector<std::string>& values) {
  if (values.empty()) {
    return {};
  }

  std::string prefix = values.front();
  for (size_t i = 1U; i < values.size(); ++i) {
    size_t j = 0U;
    while (j < prefix.size() && j < values[i].size() && prefix[j] == values[i][j]) {
      ++j;
    }
    prefix.resize(j);
    if (prefix.empty()) {
      break;
    }
  }
  return prefix;
}

bool parse_uint32_value(const std::string& text, uint32_t* out_value) {
  if (out_value == nullptr || text.empty()) {
    return false;
  }

  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || (end != nullptr && *end != '\0') ||
      parsed > static_cast<unsigned long>(std::numeric_limits<uint32_t>::max())) {
    return false;
  }
  *out_value = static_cast<uint32_t>(parsed);
  return true;
}

uint16_t clamp_terminal_columns(const uint16_t value) {
  if (value == 0U) {
    return kDefaultTerminalColumns;
  }
  return std::max<uint16_t>(40U, std::min<uint16_t>(value, 240U));
}

uint16_t clamp_terminal_rows(const uint16_t value) {
  if (value == 0U) {
    return kDefaultTerminalRows;
  }
  return std::max<uint16_t>(10U, std::min<uint16_t>(value, 120U));
}

void set_state_terminal_size(ShellState& state, const uint16_t columns, const uint16_t rows) {
  state.terminal_columns = clamp_terminal_columns(columns);
  state.terminal_rows = clamp_terminal_rows(rows);
}

bool transport_supports_ansi(const ShellTransport transport) {
  return transport == ShellTransport::Web || transport == ShellTransport::Ssh;
}

std::string json_escape(const std::string& text) {
  std::string escaped;
  escaped.reserve(text.size() + 16U);
  for (const char ch : text) {
    switch (ch) {
      case '\"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
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

std::string trim_trailing_newlines(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

std::string build_json_response(
    const std::vector<std::string>& args,
    const int exit_code,
    const uint32_t elapsed_ms,
    const std::string& output) {
  static constexpr const char* kRawJsonPrefix = "@@RAW_JSON@@";
  if (output.rfind(kRawJsonPrefix, 0U) == 0U) {
    std::string raw = output.substr(std::strlen(kRawJsonPrefix));
    if (!raw.empty() && raw.back() != '\n') {
      raw.push_back('\n');
    }
    return raw;
  }
  std::string json;
  json.reserve(output.size() + 256U);
  json += "{\"shell\":\"";
  json += kShellName;
  json += "\",\"version\":\"";
  json += kShellVersion;
  json += "\",\"command\":\"";
  json += json_escape(!args.empty() ? args.front() : "");
  json += "\",\"ok\":";
  json += (exit_code == 0) ? "true" : "false";
  json += ",\"exit_code\":";
  json += std::to_string(exit_code);
  json += ",\"duration_ms\":";
  json += std::to_string(elapsed_ms);
  json += ",\"output\":\"";
  json += json_escape(trim_trailing_newlines(output));
  json += "\"}\n";
  return json;
}

void append_suggestions_text(char* buffer, const size_t size, const std::vector<std::string>& values) {
  if (buffer == nullptr || size == 0U) {
    return;
  }

  size_t used = 0U;
  buffer[0] = '\0';
  for (const std::string& value : values) {
    if (value.empty()) {
      continue;
    }
    const int written = std::snprintf(buffer + used, size - used, "%s%s", used == 0U ? "" : "\n", value.c_str());
    if (written <= 0) {
      break;
    }
    used = std::min(size - 1U, used + static_cast<size_t>(written));
    if (used >= (size - 1U)) {
      break;
    }
  }
  buffer[used] = '\0';
}

bool command_might_take_path(const std::string& command, const size_t argument_index, const std::string& current_prefix) {
  if (!current_prefix.empty() && (current_prefix.front() == '/' || current_prefix.find('/') != std::string::npos)) {
    return true;
  }
  if (command == "help" || command == "set" || command == "echo" || command == "uptime" || command == "status" ||
      command == "htop" || command == "mtop" || command == "sleep" || command == "EOF" || command == "eof" ||
      command == "reboot" || command == "poweroff" || command == "mfetch" || command == "espfetch" ||
      command == "clear" || command == "uname" || command == "date" || command == "pwd" || command == "espnow" ||
      command == "robot" || command == "ps" || command == "free" || command == "kill" || command == "ping" ||
      command == "wifi" || command == "mros" || command == "mshell" ||
      command == "mros-deuscara-update" || command == "mros7dofs3-update" ||
      command == "ssh" || command == "change" || command == "su" || command == "exit" ||
      command == "lsblk" || command == "math") {
    return false;
  }
  if (command == "grep") {
    return argument_index >= 2U;
  }
  if (command == "mount" || command == "umount") {
    return false;
  }
  return argument_index >= 1U;
}

bool build_path_matches(
    const ShellState& state,
    const std::string& raw_prefix,
    std::vector<std::string>* matches) {
  if (matches == nullptr) {
    return false;
  }

  matches->clear();
  std::string directory_token;
  std::string entry_prefix = raw_prefix;
  const size_t slash_pos = raw_prefix.find_last_of('/');
  if (slash_pos != std::string::npos) {
    directory_token = raw_prefix.substr(0U, slash_pos + 1U);
    entry_prefix = raw_prefix.substr(slash_pos + 1U);
  }

  const std::string directory_path =
      directory_token.empty() ? state.cwd : shell_normalize_path(state, directory_token);
  std::vector<ShellFsEntry> entries;
  std::string error;
  if (!shell_read_directory(state, directory_path, false, false, &entries, &error)) {
    return false;
  }

  for (const ShellFsEntry& entry : entries) {
    if (!starts_with_text(entry.name, entry_prefix)) {
      continue;
    }
    std::string candidate = directory_token + entry.name;
    if (entry.is_dir) {
      candidate.push_back('/');
    }
    matches->push_back(candidate);
  }

  std::sort(matches->begin(), matches->end());
  return !matches->empty();
}

const ShellCommandRegistration* find_command(const std::string& command_name);
void print_command_not_found(ShellState& state, const std::string& command_name);
std::string trim_copy(const char* text);
std::vector<std::string> tokenize_command_line(const std::string& line);

ShellState g_shell_state {};
TransportTerminalSize g_serial_terminal_size {kDefaultTerminalColumns, kDefaultTerminalRows};
TransportTerminalSize g_web_terminal_size {120U, 32U};
TransportTerminalSize g_ssh_terminal_size {kDefaultSshColumns, kDefaultSshRows};
std::vector<ShellAliasRecord> g_aliases;
bool g_aliases_loaded = false;
ShellCompletionCache g_completion_cache;
bool g_completion_cache_valid = false;
uint32_t g_completion_cache_revision = 0U;

const TransportTerminalSize& transport_terminal_size(const ShellTransport transport) {
  switch (transport) {
    case ShellTransport::Web:
      return g_web_terminal_size;
    case ShellTransport::Ssh:
      return g_ssh_terminal_size;
    case ShellTransport::SerialConsole:
    case ShellTransport::Current:
    case ShellTransport::System:
    default:
      return g_serial_terminal_size;
  }
}

void apply_transport_terminal_size(ShellState& state, const ShellTransport transport) {
  const TransportTerminalSize& size = transport_terminal_size(transport);
  set_state_terminal_size(state, size.columns, size.rows);
}

uint32_t capability_mask_for_account(const mros::ssh::UserAccount& user) {
  if (user.root) {
    return kShellCapabilityRoot;
  }
  return user.admin ? kShellCapabilityAdmin : kShellCapabilityUserDefault;
}

void apply_user_identity(ShellState& state, const mros::ssh::UserAccount& user) {
  state.session_username = user.username.c_str();
  state.session_display_name = user.display_name.c_str();
  state.session_admin = user.admin;
  state.session_can_sudo = user.sudo;
  state.root_session = user.root;
  state.capability_mask = capability_mask_for_account(user);
}

bool apply_user_identity_by_name(ShellState& state, const char* username) {
  if (username == nullptr || username[0] == '\0') {
    return false;
  }
  mros::ssh::UserAccount user;
  if (!mros::ssh::get_user(String(username), &user)) {
    return false;
  }
  apply_user_identity(state, user);
  return true;
}

void initialize_session_identity(ShellState& state) {
  const mros::ssh::IdentityConfig identity = mros::ssh::identity_get();
  mros::ssh::UserAccount user;
  if (mros::ssh::get_user(identity.username, &user)) {
    apply_user_identity(state, user);
    return;
  }
  state.session_username = identity.username.c_str();
  state.session_display_name = identity.display_name.c_str();
  state.session_admin = identity.user_admin;
  state.session_can_sudo = identity.user_sudo;
  state.capability_mask = identity.user_admin ? kShellCapabilityAdmin : kShellCapabilityUserDefault;
}

uint32_t count_root_sessions_except(const ShellState* except_state) {
  uint32_t count = 0U;
  if (except_state != &g_shell_state && g_shell_state.root_session) {
    ++count;
  }
  for (const ShellSession& session : g_session_pool) {
    if (session.in_use && &session.state != except_state && session.state.root_session) {
      ++count;
    }
  }
  return count;
}

void copy_audit_text(char* dst, const size_t dst_size, const char* src) {
  if (dst == nullptr || dst_size == 0U) return;
  std::snprintf(dst, dst_size, "%s", src != nullptr ? src : "");
}

bool shell_capability_allowed(const ShellState& state, const uint32_t required) {
  const uint32_t granted = state.root_session ? kShellCapabilityRoot : state.capability_mask;
  return (required == 0U) || ((granted & required) == required);
}

bool insecure_serial_shell_allowed() {
#if MROS_ALLOW_INSECURE_SERIAL_SHELL
  return true;
#else
  return false;
#endif
}

void load_serial_auth_setting() {
  if (g_serial_auth_loaded) {
    return;
  }
  g_serial_auth_loaded = true;
  mros::platform::NvsNamespace ns;
  bool required = true;
  if (ns.open("security", true,
              mros::platform::NvsPartitionMode::UserPartitionsThenDefault) &&
      ns.get_bool("serial_auth", &required)) {
    g_serial_auth_required = required;
  }
  if (!insecure_serial_shell_allowed()) {
    g_serial_auth_required = true;
  }
}

bool save_serial_auth_setting(const bool required) {
  if (!required && !insecure_serial_shell_allowed()) {
    return false;
  }
  mros::platform::NvsNamespace ns;
  if (!ns.open("security", false,
               mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    return false;
  }
  return ns.set_bool("serial_auth", required);
}

void reset_serial_auth_prompt() {
  g_serial_auth_stage = SerialAuthStage::None;
  g_serial_auth_username.clear();
}

void enter_serial_shell_mode() {
  reset_serial_auth_prompt();
  clear_all_serial_input_buffers();
  g_shell_state.serial_input_length = 0U;
  g_shell_state.serial_input_buffer[0] = '\0';
  mros_console_set_serial_mirror_suppressed(true);
  serial_console_write_line();
  serial_console_write_line("[mshell] serial shell mode active at 115200.");
  serial_console_write_line("[mshell] runtime logs suppressed. type exit or logout to return.");
  serial_console_write_text(shell_prompt(g_shell_state).c_str());
}

void leave_serial_shell_mode() {
  reset_serial_auth_prompt();
  clear_all_serial_input_buffers();
  g_shell_state.serial_input_length = 0U;
  g_shell_state.serial_input_buffer[0] = '\0';
  serial_console_write_line();
  serial_console_write_line("[mshell] leaving serial shell mode; runtime logs restored.");
  mros_console_set_serial_mirror_suppressed(false);
}

bool is_valid_alias_name(const std::string& name) {
  if (name.empty()) {
    return false;
  }
  for (const char ch : name) {
    const bool ok =
        std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-' || ch == '.';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string alias_file_path() {
  const char* mount_point =
      g_shell_state.config.storage_mount_point != nullptr ? g_shell_state.config.storage_mount_point : "/littlefs";
  return std::string(mount_point) + "/ESPUSER/mshell_aliases.txt";
}

void ensure_alias_dir() {
  const char* mount_point =
      g_shell_state.config.storage_mount_point != nullptr ? g_shell_state.config.storage_mount_point : "/littlefs";
  const std::string dir = std::string(mount_point) + "/ESPUSER";
  mkdir(dir.c_str(), 0775);
}

void load_aliases_if_needed() {
  if (g_aliases_loaded) {
    return;
  }
  g_aliases_loaded = true;
  g_aliases.clear();
  if (!shell_is_storage_mounted(g_shell_state)) {
    return;
  }
  FILE* file = std::fopen(alias_file_path().c_str(), "rb");
  if (file == nullptr) {
    return;
  }
  char line[256] = {};
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    std::string raw = trim_copy(line);
    if (raw.empty() || raw.front() == '#') {
      continue;
    }
    const size_t eq = raw.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string name = trim_copy(raw.substr(0U, eq).c_str());
    const std::string value = trim_copy(raw.substr(eq + 1U).c_str());
    if (is_valid_alias_name(name) && !value.empty()) {
      g_aliases.push_back({name, value});
    }
  }
  std::fclose(file);
}

const ShellAliasRecord* find_alias_record(const std::string& name) {
  load_aliases_if_needed();
  for (const ShellAliasRecord& alias : g_aliases) {
    if (alias.name == name) {
      return &alias;
    }
  }
  return nullptr;
}

void rebuild_completion_cache() {
  load_aliases_if_needed();
  g_completion_cache.command_names.clear();
  g_completion_cache.alias_names.clear();
  g_completion_cache.all_names.clear();
  g_completion_cache.max_command_name_width = 0U;

  const std::vector<ShellCommandRegistration>& commands = shell_commands();
  g_completion_cache.command_names.reserve(commands.size());
  for (const ShellCommandRegistration& command : commands) {
    if (command.name == nullptr || command.name[0] == '\0') {
      continue;
    }
    g_completion_cache.command_names.emplace_back(command.name);
    g_completion_cache.max_command_name_width =
        std::max(g_completion_cache.max_command_name_width,
                 g_completion_cache.command_names.back().size());
  }

  g_completion_cache.alias_names.reserve(g_aliases.size());
  for (const ShellAliasRecord& alias : g_aliases) {
    if (!alias.name.empty()) {
      g_completion_cache.alias_names.push_back(alias.name);
    }
  }

  g_completion_cache.all_names.reserve(
      g_completion_cache.command_names.size() + g_completion_cache.alias_names.size());
  g_completion_cache.all_names.insert(g_completion_cache.all_names.end(),
                                      g_completion_cache.command_names.begin(),
                                      g_completion_cache.command_names.end());
  g_completion_cache.all_names.insert(g_completion_cache.all_names.end(),
                                      g_completion_cache.alias_names.begin(),
                                      g_completion_cache.alias_names.end());
  std::sort(g_completion_cache.all_names.begin(), g_completion_cache.all_names.end());
  g_completion_cache.all_names.erase(
      std::unique(g_completion_cache.all_names.begin(), g_completion_cache.all_names.end()),
      g_completion_cache.all_names.end());
  g_completion_cache.revision = ++g_completion_cache_revision;
  g_completion_cache_valid = true;
}

void append_cached_name_matches(
    const std::vector<std::string>& names,
    const std::string& prefix,
    std::vector<std::string>* matches) {
  if (matches == nullptr) {
    return;
  }
  for (const std::string& name : names) {
    if (starts_with_text(name, prefix)) {
      matches->push_back(name);
    }
  }
}

std::string token_span_text(const std::string& source, const ShellTokenSpan& token) {
  if (token.is_owned()) {
    return token.owned;
  }
  return source.substr(token.start, token.len);
}

std::vector<std::string> materialize_token_spans(
    const std::string& source,
    const std::vector<ShellTokenSpan>& spans) {
  std::vector<std::string> tokens;
  tokens.reserve(spans.size());
  for (const ShellTokenSpan& span : spans) {
    tokens.push_back(token_span_text(source, span));
  }
  return tokens;
}

bool expand_alias_tokens(std::vector<std::string>* tokens) {
  if (tokens == nullptr || tokens->empty()) {
    return false;
  }
  const ShellAliasRecord* alias = find_alias_record(tokens->front());
  if (alias == nullptr) {
    return false;
  }
  std::vector<std::string> replacement = tokenize_command_line(alias->value);
  if (replacement.empty()) {
    return false;
  }
  replacement.reserve(replacement.size() + tokens->size() - 1U);
  replacement.insert(replacement.end(), tokens->begin() + 1, tokens->end());
  *tokens = replacement;
  return true;
}

void shell_error(ShellState& state, const char* message) {
  if (message == nullptr || message[0] == '\0') {
    return;
  }
  shell_printf(state, "%s: %s\n", kShellName, message);
}

bool is_space_char(const char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

std::string trim_copy(const char* text) {
  if (text == nullptr) {
    return {};
  }

  const char* start = text;
  while (*start != '\0' && is_space_char(*start)) {
    ++start;
  }

  const char* end = start + std::strlen(start);
  while (end > start && is_space_char(*(end - 1))) {
    --end;
  }

  return std::string(start, static_cast<size_t>(end - start));
}

std::vector<ShellTokenSpan> tokenize_command_line_spans(const std::string& line) {
  std::vector<ShellTokenSpan> tokens;
  std::string current;
  tokens.reserve(std::max<size_t>(4U, line.size() / 4U));
  current.reserve(std::min<size_t>(line.size(), 64U));
  bool in_single_quote = false;
  bool in_double_quote = false;
  bool escaping = false;
  size_t raw_start = std::string::npos;
  size_t raw_len = 0U;

  auto ensure_owned = [&]() {
    if (raw_len > 0U) {
      current.append(line, raw_start, raw_len);
      raw_start = std::string::npos;
      raw_len = 0U;
    }
  };

  auto append_raw_char = [&](const size_t pos) {
    if (!current.empty()) {
      current.push_back(line[pos]);
      return;
    }
    if (raw_len == 0U) {
      raw_start = pos;
      raw_len = 1U;
      return;
    }
    if ((raw_start + raw_len) == pos) {
      ++raw_len;
      return;
    }
    ensure_owned();
    current.push_back(line[pos]);
  };

  auto emit_current = [&]() {
    if (!current.empty()) {
      ShellTokenSpan span {};
      span.start = std::string::npos;
      span.len = 0U;
      span.owned = current;
      tokens.push_back(std::move(span));
      current.clear();
      raw_start = std::string::npos;
      raw_len = 0U;
      return;
    }
    if (raw_len > 0U) {
      ShellTokenSpan span {};
      span.start = raw_start;
      span.len = raw_len;
      tokens.push_back(span);
      raw_start = std::string::npos;
      raw_len = 0U;
    }
  };

  auto emit_operator = [&](const size_t pos, const size_t len) {
    emit_current();
    ShellTokenSpan span {};
    span.start = pos;
    span.len = len;
    tokens.push_back(span);
  };

  for (size_t i = 0U; i < line.size(); ++i) {
    const char ch = line[i];
    if (escaping) {
      ensure_owned();
      current.push_back(ch);
      escaping = false;
      continue;
    }

    if (ch == '\\' && !in_single_quote) {
      ensure_owned();
      escaping = true;
      continue;
    }

    if (ch == '\'' && !in_double_quote) {
      ensure_owned();
      in_single_quote = !in_single_quote;
      continue;
    }

    if (ch == '"' && !in_single_quote) {
      ensure_owned();
      in_double_quote = !in_double_quote;
      continue;
    }

    if (!in_single_quote && !in_double_quote && is_space_char(ch)) {
      emit_current();
      continue;
    }

    if (!in_single_quote && !in_double_quote &&
        (ch == '|' || ch == '>' || ch == '<' || ch == ';' || ch == '&')) {
      if ((ch == '>' || ch == '<' || ch == '|' || ch == '&') &&
          (i + 1U) < line.size() && line[i + 1U] == ch) {
        emit_operator(i, 2U);
        ++i;
      } else if (ch == '>' && (i + 1U) < line.size() && line[i + 1U] == '>') {
        emit_operator(i, 2U);
        ++i;
      } else {
        emit_operator(i, 1U);
      }
      continue;
    }

    append_raw_char(i);
  }

  if (escaping) {
    ensure_owned();
    current.push_back('\\');
  }

  emit_current();

  return tokens;
}

std::vector<std::string> tokenize_command_line(const std::string& line) {
  const std::vector<ShellTokenSpan> spans = tokenize_command_line_spans(line);
  return materialize_token_spans(line, spans);
}

bool parse_pipeline_tokens(
    ShellState& state,
    const std::vector<std::string>& tokens,
    std::vector<std::vector<std::string>>* stages,
    ShellRedirect* redirect) {
  if (stages == nullptr || redirect == nullptr) {
    return false;
  }

  stages->clear();
  stages->reserve(2U);
  *redirect = {};
  std::vector<std::string> current_stage;
  current_stage.reserve(tokens.size());

  for (size_t i = 0U; i < tokens.size(); ++i) {
    const std::string& token = tokens[i];
    if (token == "|") {
      if (current_stage.empty()) {
        shell_error(state, "invalid null command");
        return false;
      }
      stages->push_back(current_stage);
      current_stage.clear();
      current_stage.reserve(tokens.size() - i);
      continue;
    }

    if (token == ">" || token == ">>" || token == "<" || token == "<<") {
      if (current_stage.empty()) {
        shell_error(state, "redirection requires a command");
        return false;
      }
      if ((i + 1U) >= tokens.size() || is_operator_token(tokens[i + 1U])) {
        shell_error(state, "missing redirection target");
        return false;
      }
      if (token == "<" || token == "<<") {
        redirect->input_enabled = true;
        redirect->heredoc = token == "<<";
        redirect->input_target = tokens[i + 1U];
      } else {
        stages->push_back(current_stage);
        current_stage.clear();
        redirect->enabled = true;
        redirect->append = token == ">>";
        redirect->target = tokens[i + 1U];
        if ((i + 2U) != tokens.size()) {
          shell_error(state, "redirection must appear at the end of the command");
          return false;
        }
        return true;
      }
      ++i;
      continue;
    }

    current_stage.push_back(token);
  }

  if (current_stage.empty()) {
    shell_error(state, "invalid null command");
    return false;
  }

  stages->push_back(current_stage);
  return true;
}

bool parse_stage_execution_options(
    ShellState& state,
    const std::vector<std::string>& stage_tokens,
    StageExecutionOptions* options) {
  if (options == nullptr || stage_tokens.empty()) {
    return false;
  }

  options->args.clear();
  options->args.reserve(stage_tokens.size());
  options->args.push_back(stage_tokens.front());
  // Global default JSON mode is intentionally disabled.
  // JSON is only enabled with explicit --json per command/stage.
  options->json_output = false;
  options->json_option_explicit = false;
  options->timeout_ms = state.command_timeout_ms;

  for (size_t i = 1U; i < stage_tokens.size(); ++i) {
    const std::string& arg = stage_tokens[i];
    if (arg == "--json") {
      options->json_output = true;
      options->json_option_explicit = true;
      continue;
    }
    if (arg == "--text") {
      options->json_output = false;
      options->json_option_explicit = true;
      continue;
    }
    if (arg == "--cmd-timeout-ms") {
      if ((i + 1U) >= stage_tokens.size()) {
        shell_error(state, "option '--cmd-timeout-ms' requires a value");
        return false;
      }
      uint32_t timeout_ms = 0U;
      if (!parse_uint32_value(stage_tokens[i + 1U], &timeout_ms)) {
        shell_error(state, "invalid value for '--cmd-timeout-ms'");
        return false;
      }
      options->timeout_ms = timeout_ms;
      ++i;
      continue;
    }
    options->args.push_back(arg);
  }

  if (options->args.empty()) {
    shell_error(state, "invalid null command");
    return false;
  }
  return true;
}

bool is_var_char(const char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
         (ch >= '0' && ch <= '9') || ch == '_';
}

std::string env_value(const ShellState& state, const std::string& name) {
  const auto it = state.env_vars.find(name);
  if (it != state.env_vars.end()) {
    return it->second;
  }
  if (name == "PWD") {
    return state.cwd;
  }
  if (name == "USER") {
    return state.root_session ? mros::ssh::root_username() : state.session_username;
  }
  if (name == "HOME") {
    return "/ESPUSER";
  }
  if (name == "SHELL") {
    return kShellName;
  }
  return {};
}

std::string execute_substitution(ShellState& state, const std::string& command_text) {
  std::string output;
  std::string* previous_capture = state.captured_output;
  state.captured_output = &output;
  execute_line_on_state(state, command_text.c_str(), false, state.active_transport);
  state.captured_output = previous_capture;
  return trim_trailing_newlines(output);
}

std::string expand_text(ShellState& state, const std::string& text) {
  std::string output;
  for (size_t i = 0U; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch != '$' || (i + 1U) >= text.size()) {
      output.push_back(ch);
      continue;
    }

    if (text[i + 1U] == '(') {
      int depth = 1;
      size_t j = i + 2U;
      for (; j < text.size(); ++j) {
        if (text[j] == '(') {
          ++depth;
        } else if (text[j] == ')') {
          --depth;
          if (depth == 0) {
            break;
          }
        }
      }
      if (j < text.size() && depth == 0) {
        output += execute_substitution(state, text.substr(i + 2U, j - (i + 2U)));
        i = j;
        continue;
      }
    }

    size_t j = i + 1U;
    if (text[j] == '{') {
      ++j;
      const size_t start = j;
      while (j < text.size() && text[j] != '}') {
        ++j;
      }
      if (j < text.size()) {
        output += env_value(state, text.substr(start, j - start));
        i = j;
        continue;
      }
    }

    const size_t start = j;
    while (j < text.size() && is_var_char(text[j])) {
      ++j;
    }
    if (j > start) {
      output += env_value(state, text.substr(start, j - start));
      i = j - 1U;
      continue;
    }
    output.push_back(ch);
  }
  return output;
}

bool contains_glob_pattern(const std::string& text) {
  return text.find('*') != std::string::npos || text.find('?') != std::string::npos;
}

bool expand_glob_token(ShellState& state, const std::string& token, std::vector<std::string>* out_tokens) {
  if (out_tokens == nullptr) {
    return false;
  }
  if (!contains_glob_pattern(token)) {
    out_tokens->push_back(token);
    return true;
  }

  const size_t slash_pos = token.find_last_of('/');
  const std::string raw_dir = slash_pos == std::string::npos ? state.cwd : token.substr(0U, slash_pos);
  const std::string pattern = slash_pos == std::string::npos ? token : token.substr(slash_pos + 1U);
  const std::string dir = raw_dir.empty() ? state.cwd : shell_normalize_path(state, raw_dir);

  std::vector<ShellFsEntry> entries;
  std::string error;
  if (!shell_read_directory(state, dir, false, false, &entries, &error)) {
    out_tokens->push_back(token);
    return true;
  }

  auto matches_pattern = [&](const std::string& name) {
    size_t p = 0U;
    size_t n = 0U;
    size_t star = std::string::npos;
    size_t star_match = 0U;
    while (n < name.size()) {
      if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
        ++p;
        ++n;
      } else if (p < pattern.size() && pattern[p] == '*') {
        star = p++;
        star_match = n;
      } else if (star != std::string::npos) {
        p = star + 1U;
        n = ++star_match;
      } else {
        return false;
      }
    }
    while (p < pattern.size() && pattern[p] == '*') {
      ++p;
    }
    return p == pattern.size();
  };

  bool matched = false;
  for (const ShellFsEntry& entry : entries) {
    if (!matches_pattern(entry.name)) {
      continue;
    }
    matched = true;
    const std::string prefix = slash_pos == std::string::npos ? "" : token.substr(0U, slash_pos + 1U);
    out_tokens->push_back(prefix + entry.name);
  }
  if (!matched) {
    out_tokens->push_back(token);
  }
  return true;
}

void expand_tokens(ShellState& state, std::vector<std::string>* tokens) {
  if (tokens == nullptr) {
    return;
  }
  std::vector<std::string> expanded;
  for (const std::string& token : *tokens) {
    if (is_operator_token(token)) {
      expanded.push_back(token);
      continue;
    }
    const std::string text = expand_text(state, token);
    expand_glob_token(state, text, &expanded);
  }
  *tokens = expanded;
}

bool write_redirection_target(
    ShellState& state,
    const std::string& target,
    const bool append,
    const std::string& content) {
  std::string normalized_target;
  if (!target.empty() && target.front() != '/' && state.cwd == "/" && shell_is_storage_mounted(state)) {
    const char* mount_point =
        state.config.storage_mount_point != nullptr ? state.config.storage_mount_point : "/littlefs";
    normalized_target = shell_normalize_path(state, std::string(mount_point) + "/" + target);
  } else {
    normalized_target = shell_normalize_path(state, target);
  }
  if (!shell_is_user_writable_path(state, normalized_target)) {
    shell_printf(state, "%s: redirection target must be inside /ESPUSER\n", kShellName);
    return false;
  }
  if (!shell_is_storage_mounted(state)) {
    shell_error(state, "LittleFS is not mounted");
    return false;
  }

  bool parent_is_dir = false;
  std::string error;
  if (!shell_path_exists(state, shell_parent_path(normalized_target), &parent_is_dir, nullptr, &error) || !parent_is_dir) {
    shell_printf(state, "%s: cannot open '%s': %s\n", kShellName, target.c_str(), error.c_str());
    return false;
  }

  FILE* file = std::fopen(normalized_target.c_str(), append ? "ab" : "wb");
  if (file == nullptr) {
    shell_printf(state, "%s: cannot open '%s' for writing\n", kShellName, target.c_str());
    return false;
  }

  const size_t bytes_to_write = content.size();
  const bool ok = bytes_to_write == 0U || std::fwrite(content.data(), 1U, bytes_to_write, file) == bytes_to_write;
  std::fclose(file);
  if (!ok) {
    shell_printf(state, "%s: failed writing '%s'\n", kShellName, target.c_str());
    return false;
  }
  return true;
}

bool load_redirect_input(
    ShellState& state,
    const ShellRedirect& redirect,
    std::string* input_buffer) {
  if (input_buffer == nullptr) {
    return false;
  }
  input_buffer->clear();
  if (!redirect.input_enabled) {
    return true;
  }
  if (redirect.heredoc) {
    *input_buffer = state.heredoc_buffer;
    return true;
  }

  const std::string normalized = shell_normalize_path(state, redirect.input_target);
  std::string actual_path;
  std::string error;
  if (!shell_openable_file_path(state, normalized, &actual_path, &error)) {
    shell_printf(state, "%s: %s\n", redirect.input_target.c_str(), error.c_str());
    return false;
  }

  FILE* file = std::fopen(actual_path.c_str(), "rb");
  if (file == nullptr) {
    shell_printf(state, "%s: unable to open file\n", redirect.input_target.c_str());
    return false;
  }

  char buffer[256] = {};
  size_t read_size = 0U;
  while ((read_size = std::fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
    input_buffer->append(buffer, read_size);
  }
  std::fclose(file);
  return true;
}

bool execute_pipeline(
    ShellState& state,
    const std::vector<std::vector<std::string>>& stages,
    const ShellRedirect& redirect) {
  std::string stage_input;
  std::string stage_output;
  if (!load_redirect_input(state, redirect, &stage_input)) {
    return false;
  }

  for (size_t i = 0U; i < stages.size(); ++i) {
    const std::vector<std::string>& stage_tokens = stages[i];
    if (stage_tokens.empty()) {
      shell_error(state, "invalid null command");
      return false;
    }

    StageExecutionOptions stage_options {};
    if (!parse_stage_execution_options(state, stage_tokens, &stage_options)) {
      return false;
    }

    const ShellCommandRegistration* command = find_command(stage_options.args.front());
    if (command == nullptr || command->handler == nullptr) {
      print_command_not_found(state, stage_options.args.front());
      return false;
    }
    if (!shell_capability_allowed(state, command->capabilities)) {
      shell_printf(state, "%s: 403 capability denied (%s requires %s)\n",
                   kShellName,
                   stage_options.args.front().c_str(),
                   capabilities_text(command->capabilities));
      audit_record("capability-denied", stage_options.args.front().c_str());
      return false;
    }

    stage_output.clear();
    std::string* previous_capture = state.captured_output;
    state.captured_output = &stage_output;
    const std::string* stdin_ptr =
        (i == 0U && stage_input.empty() && !redirect.input_enabled) ? nullptr : &stage_input;
    ShellContext ctx {state, stage_options.args, stdin_ptr, stage_options.json_output, state.active_transport};
    const uint32_t started_ms = mros::platform::mros_millis();
    int result = command->handler(ctx);
    const uint32_t elapsed_ms =
        mros::platform::mros_millis() - started_ms;
    state.captured_output = previous_capture;

    const bool timed_out = stage_options.timeout_ms > 0U && elapsed_ms > stage_options.timeout_ms;
    if (timed_out) {
      if (!stage_output.empty() && stage_output.back() != '\n') {
        stage_output.push_back('\n');
      }
      stage_output += std::string(kShellName) + ": command exceeded timeout (" + std::to_string(elapsed_ms) + "ms > " +
                    std::to_string(stage_options.timeout_ms) + "ms)\n";
      result = 124;
    }

    const bool apply_json = stage_options.json_output &&
                            (stage_options.json_option_explicit || (i + 1U) == stages.size());
    if (apply_json) {
      stage_output = build_json_response(stage_options.args, result, elapsed_ms, stage_output);
    }

    if (result != 0) {
      if (!stage_output.empty()) {
        if (state.captured_output != nullptr) {
          state.captured_output->append(stage_output);
          state.last_output_ended_with_newline =
              !stage_output.empty() && stage_output.back() == '\n';
        } else {
          shell_write(state, stage_output.c_str());
        }
      }
      return false;
    }

    stage_input = stage_output;
  }

  if (redirect.enabled) {
    return write_redirection_target(state, redirect.target, redirect.append, stage_input);
  }

  if (!stage_input.empty()) {
    if (state.captured_output != nullptr) {
      state.captured_output->append(stage_input);
      state.last_output_ended_with_newline =
          !stage_input.empty() && stage_input.back() == '\n';
    } else {
      shell_write(state, stage_input.c_str());
    }
  }
  return true;
}

void trim_statement_tokens(std::vector<std::string>* tokens) {
  if (tokens == nullptr) {
    return;
  }
  while (!tokens->empty() && tokens->front() == ";") {
    tokens->erase(tokens->begin());
  }
  while (!tokens->empty() && tokens->back() == ";") {
    tokens->pop_back();
  }
}

bool parse_if_statement(
    ShellState& state,
    const std::vector<std::string>& tokens,
    const size_t start,
    size_t* consumed,
    std::vector<std::string>* condition_tokens,
    std::vector<std::string>* then_tokens,
    std::vector<std::string>* else_tokens) {
  if (consumed == nullptr || condition_tokens == nullptr || then_tokens == nullptr ||
      else_tokens == nullptr || start >= tokens.size() || tokens[start] != "if") {
    return false;
  }

  size_t then_pos = std::string::npos;
  size_t else_pos = std::string::npos;
  size_t fi_pos = std::string::npos;
  int if_depth = 0;
  int for_depth = 0;
  for (size_t i = start + 1U; i < tokens.size(); ++i) {
    const std::string& token = tokens[i];
    if (token == "if") {
      ++if_depth;
      continue;
    }
    if (token == "for") {
      ++for_depth;
      continue;
    }
    if (token == "done") {
      if (for_depth > 0) {
        --for_depth;
      }
      continue;
    }
    if (token == "fi") {
      if (if_depth > 0) {
        --if_depth;
        continue;
      }
      if (for_depth == 0) {
        fi_pos = i;
        break;
      }
    }
    if (if_depth == 0 && for_depth == 0) {
      if (token == "then" && then_pos == std::string::npos) {
        then_pos = i;
        continue;
      }
      if (token == "else" && then_pos != std::string::npos && else_pos == std::string::npos) {
        else_pos = i;
      }
    }
  }

  if (then_pos == std::string::npos || fi_pos == std::string::npos || then_pos <= (start + 1U)) {
    shell_error(state, "invalid if syntax");
    return false;
  }

  *condition_tokens = std::vector<std::string>(
      tokens.begin() + static_cast<std::ptrdiff_t>(start + 1U),
      tokens.begin() + static_cast<std::ptrdiff_t>(then_pos));
  if (else_pos == std::string::npos) {
    *then_tokens = std::vector<std::string>(
        tokens.begin() + static_cast<std::ptrdiff_t>(then_pos + 1U),
        tokens.begin() + static_cast<std::ptrdiff_t>(fi_pos));
    else_tokens->clear();
  } else {
    *then_tokens = std::vector<std::string>(
        tokens.begin() + static_cast<std::ptrdiff_t>(then_pos + 1U),
        tokens.begin() + static_cast<std::ptrdiff_t>(else_pos));
    *else_tokens = std::vector<std::string>(
        tokens.begin() + static_cast<std::ptrdiff_t>(else_pos + 1U),
        tokens.begin() + static_cast<std::ptrdiff_t>(fi_pos));
  }

  trim_statement_tokens(condition_tokens);
  trim_statement_tokens(then_tokens);
  trim_statement_tokens(else_tokens);
  *consumed = (fi_pos - start) + 1U;
  return true;
}

bool parse_for_statement(
    ShellState& state,
    const std::vector<std::string>& tokens,
    const size_t start,
    size_t* consumed,
    std::string* variable_name,
    std::vector<std::string>* item_tokens,
    std::vector<std::string>* body_tokens) {
  if (consumed == nullptr || variable_name == nullptr || item_tokens == nullptr ||
      body_tokens == nullptr || (start + 3U) >= tokens.size() || tokens[start] != "for") {
    return false;
  }
  if (tokens[start + 2U] != "in") {
    shell_error(state, "for syntax must use: for VAR in ... ; do ... ; done");
    return false;
  }

  size_t do_pos = std::string::npos;
  size_t done_pos = std::string::npos;
  int if_depth = 0;
  int for_depth = 0;
  for (size_t i = start + 3U; i < tokens.size(); ++i) {
    const std::string& token = tokens[i];
    if (token == "if") {
      ++if_depth;
      continue;
    }
    if (token == "fi") {
      if (if_depth > 0) {
        --if_depth;
      }
      continue;
    }
    if (token == "for") {
      ++for_depth;
      continue;
    }
    if (token == "done") {
      if (if_depth == 0 && for_depth == 0) {
        done_pos = i;
        break;
      }
      if (for_depth > 0) {
        --for_depth;
      }
      continue;
    }
    if (token == "do" && if_depth == 0 && for_depth == 0 && do_pos == std::string::npos) {
      do_pos = i;
    }
  }

  if (do_pos == std::string::npos || done_pos == std::string::npos || do_pos <= (start + 3U)) {
    shell_error(state, "invalid for syntax");
    return false;
  }

  *variable_name = tokens[start + 1U];
  *item_tokens = std::vector<std::string>(
      tokens.begin() + static_cast<std::ptrdiff_t>(start + 3U),
      tokens.begin() + static_cast<std::ptrdiff_t>(do_pos));
  *body_tokens = std::vector<std::string>(
      tokens.begin() + static_cast<std::ptrdiff_t>(do_pos + 1U),
      tokens.begin() + static_cast<std::ptrdiff_t>(done_pos));
  trim_statement_tokens(item_tokens);
  trim_statement_tokens(body_tokens);
  *consumed = (done_pos - start) + 1U;
  return true;
}

bool execute_token_sequence(ShellState& state, const std::vector<std::string>& tokens);

bool execute_if_tokens(
    ShellState& state,
    const std::vector<std::string>& condition_tokens,
    const std::vector<std::string>& then_tokens,
    const std::vector<std::string>& else_tokens) {
  const bool condition_ok = execute_token_sequence(state, condition_tokens);
  if (condition_ok) {
    return then_tokens.empty() ? true : execute_token_sequence(state, then_tokens);
  }
  return else_tokens.empty() ? true : execute_token_sequence(state, else_tokens);
}

bool execute_for_tokens(
    ShellState& state,
    const std::string& variable_name,
    const std::vector<std::string>& item_tokens,
    const std::vector<std::string>& body_tokens) {
  if (variable_name.empty()) {
    shell_error(state, "for variable name is missing");
    return false;
  }

  std::vector<std::string> items;
  for (const std::string& token : item_tokens) {
    if (token == ";") {
      continue;
    }
    const std::string expanded = expand_text(state, token);
    expand_glob_token(state, expanded, &items);
  }

  const auto previous_it = state.env_vars.find(variable_name);
  const bool had_previous = previous_it != state.env_vars.end();
  const std::string previous_value = had_previous ? previous_it->second : std::string();
  bool ok = true;
  for (const std::string& item : items) {
    state.env_vars[variable_name] = item;
    ok = body_tokens.empty() ? true : execute_token_sequence(state, body_tokens);
    if (!ok) {
      break;
    }
  }
  if (had_previous) {
    state.env_vars[variable_name] = previous_value;
  } else {
    state.env_vars.erase(variable_name);
  }
  return ok;
}

bool execute_simple_tokens(ShellState& state, std::vector<std::string> tokens) {
  if (tokens.empty()) {
    return true;
  }
  expand_alias_tokens(&tokens);
  expand_tokens(state, &tokens);

  std::vector<std::vector<std::string>> stages;
  ShellRedirect redirect {};
  if (!parse_pipeline_tokens(state, tokens, &stages, &redirect)) {
    return false;
  }

  if (redirect.input_enabled && redirect.heredoc) {
    std::vector<std::string> rebuilt;
    bool skip_next = false;
    for (size_t i = 0U; i < tokens.size(); ++i) {
      if (skip_next) {
        skip_next = false;
        continue;
      }
      if (tokens[i] == "<<") {
        skip_next = true;
        continue;
      }
      rebuilt.push_back(tokens[i]);
    }

    state.heredoc_active = true;
    state.heredoc_delimiter = redirect.input_target;
    state.heredoc_buffer.clear();
    state.heredoc_command.clear();
    for (size_t i = 0U; i < rebuilt.size(); ++i) {
      if (i > 0U) {
        state.heredoc_command.push_back(' ');
      }
      state.heredoc_command += rebuilt[i];
    }
    return true;
  }

  return execute_pipeline(state, stages, redirect);
}

bool execute_token_sequence(ShellState& state, const std::vector<std::string>& tokens) {
  bool last_ok = true;
  std::string previous_connector;
  size_t cursor = 0U;

  while (cursor < tokens.size()) {
    if (tokens[cursor] == ";") {
      previous_connector = ";";
      ++cursor;
      continue;
    }

    const bool should_execute =
        previous_connector.empty() || previous_connector == ";" ||
        (previous_connector == "&&" && last_ok) ||
        (previous_connector == "||" && !last_ok);

    size_t consumed = 0U;
    if (tokens[cursor] == "if") {
      std::vector<std::string> condition_tokens;
      std::vector<std::string> then_tokens;
      std::vector<std::string> else_tokens;
      if (!parse_if_statement(
              state,
              tokens,
              cursor,
              &consumed,
              &condition_tokens,
              &then_tokens,
              &else_tokens)) {
        return false;
      }
      if (should_execute) {
        last_ok = execute_if_tokens(state, condition_tokens, then_tokens, else_tokens);
      }
    } else if (tokens[cursor] == "for") {
      std::string variable_name;
      std::vector<std::string> item_tokens;
      std::vector<std::string> body_tokens;
      if (!parse_for_statement(
              state,
              tokens,
              cursor,
              &consumed,
              &variable_name,
              &item_tokens,
              &body_tokens)) {
        return false;
      }
      if (should_execute) {
        last_ok = execute_for_tokens(state, variable_name, item_tokens, body_tokens);
      }
    } else {
      size_t next = cursor;
      while (next < tokens.size() && tokens[next] != ";" && tokens[next] != "&&" && tokens[next] != "||") {
        ++next;
      }
      consumed = next - cursor;
      std::vector<std::string> segment(
          tokens.begin() + static_cast<std::ptrdiff_t>(cursor),
          tokens.begin() + static_cast<std::ptrdiff_t>(next));
      if (should_execute) {
        last_ok = execute_simple_tokens(state, segment);
      }
    }

    cursor += consumed;
    if (cursor == 0U) {
      shell_error(state, "parser made no progress");
      return false;
    }

    if (cursor >= tokens.size()) {
      break;
    }
    if (tokens[cursor] == ";" || tokens[cursor] == "&&" || tokens[cursor] == "||") {
      previous_connector = tokens[cursor];
      ++cursor;
      continue;
    }
    previous_connector = ";";
  }

  return last_ok;
}

const ShellCommandRegistration* find_command(const std::string& command_name) {
  const auto& commands = shell_commands();
  for (const ShellCommandRegistration& command : commands) {
    if (command.name != nullptr && command_name == command.name) {
      return &command;
    }
  }
  return nullptr;
}

std::string lower_ascii_copy(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

uint16_t edit_distance_limited(const std::string& lhs, const std::string& rhs, const uint16_t limit) {
  const size_t lhs_size = lhs.size();
  const size_t rhs_size = rhs.size();
  if (lhs_size == 0U) {
    return static_cast<uint16_t>(std::min<size_t>(rhs_size, limit + 1U));
  }
  if (rhs_size == 0U) {
    return static_cast<uint16_t>(std::min<size_t>(lhs_size, limit + 1U));
  }
  const size_t diff = lhs_size > rhs_size ? lhs_size - rhs_size : rhs_size - lhs_size;
  if (diff > limit) {
    return static_cast<uint16_t>(limit + 1U);
  }

  std::vector<uint16_t> prev(rhs_size + 1U);
  std::vector<uint16_t> curr(rhs_size + 1U);
  for (size_t j = 0U; j <= rhs_size; ++j) {
    prev[j] = static_cast<uint16_t>(j);
  }
  for (size_t i = 1U; i <= lhs_size; ++i) {
    curr[0] = static_cast<uint16_t>(i);
    uint16_t row_best = curr[0];
    for (size_t j = 1U; j <= rhs_size; ++j) {
      const uint16_t cost = lhs[i - 1U] == rhs[j - 1U] ? 0U : 1U;
      curr[j] = static_cast<uint16_t>(
          std::min<uint16_t>(
              std::min<uint16_t>(static_cast<uint16_t>(prev[j] + 1U),
                                 static_cast<uint16_t>(curr[j - 1U] + 1U)),
              static_cast<uint16_t>(prev[j - 1U] + cost)));
      row_best = std::min<uint16_t>(row_best, curr[j]);
    }
    if (row_best > limit) {
      return static_cast<uint16_t>(limit + 1U);
    }
    prev.swap(curr);
  }
  return prev[rhs_size];
}

bool is_subsequence_match(const std::string& needle, const std::string& haystack) {
  if (needle.empty()) {
    return true;
  }
  size_t cursor = 0U;
  for (const char ch : haystack) {
    if (cursor < needle.size() && needle[cursor] == ch) {
      ++cursor;
      if (cursor == needle.size()) {
        return true;
      }
    }
  }
  return false;
}

struct RankedCommandSuggestion {
  std::string name;
  uint16_t score = 0U;
  uint16_t distance = 0U;
};

std::vector<RankedCommandSuggestion> ranked_command_suggestions(const std::string& command_name) {
  if (!g_completion_cache_valid) {
    rebuild_completion_cache();
  }
  std::vector<RankedCommandSuggestion> ranked;
  const std::string needle = lower_ascii_copy(command_name);
  if (needle.size() < 2U) {
    return ranked;
  }
  const uint16_t threshold =
      static_cast<uint16_t>(std::max<size_t>(2U, std::min<size_t>(4U, (needle.size() + 2U) / 3U)));

  for (const std::string& candidate : g_completion_cache.all_names) {
    const std::string lower = lower_ascii_copy(candidate);
    uint16_t distance = static_cast<uint16_t>(threshold + 1U);
    uint16_t score = 1000U;
    if (starts_with_text(lower, needle)) {
      distance = 0U;
      score = 0U;
    } else if (lower.find(needle) != std::string::npos) {
      distance = 1U;
      score = 20U;
    } else {
      const uint16_t relaxed =
          (starts_with_text(needle, "esp") && starts_with_text(lower, "esp"))
              ? static_cast<uint16_t>(threshold + 2U)
              : threshold;
      distance = edit_distance_limited(needle, lower, relaxed);
      if (distance <= relaxed) {
        score = static_cast<uint16_t>(80U + (distance * 12U));
      } else if (starts_with_text(needle, "esp") && starts_with_text(lower, "esp") &&
                 is_subsequence_match(needle, lower)) {
        distance = static_cast<uint16_t>(threshold + 1U);
        score = 140U;
      }
    }
    if (score < 1000U) {
      ranked.push_back({candidate, score, distance});
    }
  }

  std::sort(ranked.begin(), ranked.end(), [](const RankedCommandSuggestion& lhs,
                                             const RankedCommandSuggestion& rhs) {
    if (lhs.score != rhs.score) return lhs.score < rhs.score;
    if (lhs.distance != rhs.distance) return lhs.distance < rhs.distance;
    if (lhs.name.size() != rhs.name.size()) return lhs.name.size() < rhs.name.size();
    return lhs.name < rhs.name;
  });
  if (ranked.size() > 3U) {
    ranked.resize(3U);
  }
  return ranked;
}

void print_command_not_found(ShellState& state, const std::string& command_name) {
  shell_printf(state, "%s: command not found\n", command_name.c_str());
  const std::vector<RankedCommandSuggestion> suggestions = ranked_command_suggestions(command_name);
  if (!suggestions.empty()) {
    shell_write(state, "did you mean: ");
    for (size_t i = 0U; i < suggestions.size(); ++i) {
      if (i > 0U) {
        shell_write(state, ", ");
      }
      shell_write(state, suggestions[i].name.c_str());
    }
    shell_write(state, "\n");
  }
  const std::string search =
      command_name.size() > 4U ? command_name.substr(0U, 4U) : command_name;
  shell_printf(state, "try: help --search %s\n", search.c_str());
}

void flush_serial_buffer(const SerialInputSource source) {
  size_t& input_length = serial_input_length_for_source(source);
  char* input_buffer = serial_input_buffer_for_source(source);
  if (input_length == 0U) {
    return;
  }

  input_buffer[input_length] = '\0';
  const std::string line = trim_copy(input_buffer);
  const std::string command_line = lower_ascii_copy(line);

  if (!mros_console_is_serial_mirror_suppressed() &&
      g_serial_auth_stage == SerialAuthStage::AwaitUser) {
    clear_serial_input_buffer(source);
    if (command_line == "exit" || command_line == "logout") {
      reset_serial_auth_prompt();
      serial_console_write_line("[mshell] serial auth cancelled.");
      return;
    }
    if (line.empty() || line.size() > 32U) {
      serial_console_write_line("[mshell] username must be 1-32 chars.");
      serial_console_write_text("username: ");
      return;
    }
    g_serial_auth_username = line;
    g_serial_auth_stage = SerialAuthStage::AwaitPassword;
    serial_console_write_text("password: ");
    return;
  }

  if (!mros_console_is_serial_mirror_suppressed() &&
      g_serial_auth_stage == SerialAuthStage::AwaitPassword) {
    clear_serial_input_buffer(source);
    const String username(g_serial_auth_username.c_str());
    mros::ssh::UserAccount user;
    const bool ok = username != mros::ssh::root_username() &&
                    mros::ssh::get_user(username, &user) &&
                    mros::ssh::verify_password(username, String(line.c_str()));
    if (!ok) {
      reset_serial_auth_prompt();
      serial_console_write_line();
      serial_console_write_line("[mshell] serial authentication failed.");
      serial_console_write_line("[mshell] type mshell to retry, or continue watching runtime logs.");
      return;
    }
    apply_user_identity(g_shell_state, user);
    enter_serial_shell_mode();
    return;
  }

  if (!mros_console_is_serial_mirror_suppressed() && command_line == "mshell") {
    if (serial_auth_required()) {
      clear_serial_input_buffer(source);
      g_serial_auth_stage = SerialAuthStage::AwaitUser;
      g_serial_auth_username.clear();
      serial_console_write_line();
      serial_console_write_line("[mshell] serial authentication required. type exit to cancel.");
      serial_console_write_text("username: ");
      return;
    }
    enter_serial_shell_mode();
    return;
  }
  if (!mros_console_is_serial_mirror_suppressed()) {
    clear_serial_input_buffer(source);
    if (!line.empty()) {
      serial_console_write_line();
      serial_console_write_line("[mshell] serial commands are locked. type mshell to authenticate.");
    }
    return;
  }
  if (mros_console_is_serial_mirror_suppressed() &&
      (command_line == "exit" || command_line == "logout")) {
    leave_serial_shell_mode();
    return;
  }
  execute_line(input_buffer, true, ShellTransport::SerialConsole);
  clear_serial_input_buffer(source);
}

bool build_completion_result(
    const ShellState& state,
    const std::string& input,
    std::string* completed_line,
    std::vector<std::string>* suggestions) {
  if (completed_line == nullptr || suggestions == nullptr) {
    return false;
  }

  *completed_line = input;
  suggestions->clear();

  const bool trailing_space = !input.empty() && is_space_char(input.back());
  std::vector<std::string> tokens = tokenize_command_line(input);
  size_t current_index = tokens.empty() ? 0U : tokens.size() - 1U;
  std::string current_prefix;

  if (tokens.empty()) {
    current_index = 0U;
  } else if (trailing_space || is_operator_token(tokens.back())) {
    current_index = tokens.size();
  } else {
    current_prefix = tokens.back();
  }

  size_t stage_start = 0U;
  for (size_t i = 0U; i < current_index && i < tokens.size(); ++i) {
    if (is_operator_token(tokens[i])) {
      stage_start = i + 1U;
    }
  }
  const size_t argument_index = current_index >= stage_start ? (current_index - stage_start) : 0U;
  const std::string command = (stage_start < tokens.size() && !is_operator_token(tokens[stage_start])) ? tokens[stage_start] : "";

  std::vector<std::string> matches;
  const ShellCompletionCache& cache = shell_completion_cache();
  if (argument_index == 0U) {
    append_cached_name_matches(cache.all_names, current_prefix, &matches);
  } else if (command == "help" && argument_index == 1U) {
    append_cached_name_matches(cache.all_names, current_prefix, &matches);
  } else if (command == "mshell" && argument_index == 1U) {
    const std::vector<std::string> names = {"alias", "connect", "disconnect", "status", "features", "history", "jobs"};
    for (const std::string& name : names) {
      if (starts_with_text(name, current_prefix)) {
        matches.push_back(name);
      }
    }
  } else if (command == "mshell" && argument_index == 2U && stage_start + 1U < tokens.size() &&
             tokens[stage_start + 1U] == "alias") {
    const std::vector<std::string> names = {"list", "add", "remove", "save", "reload"};
    for (const std::string& name : names) {
      if (starts_with_text(name, current_prefix)) {
        matches.push_back(name);
      }
    }
  } else if (command == "mshell" && argument_index == 2U && stage_start + 1U < tokens.size() &&
             (tokens[stage_start + 1U] == "connect" || tokens[stage_start + 1U] == "disconnect" ||
              tokens[stage_start + 1U] == "status")) {
    const std::vector<std::string> names = {"s3", "t41", "c3", "all"};
    for (const std::string& name : names) {
      if (starts_with_text(name, current_prefix)) {
        matches.push_back(name);
      }
    }
  } else if ((command == "mros-deuscara-update" || command == "mros7dofs3-update") &&
             argument_index == 1U) {
    const std::vector<std::string> names = {"check", "status", "plan"};
    for (const std::string& name : names) {
      if (starts_with_text(name, current_prefix)) {
        matches.push_back(name);
      }
    }
  } else if (command == "set" && argument_index == 1U) {
    for (const char* name : shell_setting_names()) {
      if (name != nullptr && starts_with_text(name, current_prefix)) {
        matches.emplace_back(name);
      }
    }
  } else if (command == "set" && argument_index == 2U) {
    std::vector<std::string> value_names = {"off", "on", "0", "1"};
    if (stage_start < tokens.size() && (stage_start + 1U) < tokens.size()) {
      const std::string& setting_name = tokens[stage_start + 1U];
      if (setting_name == "output-mode" || setting_name == "output-format") {
        value_names = {"text"};
      }
    }
    for (const std::string& value_name : value_names) {
      if (starts_with_text(value_name, current_prefix)) {
        matches.push_back(value_name);
      }
    }
  } else if (command == "update-c6" && argument_index == 1U) {
    if (starts_with_text("default", current_prefix)) {
      matches.emplace_back("default");
    }
    std::vector<std::string> path_matches;
    if (current_prefix.empty() || command_might_take_path(command, argument_index, current_prefix)) {
      build_path_matches(state, current_prefix, &path_matches);
      matches.insert(matches.end(), path_matches.begin(), path_matches.end());
    }
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
  } else if (command == "robot" && argument_index == 1U) {
    const std::vector<std::string> names = {"pos", "mov", "gripper", "grippper", "turret", "speed", "emg"};
    for (const std::string& name : names) {
      if (starts_with_text(name, current_prefix)) {
        matches.push_back(name);
      }
    }
  } else if (command == "robot" && argument_index == 2U) {
    if ((stage_start + 1U) < tokens.size()) {
      const std::string& sub = tokens[stage_start + 1U];
      if (sub == "pos") {
        if (starts_with_text("default", current_prefix)) {
          matches.emplace_back("default");
        }
      } else if (sub == "emg") {
        for (const char* value_name : {"off", "on"}) {
          if (starts_with_text(value_name, current_prefix)) {
            matches.push_back(value_name);
          }
        }
      } else if (sub == "turret") {
        if (starts_with_text("pos", current_prefix)) {
          matches.emplace_back("pos");
        }
      }
    }
  } else if (command_might_take_path(command, argument_index, current_prefix)) {
    build_path_matches(state, current_prefix, &matches);
  }

  if (matches.empty()) {
    return false;
  }

  *suggestions = matches;
  std::string replacement = matches.size() == 1U ? matches.front() : shared_prefix(matches);
  if (replacement.empty() || replacement == current_prefix) {
    return false;
  }

  if (current_index < tokens.size()) {
    tokens[current_index] = replacement;
  } else {
    tokens.push_back(replacement);
  }

  completed_line->clear();
  for (size_t i = 0U; i < tokens.size(); ++i) {
    if (i > 0U) {
      if (is_operator_token(tokens[i]) || is_operator_token(tokens[i - 1U])) {
        completed_line->append(" ");
      } else {
        completed_line->push_back(' ');
      }
    }
    completed_line->append(tokens[i]);
  }
  return true;
}

}  // namespace

const std::vector<ShellAliasRecord>& shell_aliases() {
  load_aliases_if_needed();
  return g_aliases;
}

const ShellCompletionCache& shell_completion_cache() {
  if (!g_completion_cache_valid) {
    rebuild_completion_cache();
  }
  return g_completion_cache;
}

void shell_invalidate_completion_cache() {
  g_completion_cache_valid = false;
}

bool shell_alias_save(std::string* error) {
  if (!shell_is_storage_mounted(g_shell_state)) {
    if (error != nullptr) {
      *error = "LittleFS is not mounted";
    }
    return false;
  }

  ensure_alias_dir();
  FILE* file = std::fopen(alias_file_path().c_str(), "wb");
  if (file == nullptr) {
    if (error != nullptr) {
      *error = "cannot open alias file for writing";
    }
    return false;
  }

  bool ok = true;
  for (const ShellAliasRecord& alias : g_aliases) {
    const std::string line = alias.name + "=" + alias.value + "\n";
    if (std::fwrite(line.data(), 1U, line.size(), file) != line.size()) {
      ok = false;
      break;
    }
  }
  std::fclose(file);

  if (!ok && error != nullptr) {
    *error = "cannot write alias file";
  }
  return ok;
}

bool shell_alias_reload(std::string* error) {
  if (!shell_is_storage_mounted(g_shell_state)) {
    if (error != nullptr) {
      *error = "LittleFS is not mounted";
    }
    return false;
  }
  g_aliases_loaded = false;
  load_aliases_if_needed();
  shell_invalidate_completion_cache();
  return true;
}

bool shell_alias_add(const std::string& name, const std::string& value, std::string* error) {
  load_aliases_if_needed();
  const std::string clean_name = trim_copy(name.c_str());
  const std::string clean_value = trim_copy(value.c_str());
  if (!is_valid_alias_name(clean_name)) {
    if (error != nullptr) {
      *error = "alias name may contain only letters, numbers, dot, dash or underscore";
    }
    return false;
  }
  if (clean_value.empty()) {
    if (error != nullptr) {
      *error = "alias command cannot be empty";
    }
    return false;
  }

  const std::vector<ShellAliasRecord> backup = g_aliases;
  bool replaced = false;
  for (ShellAliasRecord& alias : g_aliases) {
    if (alias.name == clean_name) {
      alias.value = clean_value;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    g_aliases.push_back({clean_name, clean_value});
    std::sort(g_aliases.begin(), g_aliases.end(), [](const ShellAliasRecord& a, const ShellAliasRecord& b) {
      return a.name < b.name;
    });
  }

  if (!shell_alias_save(error)) {
    g_aliases = backup;
    return false;
  }
  shell_invalidate_completion_cache();
  return true;
}

bool shell_alias_remove(const std::string& name, std::string* error) {
  load_aliases_if_needed();
  const std::string clean_name = trim_copy(name.c_str());
  const auto old_size = g_aliases.size();
  const std::vector<ShellAliasRecord> backup = g_aliases;
  g_aliases.erase(
      std::remove_if(g_aliases.begin(), g_aliases.end(), [&](const ShellAliasRecord& alias) {
        return alias.name == clean_name;
      }),
      g_aliases.end());

  if (g_aliases.size() == old_size) {
    if (error != nullptr) {
      *error = "alias not found";
    }
    return false;
  }
  if (!shell_alias_save(error)) {
    g_aliases = backup;
    return false;
  }
  shell_invalidate_completion_cache();
  return true;
}

void init(const ShellConfig& config) {
  g_shell_state.config = config;
  if (g_shell_state.config.hostname == nullptr || g_shell_state.config.hostname[0] == '\0') {
    g_shell_state.config.hostname = "mros";
  }
  if (g_shell_state.config.storage_mount_point == nullptr ||
      g_shell_state.config.storage_mount_point[0] == '\0') {
    g_shell_state.config.storage_mount_point = "/littlefs";
  }
  g_shell_state.cwd = "/";
  g_shell_state.previous_cwd = "/";
  g_shell_state.json_output_default = false;
  g_shell_state.command_timeout_ms = 0U;
  clear_all_serial_input_buffers();
  g_shell_state.serial_input_length = 0U;
  g_shell_state.serial_input_buffer[0] = '\0';
  g_shell_state.last_serial_activity_ms = 0U;
  g_shell_state.last_output_ended_with_newline = true;
  g_shell_state.root_session = false;
  initialize_session_identity(g_shell_state);
  g_shell_state.history.clear();
  g_shell_state.env_vars.clear();
  g_shell_state.active_transport = ShellTransport::SerialConsole;
  g_shell_state.remote_target = 0U;
  apply_transport_terminal_size(g_shell_state, ShellTransport::SerialConsole);
  g_shell_state.initialized = true;
}

bool execute_line_on_state(
    ShellState& state,
    const char* line,
    const bool echo_command,
    const ShellTransport transport) {
  if (!state.initialized) {
    if (&state == &g_shell_state) {
      init({});
    } else {
      state.config = g_shell_state.config;
      state.cwd = "/";
      state.previous_cwd = "/";
      state.last_output_ended_with_newline = true;
      initialize_session_identity(state);
      state.history.clear();
      state.env_vars.clear();
      state.active_transport = transport == ShellTransport::Current ? ShellTransport::Ssh : transport;
      state.remote_target = 0U;
      apply_transport_terminal_size(state, state.active_transport);
      state.initialized = true;
    }
  }

  if (state.heredoc_active) {
    const std::string incoming = line != nullptr ? line : "";
    const std::string trimmed_incoming = trim_copy(incoming.c_str());
    if (trimmed_incoming == state.heredoc_delimiter) {
      const std::string command = state.heredoc_command;
      const std::string buffer = state.heredoc_buffer;
      state.heredoc_active = false;
      state.heredoc_delimiter.clear();
      state.heredoc_command.clear();
      state.heredoc_buffer = buffer;
      const bool ok = execute_line_on_state(state, command.c_str(), false, transport);
      state.heredoc_buffer.clear();
      return ok;
    }
    state.heredoc_buffer += incoming;
    state.heredoc_buffer.push_back('\n');
    return true;
  }

  const ShellTransport previous_transport = state.active_transport;
  const uint16_t previous_columns = state.terminal_columns;
  const uint16_t previous_rows = state.terminal_rows;
  if (transport != ShellTransport::Current) {
    state.active_transport = transport;
    apply_transport_terminal_size(state, transport);
  }

  const std::string trimmed = trim_copy(line);
  if (trimmed.empty()) {
    if (transport != ShellTransport::Current) {
      state.active_transport = previous_transport;
      set_state_terminal_size(state, previous_columns, previous_rows);
    }
    return true;
  }

  if (&state == &g_shell_state &&
      transport == ShellTransport::SerialConsole &&
      !mros_console_is_serial_mirror_suppressed()) {
    shell_write_line(state, "[mshell] serial commands are locked. type mshell to authenticate.");
    if (transport != ShellTransport::Current) {
      state.active_transport = previous_transport;
      set_state_terminal_size(state, previous_columns, previous_rows);
    }
    return false;
  }

  if (state.history.empty() || state.history.back() != trimmed) {
    state.history.push_back(trimmed);
    if (state.history.size() > 64U) {
      state.history.erase(state.history.begin());
    }
  }

  if (echo_command) {
    const std::string prompt = shell_prompt(state);
    shell_printf(state, "%s%s\n", prompt.c_str(), trimmed.c_str());
  }

  std::vector<std::string> tokens = tokenize_command_line(trimmed);
  if (tokens.empty()) {
    if (transport != ShellTransport::Current) {
      state.active_transport = previous_transport;
      set_state_terminal_size(state, previous_columns, previous_rows);
    }
    return true;
  }
  if (remote::active_target(state) != remote::Target::None && !tokens.empty() && tokens.front() != "mshell") {
    if (tokens.front() == "exit") {
      std::string message;
      const remote::Target old_target = remote::active_target(state);
      (void)remote::disconnect_session(state, old_target, &message);
      shell_write_line(state, message.empty() ? "mshell remote: returned to local S3 shell" : message.c_str());
      if (transport != ShellTransport::Current) {
        state.active_transport = previous_transport;
        set_state_terminal_size(state, previous_columns, previous_rows);
      }
      return true;
    }
    if (tokens.front() == "logout") {
      std::string message;
      (void)remote::disconnect_session(state, remote::active_target(state), &message);
      state.close_requested = true;
      if (transport != ShellTransport::Current) {
        state.active_transport = previous_transport;
        set_state_terminal_size(state, previous_columns, previous_rows);
      }
      return true;
    }
    std::string remote_output;
    std::string remote_error;
    const bool ok = remote::execute_active_line(state, trimmed, &remote_output, &remote_error);
    if (!remote_output.empty()) {
      shell_write(state, remote_output.c_str());
    } else if (!ok && !remote_error.empty()) {
      shell_write_line(state, remote_error.c_str());
    }
    if (echo_command) {
      if (!state.last_output_ended_with_newline) {
        shell_write(state, "\n");
      }
      const std::string prompt = shell_prompt(state);
      shell_write(state, prompt.c_str());
    }
    if (transport != ShellTransport::Current) {
      state.active_transport = previous_transport;
      set_state_terminal_size(state, previous_columns, previous_rows);
    }
    return ok;
  }
  const bool ok = execute_token_sequence(state, tokens);
  if (!ok) {
    if (echo_command) {
      if (!state.last_output_ended_with_newline) {
        shell_write(state, "\n");
      }
      const std::string prompt = shell_prompt(state);
      shell_write(state, prompt.c_str());
    }
    if (transport != ShellTransport::Current) {
      state.active_transport = previous_transport;
      set_state_terminal_size(state, previous_columns, previous_rows);
    }
    return false;
  }
  if (echo_command) {
    if (!state.last_output_ended_with_newline) {
      shell_write(state, "\n");
    }
    const std::string prompt = shell_prompt(state);
    shell_write(state, prompt.c_str());
  }
  if (transport != ShellTransport::Current) {
    state.active_transport = previous_transport;
    set_state_terminal_size(state, previous_columns, previous_rows);
  }
  return ok;
}

bool execute_line(const char* line, const bool echo_command, const ShellTransport transport) {
  return execute_line_on_state(g_shell_state, line, echo_command, transport);
}

bool execute_line_capture(
    const char* line,
    std::string* output,
    const bool echo_command,
    const ShellTransport transport) {
  if (output == nullptr) {
    return execute_line(line, echo_command, transport);
  }
  if (!g_shell_state.initialized) {
    init({});
  }

  std::string* previous_capture = g_shell_state.captured_output;
  ShellStreamCallback previous_stream_callback = g_shell_state.captured_stream_callback;
  void* previous_stream_user_data = g_shell_state.captured_stream_user_data;
  output->clear();
  g_shell_state.captured_output = output;
  g_shell_state.captured_stream_callback = nullptr;
  g_shell_state.captured_stream_user_data = nullptr;
  const bool ok = execute_line(line, echo_command, transport);
  g_shell_state.captured_output = previous_capture;
  g_shell_state.captured_stream_callback = previous_stream_callback;
  g_shell_state.captured_stream_user_data = previous_stream_user_data;
  return ok;
}

bool execute_line_capture_stream(
    const char* line,
    std::string* output,
    ShellStreamCallback stream_callback,
    void* stream_user_data,
    const bool echo_command,
    const ShellTransport transport) {
  if (output == nullptr) {
    return execute_line(line, echo_command, transport);
  }
  if (!g_shell_state.initialized) {
    init({});
  }

  std::string* previous_capture = g_shell_state.captured_output;
  ShellStreamCallback previous_stream_callback = g_shell_state.captured_stream_callback;
  void* previous_stream_user_data = g_shell_state.captured_stream_user_data;
  output->clear();
  g_shell_state.captured_output = output;
  g_shell_state.captured_stream_callback = stream_callback;
  g_shell_state.captured_stream_user_data = stream_user_data;
  const bool ok = execute_line(line, echo_command, transport);
  g_shell_state.captured_output = previous_capture;
  g_shell_state.captured_stream_callback = previous_stream_callback;
  g_shell_state.captured_stream_user_data = previous_stream_user_data;
  return ok;
}

bool execute_line_capture_as_user(
    const char* line,
    const char* username,
    std::string* output,
    const bool echo_command,
    const ShellTransport transport) {
  ShellSession* session = create_session(transport);
  if (session == nullptr) {
    if (output != nullptr) {
      *output = "mros-shell: session allocation failed\n";
    }
    return false;
  }
  if (!session_apply_user(session, username)) {
    if (output != nullptr) {
      *output = "mros-shell: authenticated user is not available\n";
    }
    destroy_session(session);
    return false;
  }
  const bool ok = execute_session_line(session, line, output, echo_command);
  destroy_session(session);
  return ok;
}

ShellSession* create_session(const ShellTransport transport) {
  if (!g_shell_state.initialized) {
    init({});
  }
  ShellSession* session = nullptr;
  for (ShellSession& candidate : g_session_pool) {
    if (!candidate.in_use) {
      session = &candidate;
      break;
    }
  }
  if (session == nullptr) {
    audit_record("session-denied", "pool full");
    return nullptr;
  }
  *session = ShellSession();
  session->in_use = true;
  session->id = g_next_shell_session_id++;
  if (session->id == 0U) {
    session->id = g_next_shell_session_id++;
  }
  session->created_ms = mros::platform::mros_millis();
  session->state.config = g_shell_state.config;
  session->state.cwd = "/";
  session->state.previous_cwd = "/";
  session->state.json_output_default = g_shell_state.json_output_default;
  session->state.command_timeout_ms = g_shell_state.command_timeout_ms;
  session->state.last_output_ended_with_newline = true;
  session->state.root_session = false;
  initialize_session_identity(session->state);
  session->state.history.clear();
  session->state.env_vars.clear();
  session->state.close_requested = false;
  session->state.remote_target = 0U;
  session->state.active_transport = transport == ShellTransport::Current ? ShellTransport::Ssh : transport;
  apply_transport_terminal_size(session->state, session->state.active_transport);
  session->state.initialized = true;
  audit_record("session-open", std::to_string(session->id).c_str());
  return session;
}

bool session_apply_user(ShellSession* session, const char* username) {
  return session != nullptr && session->in_use &&
         apply_user_identity_by_name(session->state, username);
}

void destroy_session(ShellSession* session) {
  if (session == nullptr) {
    return;
  }
  if (!session->in_use) {
    return;
  }
  audit_record("session-close", std::to_string(session->id).c_str());
  *session = ShellSession();
}

bool execute_session_line(
    ShellSession* session,
    const char* line,
    std::string* output,
    const bool echo_command) {
  if (session == nullptr) {
    return false;
  }
  std::string* previous_capture = session->state.captured_output;
  if (output != nullptr) {
    output->clear();
    session->state.captured_output = output;
  }
  const bool ok = execute_line_on_state(
      session->state,
      line,
      echo_command,
      session->state.active_transport);
  session->state.captured_output = previous_capture;
  return ok;
}

bool execute_session_line_capture_stream(
    ShellSession* session,
    const char* line,
    std::string* output,
    ShellStreamCallback stream_callback,
    void* stream_user_data,
    const bool echo_command) {
  if (session == nullptr) {
    return false;
  }
  std::string* previous_capture = session->state.captured_output;
  ShellStreamCallback previous_stream_callback =
      session->state.captured_stream_callback;
  void* previous_stream_user_data = session->state.captured_stream_user_data;
  if (output != nullptr) {
    output->clear();
    session->state.captured_output = output;
  }
  session->state.captured_stream_callback = stream_callback;
  session->state.captured_stream_user_data = stream_user_data;
  const bool ok = execute_line_on_state(
      session->state,
      line,
      echo_command,
      session->state.active_transport);
  session->state.captured_output = previous_capture;
  session->state.captured_stream_callback = previous_stream_callback;
  session->state.captured_stream_user_data = previous_stream_user_data;
  return ok;
}

void set_transport_terminal_size(
    const ShellTransport transport,
    const uint16_t columns,
    const uint16_t rows) {
  TransportTerminalSize size {};
  size.columns = clamp_terminal_columns(columns);
  size.rows = clamp_terminal_rows(rows);

  switch (transport) {
    case ShellTransport::Web:
      g_web_terminal_size = size;
      break;
    case ShellTransport::Ssh:
      g_ssh_terminal_size = size;
      break;
    case ShellTransport::SerialConsole:
    case ShellTransport::Current:
    case ShellTransport::System:
    default:
      g_serial_terminal_size = size;
      break;
  }

  if (transport == ShellTransport::SerialConsole &&
      g_shell_state.active_transport == ShellTransport::SerialConsole) {
    apply_transport_terminal_size(g_shell_state, ShellTransport::SerialConsole);
  }
}

void session_set_terminal_size(
    ShellSession* session,
    const uint16_t columns,
    const uint16_t rows) {
  if (session == nullptr) {
    return;
  }
  set_state_terminal_size(session->state, columns, rows);
}

std::string prompt_for_transport(const ShellTransport transport) {
  if (!g_shell_state.initialized) {
    init({});
  }

  const ShellTransport resolved =
      (transport == ShellTransport::Current) ? g_shell_state.active_transport : transport;
  const ShellTransport previous_transport = g_shell_state.active_transport;
  const uint16_t previous_columns = g_shell_state.terminal_columns;
  const uint16_t previous_rows = g_shell_state.terminal_rows;
  g_shell_state.active_transport = resolved;
  apply_transport_terminal_size(g_shell_state, resolved);
  const std::string prompt = shell_prompt(g_shell_state);
  g_shell_state.active_transport = previous_transport;
  set_state_terminal_size(g_shell_state, previous_columns, previous_rows);
  return prompt;
}

const char* session_prompt(ShellSession* session) {
  if (session == nullptr) {
    return "";
  }
  session->prompt_cache = shell_prompt(session->state);
  return session->prompt_cache.c_str();
}

bool session_close_requested(const ShellSession* session) {
  return session != nullptr && session->state.close_requested;
}

void session_request_close(ShellSession* session) {
  if (session != nullptr) {
    session->state.close_requested = true;
  }
}

bool session_is_root(const ShellSession* session) {
  return session != nullptr && session->state.root_session;
}

uint32_t session_id(const ShellSession* session) {
  return (session != nullptr && session->in_use) ? session->id : 0U;
}

uint32_t active_session_count() {
  uint32_t count = 0U;
  for (const ShellSession& session : g_session_pool) {
    if (session.in_use) ++count;
  }
  return count;
}

uint32_t active_root_session_count() {
  uint32_t count = g_shell_state.root_session ? 1U : 0U;
  for (const ShellSession& session : g_session_pool) {
    if (session.in_use && session.state.root_session) ++count;
  }
  return count;
}

uint32_t session_capacity() {
  return static_cast<uint32_t>(kShellSessionPoolSize);
}

uint32_t capability_mask_for_user(const char* username) {
  if (username == nullptr || username[0] == '\0') {
    return 0U;
  }
  mros::ssh::UserAccount user;
  if (!mros::ssh::get_user(String(username), &user)) {
    return 0U;
  }
  return capability_mask_for_account(user);
}

const char* capabilities_text(const uint32_t capabilities) {
  static char text[96] = {};
  text[0] = '\0';
  auto add = [&](const uint32_t bit, const char* name) {
    if ((capabilities & bit) == 0U) return;
    if (text[0] != '\0') std::strncat(text, ",", sizeof(text) - std::strlen(text) - 1U);
    std::strncat(text, name, sizeof(text) - std::strlen(text) - 1U);
  };
  add(ShellCapabilityRead, "read");
  add(ShellCapabilityWrite, "write");
  add(ShellCapabilityRobot, "robot");
  add(ShellCapabilityNetwork, "network");
  add(ShellCapabilityUpdate, "update");
  add(ShellCapabilityDebug, "debug");
  add(ShellCapabilityRoot, "root");
  return text[0] != '\0' ? text : "none";
}

void audit_record(const char* event, const char* detail) {
  ShellAuditRecord& record = g_audit_ring[g_audit_pos % kShellAuditRingSize];
  record.seq = ++g_audit_seq;
  record.ms = mros::platform::mros_millis();
  copy_audit_text(record.event, sizeof(record.event), event);
  copy_audit_text(record.detail, sizeof(record.detail), detail);
  g_audit_pos = (g_audit_pos + 1U) % kShellAuditRingSize;
}

std::string audit_report() {
  std::string out;
  out += "seq   ms         event                 detail\n";
  out += "----  ---------  --------------------  --------------------------------\n";
  const uint32_t total = std::min<uint32_t>(g_audit_seq, kShellAuditRingSize);
  const uint32_t start_seq = g_audit_seq >= total ? (g_audit_seq - total + 1U) : 1U;
  for (uint32_t seq = start_seq; seq <= g_audit_seq; ++seq) {
    for (const ShellAuditRecord& record : g_audit_ring) {
      if (record.seq != seq) continue;
      char line[192] = {};
      std::snprintf(line, sizeof(line), "%-4lu  %-9lu  %-20s  %s\n",
                    static_cast<unsigned long>(record.seq),
                    static_cast<unsigned long>(record.ms),
                    record.event,
                    record.detail);
      out += line;
      break;
    }
  }
  return out;
}

void audit_clear() {
  for (ShellAuditRecord& record : g_audit_ring) {
    record = ShellAuditRecord();
  }
  g_audit_pos = 0U;
  g_audit_seq = 0U;
}

bool shell_can_enter_root(const ShellState& state) {
  return state.root_session || count_root_sessions_except(&state) == 0U;
}

void handle_serial_input_byte(const uint8_t raw, const SerialInputSource source) {
  size_t& input_length = serial_input_length_for_source(source);
  char* input_buffer = serial_input_buffer_for_source(source);
  g_shell_state.last_serial_activity_ms = mros::platform::mros_millis();

  const char ch = static_cast<char>(raw);
  if (ch == '\r' || ch == '\n') {
    if (serial_shell_line_edit_active()) {
      serial_console_write_line();
    }
    flush_serial_buffer(source);
    return;
  }

  if (ch == '\b' || static_cast<unsigned char>(ch) == 127U) {
    if (input_length > 0U) {
      --input_length;
      input_buffer[input_length] = '\0';
      serial_console_echo_backspace(source);
    }
    return;
  }

  if (static_cast<unsigned char>(ch) < 32U) {
    return;
  }

  if (input_length >= (kSerialInputBufferSize - 1U)) {
    shell_error(g_shell_state, "input line too long");
    clear_serial_input_buffer(source);
    return;
  }

  input_buffer[input_length++] = ch;
  input_buffer[input_length] = '\0';
  serial_console_echo_char(ch, source);
}

void poll_serial() {
  if (!g_shell_state.initialized) {
    return;
  }

  while (ensure_serial_console_ready() &&
         mros::platform::mros_uart_available(kShellSerialPort) > 0) {
    uint8_t raw = 0U;
    if (!mros::platform::mros_uart_read_byte(kShellSerialPort, &raw, 0)) {
      break;
    }
    handle_serial_input_byte(raw, SerialInputSource::Uart0);
  }

#if defined(CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED) && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
  if (ensure_usb_serial_console_ready()) {
    uint8_t raw = 0U;
    while (usb_serial_jtag_read_bytes(&raw, 1U, 0) == 1) {
      handle_serial_input_byte(raw, SerialInputSource::UsbJtag);
    }
  }
#endif
}

const char* current_working_directory() {
  if (!g_shell_state.initialized) {
    init({});
  }
  return g_shell_state.cwd.c_str();
}

bool complete_input(
    const char* input,
    char* completed_line,
    const size_t completed_line_size,
    char* suggestions,
    const size_t suggestions_size) {
  if (!g_shell_state.initialized) {
    init({});
  }

  std::string completed;
  std::vector<std::string> matches;
  const bool changed = build_completion_result(
      g_shell_state,
      input != nullptr ? std::string(input) : std::string(),
      &completed,
      &matches);

  if (completed_line != nullptr && completed_line_size > 0U) {
    std::snprintf(completed_line, completed_line_size, "%s", completed.c_str());
  }
  if (suggestions != nullptr && suggestions_size > 0U) {
    append_suggestions_text(suggestions, suggestions_size, matches);
  }
  return changed;
}

bool suggest_input(const char* input, char* suggestions, const size_t suggestions_size) {
  if (!g_shell_state.initialized) {
    init({});
  }

  std::string completed;
  std::vector<std::string> matches;
  build_completion_result(g_shell_state, input != nullptr ? std::string(input) : std::string(), &completed, &matches);
  if (suggestions != nullptr && suggestions_size > 0U) {
    append_suggestions_text(suggestions, suggestions_size, matches);
  }
  return !matches.empty();
}

bool serial_has_pending_input() {
  const bool uart_has_pending =
      ensure_serial_console_ready() &&
      mros::platform::mros_uart_available(kShellSerialPort) > 0;
#if defined(CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED) && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
  const bool usb_has_pending =
      ensure_usb_serial_console_ready() &&
      usb_serial_jtag_is_connected();
  return uart_has_pending || usb_has_pending;
#else
  return uart_has_pending;
#endif
}

bool serial_recently_active(const uint32_t within_ms) {
  if (!g_shell_state.initialized || g_shell_state.last_serial_activity_ms == 0U) {
    return false;
  }
  return (mros::platform::mros_millis() -
          g_shell_state.last_serial_activity_ms) <= within_ms;
}

bool has_partial_input() {
  return any_serial_input_buffer_has_data();
}

bool serial_auth_required() {
  load_serial_auth_setting();
  return g_serial_auth_required;
}

bool set_serial_auth_required(const bool required) {
  if (!required && !insecure_serial_shell_allowed()) {
    g_serial_auth_loaded = true;
    g_serial_auth_required = true;
    reset_serial_auth_prompt();
    return false;
  }
  g_serial_auth_loaded = true;
  g_serial_auth_required = required;
  reset_serial_auth_prompt();
  return save_serial_auth_setting(required);
}

const char* serial_auth_mode_text() {
  return serial_auth_required() ? "on" : "off";
}

void shell_emit_text(ShellState& state, const char* text) {
  if (text == nullptr || text[0] == '\0') {
    return;
  }

  const size_t length = std::strlen(text);
  state.last_output_ended_with_newline = length > 0U && text[length - 1U] == '\n';

  if (state.captured_output != nullptr) {
    state.captured_output->append(text);
    if (state.captured_stream_callback != nullptr) {
      state.captured_stream_callback(text, state.captured_stream_user_data);
    }
    return;
  }

  if (state.active_transport == ShellTransport::SerialConsole &&
      mros_console_is_serial_mirror_suppressed()) {
    serial_console_write_text(text);
    return;
  }

  if (state.config.output_callback != nullptr) {
    state.config.output_callback(text, state.config.user_data);
    return;
  }

  serial_console_write_text(text);
}

ShellOutputSink shell_output_sink(ShellState& state) {
  ShellOutputSink sink {};
  sink.state = &state;
  return sink;
}

void shell_sink_write(ShellOutputSink& sink, const char* text, const size_t len) {
  if (sink.state == nullptr || text == nullptr || len == 0U) {
    return;
  }

  size_t offset = 0U;
  char chunk[257] = {};
  while (offset < len) {
    const size_t copy_len = std::min(sizeof(chunk) - 1U, len - offset);
    std::memcpy(chunk, text + offset, copy_len);
    chunk[copy_len] = '\0';
    shell_emit_text(*sink.state, chunk);
    sink.bytes_written += copy_len;
    offset += copy_len;
  }
}

void shell_sink_write_cstr(ShellOutputSink& sink, const char* text) {
  if (text == nullptr) {
    return;
  }
  shell_sink_write(sink, text, std::strlen(text));
}

void shell_sink_printf(ShellOutputSink& sink, const char* format, ...) {
  if (format == nullptr) {
    return;
  }
  char buffer[512] = {};
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  buffer[sizeof(buffer) - 1U] = '\0';
  shell_sink_write_cstr(sink, buffer);
}

void shell_write(ShellState& state, const char* text) {
  ShellOutputSink sink = shell_output_sink(state);
  shell_sink_write_cstr(sink, text);
}

void shell_write_line(ShellState& state, const char* text) {
  if (text == nullptr) {
    shell_write(state, "\n");
    return;
  }

  shell_write(state, text);
  shell_write(state, "\n");
}

void shell_printf(ShellState& state, const char* format, ...) {
  if (format == nullptr) {
    return;
  }

  char buffer[512] = {};
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  buffer[sizeof(buffer) - 1U] = '\0';
  shell_write(state, buffer);
}

bool shell_supports_ansi(const ShellState& state) {
  return transport_supports_ansi(state.active_transport);
}

size_t shell_terminal_columns(const ShellState& state) {
  return static_cast<size_t>(std::max<uint16_t>(40U, state.terminal_columns));
}

size_t shell_terminal_rows(const ShellState& state) {
  return static_cast<size_t>(std::max<uint16_t>(10U, state.terminal_rows));
}

std::string shell_ansi_wrap(const ShellState& state, const char* sgr, const std::string& text) {
  if (!shell_supports_ansi(state) || sgr == nullptr || sgr[0] == '\0' || text.empty()) {
    return text;
  }
  return std::string("\033[") + sgr + "m" + text + "\033[0m";
}

std::string shell_ansi_reset() {
  return "\033[0m";
}

std::string shell_prompt(const ShellState& state) {
  if (state.heredoc_active) {
    return shell_supports_ansi(state)
               ? shell_ansi_wrap(state, "38;5;215;1", "heredoc") + shell_ansi_wrap(state, "38;5;246", "> ")
               : "heredoc> ";
  }
  const mros::ssh::IdentityConfig identity = mros::ssh::identity_get();
  std::string user = state.root_session ? mros::ssh::root_username() : state.session_username;
  if (user.empty()) {
    user = identity.username.c_str();
  }
  std::string device = identity.device_name.c_str();
  if (device.empty()) {
    device = (state.config.hostname != nullptr && state.config.hostname[0] != '\0') ? state.config.hostname : "mros";
  }
  const std::string remote_suffix = remote::prompt_suffix(state);
  if (!shell_supports_ansi(state)) {
    return user + "@" + device + remote_suffix + ":" + state.cwd + (state.root_session ? "# " : "$ ");
  }
  std::string prompt;
  prompt.reserve(user.size() + device.size() + remote_suffix.size() + state.cwd.size() + 64U);
  prompt += shell_ansi_wrap(state, state.root_session ? "38;5;203;1" : "38;5;112;1", user);
  prompt += shell_ansi_wrap(state, "38;5;244", "@");
  prompt += shell_ansi_wrap(state, "38;5;81;1", device);
  if (!remote_suffix.empty()) {
    prompt += shell_ansi_wrap(state, "38;5;214;1", remote_suffix);
  }
  prompt += shell_ansi_wrap(state, "38;5;244", ":");
  prompt += shell_ansi_wrap(state, "38;5;223", state.cwd);
  prompt += shell_ansi_wrap(state, state.root_session ? "38;5;203;1" : "38;5;246", state.root_session ? "# " : "$ ");
  return prompt;
}

bool shell_is_storage_mounted(const ShellState& state) {
  if (state.config.is_storage_mounted_callback == nullptr) {
    return false;
  }
  return state.config.is_storage_mounted_callback(state.config.user_data);
}

const std::vector<ShellCommandRegistration>& shell_commands() {
  static const std::vector<ShellCommandRegistration> commands = {
      {"help", shell_cmd_help, "show mshell help", shell_help_help},
      {"man", shell_cmd_man, "show a command manual page", shell_help_man},
      {"set", shell_cmd_set, "read or change runtime settings", shell_help_set, ShellCapabilityWrite},
      {"ls", shell_cmd_ls, "list directory contents", shell_help_ls},
      {"tree", shell_cmd_tree, "print a directory tree", shell_help_tree},
      {"cat", shell_cmd_cat, "concatenate files to stdout", shell_help_cat},
      {"head", shell_cmd_head, "print the first lines of files", shell_help_head},
      {"tail", shell_cmd_tail, "print the last lines of files", shell_help_tail},
      {"wc", shell_cmd_wc, "count lines, words and bytes", shell_help_wc},
      {"more", shell_cmd_more, "page through text output", shell_help_more},
      {"less", shell_cmd_less, "bounded non-interactive text pager", shell_help_less},
      {"nl", shell_cmd_nl, "number lines of files", shell_help_nl},
      {"du", shell_cmd_du, "estimate file and directory space usage", shell_help_du},
      {"stat", shell_cmd_stat, "display file or filesystem status", shell_help_stat},
      {"find", shell_cmd_find, "search for files in a directory hierarchy", shell_help_find},
      {"sha256sum", shell_cmd_sha256sum, "compute SHA-256 message digests", shell_help_sha256sum},
      {"md5sum", shell_cmd_md5sum, "compute MD5 message digests", shell_help_md5sum},
      {"cksum", shell_cmd_cksum, "compute POSIX CRC checksums", shell_help_cksum},
      {"crc32", shell_cmd_crc32, "compute CRC-32 checksums", shell_help_crc32},
      {"file", shell_cmd_file, "identify file type from metadata and magic bytes", shell_help_file},
      {"hexdump", shell_cmd_hexdump, "display file contents in hexadecimal", shell_help_hexdump},
      {"od", shell_cmd_od, "dump files in octal or hexadecimal", shell_help_od},
      {"xxd", shell_cmd_xxd, "display file contents in xxd-style hexadecimal", shell_help_xxd},
      {"sleep", shell_cmd_sleep, "delay for a specified time", shell_help_sleep},
      {"tee", shell_cmd_tee, "read stdin and write to stdout and files", shell_help_tee, ShellCapabilityWrite},
      {"EOF", shell_cmd_eof, "no-op heredoc sentinel", shell_help_eof},
      {"eof", shell_cmd_eof, "no-op heredoc sentinel", shell_help_eof},
      {"sort", shell_cmd_sort, "sort lines of text files", shell_help_sort},
      {"uniq", shell_cmd_uniq, "report or omit repeated lines", shell_help_uniq},
      {"cut", shell_cmd_cut, "remove sections from each line", shell_help_cut},
      {"tr", shell_cmd_tr, "translate or delete characters", shell_help_tr},
      {"sed", shell_cmd_sed, "bounded sed substitution subset", shell_help_sed},
      {"awk", shell_cmd_awk, "bounded awk field-print subset", shell_help_awk},
      {"fold", shell_cmd_fold, "wrap input lines to a fixed width", shell_help_fold},
      {"fmt", shell_cmd_fmt, "simple paragraph text formatter", shell_help_fmt},
      {"expand", shell_cmd_expand, "convert tabs to spaces", shell_help_expand},
      {"unexpand", shell_cmd_unexpand, "convert leading spaces to tabs", shell_help_unexpand},
      {"column", shell_cmd_column, "format whitespace tables into aligned columns", shell_help_column},
      {"bc", shell_cmd_bc, "evaluate simple numeric expressions", shell_help_bc},
      {"units", shell_cmd_units, "convert common robot and electronics units", shell_help_units},
      {"xargs", shell_cmd_xargs, "build and execute command lines from stdin", shell_help_xargs},
      {"basename", shell_cmd_basename, "strip directory and suffix from names", shell_help_basename},
      {"dirname", shell_cmd_dirname, "strip last component from file names", shell_help_dirname},
      {"realpath", shell_cmd_realpath, "print the resolved path", shell_help_realpath},
      {"cmp", shell_cmd_cmp, "compare two files byte by byte", shell_help_cmp},
      {"strings", shell_cmd_strings, "print printable strings in files", shell_help_strings},
      {"sync", shell_cmd_sync, "flush pending storage writes", shell_help_sync},
      {"env", shell_cmd_env, "print the shell environment", shell_help_env},
      {"export", shell_cmd_export, "set shell environment variables", shell_help_export},
      {"printenv", shell_cmd_printenv, "print shell environment variables", shell_help_printenv},
      {"history", shell_cmd_history, "show recent command history", shell_help_history},
      {"which", shell_cmd_which, "locate a command", shell_help_which},
      {"test", shell_cmd_test, "check file types and string values", shell_help_test},
      {"[", shell_cmd_test, "compatibility alias for test", shell_help_test},
      {"read", shell_cmd_read, "read stdin into a shell variable", shell_help_read},
      {"true", shell_cmd_true, "do nothing successfully", shell_help_true},
      {"false", shell_cmd_false, "do nothing unsuccessfully", shell_help_false},
      {"whoami", shell_cmd_whoami, "print the effective username", shell_help_whoami},
      {"id", shell_cmd_id, "print user and group information", shell_help_id},
      {"groups", shell_cmd_groups, "print group memberships", shell_help_groups},
      {"chmod", shell_cmd_chmod, "change staged file mode metadata", shell_help_chmod, ShellCapabilityWrite},
      {"chown", shell_cmd_chown, "change staged file owner metadata", shell_help_chown, ShellCapabilityRoot},
      {"sudo", shell_cmd_sudo, "execute a command as root temporarily", shell_help_sudo},
      {"passwd", shell_cmd_passwd, "change a user password", shell_help_passwd, ShellCapabilityWrite},
      {"pushd", shell_cmd_pushd, "push cwd and change directory", shell_help_pushd},
      {"popd", shell_cmd_popd, "pop directory stack and change directory", shell_help_popd},
      {"dirs", shell_cmd_dirs, "show directory stack", shell_help_dirs},
      {"cd", shell_cmd_cd, "change the shell working directory", shell_help_cd},
      {"grep", shell_cmd_grep, "search text inside files", shell_help_grep},
      {"mount", shell_cmd_mount, "mount local or remote filesystems", shell_help_mount},
      {"umount", shell_cmd_umount, "unmount remote filesystem providers", shell_help_umount},
      {"cp", shell_cmd_cp, "copy files and directories", shell_help_cp, ShellCapabilityWrite},
      {"mv", shell_cmd_mv, "move or rename files and directories", shell_help_mv, ShellCapabilityWrite},
      {"rm", shell_cmd_rm, "remove files or directories", shell_help_rm, ShellCapabilityWrite},
      {"touch", shell_cmd_touch, "create files or update timestamps", shell_help_touch, ShellCapabilityWrite},
      {"mkdir", shell_cmd_mkdir, "create directories", shell_help_mkdir, ShellCapabilityWrite},
      {"source", shell_cmd_source, "run commands from a script file", shell_help_source},
      {"script", shell_cmd_source, "run commands from a script file", shell_help_source},
      {"sh", shell_cmd_sh, "run a shell script file", shell_help_sh},
      {"clear", shell_cmd_clear, "clear the console output buffer", shell_help_clear},
      {"cls", shell_cmd_clear, "clear the console output buffer", shell_help_clear},
      {"reset-screen", shell_cmd_clear, "clear the console output buffer", shell_help_clear},
      {"uname", shell_cmd_uname, "print system information", shell_help_uname},
      {"date", shell_cmd_date, "print the current date and time", shell_help_date},
      {"pwd", shell_cmd_pwd, "print the current working directory", shell_help_pwd},
      {"ps", shell_cmd_ps, "show RTOS task list", shell_help_ps},
      {"lsblk", shell_cmd_lsblk, "list recognized filesystem partitions", shell_help_lsblk},
      {"df", shell_cmd_df, "show filesystem usage", shell_help_df},
      {"free", shell_cmd_free, "show heap and memory usage", shell_help_free},
      {"ping", shell_cmd_ping, "send ICMP echo requests", shell_help_ping},
      {"wifi", shell_cmd_wifi, "manage WiFi station, hotspot and diagnostics", shell_help_wifi, ShellCapabilityNetwork},
      {"devices", shell_cmd_devices, "test connected device links on demand", shell_help_devices},
      {"dpm", shell_cmd_dpm, "inspect and control Device Process Manager", shell_help_dpm,
       ShellCapabilityDebug, "Tanı",
       "dpm status|report|tasks|policy|wake|why-awake|trace|reset-stats",
       "dpm tasks --json\n"
       "dpm policy set conservative\n"
       "dpm wake fk_preview_task manual",
       "low", true, false},
      {"coredump", shell_cmd_coredump, "inspect, download or clear crash coredumps", shell_help_coredump,
       ShellCapabilityDebug, "Tanı",
       "coredump status|summary|download|clear [--json]",
       "coredump status --json\n"
       "coredump download",
       "medium", true, false},
      {"heap", shell_cmd_heap_trace, "inspect heap trace support and counters", shell_help_heap_trace,
       ShellCapabilityDebug, "Tanı",
       "heap trace start|stop|summary|dump|clear [--json]",
       "heap trace summary --json\n"
       "heap trace start --json",
       "low", true, false},
      {"watch", shell_cmd_watch, "repeat a command on an interval", shell_help_watch},
      {"config", shell_cmd_config, "manage shell configuration files", shell_help_config, ShellCapabilityWrite},
      {"mros", shell_cmd_mros, "show MROS diagnostics and live link monitors", shell_help_mros},
      {"mshell", shell_cmd_mshell, "manage mshell aliases and shell features", shell_help_mshell},
      {"mros-deuscara-update", shell_cmd_mros7dofs3_update, "check staged S3 firmware update support", shell_help_mros7dofs3_update, ShellCapabilityUpdate},
      {"mros7dofs3-update", shell_cmd_mros7dofs3_update, "legacy alias for mros-deuscara-update", shell_help_mros7dofs3_update, ShellCapabilityUpdate},
      {"medit", shell_cmd_medit, "SSH-only text editor", shell_help_medit, ShellCapabilityWrite},
      {"tar", shell_cmd_tar, "create, list or extract ustar archives", shell_help_tar, ShellCapabilityWrite},
      {"gzip", shell_cmd_gzip, "write gzip stored-deflate files", shell_help_gzip, ShellCapabilityWrite},
      {"gunzip", shell_cmd_gunzip, "extract gzip stored-deflate files", shell_help_gunzip, ShellCapabilityWrite},
      {"zcat", shell_cmd_zcat, "print gzip stored-deflate contents", shell_help_zcat},
      {"curl", shell_cmd_curl, "transfer HTTP/HTTPS resources", shell_help_curl, ShellCapabilityNetwork},
      {"wget", shell_cmd_wget, "download HTTP/HTTPS resources", shell_help_wget, ShellCapabilityNetwork},
      {"hostname", shell_cmd_hostname, "show or stage system hostname", shell_help_hostname},
      {"who", shell_cmd_who, "show active shell sessions", shell_help_who},
      {"w", shell_cmd_w, "show users and active session summary", shell_help_w},
      {"last", shell_cmd_last, "show shell audit login/session events", shell_help_last},
      {"dmesg", shell_cmd_dmesg, "print kernel-style runtime log ring", shell_help_dmesg},
      {"logger", shell_cmd_logger, "append a message to the runtime log", shell_help_logger},
      {"journalctl", shell_cmd_journalctl, "query runtime logs with journalctl-like flags", shell_help_journalctl},
      {"systemctl", shell_cmd_systemctl, "inspect and control MROS services", shell_help_systemctl, ShellCapabilityRoot},
      {"service", shell_cmd_service, "compatibility wrapper for systemctl", shell_help_service, ShellCapabilityRoot},
      {"gpio", shell_cmd_gpio, "inspect or control GPIO pins", shell_help_gpio, ShellCapabilityRoot},
      {"gpioget", shell_cmd_gpioget, "read a GPIO pin", shell_help_gpioget},
      {"gpioset", shell_cmd_gpioset, "write a GPIO pin", shell_help_gpioset, ShellCapabilityRoot},
      {"gpioinfo", shell_cmd_gpioinfo, "list known GPIO pin ownership", shell_help_gpioinfo},
      {"i2cdetect", shell_cmd_i2cdetect, "scan the active I2C bus", shell_help_i2cdetect, ShellCapabilityDebug},
      {"i2cget", shell_cmd_i2cget, "read an I2C register", shell_help_i2cget, ShellCapabilityDebug},
      {"i2cset", shell_cmd_i2cset, "write an I2C register", shell_help_i2cset, ShellCapabilityRoot},
      {"i2cdump", shell_cmd_i2cdump, "dump I2C registers", shell_help_i2cdump, ShellCapabilityDebug},
      {"pwm", shell_cmd_pwm, "inspect or control PCA9685 PWM outputs", shell_help_pwm, ShellCapabilityRoot},
      {"adc", shell_cmd_adc, "read ADC-capable GPIO pins", shell_help_adc, ShellCapabilityDebug},
      {"spi", shell_cmd_spi, "inspect Teensy41/C3 SPI links", shell_help_spi, ShellCapabilityDebug},
      {"uart", shell_cmd_uart, "inspect or send UART diagnostics", shell_help_uart, ShellCapabilityRoot},
      {"ssh", shell_cmd_ssh, "connect to a remote SSH host", shell_help_ssh, ShellCapabilityNetwork},
      {"change", shell_cmd_change, "change shell user and device identity", shell_help_change, ShellCapabilityWrite},
      {"su", shell_cmd_su, "switch to root role", shell_help_su},
      {"exit", shell_cmd_exit, "leave root role or close a shell session", shell_help_exit},
      {"update-system", shell_cmd_update_system, "install a firmware .bin from LittleFS", shell_help_update_system, ShellCapabilityUpdate},
      {"echo", shell_cmd_echo, "write arguments to the shell output", shell_help_echo},
      {"uptime", shell_cmd_uptime, "show device uptime", shell_help_uptime},
      {"status", shell_cmd_status, "show system status summary", shell_help_status},
      {"log", shell_cmd_log, "show recent console log lines", shell_help_log},
      {"mtop", shell_cmd_htop, "show RTOS task diagnostics", shell_help_htop},
      {"htop", shell_cmd_htop, "compatibility alias for mtop", shell_help_htop},
      {"reboot", shell_cmd_reboot, "restart the device immediately", shell_help_reboot, ShellCapabilityRoot},
      {"poweroff", shell_cmd_poweroff, "put the device into deep sleep", shell_help_poweroff, ShellCapabilityRoot},
      {"robot", shell_cmd_robot, "send robot motion and safety commands", shell_help_robot, ShellCapabilityRobot},
      {"project", shell_cmd_project, "show MROS project information and roadmap", shell_help_project},
      {"mfetch", shell_cmd_espfetch, "show project-specific system information", shell_help_espfetch},
      {"espfetch", shell_cmd_espfetch, "legacy alias for mfetch", shell_help_espfetch},
  };
  return commands;
}

const ShellCommandRegistration* shell_find_command(const char* name) {
  if (name == nullptr || name[0] == '\0') {
    return nullptr;
  }
  return find_command(name);
}

}  // namespace mros::shell
