#include "src/shell/mros_shell_internal.h"

namespace mros::shell {

void shell_help_reboot(ShellState& state) {
  shell_write_line(state, "Usage: reboot [--dry-run] [--reason TEXT]");
  shell_write_line(state, "Restart the device immediately without opening the power popup.");
  shell_write_line(state, "  --dry-run, --no-act       validate callback and print the planned action");
  shell_write_line(state, "  --reason TEXT             include an operator-readable reason in output");
}

int shell_cmd_reboot(ShellContext& ctx) {
  bool dry_run = false;
  const char* reason = nullptr;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help" || ctx.args[i] == "-h") {
      shell_help_reboot(ctx.state);
      return 0;
    }
    if (ctx.args[i] == "--dry-run" || ctx.args[i] == "--no-act") {
      dry_run = true;
      continue;
    }
    if (ctx.args[i] == "--reason") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "reboot: --reason requires text");
        return 1;
      }
      reason = ctx.args[++i].c_str();
      continue;
    }
    shell_printf(ctx.state, "reboot: unknown argument '%s'\n", ctx.args[i].c_str());
    return 1;
  }

  if (ctx.state.config.system_action_callback == nullptr) {
    shell_write_line(ctx.state, "reboot: system action callback is not configured");
    return 1;
  }

  shell_printf(ctx.state, "reboot: %s%s%s\n",
               dry_run ? "would restart device" : "restarting device",
               reason != nullptr ? " reason=" : "",
               reason != nullptr ? reason : "");
  if (dry_run) {
    return 0;
  }
  return ctx.state.config.system_action_callback(ShellSystemAction::Reboot, ctx.state.config.user_data) ? 0 : 1;
}

}  // namespace mros::shell
