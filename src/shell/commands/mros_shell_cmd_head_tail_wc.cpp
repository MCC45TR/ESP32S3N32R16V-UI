#include "src/shell/mros_shell_internal.h"

#include <cstdio>
#include <cstdlib>

#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct TextSliceOptions {
  size_t line_count = 10U;
};

struct WcCounters {
  size_t lines = 0U;
  size_t words = 0U;
  size_t bytes = 0U;
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

bool parse_line_count_option(
    ShellContext& ctx,
    size_t* index,
    TextSliceOptions* options,
    std::vector<std::string>* paths) {
  if (index == nullptr || options == nullptr || paths == nullptr) {
    return false;
  }
  const std::string& arg = ctx.args[*index];
  if (arg == "--help") {
    return true;
  }
  if (arg == "-n" || arg == "--lines") {
    if ((*index + 1U) >= ctx.args.size()) {
      shell_write_line(ctx.state, "missing line count");
      return false;
    }
    options->line_count = static_cast<size_t>(std::strtoul(ctx.args[*index + 1U].c_str(), nullptr, 10));
    *index += 1U;
    return true;
  }
  if (arg.rfind("-n", 0U) == 0U && arg.size() > 2U) {
    options->line_count = static_cast<size_t>(std::strtoul(arg.c_str() + 2, nullptr, 10));
    return true;
  }
  paths->push_back(arg);
  return true;
}

bool split_text_lines(const std::string& text, std::vector<std::string>* lines) {
  if (lines == nullptr) {
    return false;
  }
  lines->clear();
  std::string current;
  for (const char ch : text) {
    current.push_back(ch);
    if (ch == '\n') {
      lines->push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    lines->push_back(current);
  }
  return true;
}

bool read_text_source(
    ShellContext& ctx,
    const std::string& source,
    std::string* content,
    std::string* label) {
  if (content == nullptr || label == nullptr) {
    return false;
  }
  content->clear();
  *label = source;
  if (source == "-") {
    if (ctx.stdin_buffer == nullptr) {
      shell_write_line(ctx.state, "stdin is not available");
      return false;
    }
    *content = *ctx.stdin_buffer;
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

  char buffer[256] = {};
  size_t read_size = 0U;
  while ((read_size = std::fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
    content->append(buffer, read_size);
  }
  std::fclose(file);
  *label = shell_visible_path(ctx.state, normalized);
  return true;
}

void print_lines(ShellState& state, const std::vector<std::string>& lines, const size_t start, const size_t end) {
  for (size_t i = start; i < end && i < lines.size(); ++i) {
    shell_write(state, lines[i].c_str());
  }
}

WcCounters count_text(const std::string& text) {
  WcCounters counters {};
  counters.bytes = text.size();
  bool in_word = false;
  for (const unsigned char ch : text) {
    if (ch == '\n') {
      ++counters.lines;
    }
    const bool is_space = ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '\f' || ch == '\v';
    if (is_space) {
      in_word = false;
      continue;
    }
    if (!in_word) {
      ++counters.words;
      in_word = true;
    }
  }
  return counters;
}

}  // namespace

void shell_help_head(ShellState& state) {
  shell_write_line(state, "Usage: head [OPTION]... [FILE]...");
  shell_write_line(state, "Print the first lines of each FILE to standard output.");
  shell_write_line(state, "  -n, --lines NUM          print the first NUM lines (default: 10)");
}

int shell_cmd_head(ShellContext& ctx) {
  TextSliceOptions options {};
  std::vector<std::string> paths;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_head(ctx.state);
      return 0;
    }
    if (!parse_line_count_option(ctx, &i, &options, &paths)) {
      shell_write_line(ctx.state, "head: invalid arguments");
      return 1;
    }
  }
  if (paths.empty()) {
    paths.push_back("-");
  }

  int result = 0;
  for (size_t i = 0U; i < paths.size(); ++i) {
    std::string content;
    std::string label;
    if (!read_text_source(ctx, paths[i], &content, &label)) {
      result = 1;
      continue;
    }
    std::vector<std::string> lines;
    split_text_lines(content, &lines);
    if (paths.size() > 1U) {
      if (i > 0U) {
        shell_write_line(ctx.state, "");
      }
      shell_printf(ctx.state, "==> %s <==\n", label.c_str());
    }
    print_lines(ctx.state, lines, 0U, options.line_count);
  }
  return result;
}

void shell_help_tail(ShellState& state) {
  shell_write_line(state, "Usage: tail [OPTION]... [FILE]...");
  shell_write_line(state, "Print the last lines of each FILE to standard output.");
  shell_write_line(state, "  -n, --lines NUM          print the last NUM lines (default: 10)");
}

int shell_cmd_tail(ShellContext& ctx) {
  TextSliceOptions options {};
  std::vector<std::string> paths;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_tail(ctx.state);
      return 0;
    }
    if (!parse_line_count_option(ctx, &i, &options, &paths)) {
      shell_write_line(ctx.state, "tail: invalid arguments");
      return 1;
    }
  }
  if (paths.empty()) {
    paths.push_back("-");
  }

  int result = 0;
  for (size_t i = 0U; i < paths.size(); ++i) {
    std::string content;
    std::string label;
    if (!read_text_source(ctx, paths[i], &content, &label)) {
      result = 1;
      continue;
    }
    std::vector<std::string> lines;
    split_text_lines(content, &lines);
    const size_t start = lines.size() > options.line_count ? (lines.size() - options.line_count) : 0U;
    if (paths.size() > 1U) {
      if (i > 0U) {
        shell_write_line(ctx.state, "");
      }
      shell_printf(ctx.state, "==> %s <==\n", label.c_str());
    }
    print_lines(ctx.state, lines, start, lines.size());
  }
  return result;
}

