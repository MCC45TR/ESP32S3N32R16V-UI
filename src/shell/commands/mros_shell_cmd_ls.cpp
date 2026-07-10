#include "src/shell/mros_shell_internal.h"
#include "src/shell/mshell_remote.h"

#include <ctime>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace mros::shell {
namespace {

enum class LsSortKey {
  Name,
  Size,
  Time,
};

struct LsFlags {
  bool show_all = false;
  bool almost_all = false;
  bool long_format = false;
  bool human_readable = false;
  bool recursive = false;
  bool reverse = false;
  bool directory_as_file = false;
  bool classify = false;
  bool single_column = false;
  LsSortKey sort_key = LsSortKey::Name;
};

}  // namespace

void shell_help_ls(ShellState& state) {
  shell_write_line(state, "Usage: ls [OPTION]... [FILE]...");
  shell_write_line(state, "List information about the FILEs.");
  shell_write_line(state, "  -1                         list one file per line");
  shell_write_line(state, "  -A, --almost-all           do not list implied . and ..");
  shell_write_line(state, "  -a, --all                  do not ignore entries starting with .");
  shell_write_line(state, "  -d, --directory            list directories themselves, not their contents");
  shell_write_line(state, "  -F, --classify             append indicator to entries");
  shell_write_line(state, "  -h, --human-readable       with -l, print sizes in a readable form");
  shell_write_line(state, "  -l                         use a long listing format");
  shell_write_line(state, "  -R, --recursive            list subdirectories recursively");
  shell_write_line(state, "  -r, --reverse              reverse sort order");
  shell_write_line(state, "  -S                         sort by file size, largest first");
  shell_write_line(state, "  -t                         sort by modification time, newest first");
}

namespace {

bool parse_ls_args(
    ShellContext& ctx,
    LsFlags* flags,
    std::vector<std::string>* paths,
    bool* help_requested) {
  if (flags == nullptr || paths == nullptr || help_requested == nullptr) {
    return false;
  }

  *help_requested = false;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      *help_requested = true;
      return true;
    }
    if (arg == "--all") {
      flags->show_all = true;
      continue;
    }
    if (arg == "--almost-all") {
      flags->almost_all = true;
      continue;
    }
    if (arg == "--directory") {
      flags->directory_as_file = true;
      continue;
    }
    if (arg == "--classify") {
      flags->classify = true;
      continue;
    }
    if (arg == "--human-readable") {
      flags->human_readable = true;
      continue;
    }
    if (arg == "--recursive") {
      flags->recursive = true;
      continue;
    }
    if (arg == "--reverse") {
      flags->reverse = true;
      continue;
    }
    if (arg == "--size" || arg == "--sort=size") {
      flags->sort_key = LsSortKey::Size;
      continue;
    }
    if (arg == "--time" || arg == "--sort=time") {
      flags->sort_key = LsSortKey::Time;
      continue;
    }
    if (arg == "--sort=name") {
      flags->sort_key = LsSortKey::Name;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case '1':
            flags->single_column = true;
            break;
          case 'A':
            flags->almost_all = true;
            break;
          case 'a':
            flags->show_all = true;
            break;
          case 'd':
            flags->directory_as_file = true;
            break;
          case 'F':
            flags->classify = true;
            break;
          case 'h':
            flags->human_readable = true;
            break;
          case 'l':
            flags->long_format = true;
            break;
          case 'R':
            flags->recursive = true;
            break;
          case 'r':
            flags->reverse = true;
            break;
          case 'S':
            flags->sort_key = LsSortKey::Size;
            break;
          case 't':
            flags->sort_key = LsSortKey::Time;
            break;
          default:
            shell_printf(ctx.state, "ls: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }
    paths->push_back(arg);
  }

  return true;
}

void format_permissions(const ShellFsEntry& entry, char out[11]) {
  if (entry.is_dir) {
    std::snprintf(out, 11, "drwxr-xr-x");
    return;
  }

  if (!entry.has_stat) {
    std::snprintf(out, 11, "-rw-r--r--");
    return;
  }

  const mode_t mode = entry.info.st_mode;
  out[0] = S_ISDIR(mode) ? 'd' : '-';
  out[1] = (mode & S_IRUSR) ? 'r' : '-';
  out[2] = (mode & S_IWUSR) ? 'w' : '-';
  out[3] = (mode & S_IXUSR) ? 'x' : '-';
  out[4] = (mode & S_IRGRP) ? 'r' : '-';
  out[5] = (mode & S_IWGRP) ? 'w' : '-';
  out[6] = (mode & S_IXGRP) ? 'x' : '-';
  out[7] = (mode & S_IROTH) ? 'r' : '-';
  out[8] = (mode & S_IWOTH) ? 'w' : '-';
  out[9] = (mode & S_IXOTH) ? 'x' : '-';
  out[10] = '\0';
}

std::string format_size(const ShellFsEntry& entry, const LsFlags& flags) {
  const unsigned long long size =
      entry.has_stat ? static_cast<unsigned long long>(entry.info.st_size) : 0ULL;
  if (!flags.human_readable) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%llu", size);
    return buffer;
  }

