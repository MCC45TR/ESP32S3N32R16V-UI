#include "src/shell/mros_shell_internal.h"
#include "src/shell/mshell_remote.h"

#include <cstdlib>
#include <cstdio>
#include <limits>

#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct ShellSettingEntry {
  const char* name;
  const char* summary;
};

const std::vector<ShellSettingEntry>& shell_setting_entries() {
  static const std::vector<ShellSettingEntry> entries = {
      {"motor-power", "set motor power state (0/1 or off/on)"},
      {"oe", "set PCA9685 output-enable state (0/1 or off/on)"},
      {"failsafe", "set C3 failsafe mode (0-3)"},
      {"c3-flags", "set outbound C3 command flags (0-255)"},
      {"c3-setpoint", "set outbound C3 PID setpoint in x100 units"},
      {"pid-kp", "set turret PID Kp"},
      {"pid-ki", "set turret PID Ki"},
      {"pid-kd", "set turret PID Kd"},
      {"pid-imax", "set turret PID integral limit"},
      {"pid-dspc", "set turret PID dead-space compensation"},
      {"output-lock", "set turret PID output lock (0/1 or off/on)"},
      {"output-mode", "set shell output mode (text only; JSON requires --json)"},
      {"cmd-timeout-ms", "set shell command timeout in milliseconds (0=off)"},
      {"serial-auth", "require username/password before serial mshell opens (on/off)"},
      {"uart-shell-bridge", "set MSHELL2 UART bridge mode (off/listen/on)"},
  };
  return entries;
}

const std::vector<const char*>& shell_setting_names_storage() {
  static const std::vector<const char*> names = {
      "motor-power",
      "oe",
      "failsafe",
      "c3-flags",
      "c3-setpoint",
      "pid-kp",
      "pid-ki",
      "pid-kd",
      "pid-imax",
      "pid-dspc",
      "output-lock",
      "output-mode",
      "output-format",
      "cmd-timeout-ms",
      "cmd-timeout",
      "serial-auth",
      "serialauth",
      "uart-shell-bridge",
      "uartshellbridge",
  };
  return names;
}

std::string join_value_tokens(const std::vector<std::string>& args, const size_t start_index) {
  std::string value;
  for (size_t i = start_index; i < args.size(); ++i) {
    if (!value.empty()) {
      value.push_back(' ');
    }
    value += args[i];
  }
  return value;
}

std::string normalize_setting_key(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      out.push_back(static_cast<char>(ch - 'A' + 'a'));
      continue;
    }
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
      out.push_back(ch);
    }
  }
  return out;
}

bool parse_output_mode(const std::string& raw, bool* json_mode) {
  if (json_mode == nullptr) {
    return false;
  }

  const std::string key = normalize_setting_key(raw);
  if (key == "json" || key == "on" || key == "1" || key == "true") {
    *json_mode = true;
    return true;
  }
  if (key == "text" || key == "off" || key == "0" || key == "false") {
    *json_mode = false;
    return true;
  }
  return false;
}

bool parse_timeout_ms(const std::string& raw, uint32_t* timeout_ms) {
  if (timeout_ms == nullptr || raw.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(raw.c_str(), &end, 10);
  if (end == raw.c_str() || (end != nullptr && *end != '\0') ||
      value > static_cast<unsigned long>(std::numeric_limits<uint32_t>::max())) {
    return false;
  }
  *timeout_ms = static_cast<uint32_t>(value);
  return true;
}

bool parse_on_off(const std::string& raw, bool* out_value) {
  if (out_value == nullptr) {
    return false;
  }
  const std::string key = normalize_setting_key(raw);
  if (key == "on" || key == "1" || key == "true" || key == "yes" || key == "enable" || key == "enabled") {
    *out_value = true;
    return true;
  }
  if (key == "off" || key == "0" || key == "false" || key == "no" || key == "disable" || key == "disabled") {
    *out_value = false;
    return true;
  }
  return false;
}

void print_setting_list(ShellState& state) {
  shell_write_line(state, "Available settings:");
  for (const ShellSettingEntry& entry : shell_setting_entries()) {
    shell_printf(
        state,
        "  %-22s %s\n",
        entry.name != nullptr ? entry.name : "",
        entry.summary != nullptr ? entry.summary : "");
  }
  shell_write_line(state, "");
  shell_write_line(state, "Use 'set <name>' to read a value or 'set <name> <value>' to change it.");
}

}  // namespace

const std::vector<const char*>& shell_setting_names() {
  return shell_setting_names_storage();
}

void shell_help_set(ShellState& state) {
  shell_write_line(state, "Usage: set [SETTING] [VALUE]");
  shell_write_line(state, "Without arguments, lists all supported runtime settings.");
  shell_write_line(state, "With only a setting name, prints the current value.");
  shell_write_line(state, "With a value, applies the new setting immediately.");
  shell_write_line(
      state,
      "Examples: set brightness 25, set wifi off, set cpu-power-mode performans, set output-mode text, set cmd-timeout-ms 3000, set serial-auth on, set uart-shell-bridge listen");
}