void shell_help_wc(ShellState& state) {
  shell_write_line(state, "Usage: wc [OPTION]... [FILE]...");
  shell_write_line(state, "Print newline, word and byte counts for each FILE.");
  shell_write_line(state, "  -c, --bytes              print the byte counts");
  shell_write_line(state, "  -l, --lines              print the newline counts");
  shell_write_line(state, "  -w, --words              print the word counts");
}

int shell_cmd_wc(ShellContext& ctx) {
  bool show_lines = false;
  bool show_words = false;
  bool show_bytes = false;
  std::vector<std::string> paths;

  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      shell_help_wc(ctx.state);
      return 0;
    }
    if (arg == "-l" || arg == "--lines") {
      show_lines = true;
      continue;
    }
    if (arg == "-w" || arg == "--words") {
      show_words = true;
      continue;
    }
    if (arg == "-c" || arg == "--bytes") {
      show_bytes = true;
      continue;
    }
    paths.push_back(arg);
  }

  if (!show_lines && !show_words && !show_bytes) {
    show_lines = true;
    show_words = true;
    show_bytes = true;
  }
  if (paths.empty()) {
    paths.push_back("-");
  }

  WcCounters total {};
  int result = 0;
  for (const std::string& path : paths) {
    std::string content;
    std::string label;
    if (!read_text_source(ctx, path, &content, &label)) {
      result = 1;
      continue;
    }
    const WcCounters current = count_text(content);
    total.lines += current.lines;
    total.words += current.words;
    total.bytes += current.bytes;
    if (show_lines) shell_printf(ctx.state, "%8lu ", static_cast<unsigned long>(current.lines));
    if (show_words) shell_printf(ctx.state, "%8lu ", static_cast<unsigned long>(current.words));
    if (show_bytes) shell_printf(ctx.state, "%8lu ", static_cast<unsigned long>(current.bytes));
    shell_write_line(ctx.state, label.c_str());
  }

  if (paths.size() > 1U) {
    if (show_lines) shell_printf(ctx.state, "%8lu ", static_cast<unsigned long>(total.lines));
    if (show_words) shell_printf(ctx.state, "%8lu ", static_cast<unsigned long>(total.words));
    if (show_bytes) shell_printf(ctx.state, "%8lu ", static_cast<unsigned long>(total.bytes));
    shell_write_line(ctx.state, "total");
  }
  return result;
}

}  // namespace mros::shell
