#include "src/shell/mros_shell_internal.h"

namespace mros::shell {

void shell_help_pwd(ShellState& state) {
  shell_write_line(state, "Usage: pwd");
  shell_write_line(state, "Print the full current working directory.");
}

int shell_cmd_pwd(ShellContext& ctx) {
  if (ctx.args.size() > 1U) {
    if (ctx.args[1] == "--help" || ctx.args[1] == "-h") {
      shell_help_pwd(ctx.state);
      return 0;
    }
    shell_write_line(ctx.state, "pwd: too many arguments");
    return 1;
  }

  shell_write_line(ctx.state, ctx.state.cwd.c_str());
  return 0;
}

}  // namespace mros::shell
