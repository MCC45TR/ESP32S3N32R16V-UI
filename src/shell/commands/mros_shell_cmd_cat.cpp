#include "src/shell/mros_shell_internal.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct CatFlags {
  bool number_all = false;
  bool number_nonblank = false;
  bool squeeze_blank = false;
  bool show_ends = false;
  bool show_tabs = false;
  bool show_nonprinting = false;
  size_t max_bytes = 0U;
};

}  // namespace

void shell_help_cat(ShellState& state) {
  shell_write_line(state, "Usage: cat [OPTION]... [FILE]...");
  shell_write_line(state, "Concatenate FILE(s) to standard output.");
  shell_write_line(state, "  -A, --show-all           equivalent to -vET");
  shell_write_line(state, "  -b, --number-nonblank    number nonempty output lines");
  shell_write_line(state, "  -E, --show-ends          display $ at end of each line");
  shell_write_line(state, "  -n, --number             number all output lines");
  shell_write_line(state, "  -s, --squeeze-blank      suppress repeated empty output lines");
  shell_write_line(state, "  -T, --show-tabs          display TAB characters as ^I");
  shell_write_line(state, "  -v, --show-nonprinting   use ^ and M- notation");
  shell_write_line(state, "      --max-bytes NUM      stop after NUM input bytes");
  shell_write_line(state, "  -                         read from pipeline/stdin");
}

namespace {

bool parse_cat_args(
    ShellContext& ctx,
    CatFlags* flags,
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
    if (arg == "--show-all") {
      flags->show_nonprinting = true;
      flags->show_tabs = true;
      flags->show_ends = true;
      continue;
    }
    if (arg == "--number") {
      flags->number_all = true;
      continue;
    }
    if (arg == "--number-nonblank") {
      flags->number_nonblank = true;
      continue;
    }
    if (arg == "--squeeze-blank") {
      flags->squeeze_blank = true;
      continue;
    }
    if (arg == "--show-ends") {
      flags->show_ends = true;
      continue;
    }
    if (arg == "--show-tabs") {
      flags->show_tabs = true;
      continue;
    }
    if (arg == "--show-nonprinting") {
      flags->show_nonprinting = true;
      continue;
    }
    if (arg == "--max-bytes") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "cat: --max-bytes requires a value");
        return false;
      }
      flags->max_bytes = static_cast<size_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10));
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'A':
            flags->show_nonprinting = true;
            flags->show_tabs = true;
            flags->show_ends = true;
            break;
          case 'b':
            flags->number_nonblank = true;
            break;
          case 'E':
            flags->show_ends = true;
            break;
          case 'n':
            flags->number_all = true;
            break;
          case 's':
            flags->squeeze_blank = true;
            break;
          case 'T':
            flags->show_tabs = true;
            break;
          case 'v':
            flags->show_nonprinting = true;
            break;
          default:
            shell_printf(ctx.state, "cat: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }
    paths->push_back(arg);
  }

  return true;
}

void append_visible_char(const unsigned char ch, const CatFlags& flags, std::string* out) {
  if (out == nullptr) {
    return;
  }

  if (ch == '\t') {
    if (flags.show_tabs) {
      out->append("^I");
    } else {
      out->push_back('\t');
    }
    return;
  }

  if (!flags.show_nonprinting) {
    out->push_back(static_cast<char>(ch));
    return;
  }

  if (ch < 32U) {
    out->push_back('^');
    out->push_back(static_cast<char>(ch + 64U));
    return;
  }

  if (ch == 127U) {
    out->append("^?");
    return;
  }

  if (ch > 127U) {
    out->append("M-");
    append_visible_char(static_cast<unsigned char>(ch - 128U), flags, out);
    return;
  }

  out->push_back(static_cast<char>(ch));
}

