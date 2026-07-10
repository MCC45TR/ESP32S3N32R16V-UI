#include "src/shell/mros_shell_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include <string>
#include <vector>

namespace mros::shell {
namespace {

constexpr size_t kDefaultPageLines = 24U;
constexpr size_t kMaxToolBytes = 96U * 1024U;

std::string visible_path(const ShellState& state, const std::string& normalized_path) {
  const std::string mount = shell_storage_mount_path(state);
  if (normalized_path == mount) return "/fs";
  if (normalized_path.rfind(mount + "/", 0U) == 0U) {
    return "/fs" + normalized_path.substr(mount.size());
  }
  return normalized_path;
}

bool parse_size(const std::string& text, size_t* out) {
  if (out == nullptr || text.empty()) return false;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
  *out = static_cast<size_t>(parsed);
  return true;
}

bool read_text_source(ShellContext& ctx,
                      const std::string& source,
                      std::string* content,
                      std::string* label) {
  if (content == nullptr || label == nullptr) return false;
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

  char buffer[384] = {};
  size_t total = 0U;
  size_t read_size = 0U;
  bool truncated = false;
  while ((read_size = std::fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
    const size_t room = total < kMaxToolBytes ? (kMaxToolBytes - total) : 0U;
    if (read_size > room) {
      if (room > 0U) content->append(buffer, room);
      truncated = true;
      break;
    }
    content->append(buffer, read_size);
    total += read_size;
  }
  std::fclose(file);
  *label = visible_path(ctx.state, normalized);
  if (truncated) {
    shell_write_line(ctx.state, "text tool: input truncated at 96KB");
  }
  return true;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::string current;
  for (const char ch : text) {
    current.push_back(ch);
    if (ch == '\n') {
      lines.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) lines.push_back(current);
  return lines;
}

std::vector<std::string> split_fields(const std::string& text) {
  std::vector<std::string> fields;
  std::string current;
  for (const char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!current.empty()) {
        fields.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) fields.push_back(current);
  return fields;
}

void print_page(ShellContext& ctx,
                const std::string& content,
                const size_t page,
                const size_t lines_per_page,
                const std::string& search) {
  const std::vector<std::string> lines = split_lines(content);
  size_t printed = 0U;
  const size_t start = (page > 0U ? (page - 1U) : 0U) * lines_per_page;
  for (size_t i = start; i < lines.size() && printed < lines_per_page; ++i) {
    if (!search.empty() && lines[i].find(search) == std::string::npos) {
      continue;
    }
    shell_write(ctx.state, lines[i].c_str());
    ++printed;
  }
  if (start + printed < lines.size()) {
    shell_printf(ctx.state,
                 "\n-- more: page=%lu lines=%lu next-page=%lu --\n",
                 static_cast<unsigned long>(page),
                 static_cast<unsigned long>(lines_per_page),
                 static_cast<unsigned long>(page + 1U));
  }
}

int page_command(ShellContext& ctx, const char* name) {
  size_t page = 1U;
  size_t lines = kDefaultPageLines;
  std::string search;
  std::string path;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") return 2;
    if (arg == "--page") {
      if ((i + 1U) >= ctx.args.size() || !parse_size(ctx.args[++i], &page) || page == 0U) {
        shell_printf(ctx.state, "%s: invalid page\n", name);
        return 1;
      }
      continue;
    }
    if (arg == "--lines" || arg == "-n") {
      if ((i + 1U) >= ctx.args.size() || !parse_size(ctx.args[++i], &lines) || lines == 0U) {
        shell_printf(ctx.state, "%s: invalid line count\n", name);
        return 1;
      }
      lines = std::min<size_t>(lines, 200U);
      continue;
    }
    if (arg == "--search") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_printf(ctx.state, "%s: --search requires text\n", name);
        return 1;
      }
      search = ctx.args[++i];
      continue;
    }
    path = arg;
  }
  if (path.empty()) path = "-";
  std::string content;
  std::string label;
  if (!read_text_source(ctx, path, &content, &label)) return 1;
  print_page(ctx, content, page, lines, search);
  return 0;
}

std::string join_args(const std::vector<std::string>& args, size_t begin, size_t end) {
  std::string out;
  for (size_t i = begin; i < end && i < args.size(); ++i) {
    if (!out.empty()) out.push_back(' ');
    out += args[i];
  }
  return out;
}

bool parse_sed_substitution(const std::string& expr,
                            std::string* from,
                            std::string* to,
                            bool* global) {
  if (from == nullptr || to == nullptr || global == nullptr) return false;
  if (expr.size() < 4U || expr[0] != 's') return false;
  const char sep = expr[1];
  const size_t p1 = expr.find(sep, 2U);
  if (p1 == std::string::npos) return false;
  const size_t p2 = expr.find(sep, p1 + 1U);
  if (p2 == std::string::npos) return false;
  *from = expr.substr(2U, p1 - 2U);
  *to = expr.substr(p1 + 1U, p2 - p1 - 1U);
  *global = expr.find('g', p2 + 1U) != std::string::npos;
  return !from->empty();
}

std::string replace_line(std::string line,
                         const std::string& from,
                         const std::string& to,
                         const bool global) {
  size_t pos = line.find(from);
  while (pos != std::string::npos) {
    line.replace(pos, from.size(), to);
    if (!global) break;
    pos = line.find(from, pos + to.size());
  }
  return line;
}

bool parse_awk_print_field(const std::string& program, size_t* field_index) {
  if (field_index == nullptr) return false;
  std::string compact;
  for (const char ch : program) {
    if (std::isspace(static_cast<unsigned char>(ch)) == 0) compact.push_back(ch);
  }
  const std::string prefix = "{print$";
  if (compact.rfind(prefix, 0U) != 0U || compact.back() != '}') return false;
  const std::string n = compact.substr(prefix.size(), compact.size() - prefix.size() - 1U);
  return parse_size(n, field_index) && *field_index > 0U;
}

}  // namespace

