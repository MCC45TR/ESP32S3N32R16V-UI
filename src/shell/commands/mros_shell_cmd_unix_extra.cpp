#include "src/shell/mros_shell_internal.h"

#include "src/security/ssh_identity.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct MetadataRecord {
  std::string path;
  std::string owner;
  std::string group;
  uint32_t mode = 0644U;
};

std::string visible_path(const ShellState& state, const std::string& normalized_path) {
  const std::string mount = shell_storage_mount_path(state);
  if (normalized_path == mount) {
    return "/fs";
  }
  if (normalized_path.rfind(mount + "/", 0U) == 0U) {
    return "/fs" + normalized_path.substr(mount.size());
  }
  return normalized_path;
}

std::string trim_copy_local(const std::string& text) {
  size_t start = 0U;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1U])) != 0) {
    --end;
  }
  return text.substr(start, end - start);
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
  *label = visible_path(ctx.state, normalized);
  return true;
}

bool read_binary_source(
    ShellContext& ctx,
    const std::string& source,
    std::vector<unsigned char>* bytes,
    std::string* label) {
  if (bytes == nullptr || label == nullptr) {
    return false;
  }
  bytes->clear();
  std::string content;
  if (!read_text_source(ctx, source, &content, label)) {
    return false;
  }
  bytes->assign(content.begin(), content.end());
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
  if (!current.empty()) {
    lines.push_back(current);
  }
  return lines;
}

std::vector<std::string> split_whitespace(const std::string& text) {
  std::vector<std::string> tokens;
  std::string current;
  for (const char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

std::string meta_file_path(const ShellState& state) {
  return shell_storage_user_root(state) + "/.mshell_meta.tsv";
}

std::vector<MetadataRecord> load_metadata(const ShellState& state) {
  std::vector<MetadataRecord> rows;
  FILE* file = std::fopen(meta_file_path(state).c_str(), "rb");
  if (file == nullptr) {
    return rows;
  }
  char line[256] = {};
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    std::string raw = trim_copy_local(line);
    if (raw.empty()) {
      continue;
    }
    const size_t p1 = raw.find('\t');
    const size_t p2 = p1 == std::string::npos ? std::string::npos : raw.find('\t', p1 + 1U);
    const size_t p3 = p2 == std::string::npos ? std::string::npos : raw.find('\t', p2 + 1U);
    if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos) {
      continue;
    }
    MetadataRecord record;
    record.path = raw.substr(0U, p1);
    record.owner = raw.substr(p1 + 1U, p2 - p1 - 1U);
    record.group = raw.substr(p2 + 1U, p3 - p2 - 1U);
    record.mode = static_cast<uint32_t>(std::strtoul(raw.substr(p3 + 1U).c_str(), nullptr, 8));
    rows.push_back(record);
  }
  std::fclose(file);
  return rows;
}

bool save_metadata(const ShellState& state, const std::vector<MetadataRecord>& rows) {
  FILE* file = std::fopen(meta_file_path(state).c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  for (const MetadataRecord& record : rows) {
    std::fprintf(
        file,
        "%s\t%s\t%s\t%04o\n",
        record.path.c_str(),
        record.owner.c_str(),
        record.group.c_str(),
        static_cast<unsigned>(record.mode & 07777U));
  }
  std::fclose(file);
  return true;
}

MetadataRecord default_metadata(const ShellState& state, const std::string& path, const bool is_dir) {
  MetadataRecord record;
  record.path = path;
  record.owner = state.session_username.empty() ? "mros" : state.session_username;
  record.group = state.session_admin ? "admin" : "users";
  record.mode = is_dir ? 0755U : 0644U;
  return record;
}

MetadataRecord get_or_create_metadata(
    const ShellState& state,
    const std::string& path,
    const bool is_dir,
    std::vector<MetadataRecord>* rows) {
  if (rows != nullptr) {
    for (MetadataRecord& record : *rows) {
      if (record.path == path) {
        return record;
      }
    }
  }
  return default_metadata(state, path, is_dir);
}

bool parse_mode_octal(const std::string& text, uint32_t* mode) {
  if (mode == nullptr || text.empty()) {
    return false;
  }
  for (const char ch : text) {
    if (ch < '0' || ch > '7') {
      return false;
    }
  }
  *mode = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 8));
  return true;
}

