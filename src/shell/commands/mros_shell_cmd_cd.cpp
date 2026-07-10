#include "src/shell/mros_shell_internal.h"

#include <string>

namespace mros::shell {

void shell_help_cd(ShellState& state) {
  shell_write_line(state, "Usage: cd [-L|-P] [dir]");
  shell_write_line(state, "Change the shell working directory.");
}

int shell_cmd_cd(ShellContext& ctx) {
  std::string target_argument;

  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      shell_help_cd(ctx.state);
      return 0;
    }
    if (arg == "-L" || arg == "-P") {
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      shell_printf(ctx.state, "cd: invalid option -- '%s'\n", arg.c_str());
      shell_help_cd(ctx.state);
      return 1;
    }
    if (!target_argument.empty()) {
      shell_write_line(ctx.state, "cd: too many arguments");
      return 1;
    }
    target_argument = arg;
  }

  if (target_argument.empty()) {
    target_argument = "/";
  } else if (target_argument == "-") {
    target_argument = ctx.state.previous_cwd;
  }

  const std::string next_path = shell_normalize_path(ctx.state, target_argument);
  bool is_dir = false;
  std::string error;
  if (!shell_path_exists(ctx.state, next_path, &is_dir, nullptr, &error)) {
    shell_printf(ctx.state, "cd: %s: %s\n", target_argument.c_str(), error.c_str());
    return 1;
  }
  if (!is_dir) {
    shell_printf(ctx.state, "cd: %s: Not a directory\n", target_argument.c_str());
    return 1;
  }

  const std::string previous = ctx.state.cwd;
  ctx.state.cwd = next_path;
  ctx.state.previous_cwd = previous;

  if (ctx.args.size() >= 2U && ctx.args[1] == "-") {
    shell_write_line(ctx.state, ctx.state.cwd.c_str());
  }
  return 0;
}

}  // namespace mros::shell
