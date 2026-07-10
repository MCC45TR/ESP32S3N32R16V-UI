#include "src/shell/mros_shell_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct MoveFlags {
  bool dry_run = false;
  bool force = false;
  bool no_clobber = false;
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

bool parse_mv_args(
    ShellContext& ctx,
    MoveFlags* flags,
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
          case 'v':
            flags->verbose = true;
            break;
          default:
            shell_printf(ctx.state, "mv: invalid option -- '%c'\n", arg[j]);
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
        *error = "target must be an existing directory when moving multiple files";
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

void shell_help_mv(ShellState& state) {
  shell_write_line(state, "Usage: mv [OPTION]... SOURCE... DEST");
  shell_write_line(state, "Move SOURCE to DEST.");
  shell_write_line(state, "      --dry-run, --no-act   show move plan without renaming");
  shell_write_line(state, "  -f, --force               make overwrite intent explicit");
  shell_write_line(state, "  -n, --no-clobber          do not overwrite an existing file");
  shell_write_line(state, "  -v, --verbose             print each moved path");
  shell_write_line(state, "Safety: moves are limited to the writable /ESPUSER area.");
}

int shell_cmd_mv(ShellContext& ctx) {
  MoveFlags flags {};
  std::vector<std::string> paths;
  bool help_requested = false;
  if (!parse_mv_args(ctx, &flags, &paths, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_mv(ctx.state);
    return 0;
  }
  if (paths.size() < 2U) {
    shell_write_line(ctx.state, "mv: missing file operand");
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
      shell_printf(ctx.state, "mv: %s\n", error.c_str());
      return 1;
    }
    if (!ensure_storage_path_ready(ctx.state, source, &error)) {
      shell_printf(ctx.state, "mv: %s\n", error.c_str());
      result = 1;
      continue;
    }

    bool source_is_dir = false;
    if (!shell_path_exists(ctx.state, source, &source_is_dir, nullptr, &error)) {
      shell_printf(ctx.state, "mv: cannot stat '%s': %s\n", source_arg.c_str(), error.c_str());
      result = 1;
      continue;
    }
    if (starts_with_path(target, source)) {
      shell_printf(ctx.state, "mv: cannot move '%s' into itself\n", source_arg.c_str());
      result = 1;
      continue;
    }

    bool target_exists = false;
    bool target_is_dir = false;
    std::string target_error;
    target_exists = shell_path_exists(ctx.state, target, &target_is_dir, nullptr, &target_error);
    if (target_exists && flags.no_clobber) {
      continue;
    }
    if (flags.dry_run) {
      shell_printf(ctx.state, "would move '%s' -> '%s'\n", source.c_str(), target.c_str());
      continue;
    }

    if (::rename(source.c_str(), target.c_str()) != 0) {
      shell_printf(
          ctx.state,
          "mv: cannot move '%s' to '%s': %s\n",
          source_arg.c_str(),
          target.c_str(),
          ::strerror(errno));
      result = 1;
      continue;
    }

    if (flags.verbose) {
      shell_printf(ctx.state, "'%s' -> '%s'\n", source.c_str(), target.c_str());
    }
  }

  return result;
}

}  // namespace mros::shell
