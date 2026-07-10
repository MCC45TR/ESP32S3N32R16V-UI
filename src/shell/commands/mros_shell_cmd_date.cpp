#include "src/shell/mros_shell_internal.h"

#include <ctime>

namespace mros::shell {

void shell_help_date(ShellState& state) {
  shell_write_line(state, "Usage: date");
  shell_write_line(state, "Print the current local date and time.");
}

int shell_cmd_date(ShellContext& ctx) {
  if (ctx.args.size() > 1U) {
    if (ctx.args[1] == "--help" || ctx.args[1] == "-h") {
      shell_help_date(ctx.state);
      return 0;
    }
    shell_write_line(ctx.state, "date: too many arguments");
    return 1;
  }

  const std::time_t now = std::time(nullptr);
  std::tm local_time {};
#if defined(_WIN32)
  localtime_s(&local_time, &now);
#else
  localtime_r(&now, &local_time);
#endif
  char buffer[64] = {};
  if (std::strftime(buffer, sizeof(buffer), "%a %b %d %H:%M:%S %Y", &local_time) == 0U) {
    shell_write_line(ctx.state, "date: unable to format time");
    return 1;
  }
  shell_write_line(ctx.state, buffer);
  return 0;
}

}  // namespace mros::shell
