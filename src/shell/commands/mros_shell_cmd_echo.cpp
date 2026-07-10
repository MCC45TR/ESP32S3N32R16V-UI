#include "src/shell/mros_shell_internal.h"

#include <string>

namespace mros::shell {

void shell_help_echo(ShellState& state) {
  shell_write_line(state, "Usage: echo [OPTION]... [STRING]...");
  shell_write_line(state, "Write STRING(s) to standard output.");
  shell_write_line(state, "  -n                         do not output the trailing newline");
}

int shell_cmd_echo(ShellContext& ctx) {
  bool newline = true;
  size_t arg_index = 1U;
  for (; arg_index < ctx.args.size(); ++arg_index) {
    if (ctx.args[arg_index] == "--help") {
      shell_help_echo(ctx.state);
      return 0;
    }
    if (ctx.args[arg_index] != "-n") {
      break;
    }
    newline = false;
  }

  std::string output;
  for (size_t i = arg_index; i < ctx.args.size(); ++i) {
    if (!output.empty()) {
      output.push_back(' ');
    }
    output += ctx.args[i];
  }

  shell_write(ctx.state, output.c_str());
  if (newline) {
    shell_write(ctx.state, "\n");
  }
  return 0;
}

}  // namespace mros::shell
