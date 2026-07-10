#include "src/shell/mros_shell_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct RmFlags {
  bool dry_run = false;
  bool force = false;
  bool recursive = false;
  bool remove_empty_dir = false;
  bool verbose = false;
};

bool ensure_storage_path_ready(ShellState& state, const std::string& path, std::string* error) {
  if (!shell_is_storage_mounted(state)) {
    if (error != nullptr) {
      *error = "LittleFS is not mounted";
    }
    return false;
  }
  if (!shell_is_user_writable_path(state, path)) {
    if (error != nullptr) {
      *error = "Operation is only supported inside /ESPUSER";
    }
    return false;
  }
  return true;
}

bool parse_rm_args(
    ShellContext& ctx,
    RmFlags* flags,
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
    if (arg == "--force") {
      flags->force = true;
      continue;
    }
    if (arg == "--dry-run" || arg == "--no-act") {
      flags->dry_run = true;
      continue;
    }
    if (arg == "--recursive") {
      flags->recursive = true;
      continue;
    }
    if (arg == "--dir") {
      flags->remove_empty_dir = true;
      continue;
    }
    if (arg == "--verbose") {
      flags->verbose = true;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'f':
            flags->force = true;
            break;
          case 'R':
          case 'r':
            flags->recursive = true;
            break;
          case 'd':
            flags->remove_empty_dir = true;
            break;
          case 'v':
            flags->verbose = true;
            break;
          default:
            shell_printf(ctx.state, "rm: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }
    paths->push_back(arg);
  }
  return true;
}

bool delete_path_recursive(
    ShellState& state,
    const std::string& path,
    const RmFlags& flags,
    std::string* error) {
  if (path == "/" || path == shell_normalize_path(state, state.config.storage_mount_point)) {
    if (error != nullptr) {
      *error = "refusing to remove the shell root or storage mount root";
    }
    return false;
  }

  bool is_dir = false;
  if (!shell_path_exists(state, path, &is_dir, nullptr, error)) {
    return flags.force;
  }

  if (!is_dir) {
    if (::remove(path.c_str()) != 0) {
      if (error != nullptr) {
        *error = ::strerror(errno);
      }
      return false;
    }
    return true;
  }

  if (!flags.recursive && !flags.remove_empty_dir) {
    if (error != nullptr) {
      *error = "Is a directory";
    }
    return false;
  }

  std::vector<ShellFsEntry> entries;
  if (!shell_read_directory(state, path, false, false, &entries, error)) {
    return false;
  }

  if (!entries.empty() && !flags.recursive) {
    if (error != nullptr) {
      *error = "Directory not empty";
    }
    return false;
  }

  if (flags.recursive) {
    for (const ShellFsEntry& entry : entries) {
      if (!delete_path_recursive(state, entry.path, flags, error)) {
        return false;
      }
    }
  }

  if (::rmdir(path.c_str()) != 0) {
    if (error != nullptr) {
      *error = ::strerror(errno);
    }
    return false;
  }
  return true;
}

}  // namespace

void shell_help_rm(ShellState& state) {
  shell_write_line(state, "Usage: rm [OPTION]... [FILE]...");
  shell_write_line(state, "Remove (unlink) the FILE(s).");
  shell_write_line(state, "  -d, --dir                 remove empty directories");
  shell_write_line(state, "      --dry-run, --no-act   show what would be removed, without deleting");
  shell_write_line(state, "  -f, --force               ignore nonexistent files and arguments");
  shell_write_line(state, "  -r, -R, --recursive       remove directories and their contents recursively");
  shell_write_line(state, "  -v, --verbose             explain what is being done");
  shell_write_line(state, "Safety: removal is limited to the writable /ESPUSER area.");
}

int shell_cmd_rm(ShellContext& ctx) {
  RmFlags flags {};
  std::vector<std::string> paths;
  bool help_requested = false;
  if (!parse_rm_args(ctx, &flags, &paths, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_rm(ctx.state);
    return 0;
  }
  if (paths.empty()) {
    shell_write_line(ctx.state, "rm: missing operand");
    return 1;
  }

  int result = 0;
  for (const std::string& path_arg : paths) {
    const std::string path = shell_normalize_path(ctx.state, path_arg);
    std::string error;
    if (!ensure_storage_path_ready(ctx.state, path, &error)) {
      shell_printf(ctx.state, "rm: cannot remove '%s': %s\n", path_arg.c_str(), error.c_str());
      result = 1;
      continue;
    }
    if (flags.dry_run) {
      bool is_dir = false;
      if (!shell_path_exists(ctx.state, path, &is_dir, nullptr, &error)) {
        if (!flags.force) {
          shell_printf(ctx.state, "rm: cannot remove '%s': %s\n", path_arg.c_str(), error.c_str());
          result = 1;
        }
        continue;
      }
      shell_printf(ctx.state, "would remove %s '%s'\n", is_dir ? "directory" : "file", path.c_str());
      continue;
    }
    if (!delete_path_recursive(ctx.state, path, flags, &error)) {
      if (!flags.force || !error.empty()) {
        shell_printf(ctx.state, "rm: cannot remove '%s': %s\n", path_arg.c_str(), error.c_str());
        result = 1;
      }
      continue;
    }
    if (flags.verbose) {
      shell_printf(ctx.state, "removed '%s'\n", path.c_str());
    }
  }
  return result;
}

}  // namespace mros::shell
