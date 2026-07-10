#include "src/shell/mros_shell_internal.h"

#include <cstdlib>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct DuOptions {
  bool all_files = false;
  bool human_readable = false;
  bool summary_only = false;
  size_t max_depth = static_cast<size_t>(-1);
};

struct FindOptions {
  std::string name_pattern;
  char type_filter = '\0';
  size_t max_depth = static_cast<size_t>(-1);
  size_t limit = 0U;
  size_t emitted = 0U;
};

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

std::string format_size(const size_t bytes, const bool human_readable) {
  if (!human_readable) {
    return std::to_string(bytes);
  }
  static const char* units[] = {"B", "K", "M", "G"};
  double scaled = static_cast<double>(bytes);
  size_t unit_index = 0U;
  while (scaled >= 1024.0 && unit_index < 3U) {
    scaled /= 1024.0;
    ++unit_index;
  }
  char buffer[32] = {};
  if (scaled >= 10.0 || unit_index == 0U) {
    std::snprintf(buffer, sizeof(buffer), "%.0f%s", scaled, units[unit_index]);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f%s", scaled, units[unit_index]);
  }
  return buffer;
}

bool wildcard_match(const char* pattern, const char* text) {
  if (pattern == nullptr || text == nullptr) {
    return false;
  }
  if (*pattern == '\0') {
    return *text == '\0';
  }
  if (*pattern == '*') {
    return wildcard_match(pattern + 1, text) || (*text != '\0' && wildcard_match(pattern, text + 1));
  }
  if (*pattern == '?') {
    return *text != '\0' && wildcard_match(pattern + 1, text + 1);
  }
  return *pattern == *text && wildcard_match(pattern + 1, text + 1);
}

size_t du_walk(
    ShellState& state,
    const std::string& path,
    const DuOptions& options,
    const size_t depth,
    bool* ok) {
  bool is_dir = false;
  struct stat info {};
  std::string error;
  if (!shell_path_exists(state, path, &is_dir, &info, &error)) {
    shell_printf(state, "du: cannot access '%s': %s\n", shell_visible_path(state, path).c_str(), error.c_str());
    if (ok != nullptr) {
      *ok = false;
    }
    return 0U;
  }

  if (!is_dir) {
    if (options.all_files && (!options.summary_only || depth == 0U)) {
      shell_printf(state, "%8s %s\n",
                   format_size(static_cast<size_t>(info.st_size), options.human_readable).c_str(),
                   shell_visible_path(state, path).c_str());
    }
    return static_cast<size_t>(info.st_size);
  }

  size_t total = 0U;
  std::vector<ShellFsEntry> entries;
  if (!shell_read_directory(state, path, false, false, &entries, &error)) {
    shell_printf(state, "du: cannot read '%s': %s\n", shell_visible_path(state, path).c_str(), error.c_str());
    if (ok != nullptr) {
      *ok = false;
    }
    return 0U;
  }

  for (const ShellFsEntry& entry : entries) {
    total += du_walk(state, entry.path, options, depth + 1U, ok);
  }

  if (depth <= options.max_depth && (!options.summary_only || depth == 0U)) {
    shell_printf(state, "%8s %s\n",
                 format_size(total, options.human_readable).c_str(),
                 shell_visible_path(state, path).c_str());
  }
  return total;
}

void find_walk(
    ShellState& state,
    const std::string& path,
    FindOptions& options,
    const size_t depth,
    int* result) {
  if (options.limit > 0U && options.emitted >= options.limit) {
    return;
  }
  bool is_dir = false;
  struct stat info {};
  std::string error;
  if (!shell_path_exists(state, path, &is_dir, &info, &error)) {
    shell_printf(state, "find: '%s': %s\n", shell_visible_path(state, path).c_str(), error.c_str());
    if (result != nullptr) {
      *result = 1;
    }
    return;
  }

  const std::string name = shell_basename(path);
  const bool name_ok = options.name_pattern.empty() || wildcard_match(options.name_pattern.c_str(), name.c_str());
  const bool type_ok = options.type_filter == '\0' ||
                       (options.type_filter == 'f' && !is_dir) ||
                       (options.type_filter == 'd' && is_dir);
  if (name_ok && type_ok) {
    shell_write_line(state, shell_visible_path(state, path).c_str());
    ++options.emitted;
    if (options.limit > 0U && options.emitted >= options.limit) {
      return;
    }
  }

  if (!is_dir || depth >= options.max_depth) {
    return;
  }

  std::vector<ShellFsEntry> entries;
  if (!shell_read_directory(state, path, false, false, &entries, &error)) {
    shell_printf(state, "find: '%s': %s\n", shell_visible_path(state, path).c_str(), error.c_str());
    if (result != nullptr) {
      *result = 1;
    }
    return;
  }
  std::sort(entries.begin(), entries.end(), [](const ShellFsEntry& left, const ShellFsEntry& right) {
    return left.name < right.name;
  });
  for (const ShellFsEntry& entry : entries) {
    if (options.limit > 0U && options.emitted >= options.limit) {
      break;
    }
    find_walk(state, entry.path, options, depth + 1U, result);
  }
}

}  // namespace

