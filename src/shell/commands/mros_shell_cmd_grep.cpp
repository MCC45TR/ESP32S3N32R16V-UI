#include "src/shell/mros_shell_internal.h"

#include <cstdio>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct GrepFlags {
  bool ignore_case = false;
  bool line_number = false;
  bool recursive = false;
  bool list_files = false;
  bool count_only = false;
  bool invert_match = false;
  bool word_regexp = false;
  bool line_regexp = false;
  bool with_filename = false;
  bool no_filename = false;
};

struct GrepMatchStats {
  bool matched = false;
  size_t match_count = 0U;
};

std::string to_lower_copy(const std::string& text) {
  std::string lowered = text;
  std::transform(
      lowered.begin(),
      lowered.end(),
      lowered.begin(),
      [](const unsigned char ch) -> char {
        return static_cast<char>(std::tolower(ch));
      });
  return lowered;
}

bool is_word_char(const char ch) {
  const unsigned char value = static_cast<unsigned char>(ch);
  return std::isalnum(value) != 0 || ch == '_';
}

bool match_pattern(const std::string& line, const std::string& pattern, const GrepFlags& flags) {
  std::string haystack = line;
  std::string needle = pattern;
  if (flags.ignore_case) {
    haystack = to_lower_copy(haystack);
    needle = to_lower_copy(needle);
  }

  bool matched = false;
  if (flags.line_regexp) {
    matched = haystack == needle;
  } else if (flags.word_regexp) {
    size_t pos = haystack.find(needle);
    while (pos != std::string::npos) {
      const bool left_ok = pos == 0U || !is_word_char(haystack[pos - 1U]);
      const size_t end = pos + needle.size();
      const bool right_ok = end >= haystack.size() || !is_word_char(haystack[end]);
      if (left_ok && right_ok) {
        matched = true;
        break;
      }
      pos = haystack.find(needle, pos + 1U);
    }
  } else {
    matched = haystack.find(needle) != std::string::npos;
  }

  return flags.invert_match ? !matched : matched;
}

void shell_help_grep_internal(ShellState& state) {
  shell_write_line(state, "Usage: grep [OPTION]... PATTERN [FILE]...");
  shell_write_line(state, "Search for PATTERN in each FILE.");
  shell_write_line(state, "  -c, --count               print only a count of matching lines");
  shell_write_line(state, "  -H                        print the file name for each match");
  shell_write_line(state, "  -h                        suppress the file name prefix");
  shell_write_line(state, "  -i, --ignore-case         ignore case distinctions");
  shell_write_line(state, "  -l, --files-with-matches  print only names of FILEs with matches");
  shell_write_line(state, "  -n, --line-number         print line number with output lines");
  shell_write_line(state, "  -r, -R, --recursive       recurse into directories");
  shell_write_line(state, "  -v, --invert-match        select non-matching lines");
  shell_write_line(state, "  -w, --word-regexp         match only whole words");
  shell_write_line(state, "  -x, --line-regexp         match only whole lines");
}

