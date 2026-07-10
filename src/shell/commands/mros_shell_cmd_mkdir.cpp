#include "src/shell/mros_shell_internal.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct MkdirFlags {
  bool parents = false;
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

bool parse_mkdir_args(
    ShellContext& ctx,
    MkdirFlags* flags,
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
    if (arg == "--parents") {
      flags->parents = true;
      continue;
    }
    if (arg == "--verbose") {
      flags->verbose = true;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'p':
            flags->parents = true;
            break;
          case 'v':
            flags->verbose = true;
            break;
          default:
            shell_printf(ctx.state, "mkdir: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }
    paths->push_back(arg);
  }
  return true;
}

bool create_directory_recursive(
    ShellState& state,
    const std::string& path,
    const MkdirFlags& flags,
    std::string* error) {
  bool is_dir = false;
  if (shell_path_exists(state, path, &is_dir, nullptr, nullptr)) {
    if (!is_dir) {
      if (error != nullptr) {
        *error = "File exists";
      }
      return false;
    }
    return flags.parents;
  }

  const std::string parent = shell_parent_path(path);
  if (parent != path) {
    bool parent_is_dir = false;
    if (!shell_path_exists(state, parent, &parent_is_dir, nullptr, nullptr)) {
      if (!flags.parents) {
        if (error != nullptr) {
          *error = "No such file or directory";
        }
        return false;
      }
      if (!create_directory_recursive(state, parent, flags, error)) {
        return false;
      }
    } else if (!parent_is_dir) {
      if (error != nullptr) {
        *error = "Not a directory";
      }
      return false;
    }
  }

  if (::mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) {
    if (error != nullptr) {
      *error = ::strerror(errno);
    }
    return false;
  }
  return true;
}

}  // namespace

void shell_help_mkdir(ShellState& state) {
  shell_write_line(state, "Usage: mkdir [OPTION]... DIRECTORY...");
  shell_write_line(state, "Create the DIRECTORY(ies), if they do not already exist.");
  shell_write_line(state, "  -p, --parents             no error if existing, make parent directories as needed");
  shell_write_line(state, "  -v, --verbose             print a message for each created directory");
}

int shell_cmd_mkdir(ShellContext& ctx) {
  MkdirFlags flags {};
  std::vector<std::string> paths;
  bool help_requested = false;
  if (!parse_mkdir_args(ctx, &flags, &paths, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_mkdir(ctx.state);
    return 0;
  }
  if (paths.empty()) {
    shell_write_line(ctx.state, "mkdir: missing operand");
    return 1;
  }

  int result = 0;
  for (const std::string& path_arg : paths) {
    const std::string path = shell_normalize_path(ctx.state, path_arg);
    std::string error;
    if (!ensure_storage_path_ready(ctx.state, path, &error)) {
      shell_printf(ctx.state, "mkdir: cannot create directory '%s': %s\n", path_arg.c_str(), error.c_str());
      result = 1;
      continue;
    }
    if (!create_directory_recursive(ctx.state, path, flags, &error)) {
      shell_printf(ctx.state, "mkdir: cannot create directory '%s': %s\n", path_arg.c_str(), error.c_str());
      result = 1;
      continue;
    }
    if (flags.verbose) {
      shell_printf(ctx.state, "mkdir: created directory '%s'\n", path.c_str());
    }
  }
  return result;
}

}  // namespace mros::shell
