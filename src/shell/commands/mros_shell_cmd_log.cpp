#include "src/shell/mros_shell_internal.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include <string>
#include <vector>

namespace mros::shell {
namespace {

constexpr size_t kLogReadBufferSize = 8192U;

struct LogOptions {
  size_t line_count = 100U;
  bool follow = false;
  size_t cycles = 0U;
  uint32_t interval_ms = 700U;
  bool ignore_case = false;
  std::string filter;
  std::string source;
  std::string export_path;
  bool mute = false;
  bool level = false;
};

char to_lower_ascii(const char ch) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

bool contains_text(const std::string& text, const std::string& pattern, const bool ignore_case) {
  if (pattern.empty()) {
    return true;
  }
  if (!ignore_case) {
    return text.find(pattern) != std::string::npos;
  }

  std::string text_lower = text;
  std::string pattern_lower = pattern;
  std::transform(text_lower.begin(), text_lower.end(), text_lower.begin(), to_lower_ascii);
  std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(), to_lower_ascii);
  return text_lower.find(pattern_lower) != std::string::npos;
}

std::string filter_log_lines(const std::string& input, const std::string& filter, const bool ignore_case) {
  if (filter.empty()) {
    return input;
  }

  std::string output;
  size_t start = 0U;
  while (start <= input.size()) {
    const size_t newline = input.find('\n', start);
    const bool has_newline = newline != std::string::npos;
    const size_t end = has_newline ? newline : input.size();
    const std::string line = input.substr(start, end - start);
    if (contains_text(line, filter, ignore_case)) {
      output += line;
      if (has_newline) {
        output.push_back('\n');
      }
    }
    if (!has_newline) {
      break;
    }
    start = newline + 1U;
  }
  return output;
}

bool parse_size_argument(const std::string& arg, size_t* out_value) {
  if (out_value == nullptr) {
    return false;
  }
  char* end = nullptr;
  const long parsed = std::strtol(arg.c_str(), &end, 10);
  if (end == arg.c_str() || (end != nullptr && *end != '\0') || parsed <= 0L) {
    return false;
  }
  *out_value = static_cast<size_t>(parsed);
  return true;
}

bool parse_u32_argument(const std::string& arg, uint32_t* out_value) {
  if (out_value == nullptr) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(arg.c_str(), &end, 10);
  if (end == arg.c_str() || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out_value = static_cast<uint32_t>(parsed);
  return true;
}

bool parse_log_args(ShellContext& ctx, LogOptions* options) {
  if (options == nullptr) {
    return false;
  }

  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      shell_help_log(ctx.state);
      return false;
    }
    if (arg == "tail") {
      continue;
    }
    if (arg == "follow") {
      options->follow = true;
      continue;
    }
    if (arg == "source") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "log: source requires a name");
        return false;
      }
      options->source = ctx.args[i + 1U];
      ++i;
      continue;
    }
    if (arg == "-n") {
      if ((i + 1U) >= ctx.args.size() || !parse_size_argument(ctx.args[i + 1U], &options->line_count)) {
        shell_write_line(ctx.state, "log: invalid line count");
        return false;
      }
      ++i;
      continue;
    }
    if (arg == "-f" || arg == "--follow") {
      options->follow = true;
      continue;
    }
    if (arg == "--cycles") {
      if ((i + 1U) >= ctx.args.size() || !parse_size_argument(ctx.args[i + 1U], &options->cycles)) {
        shell_write_line(ctx.state, "log: invalid cycle count");
        return false;
      }
      ++i;
      continue;
    }
    if (arg == "--interval-ms") {
      if ((i + 1U) >= ctx.args.size() || !parse_u32_argument(ctx.args[i + 1U], &options->interval_ms)) {
        shell_write_line(ctx.state, "log: invalid interval value");
        return false;
      }
      ++i;
      continue;
    }
    if (arg == "-g" || arg == "--filter") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "log: option requires a pattern -- 'filter'");
        return false;
      }
      options->filter = ctx.args[i + 1U];
      ++i;
      continue;
    }
    if (arg == "--source") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "log: source requires a name");
        return false;
      }
      options->source = ctx.args[i + 1U];
      ++i;
      continue;
    }
    if (arg == "export" || arg == "--export") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "log: export requires a path");
        return false;
      }
      options->export_path = ctx.args[i + 1U];
      ++i;
      continue;
    }
    if (arg == "mute") {
      options->mute = true;
      if ((i + 1U) < ctx.args.size()) {
        options->source = ctx.args[i + 1U];
        ++i;
      }
      continue;
    }
    if (arg == "level") {
      options->level = true;
      if ((i + 2U) < ctx.args.size()) {
        options->source = ctx.args[i + 1U];
        options->filter = ctx.args[i + 2U];
        i += 2U;
      }
      continue;
    }
    if (arg == "-i") {
      options->ignore_case = true;
      continue;
    }

    shell_printf(ctx.state, "log: unknown option or argument '%s'\n", arg.c_str());
    return false;
  }

  if (options->follow && options->cycles == 0U) {
    options->cycles = 30U;
  }
  return true;
}

