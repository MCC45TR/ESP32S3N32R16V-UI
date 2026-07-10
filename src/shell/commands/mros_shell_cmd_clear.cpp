#include "src/shell/mros_shell_internal.h"

namespace mros::shell {

void shell_help_clear(ShellState& state) {
  shell_write_line(state, "Usage: clear");
  shell_write_line(state, "Clear the shell output buffer shown in the console panel.");
  shell_write_line(state, "Serial consoles cannot be cleared by the device; use the terminal app clear action.");
}

int shell_cmd_clear(ShellContext& ctx) {
  if (ctx.args.size() > 1U) {
    if (ctx.args[1] == "--help" || ctx.args[1] == "-h") {
      shell_help_clear(ctx.state);
      return 0;
    }
    shell_write_line(ctx.state, "clear: too many arguments");
    return 1;
  }

  if (ctx.state.config.clear_callback == nullptr) {
    shell_write_line(ctx.state, "clear: clear callback is not configured");
    return 1;
  }

  if (ctx.transport == ShellTransport::SerialConsole ||
      ctx.state.active_transport == ShellTransport::SerialConsole) {
    shell_write_line(ctx.state,
                     "clear: serial terminal history cannot be cleared by the device; "
                     "use your terminal application's clear-screen action.");
    return 0;
  }

  return ctx.state.config.clear_callback(ctx.state.config.user_data) ? 0 : 1;
}

}  // namespace mros::shell