void emit_cat_line(
    ShellState& state,
    const std::string& raw_line,
    const CatFlags& flags,
    size_t* line_number,
    bool* previous_blank) {
  if (line_number == nullptr || previous_blank == nullptr) {
    return;
  }

  const bool has_newline = !raw_line.empty() && raw_line.back() == '\n';
  const std::string content =
      has_newline ? raw_line.substr(0U, raw_line.size() - 1U) : raw_line;
  const bool is_blank = content.empty();

  if (flags.squeeze_blank && is_blank && *previous_blank) {
    return;
  }

  std::string output;
  if (flags.number_nonblank) {
    if (!is_blank) {
      char prefix[24] = {};
      std::snprintf(prefix, sizeof(prefix), "%6lu\t", static_cast<unsigned long>(*line_number));
      output.append(prefix);
      ++(*line_number);
    }
  } else if (flags.number_all) {
    char prefix[24] = {};
    std::snprintf(prefix, sizeof(prefix), "%6lu\t", static_cast<unsigned long>(*line_number));
    output.append(prefix);
    ++(*line_number);
  }

  for (const char ch : content) {
    append_visible_char(static_cast<unsigned char>(ch), flags, &output);
  }

  if (flags.show_ends) {
    output.push_back('$');
  }
  if (has_newline) {
    output.push_back('\n');
  }

  shell_write(state, output.c_str());
  *previous_blank = is_blank;
}

void cat_text_buffer(
    ShellState& state,
    const CatFlags& flags,
    const char* data,
    const size_t length,
    size_t* line_number,
    bool* previous_blank) {
  if (data == nullptr || line_number == nullptr || previous_blank == nullptr) {
    return;
  }
  const size_t limit =
      (flags.max_bytes > 0U && flags.max_bytes < length) ? flags.max_bytes : length;
  std::string line_buffer;
  for (size_t i = 0U; i < limit; ++i) {
    line_buffer.push_back(data[i]);
    if (data[i] == '\n') {
      emit_cat_line(state, line_buffer, flags, line_number, previous_blank);
      line_buffer.clear();
    }
  }
  if (!line_buffer.empty()) {
    emit_cat_line(state, line_buffer, flags, line_number, previous_blank);
  }
  if (flags.max_bytes > 0U && length > flags.max_bytes) {
    shell_write_line(state, "\ncat: output truncated by --max-bytes");
  }
}

int cat_stdin(ShellContext& ctx, const CatFlags& flags) {
  if (ctx.stdin_buffer == nullptr) {
    shell_write_line(ctx.state, "cat: -: stdin is not available");
    return 1;
  }
  size_t line_number = 1U;
  bool previous_blank = false;
  cat_text_buffer(ctx.state,
                  flags,
                  ctx.stdin_buffer->data(),
                  ctx.stdin_buffer->size(),
                  &line_number,
                  &previous_blank);
  return 0;
}

int cat_single_file(ShellContext& ctx, const CatFlags& flags, const std::string& source) {
  const std::string normalized = shell_normalize_path(ctx.state, source);
  std::string actual_path;
  std::string error;
  if (!shell_openable_file_path(ctx.state, normalized, &actual_path, &error)) {
    shell_printf(ctx.state, "cat: %s: %s\n", source.c_str(), error.c_str());
    return 1;
  }

  FILE* file = std::fopen(actual_path.c_str(), "rb");
  if (file == nullptr) {
    shell_printf(ctx.state, "cat: %s: unable to open file\n", source.c_str());
    return 1;
  }

  size_t line_number = 1U;
  bool previous_blank = false;
  std::string line_buffer;
  size_t bytes_seen = 0U;

  int next = 0;
  while ((next = std::fgetc(file)) != EOF) {
    if (flags.max_bytes > 0U && bytes_seen >= flags.max_bytes) {
      shell_write_line(ctx.state, "\ncat: output truncated by --max-bytes");
      break;
    }
    ++bytes_seen;
    line_buffer.push_back(static_cast<char>(next));
    if (next == '\n') {
      emit_cat_line(ctx.state, line_buffer, flags, &line_number, &previous_blank);
      line_buffer.clear();
    }
  }

  if (!line_buffer.empty()) {
    emit_cat_line(ctx.state, line_buffer, flags, &line_number, &previous_blank);
  }

  std::fclose(file);
  return 0;
}

}  // namespace

int shell_cmd_cat(ShellContext& ctx) {
  CatFlags flags {};
  std::vector<std::string> paths;
  bool help_requested = false;
  if (!parse_cat_args(ctx, &flags, &paths, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_cat(ctx.state);
    return 0;
  }

  if (paths.empty()) {
    shell_write_line(ctx.state, "cat: missing file operand");
    return 1;
  }

  int result = 0;
  for (const std::string& path : paths) {
    if (path == "-") {
      if (cat_stdin(ctx, flags) != 0) {
        result = 1;
      }
      continue;
    }
    if (cat_single_file(ctx, flags, path) != 0) {
      result = 1;
    }
  }
  return result;
}

}  // namespace mros::shell