int shell_cmd_set(ShellContext& ctx) {
  if (ctx.args.size() >= 2U && (ctx.args[1] == "--help" || ctx.args[1] == "-h")) {
    shell_help_set(ctx.state);
    return 0;
  }

  if (ctx.args.size() == 1U) {
    print_setting_list(ctx.state);
    return 0;
  }

  const std::string key = normalize_setting_key(ctx.args[1]);
  const bool shell_output_setting = key == "output-mode" || key == "outputformat" || key == "output-format";
  const bool shell_timeout_setting = key == "cmd-timeout-ms" || key == "cmdtimeoutms" || key == "cmd-timeout";
  const bool serial_auth_setting = key == "serial-auth" || key == "serialauth";
  const bool uart_bridge_setting = key == "uart-shell-bridge" || key == "uartshellbridge";

  if (shell_output_setting && ctx.args.size() == 2U) {
    shell_write_line(ctx.state, "output-mode=text (forced, use --json per command)");
    return 0;
  }
  if (shell_timeout_setting && ctx.args.size() == 2U) {
    shell_printf(ctx.state, "cmd-timeout-ms=%lu\n", static_cast<unsigned long>(ctx.state.command_timeout_ms));
    return 0;
  }
  if (serial_auth_setting && ctx.args.size() == 2U) {
    shell_printf(ctx.state, "serial-auth=%s\n", serial_auth_mode_text());
    return 0;
  }
  if (uart_bridge_setting && ctx.args.size() == 2U) {
    shell_printf(ctx.state, "uart-shell-bridge=%s\n",
                 remote::bridge_mode_name(remote::bridge_mode()));
    return 0;
  }

  if (shell_output_setting) {
    const std::string value = join_value_tokens(ctx.args, 2U);
    bool json_mode = false;
    if (!parse_output_mode(value, &json_mode)) {
      shell_write_line(ctx.state, "set: invalid value for output-mode (expected text/json)");
      return 1;
    }
    if (json_mode) {
      shell_write_line(ctx.state, "set: output-mode json is disabled; use --json per command");
      return 1;
    }
    ctx.state.json_output_default = false;
    shell_write_line(ctx.state, "output-mode=text");
    return 0;
  }
  if (shell_timeout_setting) {
    const std::string value = join_value_tokens(ctx.args, 2U);
    uint32_t timeout_ms = 0U;
    if (!parse_timeout_ms(value, &timeout_ms)) {
      shell_write_line(ctx.state, "set: invalid value for cmd-timeout-ms (expected integer milliseconds)");
      return 1;
    }
    ctx.state.command_timeout_ms = timeout_ms;
    shell_printf(ctx.state, "cmd-timeout-ms=%lu\n", static_cast<unsigned long>(ctx.state.command_timeout_ms));
    return 0;
  }
  if (serial_auth_setting) {
    const std::string value = join_value_tokens(ctx.args, 2U);
    bool required = false;
    if (!parse_on_off(value, &required)) {
      shell_write_line(ctx.state, "set: invalid value for serial-auth (expected on/off)");
      return 1;
    }
    if (!set_serial_auth_required(required)) {
      shell_write_line(ctx.state, "set: serial-auth off is disabled in this production build");
      return 1;
    }
    shell_printf(ctx.state, "serial-auth=%s\n", serial_auth_mode_text());
    return 0;
  }
  if (uart_bridge_setting) {
    const std::string value = join_value_tokens(ctx.args, 2U);
    remote::BridgeMode mode = remote::BridgeMode::Off;
    if (!remote::parse_bridge_mode(value, &mode)) {
      shell_write_line(ctx.state, "set: invalid value for uart-shell-bridge (expected off/listen/on)");
      return 1;
    }
    if (!remote::set_bridge_mode(mode)) {
      shell_write_line(ctx.state, "set: failed to persist uart-shell-bridge");
      return 1;
    }
    shell_printf(ctx.state, "uart-shell-bridge=%s\n", remote::bridge_mode_name(mode));
    return 0;
  }

  if (ctx.state.config.setting_action_callback == nullptr) {
    shell_write_line(ctx.state, "set: runtime setting callback is not configured");
    return 1;
  }

  char message[768] = {};
  if (ctx.args.size() == 2U) {
    const bool ok = ctx.state.config.setting_action_callback(
        ShellSettingAction::Get,
        ctx.args[1].c_str(),
        nullptr,
        message,
        sizeof(message),
        ctx.state.config.user_data);
    shell_write_line(ctx.state, message[0] != '\0' ? message : (ok ? "OK" : "Unable to read setting."));
    return ok ? 0 : 1;
  }

  const std::string value = join_value_tokens(ctx.args, 2U);
  const bool ok = ctx.state.config.setting_action_callback(
      ShellSettingAction::Set,
      ctx.args[1].c_str(),
      value.c_str(),
      message,
      sizeof(message),
      ctx.state.config.user_data);
  shell_write_line(ctx.state, message[0] != '\0' ? message : (ok ? "Setting updated." : "Setting update failed."));
  return ok ? 0 : 1;
}

}  // namespace mros::shell