std::string join_args(const std::vector<std::string>& args, const size_t start) {
  std::string text;
  for (size_t i = start; i < args.size(); ++i) {
    if (!text.empty()) {
      text.push_back(' ');
    }
    text += args[i];
  }
  return text;
}

bool has_printable_run(const unsigned char ch) {
  return ch >= 32U && ch <= 126U;
}

std::string effective_user(const ShellState& state) {
  return state.root_session ? mros::ssh::root_username() : state.session_username;
}

std::vector<std::string> effective_groups(const ShellState& state) {
  if (state.root_session) {
    return {"root", "admin", "sudo"};
  }
  std::vector<std::string> groups = {"users"};
  if (state.session_admin) {
    groups.push_back("admin");
  }
  if (state.session_can_sudo) {
    groups.push_back("sudo");
  }
  return groups;
}

bool is_owner_or_admin(
    const ShellState& state,
    const MetadataRecord& record) {
  if (state.root_session || state.session_admin) {
    return true;
  }
  return record.owner == effective_user(state);
}

}  // namespace

void shell_help_sort(ShellState& state) {
  shell_write_line(state, "Usage: sort [OPTION]... [FILE]...");
  shell_write_line(state, "Sort lines of text.");
}

int shell_cmd_sort(ShellContext& ctx) {
  bool reverse = false;
  bool unique = false;
  std::vector<std::string> paths;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_sort(ctx.state);
      return 0;
    }
    if (ctx.args[i] == "-r") {
      reverse = true;
      continue;
    }
    if (ctx.args[i] == "-u") {
      unique = true;
      continue;
    }
    paths.push_back(ctx.args[i]);
  }
  if (paths.empty()) {
    paths.push_back("-");
  }

  int result = 0;
  for (const std::string& path : paths) {
    std::string text;
    std::string label;
    if (!read_text_source(ctx, path, &text, &label)) {
      result = 1;
      continue;
    }
    std::vector<std::string> lines = split_lines(text);
    std::sort(lines.begin(), lines.end());
    if (unique) {
      lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    }
    if (reverse) {
      std::reverse(lines.begin(), lines.end());
    }
    for (const std::string& line : lines) {
      shell_write(ctx.state, line.c_str());
    }
  }
  return result;
}

void shell_help_uniq(ShellState& state) {
  shell_write_line(state, "Usage: uniq [OPTION]... [FILE]");
  shell_write_line(state, "Report or omit repeated lines.");
}

int shell_cmd_uniq(ShellContext& ctx) {
  bool count = false;
  std::string path = "-";
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_uniq(ctx.state);
      return 0;
    }
    if (ctx.args[i] == "-c") {
      count = true;
      continue;
    }
    path = ctx.args[i];
  }
  std::string text;
  std::string label;
  if (!read_text_source(ctx, path, &text, &label)) {
    return 1;
  }
  const std::vector<std::string> lines = split_lines(text);
  std::string previous;
  size_t repeated = 0U;
  for (const std::string& line : lines) {
    if (line == previous) {
      ++repeated;
      continue;
    }
    if (!previous.empty()) {
      if (count) {
        shell_printf(ctx.state, "%4lu %s", static_cast<unsigned long>(repeated), previous.c_str());
      } else {
        shell_write(ctx.state, previous.c_str());
      }
    }
    previous = line;
    repeated = 1U;
  }
  if (!previous.empty()) {
    if (count) {
      shell_printf(ctx.state, "%4lu %s", static_cast<unsigned long>(repeated), previous.c_str());
    } else {
      shell_write(ctx.state, previous.c_str());
    }
  }
  return 0;
}

void shell_help_cut(ShellState& state) {
  shell_write_line(state, "Usage: cut -d DELIM -f LIST [FILE]...");
}