bool read_logs(ShellContext& ctx, const size_t line_count, std::string* out_text) {
  if (out_text == nullptr || ctx.state.config.log_read_callback == nullptr) {
    return false;
  }

  char* message = static_cast<char*>(
      heap_caps_malloc(kLogReadBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (message == nullptr) {
    message = static_cast<char*>(heap_caps_malloc(
        kLogReadBufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (message == nullptr) {
    shell_write_line(ctx.state, "log: unable to allocate read buffer");
    return false;
  }
  message[0] = '\0';
  const bool ok =
      ctx.state.config.log_read_callback(line_count, message, kLogReadBufferSize,
                                         ctx.state.config.user_data);
  if (!ok) {
    shell_write_line(ctx.state, message[0] != '\0' ? message : "log: unable to read logs");
    heap_caps_free(message);
    return false;
  }
  *out_text = message;
  heap_caps_free(message);
  return true;
}

std::string source_pattern(const std::string& source) {
  if (source == "shell") return "mshell";
  if (source == "system") return "[RTOS]";
  if (source == "wifi") return "wifi";
  if (source == "bus") return "SPI";
  if (source == "robot") return "TRAJ";
  if (source == "web") return "[WEB]";
  return source;
}

bool export_logs(ShellContext& ctx, const std::string& path_arg, const std::string& text) {
  const std::string path = shell_normalize_path(ctx.state, path_arg);
  if (!shell_is_user_writable_path(ctx.state, path)) {
    shell_write_line(ctx.state, "log export: path must be inside /ESPUSER");
    return false;
  }
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    shell_write_line(ctx.state, "log export: cannot open file");
    return false;
  }
  const bool ok = std::fwrite(text.data(), 1U, text.size(), file) == text.size();
  std::fclose(file);
  shell_printf(ctx.state, "log export: %s %s\n", ok ? "wrote" : "failed", path_arg.c_str());
  return ok;
}

}  // namespace

void shell_help_log(ShellState& state) {
  shell_write_line(state, "Usage: log [tail|follow] [OPTION]...");
  shell_write_line(state, "Show recent console log lines.");
  shell_write_line(state, "  tail                      alias for recent log output");
  shell_write_line(state, "  follow                    alias for --follow");
  shell_write_line(state, "  -n NUM                     output the last NUM lines");
  shell_write_line(state, "  -g, --filter TEXT          only show lines containing TEXT");
  shell_write_line(state, "  -i                         case-insensitive filter match");
  shell_write_line(state, "  -f, --follow               watch logs for a limited follow window");
  shell_write_line(state, "      --cycles NUM           follow iterations (default: 30)");
  shell_write_line(state, "      --interval-ms NUM      delay between follow reads");
  shell_write_line(state, "      --source NAME          shell|system|wifi|bus|robot|web source filter");
  shell_write_line(state, "      export PATH            write filtered logs to /ESPUSER file");
  shell_write_line(state, "      mute NAME              staged source mute setting");
  shell_write_line(state, "      level NAME LEVEL       staged source log level setting");
}

int shell_cmd_log(ShellContext& ctx) {
  if (ctx.state.config.log_read_callback == nullptr) {
    shell_write_line(ctx.state, "log: log callback is not configured");
    return 1;
  }

  LogOptions options {};
  if (!parse_log_args(ctx, &options)) {
    for (size_t i = 1U; i < ctx.args.size(); ++i) {
      if (ctx.args[i] == "--help") {
        return 0;
      }
    }
    return 1;
  }
  if (options.mute) {
    shell_printf(ctx.state, "log mute: staged source=%s (runtime filtering is not global yet)\n",
                 options.source.empty() ? "(none)" : options.source.c_str());
    return 0;
  }
  if (options.level) {
    shell_printf(ctx.state, "log level: staged source=%s level=%s\n",
                 options.source.empty() ? "(none)" : options.source.c_str(),
                 options.filter.empty() ? "(none)" : options.filter.c_str());
    return 0;
  }
  if (!options.source.empty() && options.filter.empty()) {
    options.filter = source_pattern(options.source);
    options.ignore_case = true;
  }

  std::string previous;
  const size_t rounds = options.follow ? options.cycles : 1U;
  for (size_t round = 0U; round < rounds; ++round) {
    std::string raw_logs;
    if (!read_logs(ctx, options.line_count, &raw_logs)) {
      return 1;
    }

    const std::string current = filter_log_lines(raw_logs, options.filter, options.ignore_case);
    if (!options.export_path.empty() && !options.follow) {
      return export_logs(ctx, options.export_path, current) ? 0 : 1;
    }
    if (!options.follow) {
      if (!current.empty()) {
        shell_write(ctx.state, current.c_str());
        if (current.back() != '\n') {
          shell_write(ctx.state, "\n");
        }
      }
      return 0;
    }

    if (round == 0U) {
      if (!current.empty()) {
        shell_write(ctx.state, current.c_str());
        if (current.back() != '\n') {
          shell_write(ctx.state, "\n");
        }
      }
    } else if (current != previous && !current.empty()) {
      size_t prefix = 0U;
      const size_t max_prefix = std::min(previous.size(), current.size());
      while (prefix < max_prefix && previous[prefix] == current[prefix]) {
        ++prefix;
      }
      if (prefix < current.size()) {
        shell_write(ctx.state, current.substr(prefix).c_str());
        if (current.back() != '\n') {
          shell_write(ctx.state, "\n");
        }
      }
    }

    previous = current;
    if ((round + 1U) < rounds && options.interval_ms > 0U) {
      vTaskDelay(pdMS_TO_TICKS(options.interval_ms));
    }
  }
  return 0;
}

}  // namespace mros::shell
