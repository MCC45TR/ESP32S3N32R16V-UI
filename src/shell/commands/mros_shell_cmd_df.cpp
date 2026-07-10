#include "src/shell/mros_shell_internal.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "src/drivers/storage/logger_driver.h"

namespace mros::shell {
namespace {

bool storage_info(uint64_t* total_bytes, uint64_t* used_bytes) {
  if (total_bytes == nullptr || used_bytes == nullptr || !logger_storage_ready()) {
    return false;
  }
  return logger_storage_info(total_bytes, used_bytes);
}

struct DfFlags {
  bool human_readable = false;
  bool kibibytes = true;
};

struct DfEntry {
  std::string filesystem;
  std::string mount_point;
  uint64_t total_bytes = 0U;
  uint64_t used_bytes = 0U;
  uint64_t avail_bytes = 0U;
  bool measured = false;
};

std::string format_df_size(const uint64_t bytes, const DfFlags& flags) {
  char buffer[32] = {};
  if (flags.human_readable) {
    static const char* units[] = {"B", "K", "M", "G", "T"};
    double scaled = static_cast<double>(bytes);
    size_t unit_index = 0U;
    while (scaled >= 1024.0 && unit_index < (sizeof(units) / sizeof(units[0])) - 1U) {
      scaled /= 1024.0;
      ++unit_index;
    }
    if (scaled >= 10.0 || unit_index == 0U) {
      std::snprintf(buffer, sizeof(buffer), "%.0f%s", scaled, units[unit_index]);
    } else {
      std::snprintf(buffer, sizeof(buffer), "%.1f%s", scaled, units[unit_index]);
    }
    return buffer;
  }

  const uint64_t blocks = flags.kibibytes ? ((bytes + 1023U) / 1024U) : bytes;
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(blocks));
  return buffer;
}

bool append_unique_target(const std::string& value, std::vector<std::string>* targets) {
  if (targets == nullptr) {
    return false;
  }
  if (std::find(targets->begin(), targets->end(), value) != targets->end()) {
    return false;
  }
  targets->push_back(value);
  return true;
}

bool parse_df_args(
    ShellContext& ctx,
    DfFlags* flags,
    std::vector<std::string>* targets,
    bool* help_requested) {
  if (flags == nullptr || targets == nullptr || help_requested == nullptr) {
    return false;
  }

  *help_requested = false;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      *help_requested = true;
      return true;
    }
    if (arg == "--human-readable") {
      flags->human_readable = true;
      flags->kibibytes = false;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'h':
            flags->human_readable = true;
            flags->kibibytes = false;
            break;
          case 'k':
            flags->kibibytes = true;
            break;
          default:
            shell_printf(ctx.state, "df: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }
    targets->push_back(shell_normalize_path(ctx.state, arg));
  }
  return true;
}

bool build_df_entry(ShellState& state, const std::string& target, DfEntry* entry, std::string* error) {
  if (entry == nullptr) {
    return false;
  }

  if (target == "/") {
    entry->filesystem = "shellfs";
    entry->mount_point = "/";
    entry->measured = false;
    return true;
  }

  if (!shell_is_storage_path(state, target)) {
    if (error != nullptr) {
      *error = "unsupported path";
    }
    return false;
  }

  if (!shell_is_storage_mounted(state)) {
    if (error != nullptr) {
      *error = "LittleFS is not mounted";
    }
    return false;
  }

  const std::string mount_point =
      shell_normalize_path(state, state.config.storage_mount_point != nullptr ? state.config.storage_mount_point : "/littlefs");
  uint64_t total = 0U;
  uint64_t used = 0U;
  if (!storage_info(&total, &used)) {
    if (error != nullptr) {
      *error = "storage info unavailable";
    }
    return false;
  }
  const uint64_t free = total >= used ? (total - used) : 0U;

  entry->filesystem = "littlefs";
  entry->mount_point = "/fs";
  entry->total_bytes = total;
  entry->used_bytes = used;
  entry->avail_bytes = free;
  entry->measured = true;
  return true;
}

}  // namespace

void shell_help_df(ShellState& state) {
  shell_write_line(state, "Usage: df [OPTION]... [FILE]...");
  shell_write_line(state, "Show filesystem space usage for the shell root and LittleFS storage.");
  shell_write_line(state, "  -h, --human-readable      print sizes in a readable form");
  shell_write_line(state, "  -k                        show sizes in 1K blocks (default)");
}

int shell_cmd_df(ShellContext& ctx) {
  DfFlags flags {};
  std::vector<std::string> targets;
  bool help_requested = false;
  if (!parse_df_args(ctx, &flags, &targets, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_df(ctx.state);
    return 0;
  }

  if (targets.empty()) {
    append_unique_target("/", &targets);
    if (shell_is_storage_mounted(ctx.state)) {
      append_unique_target(
          shell_normalize_path(
              ctx.state,
              ctx.state.config.storage_mount_point != nullptr ? ctx.state.config.storage_mount_point : "/littlefs"),
          &targets);
    }
  }

  shell_printf(
      ctx.state,
      "%-12s %10s %10s %10s %5s %s\n",
      "Filesystem",
      flags.human_readable ? "Size" : "1K-blocks",
      "Used",
      "Avail",
      "Use%",
      "Mounted on");

  int result = 0;
  std::vector<std::string> emitted;
  for (const std::string& target : targets) {
    DfEntry entry {};
    std::string error;
    if (!build_df_entry(ctx.state, target, &entry, &error)) {
      shell_printf(ctx.state, "df: %s: %s\n", target.c_str(), error.c_str());
      result = 1;
      continue;
    }
    if (!append_unique_target(entry.mount_point, &emitted)) {
      continue;
    }

    if (!entry.measured) {
      shell_printf(ctx.state, "%-12s %10s %10s %10s %5s %s\n", entry.filesystem.c_str(), "-", "-", "-", "-", entry.mount_point.c_str());
      continue;
    }

    const uint64_t percent = entry.total_bytes == 0U ? 0U : ((entry.used_bytes * 100U) / entry.total_bytes);
    shell_printf(
        ctx.state,
        "%-12s %10s %10s %10s %4llu%% %s\n",
        entry.filesystem.c_str(),
        format_df_size(entry.total_bytes, flags).c_str(),
        format_df_size(entry.used_bytes, flags).c_str(),
        format_df_size(entry.avail_bytes, flags).c_str(),
        static_cast<unsigned long long>(percent),
        entry.mount_point.c_str());
  }

  return result;
}

}  // namespace mros::shell