void shell_help_more(ShellState& state) {
  shell_write_line(state, "Usage: more FILE [--page N] [--lines N]");
  shell_write_line(state, "Display a bounded page of text.");
}

int shell_cmd_more(ShellContext& ctx) {
  const int rc = page_command(ctx, "more");
  if (rc == 2) {
    shell_help_more(ctx.state);
    return 0;
  }
  return rc;
}

void shell_help_less(ShellState& state) {
  shell_write_line(state, "Usage: less FILE [--page N] [--lines N] [--search TEXT]");
  shell_write_line(state, "Non-interactive bounded pager for serial/web shell.");
}

int shell_cmd_less(ShellContext& ctx) {
  const int rc = page_command(ctx, "less");
  if (rc == 2) {
    shell_help_less(ctx.state);
    return 0;
  }
  return rc;
}

void shell_help_nl(ShellState& state) {
  shell_write_line(state, "Usage: nl [FILE|-]");
  shell_write_line(state, "Number lines from a file or pipeline.");
}

int shell_cmd_nl(ShellContext& ctx) {
  std::string path = ctx.args.size() >= 2U ? ctx.args[1] : "-";
  if (path == "--help") {
    shell_help_nl(ctx.state);
    return 0;
  }
  std::string content;
  std::string label;
  if (!read_text_source(ctx, path, &content, &label)) return 1;
  const std::vector<std::string> lines = split_lines(content);
  for (size_t i = 0U; i < lines.size(); ++i) {
    shell_printf(ctx.state, "%6lu\t%s", static_cast<unsigned long>(i + 1U), lines[i].c_str());
  }
  return 0;
}

void shell_help_xxd(ShellState& state) {
  shell_write_line(state, "Usage: xxd [-l NUM] [-s NUM] [FILE|-]");
  shell_write_line(state, "Compatibility hex dump alias with ASCII column.");
}