int shell_cmd_cut(ShellContext& ctx) {
  char delim = ' ';
  size_t field = 1U;
  std::vector<std::string> paths;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_cut(ctx.state);
      return 0;
    }
    if ((ctx.args[i] == "-d" || ctx.args[i] == "--delimiter") && (i + 1U) < ctx.args.size()) {
      delim = ctx.args[++i].empty() ? ' ' : ctx.args[i][0];
      continue;
    }
    if ((ctx.args[i] == "-f" || ctx.args[i] == "--fields") && (i + 1U) < ctx.args.size()) {
      field = static_cast<size_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10));
      if (field == 0U) field = 1U;
      continue;
    }
    paths.push_back(ctx.args[i]);
  }
  if (paths.empty()) {
    paths.push_back("-");
  }
  int result = 0;
  for (const std::string& path : paths) {
    std::string text;
    std::string label;
    if (!read_text_source(ctx, path, &text, &label)) {
      result = 1;
      continue;
    }
    const std::vector<std::string> lines = split_lines(text);
    for (const std::string& line : lines) {
      size_t current_field = 1U;
      size_t start = 0U;
      size_t end = 0U;
      while (start <= line.size()) {
        end = line.find(delim, start);
        const size_t actual_end = end == std::string::npos ? line.size() : end;
        if (current_field == field) {
          shell_write_line(ctx.state, line.substr(start, actual_end - start).c_str());
          break;
        }
        if (end == std::string::npos) {
          break;
        }
        start = end + 1U;
        ++current_field;
      }
    }
  }
  return result;
}

void shell_help_tr(ShellState& state) {
  shell_write_line(state, "Usage: tr [-d] SET1 [SET2]");
}

int shell_cmd_tr(ShellContext& ctx) {
  bool delete_mode = false;
  size_t arg_index = 1U;
  if (arg_index < ctx.args.size() && ctx.args[arg_index] == "-d") {
    delete_mode = true;
    ++arg_index;
  }
  if (arg_index >= ctx.args.size()) {
    shell_help_tr(ctx.state);
    return 1;
  }
  const std::string set1 = ctx.args[arg_index++];
  const std::string set2 = delete_mode || arg_index >= ctx.args.size() ? std::string() : ctx.args[arg_index];
  if (ctx.stdin_buffer == nullptr) {
    shell_write_line(ctx.state, "tr: stdin is required");
    return 1;
  }
  std::string out;
  for (const char ch : *ctx.stdin_buffer) {
    const size_t pos = set1.find(ch);
    if (delete_mode) {
      if (pos == std::string::npos) out.push_back(ch);
      continue;
    }
    if (pos == std::string::npos) {
      out.push_back(ch);
      continue;
    }
    out.push_back(pos < set2.size() ? set2[pos] : set2.empty() ? ch : set2.back());
  }
  shell_write(ctx.state, out.c_str());
  return 0;
}

void shell_help_xargs(ShellState& state) {
  shell_write_line(state, "Usage: COMMAND | xargs [COMMAND [ARG]...]");
}

int shell_cmd_xargs(ShellContext& ctx) {
  if (ctx.stdin_buffer == nullptr) {
    shell_write_line(ctx.state, "xargs: stdin is required");
    return 1;
  }
  std::vector<std::string> items = split_whitespace(*ctx.stdin_buffer);
  std::string command = ctx.args.size() > 1U ? join_args(ctx.args, 1U) : "echo";
  for (const std::string& item : items) {
    if (!command.empty()) {
      command.push_back(' ');
    }
    command += item;
  }
  return execute_line_on_state(ctx.state, command.c_str(), false, ctx.transport) ? 0 : 1;
}

void shell_help_basename(ShellState& state) {
  shell_write_line(state, "Usage: basename NAME...");
}

int shell_cmd_basename(ShellContext& ctx) {
  if (ctx.args.size() <= 1U) {
    shell_help_basename(ctx.state);
    return 1;
  }
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    shell_write_line(ctx.state, shell_basename(ctx.args[i]).c_str());
  }
  return 0;
}

void shell_help_dirname(ShellState& state) {
  shell_write_line(state, "Usage: dirname NAME...");
}

int shell_cmd_dirname(ShellContext& ctx) {
  if (ctx.args.size() <= 1U) {
    shell_help_dirname(ctx.state);
    return 1;
  }
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    shell_write_line(ctx.state, shell_parent_path(shell_normalize_path(ctx.state, ctx.args[i])).c_str());
  }
  return 0;
}

void shell_help_realpath(ShellState& state) {
  shell_write_line(state, "Usage: realpath FILE...");
}

int shell_cmd_realpath(ShellContext& ctx) {
  if (ctx.args.size() <= 1U) {
    shell_help_realpath(ctx.state);
    return 1;
  }
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    shell_write_line(ctx.state, shell_normalize_path(ctx.state, ctx.args[i]).c_str());
  }
  return 0;
}