  static const char* units[] = {"B", "K", "M", "G"};
  double scaled = static_cast<double>(size);
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

std::string format_time(const ShellFsEntry& entry) {
  if (!entry.has_stat) {
    return "--- -- --:--";
  }

  std::tm timeinfo {};
  const time_t raw_time = entry.info.st_mtime;
#if defined(_WIN32)
  localtime_s(&timeinfo, &raw_time);
#else
  localtime_r(&raw_time, &timeinfo);
#endif
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%b %d %H:%M", &timeinfo);
  return buffer;
}

std::string display_name_for_entry(const ShellFsEntry& entry, const LsFlags& flags) {
  std::string name = entry.display_name;
  if (!flags.classify) {
    return name;
  }
  if (entry.is_dir) {
    name.push_back('/');
  } else if (entry.has_stat && (entry.info.st_mode & S_IXUSR) != 0) {
    name.push_back('*');
  }
  return name;
}

void print_entry_line(ShellState& state, const ShellFsEntry& entry, const LsFlags& flags) {
  if (!flags.long_format) {
    shell_write_line(state, display_name_for_entry(entry, flags).c_str());
    return;
  }

  char permissions[11] = {};
  format_permissions(entry, permissions);
  const std::string size = format_size(entry, flags);
  const std::string timestamp = format_time(entry);
  const std::string name = display_name_for_entry(entry, flags);
  shell_printf(
      state,
      "%s %3d %8s %s %s\n",
      permissions,
      1,
      size.c_str(),
      timestamp.c_str(),
      name.c_str());
}

bool entry_less_than(const ShellFsEntry& left, const ShellFsEntry& right, const LsFlags& flags) {
  switch (flags.sort_key) {
    case LsSortKey::Size: {
      const long long left_size = left.has_stat ? static_cast<long long>(left.info.st_size) : 0LL;
      const long long right_size = right.has_stat ? static_cast<long long>(right.info.st_size) : 0LL;
      if (left_size != right_size) {
        return left_size > right_size;
      }
      break;
    }
    case LsSortKey::Time: {
      const long long left_time = left.has_stat ? static_cast<long long>(left.info.st_mtime) : 0LL;
      const long long right_time = right.has_stat ? static_cast<long long>(right.info.st_mtime) : 0LL;
      if (left_time != right_time) {
        return left_time > right_time;
      }
      break;
    }
    case LsSortKey::Name:
    default:
      break;
  }
  return left.name < right.name;
}

bool should_keep_entry(const ShellFsEntry& entry, const LsFlags& flags) {
  if (flags.show_all) {
    return true;
  }
  if (flags.almost_all) {
    return entry.name != "." && entry.name != "..";
  }
  return entry.name.empty() || entry.name.front() != '.';
}

bool build_target_entry(
    const ShellState& state,
    const std::string& normalized_path,
    const std::string& display_name,
    ShellFsEntry* out_entry,
    std::string* error) {
  if (out_entry == nullptr) {
    return false;
  }

  if (shell_is_virtual_root(normalized_path)) {
    ShellFsEntry entry {};
    entry.name = "/";
    entry.display_name = display_name;
    entry.path = "/";
    entry.is_dir = true;
    entry.is_virtual = true;
    entry.has_stat = false;
    *out_entry = entry;
    return true;
  }

  bool is_dir = false;
  struct stat info {};
  if (!shell_path_exists(state, normalized_path, &is_dir, &info, error)) {
    return false;
  }

  ShellFsEntry entry {};
  entry.name = shell_basename(normalized_path);
  entry.display_name = display_name;
  entry.path = normalized_path;
  entry.is_dir = is_dir;
  entry.is_virtual = false;
  entry.has_stat = true;
  entry.info = info;
  *out_entry = entry;
  return true;
}

int list_directory(
    ShellState& state,
    const std::string& path,
    const std::string& label,
    const LsFlags& flags,
    const bool print_header) {
  std::vector<ShellFsEntry> entries;
  std::string error;
  const bool include_dot = flags.show_all;
  const bool include_dotdot = flags.show_all && !shell_is_virtual_root(path);
  if (!shell_read_directory(state, path, include_dot, include_dotdot, &entries, &error)) {
    shell_printf(state, "ls: cannot access '%s': %s\n", label.c_str(), error.c_str());
    return 1;
  }

  entries.erase(
      std::remove_if(
          entries.begin(),
          entries.end(),
          [&flags](const ShellFsEntry& entry) {
            return !should_keep_entry(entry, flags);
          }),
      entries.end());

  std::sort(
      entries.begin(),
      entries.end(),
      [&flags](const ShellFsEntry& left, const ShellFsEntry& right) {
        return entry_less_than(left, right, flags);
      });
  if (flags.reverse) {
    std::reverse(entries.begin(), entries.end());
  }

  if (print_header) {
    shell_printf(state, "%s:\n", label.c_str());
  }

  for (const ShellFsEntry& entry : entries) {
    print_entry_line(state, entry, flags);
  }

  if (flags.recursive) {
    for (const ShellFsEntry& entry : entries) {
      if (!entry.is_dir || entry.name == "." || entry.name == "..") {
        continue;
      }
      shell_write_line(state, "");
      list_directory(state, entry.path, entry.path, flags, true);
    }
  }

  return 0;
}

}  // namespace

