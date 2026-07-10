#include "src/shell/mros_shell_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct TouchFlags {
  bool no_create = false;
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

bool parse_touch_args(
    ShellContext& ctx,
    TouchFlags* flags,
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
    if (arg == "--no-create") {
      flags->no_create = true;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'a':
          case 'm':
            break;
          case 'c':
            flags->no_create = true;
            break;
          default:
            shell_printf(ctx.state, "touch: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }
    paths->push_back(arg);
  }
  return true;
}

}  // namespace

void shell_help_touch(ShellState& state) {
  shell_write_line(state, "Usage: touch [OPTION]... FILE...");
  shell_write_line(state, "Update the access and modification times of each FILE.");
  shell_write_line(state, "  -a                         change only the access time");
  shell_write_line(state, "  -c, --no-create            do not create any files");
  shell_write_line(state, "  -m                         change only the modification time");
}

int shell_cmd_touch(ShellContext& ctx) {
  TouchFlags flags {};
  std::vector<std::string> paths;
  bool help_requested = false;
  if (!parse_touch_args(ctx, &flags, &paths, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_touch(ctx.state);
    return 0;
  }
  if (paths.empty()) {
    shell_write_line(ctx.state, "touch: missing file operand");
    return 1;
  }

  int result = 0;
  for (const std::string& path_arg : paths) {
    const std::string path = shell_normalize_path(ctx.state, path_arg);
    std::string error;
    if (!ensure_storage_path_ready(ctx.state, path, &error)) {
      shell_printf(ctx.state, "touch: cannot touch '%s': %s\n", path_arg.c_str(), error.c_str());
      result = 1;
      continue;
    }

    bool is_dir = false;
    if (shell_path_exists(ctx.state, path, &is_dir, nullptr, nullptr)) {
      if (is_dir) {
        shell_printf(ctx.state, "touch: cannot touch '%s': Is a directory\n", path_arg.c_str());
        result = 1;
        continue;
      }
      FILE* file = std::fopen(path.c_str(), "ab");
      if (file == nullptr) {
        shell_printf(ctx.state, "touch: cannot touch '%s': %s\n", path_arg.c_str(), ::strerror(errno));
        result = 1;
        continue;
      }
      std::fclose(file);
      continue;
    }

    if (flags.no_create) {
      continue;
    }

    bool parent_is_dir = false;
    if (!shell_path_exists(ctx.state, shell_parent_path(path), &parent_is_dir, nullptr, &error) || !parent_is_dir) {
      shell_printf(ctx.state, "touch: cannot touch '%s': %s\n", path_arg.c_str(), error.c_str());
      result = 1;
      continue;
    }

    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
      shell_printf(ctx.state, "touch: cannot touch '%s': %s\n", path_arg.c_str(), ::strerror(errno));
      result = 1;
      continue;
    }
    std::fclose(file);
  }

  return result;
}

}  // namespace mros::shell