void shell_help_cmp(ShellState& state) {
  shell_write_line(state, "Usage: cmp FILE1 FILE2");
}

int shell_cmd_cmp(ShellContext& ctx) {
  if (ctx.args.size() != 3U) {
    shell_help_cmp(ctx.state);
    return 1;
  }
  std::vector<unsigned char> left;
  std::vector<unsigned char> right;
  std::string label;
  if (!read_binary_source(ctx, ctx.args[1], &left, &label) ||
      !read_binary_source(ctx, ctx.args[2], &right, &label)) {
    return 1;
  }
  const size_t min_size = std::min(left.size(), right.size());
  for (size_t i = 0U; i < min_size; ++i) {
    if (left[i] != right[i]) {
      shell_printf(ctx.state, "%s %s differ: byte %lu\n", ctx.args[1].c_str(), ctx.args[2].c_str(),
                   static_cast<unsigned long>(i + 1U));
      return 1;
    }
  }
  if (left.size() != right.size()) {
    shell_printf(ctx.state, "%s %s differ: size %lu != %lu\n",
                 ctx.args[1].c_str(),
                 ctx.args[2].c_str(),
                 static_cast<unsigned long>(left.size()),
                 static_cast<unsigned long>(right.size()));
    return 1;
  }
  return 0;
}

void shell_help_strings(ShellState& state) {
  shell_write_line(state, "Usage: strings [-n NUM] FILE...");
}

int shell_cmd_strings(ShellContext& ctx) {
  size_t min_length = 4U;
  std::vector<std::string> paths;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if ((ctx.args[i] == "-n" || ctx.args[i] == "--bytes") && (i + 1U) < ctx.args.size()) {
      min_length = static_cast<size_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10));
      continue;
    }
    if (ctx.args[i] == "--help") {
      shell_help_strings(ctx.state);
      return 0;
    }
    paths.push_back(ctx.args[i]);
  }
  if (paths.empty()) {
    shell_help_strings(ctx.state);
    return 1;
  }
  int result = 0;
  for (const std::string& path : paths) {
    std::vector<unsigned char> bytes;
    std::string label;
    if (!read_binary_source(ctx, path, &bytes, &label)) {
      result = 1;
      continue;
    }
    std::string current;
    for (const unsigned char ch : bytes) {
      if (has_printable_run(ch)) {
        current.push_back(static_cast<char>(ch));
      } else {
        if (current.size() >= min_length) {
          shell_write_line(ctx.state, current.c_str());
        }
        current.clear();
      }
    }
    if (current.size() >= min_length) {
      shell_write_line(ctx.state, current.c_str());
    }
  }
  return result;
}

void shell_help_sync(ShellState& state) {
  shell_write_line(state, "Usage: sync");
}

int shell_cmd_sync(ShellContext& ctx) {
  (void)ctx;
  return 0;
}

void shell_help_env(ShellState& state) {
  shell_write_line(state, "Usage: env");
}

int shell_cmd_env(ShellContext& ctx) {
  shell_printf(ctx.state, "USER=%s\n", effective_user(ctx.state).c_str());
  shell_printf(ctx.state, "PWD=%s\n", ctx.state.cwd.c_str());
  shell_printf(ctx.state, "HOME=%s\n", "/ESPUSER");
  shell_printf(ctx.state, "SHELL=%s\n", kShellName);
  for (const auto& entry : ctx.state.env_vars) {
    shell_printf(ctx.state, "%s=%s\n", entry.first.c_str(), entry.second.c_str());
  }
  return 0;
}

void shell_help_export(ShellState& state) {
  shell_write_line(state, "Usage: export NAME=VALUE ...");
}

int shell_cmd_export(ShellContext& ctx) {
  if (ctx.args.size() <= 1U) {
    return shell_cmd_env(ctx);
  }
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const size_t eq = ctx.args[i].find('=');
    if (eq == std::string::npos || eq == 0U) {
      shell_printf(ctx.state, "export: invalid assignment '%s'\n", ctx.args[i].c_str());
      return 1;
    }
    ctx.state.env_vars[ctx.args[i].substr(0U, eq)] = ctx.args[i].substr(eq + 1U);
  }
  return 0;
}

void shell_help_printenv(ShellState& state) {
  shell_write_line(state, "Usage: printenv [NAME]...");
}

