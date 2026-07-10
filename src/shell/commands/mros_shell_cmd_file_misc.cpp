#include "src/shell/mros_shell_internal.h"

#include <mbedtls/md.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <string>
#include <vector>

namespace mros::shell {
namespace {

std::string shell_visible_path(const ShellState& state, const std::string& normalized_path) {
  const std::string mount = shell_storage_mount_path(state);
  if (normalized_path == mount) {
    return "/fs";
  }
  if (normalized_path.rfind(mount + "/", 0U) == 0U) {
    return "/fs" + normalized_path.substr(mount.size());
  }
  return normalized_path;
}

bool read_binary_source(
    ShellContext& ctx,
    const std::string& source,
    std::vector<unsigned char>* bytes,
    std::string* label) {
  if (bytes == nullptr || label == nullptr) {
    return false;
  }
  bytes->clear();
  *label = source;
  if (source == "-") {
    if (ctx.stdin_buffer == nullptr) {
      shell_write_line(ctx.state, "stdin is not available");
      return false;
    }
    bytes->assign(ctx.stdin_buffer->begin(), ctx.stdin_buffer->end());
    *label = "(stdin)";
    return true;
  }

  const std::string normalized = shell_normalize_path(ctx.state, source);
  std::string actual_path;
  std::string error;
  if (!shell_openable_file_path(ctx.state, normalized, &actual_path, &error)) {
    shell_printf(ctx.state, "%s: %s\n", source.c_str(), error.c_str());
    return false;
  }

  FILE* file = std::fopen(actual_path.c_str(), "rb");
  if (file == nullptr) {
    shell_printf(ctx.state, "%s: unable to open file\n", source.c_str());
    return false;
  }

  unsigned char buffer[256] = {};
  size_t read_size = 0U;
  while ((read_size = std::fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
    bytes->insert(bytes->end(), buffer, buffer + read_size);
  }
  std::fclose(file);
  *label = shell_visible_path(ctx.state, normalized);
  return true;
}

std::string format_time_value(const time_t raw_time) {
  std::tm time_info {};
#if defined(_WIN32)
  localtime_s(&time_info, &raw_time);
#else
  localtime_r(&raw_time, &time_info);
#endif
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &time_info);
  return buffer;
}

bool parse_double_duration(const std::string& text, double* seconds) {
  if (seconds == nullptr || text.empty()) {
    return false;
  }
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str()) {
    return false;
  }
  std::string suffix = end != nullptr ? std::string(end) : std::string();
  double scale = 1.0;
  if (suffix.empty() || suffix == "s") {
    scale = 1.0;
  } else if (suffix == "ms") {
    scale = 0.001;
  } else if (suffix == "m") {
    scale = 60.0;
  } else {
    return false;
  }
  *seconds = value * scale;
  return *seconds >= 0.0;
}

bool tee_write_target(
    ShellState& state,
    const std::string& target,
    const bool append,
    const std::string& content) {
  std::string normalized_target;
  if (!target.empty() && target.front() != '/' && state.cwd == "/" && shell_is_storage_mounted(state)) {
    normalized_target = shell_normalize_path(state, shell_storage_mount_path(state) + "/" + target);
  } else {
    normalized_target = shell_normalize_path(state, target);
  }
  if (!shell_is_user_writable_path(state, normalized_target)) {
    shell_printf(state, "tee: %s: target must be inside /ESPUSER\n", target.c_str());
    return false;
  }
  bool parent_is_dir = false;
  std::string error;
  if (!shell_path_exists(state, shell_parent_path(normalized_target), &parent_is_dir, nullptr, &error) || !parent_is_dir) {
    shell_printf(state, "tee: %s: %s\n", target.c_str(), error.c_str());
    return false;
  }
  FILE* file = std::fopen(normalized_target.c_str(), append ? "ab" : "wb");
  if (file == nullptr) {
    shell_printf(state, "tee: %s: unable to open file\n", target.c_str());
    return false;
  }
  const bool ok = content.empty() || std::fwrite(content.data(), 1U, content.size(), file) == content.size();
  std::fclose(file);
  return ok;
}

}  // namespace