void shell_help_du(ShellState& state) {
  shell_write_line(state, "Usage: du [OPTION]... [FILE]...");
  shell_write_line(state, "Estimate file space usage.");
  shell_write_line(state, "  -a                        write counts for all files, not just directories");
  shell_write_line(state, "  -d, --max-depth NUM       print totals up to NUM directory levels");
  shell_write_line(state, "  -h, --human-readable      print sizes in a readable form");
  shell_write_line(state, "  -s, --summarize           display only a total for each argument");
}

int shell_cmd_du(ShellContext& ctx) {
  DuOptions options {};
  std::vector<std::string> paths;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      shell_help_du(ctx.state);
      return 0;
    }
    if (arg == "-a") {
      options.all_files = true;
      continue;
    }
    if (arg == "-h" || arg == "--human-readable") {
      options.human_readable = true;
      continue;
    }
    if (arg == "-s" || arg == "--summarize") {
      options.summary_only = true;
      continue;
    }
    if (arg == "-d" || arg == "--max-depth") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "du: missing max depth");
        return 1;
      }
      options.max_depth = static_cast<size_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10));
      continue;
    }
    paths.push_back(arg);
  }
  if (paths.empty()) {
    paths.push_back(".");
  }

  int result = 0;
  for (const std::string& path_arg : paths) {
    bool ok = true;
    const std::string normalized = shell_normalize_path(ctx.state, path_arg);
    du_walk(ctx.state, normalized, options, 0U, &ok);
    if (!ok) {
      result = 1;
    }
  }
  return result;
}

void shell_help_find(ShellState& state) {
  shell_write_line(state, "Usage: find [PATH]... [OPTION]...");
  shell_write_line(state, "Search for files in a directory hierarchy.");
  shell_write_line(state, "  -name PATTERN            match file name with * and ?");
  shell_write_line(state, "  -type [f|d]              filter by file or directory");
  shell_write_line(state, "  -maxdepth NUM            descend at most NUM levels");
  shell_write_line(state, "  -limit, --limit NUM      stop after NUM matching entries");
}

int shell_cmd_find(ShellContext& ctx) {
  FindOptions options {};
  std::vector<std::string> roots;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      shell_help_find(ctx.state);
      return 0;
    }
    if (arg == "-name") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "find: missing pattern");
        return 1;
      }
      options.name_pattern = ctx.args[++i];
      continue;
    }
    if (arg == "-type") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "find: missing type");
        return 1;
      }
      const std::string type_value = ctx.args[++i];
      if (type_value != "f" && type_value != "d") {
        shell_write_line(ctx.state, "find: type must be 'f' or 'd'");
        return 1;
      }
      options.type_filter = type_value[0];
      continue;
    }
    if (arg == "-maxdepth") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "find: missing maxdepth value");
        return 1;
      }
      options.max_depth = static_cast<size_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10));
      continue;
    }
    if (arg == "-limit" || arg == "--limit") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "find: missing limit value");
        return 1;
      }
      options.limit = static_cast<size_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10));
      continue;
    }
    roots.push_back(arg);
  }
  if (roots.empty()) {
    roots.push_back(".");
  }

  int result = 0;
  for (const std::string& root : roots) {
    find_walk(ctx.state, shell_normalize_path(ctx.state, root), options, 0U, &result);
    if (options.limit > 0U && options.emitted >= options.limit) {
      break;
    }
  }
  if (options.limit > 0U && options.emitted >= options.limit) {
    shell_printf(ctx.state, "find: stopped after %lu entries (limit)\n", static_cast<unsigned long>(options.limit));
  }
  return result;
}

}  // namespace mros::shell
