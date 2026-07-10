#include "src/shell/mros_shell_internal.h"

#include "src/platform/mros_time.h"

#include <cstdio>

namespace mros::shell {
namespace {

void format_uptime(char* buffer, const size_t size, const unsigned long long total_seconds) {
  const unsigned long long days = total_seconds / 86400ULL;
  const unsigned long long hours = (total_seconds % 86400ULL) / 3600ULL;
  const unsigned long long minutes = (total_seconds % 3600ULL) / 60ULL;
  const unsigned long long seconds = total_seconds % 60ULL;
  std::snprintf(
      buffer,
      size,
      "up %llud %02lluh %02llum %02llus",
      days,
      hours,
      minutes,
      seconds);
}

}  // namespace

void shell_help_uptime(ShellState& state) {
  shell_write_line(state, "Usage: uptime");
  shell_write_line(state, "Show how long the device has been running.");
}

int shell_cmd_uptime(ShellContext& ctx) {
  if (ctx.args.size() > 1U) {
    if (ctx.args[1] == "--help" || ctx.args[1] == "-h") {
      shell_help_uptime(ctx.state);
      return 0;
    }
    shell_write_line(ctx.state, "uptime: too many arguments");
    return 1;
  }

  char buffer[64] = {};
  format_uptime(buffer, sizeof(buffer),
                static_cast<unsigned long long>(
                    mros::platform::mros_millis() / 1000ULL));
  shell_write_line(ctx.state, buffer);
  return 0;
}

}  // namespace mros::shell
