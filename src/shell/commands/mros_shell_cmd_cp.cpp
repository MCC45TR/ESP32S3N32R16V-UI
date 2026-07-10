#include "src/shell/mros_shell_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct CopyFlags {
  bool dry_run = false;
  bool force = false;
  bool no_clobber = false;
  bool recursive = false;
  bool verbose = false;
};

std::string join_child_path(const std::string& parent, const std::string& child) {
  if (parent == "/") {
    return "/" + child;
  }
  return parent + "/" + child;
}

bool starts_with_path(const std::string& value, const std::string& prefix) {
  return value == prefix ||
         (value.size() > prefix.size() && value.compare(0U, prefix.size(), prefix) == 0 && value[prefix.size()] == '/');
}

bool parse_cp_args(
    ShellContext& ctx,
    CopyFlags* flags,
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
    if (arg == "--no-clobber") {
      flags->no_clobber = true;
      continue;
    }
    if (arg == "--force") {
      flags->force = true;
      flags->no_clobber = false;
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
    if (arg == "--verbose") {
      flags->verbose = true;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'f':
            flags->force = true;
            flags->no_clobber = false;
            break;
          case 'n':
            if (!flags->force) {
              flags->no_clobber = true;
            }
            break;
          case 'R':
          case 'r':
            flags->recursive = true;
            break;
          case 'v':
            flags->verbose = true;
            break;
          default:
            shell_printf(ctx.state, "cp: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }
    paths->push_back(arg);
  }
  return true;
}

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

bool ensure_parent_directory(const ShellState& state, const std::string& path, std::string* error) {
  const std::string parent = shell_parent_path(path);
  bool is_dir = false;
  return shell_path_exists(state, parent, &is_dir, nullptr, error) && is_dir;
}

bool copy_file_bytes(const std::string& source, const std::string& target) {
  FILE* src = std::fopen(source.c_str(), "rb");
  if (src == nullptr) {
    return false;
  }
  FILE* dst = std::fopen(target.c_str(), "wb");
  if (dst == nullptr) {
    std::fclose(src);
    return false;
  }

  char buffer[512] = {};
  size_t read_size = 0U;
  bool ok = true;
  while ((read_size = std::fread(buffer, 1U, sizeof(buffer), src)) > 0U) {
    if (std::fwrite(buffer, 1U, read_size, dst) != read_size) {
      ok = false;
      break;
    }
  }

  if (std::ferror(src) != 0) {
    ok = false;
  }

  std::fclose(src);
  std::fclose(dst);
  return ok;
}

bool copy_entry(
    ShellState& state,
    const std::string& source,
    const std::string& target,
    const CopyFlags& flags,
    std::string* error) {
  bool source_is_dir = false;
  struct stat source_info {};
  if (!shell_path_exists(state, source, &source_is_dir, &source_info, error)) {
    return false;
  }

  if (!ensure_storage_path_ready(state, source, error) || !ensure_storage_path_ready(state, target, error)) {
    return false;
  }

  if (source_is_dir) {
    if (!flags.recursive) {
      if (error != nullptr) {
        *error = "omitting directory";
      }
      return false;
    }
    if (starts_with_path(target, source)) {
      if (error != nullptr) {
        *error = "cannot copy a directory into itself";
      }
      return false;
    }

    bool target_exists = false;
    bool target_is_dir = false;
    std::string target_error;
    target_exists = shell_path_exists(state, target, &target_is_dir, nullptr, &target_error);
    if (target_exists && !target_is_dir) {
      if (error != nullptr) {
        *error = "cannot overwrite non-directory with directory";
      }
      return false;
    }
    if (flags.dry_run) {
      shell_printf(state, "would copy directory '%s' -> '%s'\n", source.c_str(), target.c_str());
      return true;
    }
    if (!target_exists) {
      if (!ensure_parent_directory(state, target, error)) {
        return false;
      }
      if (::mkdir(target.c_str(), 0777) != 0 && errno != EEXIST) {
        if (error != nullptr) {
          *error = ::strerror(errno);
        }
        return false;
      }
    }

    std::vector<ShellFsEntry> entries;
    if (!shell_read_directory(state, source, false, false, &entries, error)) {
      return false;
    }
    for (const ShellFsEntry& entry : entries) {
      const std::string child_target = join_child_path(target, entry.name);
      if (!copy_entry(state, entry.path, child_target, flags, error)) {
        return false;
      }
    }
    if (flags.verbose) {
      shell_printf(state, "'%s' -> '%s'\n", source.c_str(), target.c_str());
    }
    return true;
  }

  bool target_exists = false;
  bool target_is_dir = false;
  std::string target_error;
  target_exists = shell_path_exists(state, target, &target_is_dir, nullptr, &target_error);
  if (target_exists && target_is_dir) {
    if (error != nullptr) {
      *error = "cannot overwrite directory with non-directory";
    }
    return false;
  }
  if (target_exists && flags.no_clobber) {
    return true;
  }
  if (!ensure_parent_directory(state, target, error)) {
    return false;
  }
  if (flags.dry_run) {
    shell_printf(state, "would copy file '%s' -> '%s'\n", source.c_str(), target.c_str());
    return true;
  }
  if (!copy_file_bytes(source, target)) {
    if (error != nullptr) {
      *error = "copy failed";
    }
    return false;
  }
  if (flags.verbose) {
    shell_printf(state, "'%s' -> '%s'\n", source.c_str(), target.c_str());
  }
  return true;
}

bool resolve_target_path(
    ShellState& state,
    const std::string& source,
    const std::string& destination_arg,
    const bool multiple_sources,
    std::string* out_target,
    std::string* error) {
  if (out_target == nullptr) {
    return false;
  }

  const std::string destination = shell_normalize_path(state, destination_arg);
  if (!ensure_storage_path_ready(state, destination, error)) {
    return false;
  }

  bool destination_is_dir = false;
  std::string destination_error;
  const bool destination_exists =
      shell_path_exists(state, destination, &destination_is_dir, nullptr, &destination_error);

  if (multiple_sources) {
    if (!destination_exists || !destination_is_dir) {
      if (error != nullptr) {
        *error = "target must be an existing directory when copying multiple files";
      }
      return false;
    }
    *out_target = join_child_path(destination, shell_basename(source));
    return true;
  }

  if (destination_exists && destination_is_dir) {
    *out_target = join_child_path(destination, shell_basename(source));
  } else {
    *out_target = destination;
  }
  return true;
}

}  // namespace

void shell_help_cp(ShellState& state) {
  shell_write_line(state, "Usage: cp [OPTION]... SOURCE... DEST");
  shell_write_line(state, "Copy SOURCE to DEST.");
  shell_write_line(state, "      --dry-run, --no-act   show copy plan without writing");
  shell_write_line(state, "  -f, --force               make overwrite intent explicit");
  shell_write_line(state, "  -n, --no-clobber          do not overwrite an existing file");
  shell_write_line(state, "  -r, -R, --recursive       copy directories recursively");
  shell_write_line(state, "  -v, --verbose             print each copied path");
  shell_write_line(state, "Safety: writes are limited to the writable /ESPUSER area.");
}

int shell_cmd_cp(ShellContext& ctx) {
  CopyFlags flags {};
  std::vector<std::string> paths;
  bool help_requested = false;
  if (!parse_cp_args(ctx, &flags, &paths, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_cp(ctx.state);
    return 0;
  }
  if (paths.size() < 2U) {
    shell_write_line(ctx.state, "cp: missing file operand");
    return 1;
  }

  const std::string destination_arg = paths.back();
  paths.pop_back();
  const bool multiple_sources = paths.size() > 1U;
  int result = 0;

  for (const std::string& source_arg : paths) {
    const std::string source = shell_normalize_path(ctx.state, source_arg);
    std::string target;
    std::string error;
    if (!resolve_target_path(ctx.state, source, destination_arg, multiple_sources, &target, &error)) {
      shell_printf(ctx.state, "cp: %s\n", error.c_str());
      return 1;
    }
    if (!copy_entry(ctx.state, source, target, flags, &error)) {
      shell_printf(ctx.state, "cp: cannot copy '%s' to '%s': %s\n", source_arg.c_str(), target.c_str(), error.c_str());
      result = 1;
    }
  }

  return result;
}

}  // namespace mros::shell
