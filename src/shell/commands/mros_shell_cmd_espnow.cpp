#include "src/shell/mros_shell_internal.h"

#include <string>

namespace mros::shell {

void shell_help_espnow(ShellState& state) {
  shell_write_line(state, "Usage: espnow <scan|list|connect TARGET>");
  shell_write_line(state, "Run simple ESP-NOW helper actions through the app callback.");
}

int shell_cmd_espnow(ShellContext& ctx) {
  if (ctx.args.size() <= 1U) {
    shell_help_espnow(ctx.state);
    return 1;
  }

  if (ctx.args[1] == "--help" || ctx.args[1] == "-h") {
    shell_help_espnow(ctx.state);
    return 0;
  }

  if (ctx.state.config.espnow_action_callback == nullptr) {
    shell_write_line(ctx.state, "espnow: ESP-NOW callback is not configured");
    return 1;
  }

  ShellEspNowAction action = ShellEspNowAction::List;
  const char* argument = nullptr;
  if (ctx.args[1] == "scan") {
    action = ShellEspNowAction::Scan;
  } else if (ctx.args[1] == "list") {
    action = ShellEspNowAction::List;
  } else if (ctx.args[1] == "connect") {
    if (ctx.args.size() < 3U) {
      shell_write_line(ctx.state, "espnow: connect requires a target");
      return 1;
    }
    action = ShellEspNowAction::Connect;
    argument = ctx.args[2].c_str();
  } else {
    shell_printf(ctx.state, "espnow: unknown subcommand '%s'\n", ctx.args[1].c_str());
    return 1;
  }

  char message[1024] = {};
  const bool ok =
      ctx.state.config.espnow_action_callback(action, argument, message, sizeof(message), ctx.state.config.user_data);
  shell_write_line(ctx.state, message[0] != '\0' ? message : (ok ? "OK" : "ESP-NOW action failed"));
  return ok ? 0 : 1;
}

}  // namespace mros::shell