int shell_cmd_printenv(ShellContext& ctx) {
  if (ctx.args.size() == 1U) {
    return shell_cmd_env(ctx);
  }
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const auto it = ctx.state.env_vars.find(ctx.args[i]);
    if (it != ctx.state.env_vars.end()) {
      shell_write_line(ctx.state, it->second.c_str());
    }
  }
  return 0;
}

void shell_help_history(ShellState& state) {
  shell_write_line(state, "Usage: history");
}

int shell_cmd_history(ShellContext& ctx) {
  for (size_t i = 0U; i < ctx.state.history.size(); ++i) {
    shell_printf(ctx.state, "%4lu  %s\n", static_cast<unsigned long>(i + 1U), ctx.state.history[i].c_str());
  }
  return 0;
}

void shell_help_which(ShellState& state) {
  shell_write_line(state, "Usage: which COMMAND...");
}

int shell_cmd_which(ShellContext& ctx) {
  if (ctx.args.size() <= 1U) {
    shell_help_which(ctx.state);
    return 1;
  }
  int result = 0;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (shell_find_command(ctx.args[i].c_str()) != nullptr) {
      shell_write_line(ctx.state, ctx.args[i].c_str());
      continue;
    }
    bool alias_found = false;
    for (const ShellAliasRecord& alias : shell_aliases()) {
      if (alias.name == ctx.args[i]) {
        shell_printf(ctx.state, "%s: aliased to %s\n", alias.name.c_str(), alias.value.c_str());
        alias_found = true;
        break;
      }
    }
    if (!alias_found) {
      result = 1;
    }
  }
  return result;
}

void shell_help_test(ShellState& state) {
  shell_write_line(state, "Usage: test EXPR");
}

int shell_cmd_test(ShellContext& ctx) {
  std::vector<std::string> args = ctx.args;
  if (!args.empty() && args.front() == "[" && !args.empty() && args.back() == "]") {
    args.pop_back();
  }
  if (args.size() == 3U && args[1] == "-e") {
    bool is_dir = false;
    std::string error;
    return shell_path_exists(ctx.state, shell_normalize_path(ctx.state, args[2]), &is_dir, nullptr, &error) ? 0 : 1;
  }
  if (args.size() == 3U && args[1] == "-f") {
    bool is_dir = false;
    std::string error;
    return (shell_path_exists(ctx.state, shell_normalize_path(ctx.state, args[2]), &is_dir, nullptr, &error) && !is_dir) ? 0 : 1;
  }
  if (args.size() == 3U && args[1] == "-d") {
    bool is_dir = false;
    std::string error;
    return (shell_path_exists(ctx.state, shell_normalize_path(ctx.state, args[2]), &is_dir, nullptr, &error) && is_dir) ? 0 : 1;
  }
  if (args.size() == 3U && args[1] == "-n") {
    return args[2].empty() ? 1 : 0;
  }
  if (args.size() == 3U && args[1] == "-z") {
    return args[2].empty() ? 0 : 1;
  }
  if (args.size() == 4U && args[2] == "=") {
    return args[1] == args[3] ? 0 : 1;
  }
  if (args.size() == 4U && args[2] == "!=") {
    return args[1] != args[3] ? 0 : 1;
  }
  return 1;
}

void shell_help_read(ShellState& state) {
  shell_write_line(state, "Usage: read NAME [VALUE...]");
}

int shell_cmd_read(ShellContext& ctx) {
  if (ctx.args.size() < 2U) {
    shell_help_read(ctx.state);
    return 1;
  }
  std::string value;
  if (ctx.args.size() > 2U) {
    value = join_args(ctx.args, 2U);
  } else if (ctx.stdin_buffer != nullptr) {
    value = *ctx.stdin_buffer;
    const size_t nl = value.find('\n');
    if (nl != std::string::npos) {
      value = value.substr(0U, nl);
    }
  } else {
    shell_write_line(ctx.state, "read: stdin is required when no value is provided");
    return 1;
  }
  ctx.state.env_vars[ctx.args[1]] = trim_copy_local(value);
  return 0;
}

void shell_help_true(ShellState& state) { shell_write_line(state, "Usage: true"); }
int shell_cmd_true(ShellContext& ctx) { (void)ctx; return 0; }