bool parse_grep_args(
    ShellContext& ctx,
    GrepFlags* flags,
    std::string* pattern,
    std::vector<std::string>* paths,
    bool* help_requested) {
  if (flags == nullptr || pattern == nullptr || paths == nullptr || help_requested == nullptr) {
    return false;
  }

  *help_requested = false;
  bool pattern_set = false;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (!pattern_set && (arg == "--help")) {
      *help_requested = true;
      return true;
    }

    if (!pattern_set && !arg.empty() && arg.front() == '-' && arg != "-") {
      if (arg == "--ignore-case") {
        flags->ignore_case = true;
        continue;
      }
      if (arg == "--line-number") {
        flags->line_number = true;
        continue;
      }
      if (arg == "--recursive") {
        flags->recursive = true;
        continue;
      }
      if (arg == "--files-with-matches") {
        flags->list_files = true;
        continue;
      }
      if (arg == "--count") {
        flags->count_only = true;
        continue;
      }
      if (arg == "--invert-match") {
        flags->invert_match = true;
        continue;
      }
      if (arg == "--word-regexp") {
        flags->word_regexp = true;
        continue;
      }
      if (arg == "--line-regexp") {
        flags->line_regexp = true;
        continue;
      }
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'H':
            flags->with_filename = true;
            break;
          case 'h':
            flags->no_filename = true;
            break;
          case 'i':
            flags->ignore_case = true;
            break;
          case 'n':
            flags->line_number = true;
            break;
          case 'R':
          case 'r':
            flags->recursive = true;
            break;
          case 'l':
            flags->list_files = true;
            break;
          case 'c':
            flags->count_only = true;
            break;
          case 'v':
            flags->invert_match = true;
            break;
          case 'w':
            flags->word_regexp = true;
            break;
          case 'x':
            flags->line_regexp = true;
            break;
          default:
            shell_printf(ctx.state, "grep: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }

    if (!pattern_set) {
      *pattern = arg;
      pattern_set = true;
      continue;
    }
    paths->push_back(arg);
  }

  return true;
}

bool should_show_filename_prefix(const GrepFlags& flags, const bool multiple_inputs) {
  if (flags.no_filename) {
    return false;
  }
  if (flags.with_filename) {
    return true;
  }
  return multiple_inputs;
}

void print_grep_line(
    ShellState& state,
    const std::string& label,
    const std::string& line,
    const size_t line_number,
    const GrepFlags& flags,
    const bool multiple_inputs) {
  if (should_show_filename_prefix(flags, multiple_inputs)) {
    shell_printf(state, "%s:", label.c_str());
  }
  if (flags.line_number) {
    shell_printf(state, "%lu:", static_cast<unsigned long>(line_number));
  }
  shell_write(state, line.c_str());
}

int grep_file(
    ShellState& state,
    const std::string& label,
    const std::string& actual_path,
    const std::string& pattern,
    const GrepFlags& flags,
    const bool multiple_inputs,
    GrepMatchStats* stats) {
  if (stats == nullptr) {
    return 1;
  }

  FILE* file = std::fopen(actual_path.c_str(), "rb");
  if (file == nullptr) {
    shell_printf(state, "grep: %s: unable to open file\n", label.c_str());
    return 1;
  }

  stats->matched = false;
  stats->match_count = 0U;

  std::string line;
  size_t line_number = 1U;
  int next = 0;
  while ((next = std::fgetc(file)) != EOF) {
    line.push_back(static_cast<char>(next));
    if (next != '\n') {
      continue;
    }

    if (match_pattern(line, pattern, flags)) {
      stats->matched = true;
      ++stats->match_count;
      if (flags.list_files) {
        break;
      }
      if (!flags.count_only) {
        print_grep_line(state, label, line, line_number, flags, multiple_inputs);
      }
    }
    line.clear();
    ++line_number;
  }

  if (!line.empty() && match_pattern(line, pattern, flags)) {
    stats->matched = true;
    ++stats->match_count;
    if (!flags.list_files && !flags.count_only) {
      print_grep_line(state, label, line, line_number, flags, multiple_inputs);
      shell_write(state, "\n");
    }
  }

  std::fclose(file);

  if (flags.list_files && stats->matched) {
    shell_write_line(state, label.c_str());
  } else if (flags.count_only) {
    if (should_show_filename_prefix(flags, multiple_inputs)) {
      shell_printf(state, "%s:", label.c_str());
    }
    shell_printf(state, "%lu\n", static_cast<unsigned long>(stats->match_count));
  }

  return 0;
}

int grep_text_stream(
    ShellState& state,
    const std::string& label,
    const std::string& text,
    const std::string& pattern,
    const GrepFlags& flags,
    const bool multiple_inputs,
    GrepMatchStats* stats) {
  if (stats == nullptr) {
    return 1;
  }

  stats->matched = false;
  stats->match_count = 0U;

  std::string line;
  size_t line_number = 1U;
  for (const char ch : text) {
    line.push_back(ch);
    if (ch != '\n') {
      continue;
    }

    if (match_pattern(line, pattern, flags)) {
      stats->matched = true;
      ++stats->match_count;
      if (flags.list_files) {
        break;
      }
      if (!flags.count_only) {
        print_grep_line(state, label, line, line_number, flags, multiple_inputs);
      }
    }
    line.clear();
    ++line_number;
  }

  if (!line.empty() && match_pattern(line, pattern, flags)) {
    stats->matched = true;
    ++stats->match_count;
    if (!flags.list_files && !flags.count_only) {
      print_grep_line(state, label, line, line_number, flags, multiple_inputs);
      shell_write(state, "\n");
    }
  }

  if (flags.list_files && stats->matched) {
    shell_write_line(state, label.c_str());
  } else if (flags.count_only) {
    if (should_show_filename_prefix(flags, multiple_inputs)) {
      shell_printf(state, "%s:", label.c_str());
    }
    shell_printf(state, "%lu\n", static_cast<unsigned long>(stats->match_count));
  }

  return 0;
}

int grep_path(
    ShellState& state,
    const std::string& source,
    const std::string& pattern,
    const GrepFlags& flags,
    const bool multiple_inputs,
    bool* any_match) {
  const std::string normalized = shell_normalize_path(state, source);
  bool is_dir = false;
  std::string error;
  if (!shell_path_exists(state, normalized, &is_dir, nullptr, &error)) {
    shell_printf(state, "grep: %s: %s\n", source.c_str(), error.c_str());
    return 1;
  }

  if (is_dir) {
    if (!flags.recursive) {
      shell_printf(state, "grep: %s: Is a directory\n", source.c_str());
      return 1;
    }

    std::vector<ShellFsEntry> entries;
    if (!shell_read_directory(state, normalized, false, false, &entries, &error)) {
      shell_printf(state, "grep: %s: %s\n", source.c_str(), error.c_str());
      return 1;
    }

    int result = 0;
    for (const ShellFsEntry& entry : entries) {
      if (entry.is_dir) {
        if (grep_path(state, entry.path, pattern, flags, true, any_match) != 0) {
          result = 1;
        }
        continue;
      }

      GrepMatchStats stats {};
      if (grep_file(state, entry.path, entry.path, pattern, flags, true, &stats) != 0) {
        result = 1;
        continue;
      }
      *any_match = *any_match || stats.matched;
    }
    return result;
  }

  std::string actual_path;
  if (!shell_openable_file_path(state, normalized, &actual_path, &error)) {
    shell_printf(state, "grep: %s: %s\n", source.c_str(), error.c_str());
    return 1;
  }

  GrepMatchStats stats {};
  const int result = grep_file(state, source, actual_path, pattern, flags, multiple_inputs, &stats);
  *any_match = *any_match || stats.matched;
  return result;
}

}  // namespace