void shell_help_stat(ShellState& state) {
  shell_write_line(state, "Usage: stat FILE...");
  shell_write_line(state, "Display file or directory status.");
}

int shell_cmd_stat(ShellContext& ctx) {
  if (ctx.args.size() <= 1U) {
    shell_write_line(ctx.state, "stat: missing file operand");
    return 1;
  }
  if (ctx.args[1] == "--help") {
    shell_help_stat(ctx.state);
    return 0;
  }

  int result = 0;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string normalized = shell_normalize_path(ctx.state, ctx.args[i]);
    bool is_dir = false;
    struct stat info {};
    std::string error;
    if (!shell_path_exists(ctx.state, normalized, &is_dir, &info, &error)) {
      shell_printf(ctx.state, "stat: cannot stat '%s': %s\n", ctx.args[i].c_str(), error.c_str());
      result = 1;
      continue;
    }
    shell_printf(ctx.state, "  File: %s\n", shell_visible_path(ctx.state, normalized).c_str());
    shell_printf(ctx.state, "  Type: %s\n", is_dir ? "directory" : "file");
    shell_printf(ctx.state, "  Size: %lld\n", static_cast<long long>(info.st_size));
    shell_printf(ctx.state, "Access: %o\n", static_cast<unsigned>(info.st_mode & 0777));
    shell_printf(ctx.state, "Modify: %s\n", format_time_value(info.st_mtime).c_str());
    shell_printf(ctx.state, "Change: %s\n", format_time_value(info.st_ctime).c_str());
  }
  return result;
}

void shell_help_sha256sum(ShellState& state) {
  shell_write_line(state, "Usage: sha256sum [FILE]...");
  shell_write_line(state, "Compute and print SHA-256 digests.");
}

int shell_cmd_sha256sum(ShellContext& ctx) {
  std::vector<std::string> paths;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_sha256sum(ctx.state);
      return 0;
    }
    paths.push_back(ctx.args[i]);
  }
  if (paths.empty()) {
    paths.push_back("-");
  }

  int result = 0;
  for (const std::string& path : paths) {
    std::vector<unsigned char> bytes;
    std::string label;
    if (!read_binary_source(ctx, path, &bytes, &label)) {
      result = 1;
      continue;
    }
    unsigned char digest[32] = {};
    const mbedtls_md_info_t* md_info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == nullptr ||
        mbedtls_md(md_info, bytes.data(), bytes.size(), digest) != 0) {
      shell_printf(ctx.state, "sha256sum: %s: hash failed\n", label.c_str());
      result = 1;
      continue;
    }
    for (size_t i = 0U; i < sizeof(digest); ++i) {
      shell_printf(ctx.state, "%02x", digest[i]);
    }
    shell_printf(ctx.state, "  %s\n", label.c_str());
  }
  return result;
}

void shell_help_hexdump(ShellState& state) {
  shell_write_line(state, "Usage: hexdump [OPTION]... [FILE]...");
  shell_write_line(state, "Display input in canonical hex+ASCII form.");
  shell_write_line(state, "  -C                        canonical hex+ASCII display");
  shell_write_line(state, "  -n, --length NUM          limit to NUM bytes");
  shell_write_line(state, "  -s, --skip NUM            skip NUM bytes first");
}