int shell_cmd_xxd(ShellContext& ctx) {
  size_t length = 0U;
  size_t skip = 0U;
  std::string path = "-";
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      shell_help_xxd(ctx.state);
      return 0;
    }
    if (arg == "-l" || arg == "-n") {
      if ((i + 1U) >= ctx.args.size() || !parse_size(ctx.args[++i], &length)) {
        shell_write_line(ctx.state, "xxd: invalid length");
        return 1;
      }
      continue;
    }
    if (arg == "-s") {
      if ((i + 1U) >= ctx.args.size() || !parse_size(ctx.args[++i], &skip)) {
        shell_write_line(ctx.state, "xxd: invalid skip");
        return 1;
      }
      continue;
    }
    path = arg;
  }
  std::string content;
  std::string label;
  if (!read_text_source(ctx, path, &content, &label)) return 1;
  const size_t start = std::min(skip, content.size());
  const size_t end = length > 0U ? std::min(content.size(), start + length) : content.size();
  for (size_t offset = start; offset < end; offset += 16U) {
    shell_printf(ctx.state, "%08lx: ", static_cast<unsigned long>(offset));
    for (size_t i = 0U; i < 16U; ++i) {
      if ((offset + i) < end) {
        shell_printf(ctx.state, "%02x", static_cast<unsigned>(static_cast<unsigned char>(content[offset + i])));
      } else {
        shell_write(ctx.state, "  ");
      }
      if ((i & 1U) == 1U) shell_write(ctx.state, " ");
    }
    shell_write(ctx.state, " ");
    for (size_t i = 0U; i < 16U && (offset + i) < end; ++i) {
      const unsigned char ch = static_cast<unsigned char>(content[offset + i]);
      shell_printf(ctx.state, "%c", (ch >= 32U && ch <= 126U) ? ch : '.');
    }
    shell_write_line(ctx.state, "");
  }
  return 0;
}

void shell_help_sed(ShellState& state) {
  shell_write_line(state, "Usage: sed 's/OLD/NEW/[g]' [FILE|-]");
  shell_write_line(state, "Bounded sed-compatible substitution subset.");
}

int shell_cmd_sed(ShellContext& ctx) {
  if (ctx.args.size() < 2U || ctx.args[1] == "--help") {
    shell_help_sed(ctx.state);
    return ctx.args.size() < 2U ? 1 : 0;
  }
  std::string from;
  std::string to;
  bool global = false;
  if (!parse_sed_substitution(ctx.args[1], &from, &to, &global)) {
    shell_write_line(ctx.state, "sed: only s/OLD/NEW/[g] is supported");
    return 1;
  }
  const std::string path = ctx.args.size() >= 3U ? ctx.args[2] : "-";
  std::string content;
  std::string label;
  if (!read_text_source(ctx, path, &content, &label)) return 1;
  const std::vector<std::string> lines = split_lines(content);
  for (const std::string& line : lines) {
    shell_write(ctx.state, replace_line(line, from, to, global).c_str());
  }
  return 0;
}

void shell_help_awk(ShellState& state) {
  shell_write_line(state, "Usage: awk '{print $N}' [FILE|-]");
  shell_write_line(state, "Bounded awk-compatible field print subset.");
}

int shell_cmd_awk(ShellContext& ctx) {
  if (ctx.args.size() < 2U || ctx.args[1] == "--help") {
    shell_help_awk(ctx.state);
    return ctx.args.size() < 2U ? 1 : 0;
  }
  const size_t file_index = ctx.args.size() >= 3U ? (ctx.args.size() - 1U) : ctx.args.size();
  const std::string program = join_args(ctx.args, 1U, file_index);
  size_t field_index = 0U;
  if (!parse_awk_print_field(program, &field_index)) {
    shell_write_line(ctx.state, "awk: only '{print $N}' is supported");
    return 1;
  }
  const std::string path = ctx.args.size() >= 3U ? ctx.args.back() : "-";
  std::string content;
  std::string label;
  if (!read_text_source(ctx, path, &content, &label)) return 1;
  const std::vector<std::string> lines = split_lines(content);
  for (const std::string& line : lines) {
    const std::vector<std::string> fields = split_fields(line);
    if (field_index <= fields.size()) {
      shell_write_line(ctx.state, fields[field_index - 1U].c_str());
    } else {
      shell_write_line(ctx.state, "");
    }
  }
  return 0;
}

}  // namespace mros::shell
