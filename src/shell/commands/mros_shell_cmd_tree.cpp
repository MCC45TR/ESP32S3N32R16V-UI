#include "src/shell/mros_shell_internal.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct TreeOptions {
  bool show_all = false;
  uint8_t max_depth = 4U;
  std::string path;
};

struct TreeCounts {
  size_t dirs = 0U;
  size_t files = 0U;
};

bool parse_depth(const std::string& text, uint8_t* depth) {
  if (depth == nullptr || text.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || (end != nullptr && *end != '\0') || parsed > 12UL) {
    return false;
  }
  *depth = static_cast<uint8_t>(parsed);
  return true;
}

bool parse_tree_args(ShellContext& ctx, TreeOptions* opts, bool* help_requested) {
  if (opts == nullptr || help_requested == nullptr) {
    return false;
  }
  *help_requested = false;
  opts->path = ctx.state.cwd;

  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help" || arg == "-h") {
      *help_requested = true;
      return true;
    }
    if (arg == "-a" || arg == "--all") {
      opts->show_all = true;
      continue;
    }
    if (arg == "-L" || arg == "--level") {
      if ((i + 1U) >= ctx.args.size() || !parse_depth(ctx.args[i + 1U], &opts->max_depth)) {
        shell_write_line(ctx.state, "tree: invalid depth for -L");
        return false;
      }
      ++i;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      shell_printf(ctx.state, "tree: invalid option '%s'\n", arg.c_str());
      return false;
    }
    opts->path = arg;
  }
  return true;
}

bool should_show_entry(const ShellFsEntry& entry, const TreeOptions& opts) {
  return opts.show_all || entry.name.empty() || entry.name.front() != '.';
}

void sort_entries(std::vector<ShellFsEntry>* entries) {
  if (entries == nullptr) {
    return;
  }
  std::sort(entries->begin(), entries->end(), [](const ShellFsEntry& left, const ShellFsEntry& right) {
    if (left.is_dir != right.is_dir) {
      return left.is_dir && !right.is_dir;
    }
    return left.name < right.name;
  });
}

int print_tree_dir(
    ShellState& state,
    const std::string& path,
    const std::string& prefix,
    const TreeOptions& opts,
    const uint8_t depth,
    TreeCounts* counts) {
  if (depth >= opts.max_depth) {
    return 0;
  }

  std::vector<ShellFsEntry> entries;
  std::string error;
  if (!shell_read_directory(state, path, false, false, &entries, &error)) {
    shell_printf(state, "%s[error opening dir: %s]\n", prefix.c_str(), error.c_str());
    return 1;
  }

  entries.erase(
      std::remove_if(entries.begin(), entries.end(), [&](const ShellFsEntry& entry) {
        return !should_show_entry(entry, opts);
      }),
      entries.end());
  sort_entries(&entries);

  int result = 0;
  for (size_t i = 0U; i < entries.size(); ++i) {
    const ShellFsEntry& entry = entries[i];
    const bool last = (i + 1U) == entries.size();
    shell_printf(
        state,
        "%s%s%s%s\n",
        prefix.c_str(),
        last ? "`-- " : "|-- ",
        entry.display_name.c_str(),
        entry.is_dir ? "/" : "");
    if (entry.is_dir) {
      ++counts->dirs;
      const std::string child_prefix = prefix + (last ? "    " : "|   ");
      if (print_tree_dir(state, entry.path, child_prefix, opts, depth + 1U, counts) != 0) {
        result = 1;
      }
    } else {
      ++counts->files;
    }
  }
  return result;
}

}  // namespace

void shell_help_tree(ShellState& state) {
  shell_write_line(state, "Usage: tree [OPTION]... [PATH]");
  shell_write_line(state, "Print a compact directory tree.");
  shell_write_line(state, "  -a, --all        include entries starting with .");
  shell_write_line(state, "  -L, --level N    descend at most N levels (0-12, default 4)");
}

int shell_cmd_tree(ShellContext& ctx) {
  TreeOptions opts {};
  bool help_requested = false;
  if (!parse_tree_args(ctx, &opts, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_tree(ctx.state);
    return 0;
  }

  const std::string normalized = shell_normalize_path(ctx.state, opts.path);
  bool is_dir = false;
  std::string error;
  if (!shell_path_exists(ctx.state, normalized, &is_dir, nullptr, &error)) {
    shell_printf(ctx.state, "tree: cannot access '%s': %s\n", opts.path.c_str(), error.c_str());
    return 1;
  }

  ShellFsEntry root {};
  root.display_name = (opts.path == ctx.state.cwd) ? normalized : opts.path;
  root.path = normalized;
  root.is_dir = is_dir;
  shell_printf(ctx.state, "%s%s\n", root.display_name.c_str(), is_dir ? "/" : "");
  if (!is_dir) {
    shell_write_line(ctx.state, "");
    shell_write_line(ctx.state, "0 directories, 1 file");
    return 0;
  }

  TreeCounts counts {};
  const int result = print_tree_dir(ctx.state, normalized, "", opts, 0U, &counts);
  shell_write_line(ctx.state, "");
  shell_printf(
      ctx.state,
      "%lu directories, %lu files\n",
      static_cast<unsigned long>(counts.dirs),
      static_cast<unsigned long>(counts.files));
  return result;
}

}  // namespace mros::shell