void shell_help_false(ShellState& state) { shell_write_line(state, "Usage: false"); }
int shell_cmd_false(ShellContext& ctx) { (void)ctx; return 1; }

void shell_help_whoami(ShellState& state) { shell_write_line(state, "Usage: whoami"); }

int shell_cmd_whoami(ShellContext& ctx) {
  shell_write_line(ctx.state, effective_user(ctx.state).c_str());
  return 0;
}

void shell_help_id(ShellState& state) { shell_write_line(state, "Usage: id"); }

int shell_cmd_id(ShellContext& ctx) {
  const std::vector<std::string> groups = effective_groups(ctx.state);
  shell_printf(ctx.state, "uid=%s gid=%s groups=", effective_user(ctx.state).c_str(), groups.front().c_str());
  for (size_t i = 0U; i < groups.size(); ++i) {
    shell_write(ctx.state, groups[i].c_str());
    if ((i + 1U) < groups.size()) {
      shell_write(ctx.state, ",");
    }
  }
  shell_write(ctx.state, "\n");
  return 0;
}

void shell_help_groups(ShellState& state) { shell_write_line(state, "Usage: groups"); }

int shell_cmd_groups(ShellContext& ctx) {
  const std::vector<std::string> groups = effective_groups(ctx.state);
  for (size_t i = 0U; i < groups.size(); ++i) {
    shell_write(ctx.state, groups[i].c_str());
    if ((i + 1U) < groups.size()) {
      shell_write(ctx.state, " ");
    }
  }
  shell_write(ctx.state, "\n");
  return 0;
}

void shell_help_chmod(ShellState& state) {
  shell_write_line(state, "Usage: chmod MODE FILE...");
}

int shell_cmd_chmod(ShellContext& ctx) {
  if (ctx.args.size() < 3U) {
    shell_help_chmod(ctx.state);
    return 1;
  }
  uint32_t mode = 0U;
  if (!parse_mode_octal(ctx.args[1], &mode)) {
    shell_write_line(ctx.state, "chmod: mode must be octal");
    return 1;
  }
  std::vector<MetadataRecord> rows = load_metadata(ctx.state);
  for (size_t i = 2U; i < ctx.args.size(); ++i) {
    const std::string path = shell_normalize_path(ctx.state, ctx.args[i]);
    bool is_dir = false;
    std::string error;
    if (!shell_path_exists(ctx.state, path, &is_dir, nullptr, &error)) {
      shell_printf(ctx.state, "chmod: %s: %s\n", ctx.args[i].c_str(), error.c_str());
      return 1;
    }
    const MetadataRecord current = get_or_create_metadata(ctx.state, path, is_dir, &rows);
    if (!is_owner_or_admin(ctx.state, current)) {
      shell_printf(ctx.state, "chmod: %s: operation not permitted\n", ctx.args[i].c_str());
      return 1;
    }
    bool updated = false;
    for (MetadataRecord& row : rows) {
      if (row.path == path) {
        row.mode = mode;
        updated = true;
      }
    }
    if (!updated) {
      MetadataRecord row = default_metadata(ctx.state, path, is_dir);
      row.mode = mode;
      rows.push_back(row);
    }
  }
  return save_metadata(ctx.state, rows) ? 0 : 1;
}

void shell_help_chown(ShellState& state) {
  shell_write_line(state, "Usage: chown OWNER[:GROUP] FILE...");
}

int shell_cmd_chown(ShellContext& ctx) {
  if (ctx.args.size() < 3U) {
    shell_help_chown(ctx.state);
    return 1;
  }
  if (!ctx.state.root_session && !ctx.state.session_admin) {
    shell_write_line(ctx.state, "chown: requires admin or root privileges");
    return 1;
  }
  const std::string spec = ctx.args[1];
  const size_t colon = spec.find(':');
  const std::string owner = colon == std::string::npos ? spec : spec.substr(0U, colon);
  const std::string group = colon == std::string::npos ? std::string() : spec.substr(colon + 1U);
  if (!mros::ssh::user_exists(owner.c_str()) && owner != mros::ssh::root_username()) {
    shell_write_line(ctx.state, "chown: unknown owner");
    return 1;
  }
  std::vector<MetadataRecord> rows = load_metadata(ctx.state);
  for (size_t i = 2U; i < ctx.args.size(); ++i) {
    const std::string path = shell_normalize_path(ctx.state, ctx.args[i]);
    bool is_dir = false;
    std::string error;
    if (!shell_path_exists(ctx.state, path, &is_dir, nullptr, &error)) {
      shell_printf(ctx.state, "chown: %s: %s\n", ctx.args[i].c_str(), error.c_str());
      return 1;
    }
    bool updated = false;
    for (MetadataRecord& row : rows) {
      if (row.path == path) {
        row.owner = owner;
        if (!group.empty()) row.group = group;
        updated = true;
      }
    }
    if (!updated) {
      MetadataRecord row = default_metadata(ctx.state, path, is_dir);
      row.owner = owner;
      if (!group.empty()) row.group = group;
      rows.push_back(row);
    }
  }
  return save_metadata(ctx.state, rows) ? 0 : 1;
}

