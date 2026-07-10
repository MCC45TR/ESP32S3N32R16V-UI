#include "src/shell/mros_shell_internal.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace mros::shell {
namespace {

bool parse_u32(const std::string& text, uint32_t* value) {
  if (value == nullptr) return false;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
  *value = static_cast<uint32_t>(parsed);
  return true;
}

std::string join_args(const std::vector<std::string>& args, const size_t start) {
  std::string out;
  for (size_t i = start; i < args.size(); ++i) {
    if (!out.empty()) out.push_back(' ');
    const bool quote = args[i].find(' ') != std::string::npos;
    if (quote) out.push_back('"');
    out += args[i];
    if (quote) out.push_back('"');
  }
  return out;
}

bool append_file(ShellState& state, const std::string& path_arg, const std::string& text) {
  const std::string path = shell_normalize_path(state, path_arg);
  if (!shell_is_user_writable_path(state, path)) {
    shell_write_line(state, "watch: log path must be inside /ESPUSER");
    return false;
  }
  bool parent_is_dir = false;
  std::string error;
  if (!shell_path_exists(state, shell_parent_path(path), &parent_is_dir, nullptr, &error) || !parent_is_dir) {
    shell_printf(state, "watch: log parent missing: %s\n", error.c_str());
    return false;
  }
  FILE* file = std::fopen(path.c_str(), "ab");
  if (file == nullptr) {
    shell_printf(state, "watch: cannot open '%s'\n", path_arg.c_str());
    return false;
  }
  const bool ok = std::fwrite(text.data(), 1U, text.size(), file) == text.size();
  std::fclose(file);
  return ok;
}

}  // namespace

void shell_help_watch(ShellState& state) {
  shell_write_line(state, "Usage: watch [OPTION]... COMMAND [ARGS]...");
  shell_write_line(state, "Repeat a shell command on an interval.");
  shell_write_line(state, "  -n, --interval SEC       delay between runs (default: 5)");
  shell_write_line(state, "      --count NUM          number of cycles (default: 12)");
  shell_write_line(state, "      --clear              emit a clear-screen escape before each cycle");
  shell_write_line(state, "      --diff               print only when command output changes");
  shell_write_line(state, "      --until TEXT         stop when output contains TEXT");
  shell_write_line(state, "      --log PATH           append output to a file under /ESPUSER");
}

int shell_cmd_watch(ShellContext& ctx) {
  uint32_t interval_sec = 5U;
  uint32_t count = 12U;
  bool clear = false;
  bool diff = false;
  std::string until;
  std::string log_path;

  size_t i = 1U;
  while (i < ctx.args.size()) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help" || arg == "-h") {
      shell_help_watch(ctx.state);
      return 0;
    }
    if (arg == "-n" || arg == "--interval") {
      if ((i + 1U) >= ctx.args.size() || !parse_u32(ctx.args[i + 1U], &interval_sec) || interval_sec == 0U) {
        shell_write_line(ctx.state, "watch: invalid interval");
        return 1;
      }
      i += 2U;
      continue;
    }
    if (arg == "--count" || arg == "--cycles") {
      if ((i + 1U) >= ctx.args.size() || !parse_u32(ctx.args[i + 1U], &count) || count == 0U) {
        shell_write_line(ctx.state, "watch: invalid count");
        return 1;
      }
      i += 2U;
      continue;
    }
    if (arg == "--clear") {
      clear = true;
      ++i;
      continue;
    }
    if (arg == "--diff") {
      diff = true;
      ++i;
      continue;
    }
    if (arg == "--until") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "watch: --until requires text");
        return 1;
      }
      until = ctx.args[i + 1U];
      i += 2U;
      continue;
    }
    if (arg == "--log") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "watch: --log requires path");
        return 1;
      }
      log_path = ctx.args[i + 1U];
      i += 2U;
      continue;
    }
    break;
  }

  if (i >= ctx.args.size()) {
    shell_write_line(ctx.state, "watch: command required");
    return 1;
  }
  if (ctx.args[i] == "watch") {
    shell_write_line(ctx.state, "watch: nested watch is not allowed");
    return 1;
  }

  const std::string command = join_args(ctx.args, i);
  std::string previous;
  int result = 0;
  if (clear && !shell_supports_ansi(ctx.state)) {
    shell_write_line(ctx.state,
                     "watch: --clear ignored on this transport; serial terminal history "
                     "cannot be cleared by the device.");
    clear = false;
  }
  for (uint32_t cycle = 1U; cycle <= count; ++cycle) {
    if (clear) {
      shell_write(ctx.state, "\033[2J\033[H");
    }
    std::string output;
    const bool ok = execute_line_capture(command.c_str(), &output, false, ctx.transport);
    result = ok ? 0 : 1;
    const bool changed = output != previous;
    if (!diff || changed || cycle == 1U) {
      shell_printf(ctx.state, "[watch] cycle=%lu/%lu interval=%lus command=%s\n",
                   static_cast<unsigned long>(cycle),
                   static_cast<unsigned long>(count),
                   static_cast<unsigned long>(interval_sec),
                   command.c_str());
      if (!output.empty()) {
        shell_write(ctx.state, output.c_str());
        if (output.back() != '\n') shell_write(ctx.state, "\n");
      }
    }
    if (!log_path.empty()) {
      std::string block = "[watch] cycle=" + std::to_string(cycle) + " command=" + command + "\n" + output + "\n";
      append_file(ctx.state, log_path, block);
    }
    if (!until.empty() && output.find(until) != std::string::npos) {
      shell_printf(ctx.state, "watch: matched --until '%s'\n", until.c_str());
      return result;
    }
    previous = output;
    if (cycle < count) {
      vTaskDelay(pdMS_TO_TICKS(interval_sec * 1000U));
    }
  }
  return result;
}

}  // namespace mros::shell