int shell_cmd_ls(ShellContext& ctx) {
  LsFlags flags {};
  std::vector<std::string> path_args;
  bool help_requested = false;
  if (!parse_ls_args(ctx, &flags, &path_args, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_ls(ctx.state);
    return 0;
  }

  if (path_args.empty()) {
    path_args.push_back(ctx.state.cwd);
  }

  const bool multiple_targets = path_args.size() > 1U;
  int result = 0;

  for (size_t i = 0U; i < path_args.size(); ++i) {
    const std::string& original = path_args[i];
    const std::string normalized = shell_normalize_path(ctx.state, original);
    if (remote::fs_is_remote_path(normalized)) {
      remote::FsMount mount = remote::FsMount::T41;
      (void)remote::fs_mount_for_path(normalized, &mount);
      remote::FsMountSnapshot snap {};
      remote::fs_snapshot(mount, &snap);
      if (i > 0U) {
        shell_write_line(ctx.state, "");
      }
      if (!snap.mounted) {
        shell_printf(
            ctx.state,
            "ls: cannot access '%s': %s (mount %s first)\n",
            original.c_str(),
            snap.error_code,
            remote::fs_mount_name(mount));
        result = 1;
        continue;
      }
      if (!snap.protocol_ready) {
        shell_printf(
            ctx.state,
            "ls: cannot access '%s': %s (t41 MSHELL2 FS support is not active)\n",
            original.c_str(),
            snap.error_code);
        result = 1;
        continue;
      }
      shell_printf(
          ctx.state,
          "ls: cannot access '%s': PEER_PROTOCOL_MISSING (FS_LIST transport pending peer support)\n",
          original.c_str());
      result = 1;
      continue;
    }
    bool is_dir = false;
    std::string error;
    if (!shell_path_exists(ctx.state, normalized, &is_dir, nullptr, &error)) {
      shell_printf(ctx.state, "ls: cannot access '%s': %s\n", original.c_str(), error.c_str());
      result = 1;
      continue;
    }

    if (i > 0U) {
      shell_write_line(ctx.state, "");
    }

    if (is_dir && !flags.directory_as_file) {
      const std::string label = original == ctx.state.cwd ? normalized : original;
      if (list_directory(ctx.state, normalized, label, flags, multiple_targets || flags.recursive) != 0) {
        result = 1;
      }
      continue;
    }

    ShellFsEntry entry {};
    if (!build_target_entry(ctx.state, normalized, original == "." ? normalized : original, &entry, &error)) {
      shell_printf(ctx.state, "ls: cannot access '%s': %s\n", original.c_str(), error.c_str());
      result = 1;
      continue;
    }
    print_entry_line(ctx.state, entry, flags);
  }

  return result;
}

}  // namespace mros::shell