int shell_cmd_hexdump(ShellContext& ctx) {
  size_t skip = 0U;
  size_t length = 0U;
  std::vector<std::string> paths;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      shell_help_hexdump(ctx.state);
      return 0;
    }
    if (arg == "-C") {
      continue;
    }
    if (arg == "-n" || arg == "--length") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "hexdump: missing length value");
        return 1;
      }
      length = static_cast<size_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10));
      continue;
    }
    if (arg == "-s" || arg == "--skip") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "hexdump: missing skip value");
        return 1;
      }
      skip = static_cast<size_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10));
      continue;
    }
    paths.push_back(arg);
  }
  if (paths.empty()) {
    paths.push_back("-");
  }

  int result = 0;
  for (const std::string& path : paths) {
    std::vector<unsigned char> bytes;
    std::string label;
    if (!read_binary_source(ctx, path, &bytes, &label)) {
      result = 1;
      continue;
    }
    const size_t start = skip < bytes.size() ? skip : bytes.size();
    const size_t end = length > 0U ? std::min(bytes.size(), start + length) : bytes.size();
    if (paths.size() > 1U) {
      shell_printf(ctx.state, "%s:\n", label.c_str());
    }
    for (size_t offset = start; offset < end; offset += 16U) {
      shell_printf(ctx.state, "%08lx  ", static_cast<unsigned long>(offset));
      for (size_t i = 0U; i < 16U; ++i) {
        if ((offset + i) < end) {
          shell_printf(ctx.state, "%02x ", bytes[offset + i]);
        } else {
          shell_write(ctx.state, "   ");
        }
        if (i == 7U) {
          shell_write(ctx.state, " ");
        }
      }
      shell_write(ctx.state, " |");
      for (size_t i = 0U; i < 16U && (offset + i) < end; ++i) {
        const unsigned char ch = bytes[offset + i];
        shell_printf(ctx.state, "%c", (ch >= 32U && ch <= 126U) ? ch : '.');
      }
      shell_write_line(ctx.state, "|");
    }
  }
  return result;
}

void shell_help_sleep(ShellState& state) {
  shell_write_line(state, "Usage: sleep NUMBER[SUFFIX]");
  shell_write_line(state, "Delay execution for the given duration.");
  shell_write_line(state, "Suffixes: ms, s, m");
}

int shell_cmd_sleep(ShellContext& ctx) {
  if (ctx.args.size() != 2U || ctx.args[1] == "--help") {
    if (ctx.args.size() == 2U && ctx.args[1] == "--help") {
      shell_help_sleep(ctx.state);
      return 0;
    }
    shell_write_line(ctx.state, "sleep: missing operand");
    return 1;
  }
  double seconds = 0.0;
  if (!parse_double_duration(ctx.args[1], &seconds)) {
    shell_write_line(ctx.state, "sleep: invalid duration");
    return 1;
  }
  const uint32_t delay_ms = static_cast<uint32_t>(seconds * 1000.0);
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
  return 0;
}

void shell_help_tee(ShellState& state) {
  shell_write_line(state, "Usage: COMMAND | tee [OPTION]... [FILE]...");
  shell_write_line(state, "Copy standard input to standard output and files.");
  shell_write_line(state, "  -a, --append             append to the given FILEs");
}

int shell_cmd_tee(ShellContext& ctx) {
  bool append = false;
  std::vector<std::string> paths;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      shell_help_tee(ctx.state);
      return 0;
    }
    if (arg == "-a" || arg == "--append") {
      append = true;
      continue;
    }
    paths.push_back(arg);
  }

  if (ctx.stdin_buffer == nullptr) {
    shell_write_line(ctx.state, "tee: stdin is required");
    return 1;
  }

  shell_write(ctx.state, ctx.stdin_buffer->c_str());
  int result = 0;
  for (const std::string& path : paths) {
    if (!tee_write_target(ctx.state, path, append, *ctx.stdin_buffer)) {
      result = 1;
    }
  }
  return result;
}

void shell_help_eof(ShellState& state) {
  shell_write_line(state, "Usage: EOF");
  shell_write_line(state, "Compatibility no-op sentinel for simple shell scripts.");
}

int shell_cmd_eof(ShellContext& ctx) {
  (void)ctx;
  return 0;
}

}  // namespace mros::shell