void shell_help_sudo(ShellState& state) {
  shell_write_line(state, "Usage: sudo -p ROOT_PASSWORD COMMAND...");
}

int shell_cmd_sudo(ShellContext& ctx) {
  if (!ctx.state.session_can_sudo && !ctx.state.root_session) {
    shell_write_line(ctx.state, "sudo: this account is not allowed to elevate");
    return 1;
  }
  size_t arg_index = 1U;
  std::string password;
  if (ctx.state.root_session) {
    password = "";
  } else {
    if (arg_index < ctx.args.size() && ctx.args[arg_index] == "-p" && (arg_index + 1U) < ctx.args.size()) {
      password = ctx.args[arg_index + 1U];
      arg_index += 2U;
    } else if (arg_index < ctx.args.size()) {
      password = ctx.args[arg_index++];
    }
    if (!mros::ssh::verify_password(mros::ssh::root_username(), String(password.c_str()))) {
      shell_write_line(ctx.state, "sudo: authentication failed");
      audit_record("sudo-failed", ctx.state.session_username.c_str());
      return 1;
    }
    if (!shell_can_enter_root(ctx.state)) {
      shell_write_line(ctx.state, "sudo: another root shell session is already active");
      audit_record("sudo-denied", "root session limit");
      return 1;
    }
  }
  if (arg_index >= ctx.args.size()) {
    shell_write_line(ctx.state, "sudo: command required");
    return 1;
  }
  const bool previous_root = ctx.state.root_session;
  const uint32_t previous_caps = ctx.state.capability_mask;
  ctx.state.root_session = true;
  ctx.state.capability_mask = kShellCapabilityRoot;
  audit_record("sudo", ctx.args[arg_index].c_str());
  const int result = execute_line_on_state(ctx.state, join_args(ctx.args, arg_index).c_str(), false, ctx.transport) ? 0 : 1;
  ctx.state.root_session = previous_root;
  ctx.state.capability_mask = previous_caps;
  return result;
}

void shell_help_passwd(ShellState& state) {
  shell_write_line(state, "Usage: passwd [USER] \"NEW_PASS\" \"NEW_PASS\"");
}

int shell_cmd_passwd(ShellContext& ctx) {
  if (ctx.args.size() < 3U) {
    shell_help_passwd(ctx.state);
    return 1;
  }
  std::string user = effective_user(ctx.state);
  size_t pass_index = 1U;
  if (ctx.args.size() >= 4U) {
    user = ctx.args[1];
    pass_index = 2U;
  }
  if ((pass_index + 1U) >= ctx.args.size()) {
    shell_help_passwd(ctx.state);
    return 1;
  }
  if (ctx.args[pass_index] != ctx.args[pass_index + 1U]) {
    shell_write_line(ctx.state, "passwd: password confirmation mismatch");
    return 1;
  }
  if (user != effective_user(ctx.state) && !ctx.state.root_session && !ctx.state.session_admin) {
    shell_write_line(ctx.state, "passwd: only admin/root may change another user's password");
    return 1;
  }
  if (user == mros::ssh::root_username() && !ctx.state.root_session) {
    shell_write_line(ctx.state, "passwd: root password changes require su first");
    return 1;
  }
  if (!mros::ssh::set_password_for_user(String(user.c_str()), String(ctx.args[pass_index].c_str()))) {
    shell_write_line(ctx.state, "passwd: password must be 8-96 chars and user must exist");
    return 1;
  }
  shell_write_line(ctx.state, "passwd: password hash saved");
  return 0;
}

}  // namespace mros::shell