void shell_help_grep(ShellState& state) {
  shell_help_grep_internal(state);
}

int shell_cmd_grep(ShellContext& ctx) {
  GrepFlags flags {};
  std::string pattern;
  std::vector<std::string> paths;
  bool help_requested = false;
  if (!parse_grep_args(ctx, &flags, &pattern, &paths, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_grep(ctx.state);
    return 0;
  }
  if (pattern.empty()) {
    shell_write_line(ctx.state, "grep: missing search pattern");
    return 1;
  }
  if (paths.empty()) {
    if (ctx.stdin_buffer == nullptr) {
      shell_write_line(ctx.state, "grep: stdin is not supported in this shell");
      return 1;
    }
    GrepMatchStats stats {};
    if (grep_text_stream(ctx.state, "(stdin)", *ctx.stdin_buffer, pattern, flags, false, &stats) != 0) {
      return 1;
    }
    return stats.matched ? 0 : 1;
  }

  int result = 0;
  bool any_match = false;
  const bool multiple_inputs = paths.size() > 1U;
  for (const std::string& path : paths) {
    if (grep_path(ctx.state, path, pattern, flags, multiple_inputs, &any_match) != 0) {
      result = 1;
    }
  }

  if (result != 0) {
    return result;
  }
  return any_match ? 0 : 1;
}

}  // namespace mros::shell
