#include "src/shell/mros_shell_internal.h"

#include <string>

namespace mros::shell {
namespace {

struct KillOptions {
  ShellTaskSignal signal = ShellTaskSignal::Term;
  const char* target = nullptr;
};

bool parse_signal_value(const std::string& value, ShellTaskSignal* signal) {
  if (signal == nullptr) {
    return false;
  }
  if (value == "TERM" || value == "term" || value == "15") {
    *signal = ShellTaskSignal::Term;
    return true;
  }
  if (value == "KILL" || value == "kill" || value == "9") {
    *signal = ShellTaskSignal::Kill;
    return true;
  }
  return false;
}

bool parse_kill_args(ShellContext& ctx, KillOptions* options, bool* help_requested, bool* list_requested) {
  if (options == nullptr || help_requested == nullptr || list_requested == nullptr) {
    return false;
  }

  *help_requested = false;
  *list_requested = false;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      *help_requested = true;
      return true;
    }
    if (arg == "--list" || arg == "-l") {
      *list_requested = true;
      continue;
    }
    if (arg == "--signal" || arg == "-s") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "kill: option requires an argument -- 's'");
        return false;
      }
      ++i;
      if (!parse_signal_value(ctx.args[i], &options->signal)) {
        shell_printf(ctx.state, "kill: unsupported signal '%s'\n", ctx.args[i].c_str());
        return false;
      }
      continue;
    }
    if (arg == "-9") {
      options->signal = ShellTaskSignal::Kill;
      continue;
    }
    if (arg == "-15") {
      options->signal = ShellTaskSignal::Term;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      shell_printf(ctx.state, "kill: invalid option '%s'\n", arg.c_str());
      return false;
    }
    if (options->target != nullptr) {
      shell_printf(ctx.state, "kill: extra operand '%s'\n", arg.c_str());
      return false;
    }
    options->target = arg.c_str();
  }
  return true;
}

}  // namespace

void shell_help_kill(ShellState& state) {
  shell_write_line(state, "Usage: kill [OPTION]... <TASK>");
  shell_write_line(state, "Stop a managed RTOS task by name or task number.");
  shell_write_line(state, "  -l, --list                list supported signal names");
  shell_write_line(state, "  -s, --signal SIGNAL       use TERM or KILL");
  shell_write_line(state, "  -9                        same as --signal KILL");
  shell_write_line(state, "  -15                       same as --signal TERM");
}

int shell_cmd_kill(ShellContext& ctx) {
  KillOptions options {};
  bool help_requested = false;
  bool list_requested = false;
  if (!parse_kill_args(ctx, &options, &help_requested, &list_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_kill(ctx.state);
    return 0;
  }
  if (list_requested) {
    shell_write_line(ctx.state, "TERM");
    shell_write_line(ctx.state, "KILL");
    return 0;
  }
  if (options.target == nullptr || options.target[0] == '\0') {
    shell_write_line(ctx.state, "kill: usage: kill [OPTION]... <TASK>");
    return 1;
  }

  if (ctx.state.config.task_signal_callback == nullptr) {
    shell_write_line(ctx.state, "kill: task signal callback is not configured");
    return 1;
  }

  char message[512] = {};
  const bool ok = ctx.state.config.task_signal_callback(
      options.signal,
      options.target,
      message,
      sizeof(message),
      ctx.state.config.user_data);
  shell_write_line(ctx.state, message[0] != '\0' ? message : (ok ? "kill: task stopped" : "kill: request failed"));
  return ok ? 0 : 1;
}

}  // namespace mros::shell
