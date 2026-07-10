#include "src/shell/mros_shell_internal.h"

#include <mbedtls/md.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace mros::shell {
namespace {

constexpr size_t kMaxToolBytes = 128U * 1024U;
constexpr size_t kDirStackMax = 16U;

std::string visible_path(const ShellState& state, const std::string& normalized) {
  const std::string mount = shell_storage_mount_path(state);
  if (normalized == mount) return "/fs";
  if (normalized.rfind(mount + "/", 0U) == 0U) return "/fs" + normalized.substr(mount.size());
  return normalized;
}

bool parse_size(const std::string& text, size_t* out) {
  if (out == nullptr || text.empty()) return false;
  char* end = nullptr;
  const unsigned long value = std::strtoul(text.c_str(), &end, 0);
  if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
  *out = static_cast<size_t>(value);
  return true;
}

bool read_bytes(ShellContext& ctx,
                const std::string& source,
                std::vector<uint8_t>* out,
                std::string* label) {
  if (out == nullptr || label == nullptr) return false;
  out->clear();
  *label = source;
  if (source == "-") {
    if (ctx.stdin_buffer == nullptr) {
      shell_write_line(ctx.state, "stdin is not available");
      return false;
    }
    out->assign(ctx.stdin_buffer->begin(), ctx.stdin_buffer->end());
    *label = "(stdin)";
    return true;
  }

  const std::string normalized = shell_normalize_path(ctx.state, source);
  std::string actual;
  std::string error;
  if (!shell_openable_file_path(ctx.state, normalized, &actual, &error)) {
    shell_printf(ctx.state, "%s: %s\n", source.c_str(), error.c_str());
    return false;
  }
  FILE* file = std::fopen(actual.c_str(), "rb");
  if (file == nullptr) {
    shell_printf(ctx.state, "%s: unable to open file\n", source.c_str());
    return false;
  }
  uint8_t buffer[384] = {};
  size_t total = 0U;
  bool truncated = false;
  while (true) {
    const size_t n = std::fread(buffer, 1U, sizeof(buffer), file);
    if (n == 0U) break;
    const size_t room = total < kMaxToolBytes ? (kMaxToolBytes - total) : 0U;
    if (n > room) {
      if (room > 0U) out->insert(out->end(), buffer, buffer + room);
      truncated = true;
      break;
    }
    out->insert(out->end(), buffer, buffer + n);
    total += n;
  }
  std::fclose(file);
  *label = visible_path(ctx.state, normalized);
  if (truncated) shell_write_line(ctx.state, "tool: input truncated at 128KB");
  return true;
}

bool read_text(ShellContext& ctx, const std::string& source, std::string* out, std::string* label) {
  std::vector<uint8_t> bytes;
  if (!read_bytes(ctx, source, &bytes, label)) return false;
  out->assign(bytes.begin(), bytes.end());
  return true;
}

std::vector<std::string> input_paths(ShellContext& ctx, size_t first, bool default_stdin = true) {
  std::vector<std::string> paths;
  for (size_t i = first; i < ctx.args.size(); ++i) {
    if (ctx.args[i] != "--json") paths.push_back(ctx.args[i]);
  }
  if (paths.empty() && default_stdin) paths.push_back("-");
  return paths;
}

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320UL : 0UL);
    }
  }
  return ~crc;
}

uint32_t posix_cksum_crc(const std::vector<uint8_t>& bytes) {
  uint32_t crc = 0U;
  for (const uint8_t b : bytes) {
    crc ^= static_cast<uint32_t>(b) << 24U;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80000000UL) ? ((crc << 1U) ^ 0x04C11DB7UL) : (crc << 1U);
    }
  }
  size_t len = bytes.size();
  while (len != 0U) {
    const uint8_t b = static_cast<uint8_t>(len & 0xFFU);
    crc ^= static_cast<uint32_t>(b) << 24U;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80000000UL) ? ((crc << 1U) ^ 0x04C11DB7UL) : (crc << 1U);
    }
    len >>= 8U;
  }
  return ~crc;
}

int digest_command(ShellContext& ctx, const char* name, mbedtls_md_type_t type, size_t first_arg) {
  const auto paths = input_paths(ctx, first_arg);
  int result = 0;
  for (const std::string& path : paths) {
    std::vector<uint8_t> bytes;
    std::string label;
    if (!read_bytes(ctx, path, &bytes, &label)) {
      result = 1;
      continue;
    }
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(type);
    unsigned char digest[64] = {};
    if (info == nullptr || mbedtls_md(info, bytes.data(), bytes.size(), digest) != 0) {
      shell_printf(ctx.state, "%s: %s: digest failed\n", name, label.c_str());
      result = 1;
      continue;
    }
    const size_t digest_len = static_cast<size_t>(mbedtls_md_get_size(info));
    for (size_t i = 0; i < digest_len; ++i) shell_printf(ctx.state, "%02x", digest[i]);
    shell_printf(ctx.state, "  %s\n", label.c_str());
  }
  return result;
}

bool starts_with(const std::vector<uint8_t>& bytes, const char* magic, size_t n) {
  return bytes.size() >= n && std::memcmp(bytes.data(), magic, n) == 0;
}

std::string classify_bytes(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) return "empty";
  if (starts_with(bytes, "\x7F""ELF", 4U)) return "ELF binary";
  if (starts_with(bytes, "\x1F\x8B", 2U)) return "gzip compressed data";
  if (starts_with(bytes, "ustar", 5U)) return "tar archive";
  if (starts_with(bytes, "\xFF\xD8\xFF", 3U)) return "JPEG image data";
  if (starts_with(bytes, "\x89PNG\r\n\x1A\n", 8U)) return "PNG image data";
  if (starts_with(bytes, "GIF87a", 6U) || starts_with(bytes, "GIF89a", 6U)) return "GIF image data";
  if (starts_with(bytes, "{", 1U) || starts_with(bytes, "[", 1U)) return "JSON text";
  size_t printable = 0U;
  size_t control = 0U;
  for (const uint8_t b : bytes) {
    if (b == '\n' || b == '\r' || b == '\t' || (b >= 32U && b < 127U)) printable++;
    else if (b < 32U) control++;
  }
  if (printable * 100U >= bytes.size() * 90U && control == 0U) return "ASCII text";
  if (printable * 100U >= bytes.size() * 80U) return "mostly text";
  return "data";
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : text) {
    cur.push_back(ch);
    if (ch == '\n') {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::vector<std::string> split_fields(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

void write_wrapped(ShellState& state, const std::string& line, size_t width) {
  size_t col = 0U;
  for (char ch : line) {
    if (ch == '\n') {
      shell_write(state, "\n");
      col = 0U;
      continue;
    }
    if (col >= width) {
      shell_write(state, "\n");
      col = 0U;
    }
    char s[2] = {ch, '\0'};
    shell_write(state, s);
    ++col;
  }
  if (line.empty() || line.back() != '\n') shell_write(state, "\n");
}

class ExprParser {
 public:
  explicit ExprParser(const std::string& text) : text_(text) {}
  bool parse(double* out) {
    if (out == nullptr) return false;
    pos_ = 0U;
    const double value = parse_expr();
    skip_ws();
    if (!ok_ || pos_ != text_.size()) return false;
    *out = value;
    return true;
  }

 private:
  void skip_ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) pos_++;
  }
  bool consume(char ch) {
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == ch) {
      pos_++;
      return true;
    }
    return false;
  }
  double parse_number() {
    skip_ws();
    char* end = nullptr;
    const double value = std::strtod(text_.c_str() + pos_, &end);
    if (end == text_.c_str() + pos_) {
      ok_ = false;
      return 0.0;
    }
    pos_ = static_cast<size_t>(end - text_.c_str());
    return value;
  }
  double parse_factor() {
    skip_ws();
    if (consume('+')) return parse_factor();
    if (consume('-')) return -parse_factor();
    if (consume('(')) {
      const double value = parse_expr();
      if (!consume(')')) ok_ = false;
      return value;
    }
    return parse_number();
  }
  double parse_term() {
    double value = parse_factor();
    while (ok_) {
      if (consume('*')) value *= parse_factor();
      else if (consume('/')) {
        const double denom = parse_factor();
        if (denom == 0.0) ok_ = false;
        else value /= denom;
      } else {
        break;
      }
    }
    return value;
  }
  double parse_expr() {
    double value = parse_term();
    while (ok_) {
      if (consume('+')) value += parse_term();
      else if (consume('-')) value -= parse_term();
      else break;
    }
    return value;
  }

  std::string text_;
  size_t pos_ = 0U;
  bool ok_ = true;
};

std::string join_args(const std::vector<std::string>& args, size_t first) {
  std::string out;
  for (size_t i = first; i < args.size(); ++i) {
    if (!out.empty()) out.push_back(' ');
    out += args[i];
  }
  return out;
}

struct UnitDef {
  const char* name;
  double factor;
  const char* family;
};

const UnitDef* find_unit(const std::string& name) {
  static const UnitDef kUnits[] = {
      {"um", 0.001, "length"}, {"mm", 1.0, "length"}, {"cm", 10.0, "length"},
      {"m", 1000.0, "length"}, {"in", 25.4, "length"}, {"inch", 25.4, "length"},
      {"deg", 1.0, "angle"}, {"rad", 57.29577951308232, "angle"},
      {"ms", 0.001, "time"}, {"s", 1.0, "time"}, {"min", 60.0, "time"},
      {"v", 1.0, "voltage"}, {"mv", 0.001, "voltage"},
      {"a", 1.0, "current"}, {"ma", 0.001, "current"},
      {"hz", 1.0, "freq"}, {"khz", 1000.0, "freq"},
      {"nm", 1.0, "torque"}, {"kgfcm", 0.0980665, "torque"}, {"ozin", 0.00706155, "torque"},
  };
  for (const UnitDef& unit : kUnits) {
    if (name == unit.name) return &unit;
  }
  return nullptr;
}

bool change_dir(ShellContext& ctx, const std::string& path) {
  const std::string normalized = shell_normalize_path(ctx.state, path);
  bool is_dir = false;
  std::string error;
  if (!shell_path_exists(ctx.state, normalized, &is_dir, nullptr, &error) || !is_dir) {
    shell_printf(ctx.state, "%s: %s\n", path.c_str(), error.empty() ? "not a directory" : error.c_str());
    return false;
  }
  ctx.state.previous_cwd = ctx.state.cwd;
  ctx.state.cwd = normalized;
  return true;
}

}  // namespace

void shell_help_file(ShellState& state) {
  shell_write_line(state, "Usage: file FILE...");
  shell_write_line(state, "Identify file type from metadata and common magic bytes.");
}

int shell_cmd_file(ShellContext& ctx) {
  if (ctx.args.size() <= 1U || ctx.args[1] == "--help") {
    shell_help_file(ctx.state);
    return ctx.args.size() <= 1U ? 1 : 0;
  }
  int rc = 0;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    std::vector<uint8_t> bytes;
    std::string label;
    if (!read_bytes(ctx, ctx.args[i], &bytes, &label)) {
      rc = 1;
      continue;
    }
    shell_printf(ctx.state, "%s: %s\n", label.c_str(), classify_bytes(bytes).c_str());
  }
  return rc;
}

void shell_help_md5sum(ShellState& state) {
  shell_write_line(state, "Usage: md5sum [FILE]...");
  shell_write_line(state, "Compute MD5 digests for files or stdin.");
}

int shell_cmd_md5sum(ShellContext& ctx) {
  if (ctx.args.size() >= 2U && ctx.args[1] == "--help") {
    shell_help_md5sum(ctx.state);
    return 0;
  }
  return digest_command(ctx, "md5sum", MBEDTLS_MD_MD5, 1U);
}

void shell_help_crc32(ShellState& state) {
  shell_write_line(state, "Usage: crc32 [FILE]...");
  shell_write_line(state, "Compute Ethernet/ZIP CRC-32 for files or stdin.");
}

int shell_cmd_crc32(ShellContext& ctx) {
  if (ctx.args.size() >= 2U && ctx.args[1] == "--help") {
    shell_help_crc32(ctx.state);
    return 0;
  }
  int rc = 0;
  for (const std::string& path : input_paths(ctx, 1U)) {
    std::vector<uint8_t> bytes;
    std::string label;
    if (!read_bytes(ctx, path, &bytes, &label)) {
      rc = 1;
      continue;
    }
    shell_printf(ctx.state, "%08lx  %s\n", static_cast<unsigned long>(crc32_update(0U, bytes.data(), bytes.size())),
                 label.c_str());
  }
  return rc;
}

void shell_help_cksum(ShellState& state) {
  shell_write_line(state, "Usage: cksum [FILE]...");
  shell_write_line(state, "Compute POSIX cksum CRC and byte count.");
}

int shell_cmd_cksum(ShellContext& ctx) {
  if (ctx.args.size() >= 2U && ctx.args[1] == "--help") {
    shell_help_cksum(ctx.state);
    return 0;
  }
  int rc = 0;
  for (const std::string& path : input_paths(ctx, 1U)) {
    std::vector<uint8_t> bytes;
    std::string label;
    if (!read_bytes(ctx, path, &bytes, &label)) {
      rc = 1;
      continue;
    }
    shell_printf(ctx.state, "%lu %lu %s\n",
                 static_cast<unsigned long>(posix_cksum_crc(bytes)),
                 static_cast<unsigned long>(bytes.size()),
                 label.c_str());
  }
  return rc;
}

void shell_help_od(ShellState& state) {
  shell_write_line(state, "Usage: od [-t x1|o1|c] [-N NUM] [-j NUM] [FILE|-]");
  shell_write_line(state, "Dump bytes in a compact BusyBox-style subset.");
}

int shell_cmd_od(ShellContext& ctx) {
  std::string type = "o1";
  size_t limit = 0U;
  size_t skip = 0U;
  std::string path = "-";
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      shell_help_od(ctx.state);
      return 0;
    }
    if (arg == "-t" && (i + 1U) < ctx.args.size()) {
      type = ctx.args[++i];
      continue;
    }
    if ((arg == "-N" || arg == "--read-bytes") && (i + 1U) < ctx.args.size()) {
      if (!parse_size(ctx.args[++i], &limit)) return 1;
      continue;
    }
    if ((arg == "-j" || arg == "--skip-bytes") && (i + 1U) < ctx.args.size()) {
      if (!parse_size(ctx.args[++i], &skip)) return 1;
      continue;
    }
    path = arg;
  }
  std::vector<uint8_t> bytes;
  std::string label;
  if (!read_bytes(ctx, path, &bytes, &label)) return 1;
  const size_t start = std::min(skip, bytes.size());
  const size_t end = limit > 0U ? std::min(bytes.size(), start + limit) : bytes.size();
  for (size_t off = start; off < end; off += 16U) {
    shell_printf(ctx.state, "%07lo", static_cast<unsigned long>(off));
    for (size_t i = 0U; i < 16U && (off + i) < end; ++i) {
      const uint8_t b = bytes[off + i];
      if (type == "x1") shell_printf(ctx.state, " %02x", b);
      else if (type == "c") shell_printf(ctx.state, " %c", (b >= 32U && b < 127U) ? b : '.');
      else shell_printf(ctx.state, " %03o", b);
    }
    shell_write(ctx.state, "\n");
  }
  shell_printf(ctx.state, "%07lo\n", static_cast<unsigned long>(end));
  return 0;
}

void shell_help_fold(ShellState& state) {
  shell_write_line(state, "Usage: fold [-w WIDTH] [FILE|-]");
}

int shell_cmd_fold(ShellContext& ctx) {
  size_t width = 80U;
  std::string path = "-";
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_fold(ctx.state);
      return 0;
    }
    if ((ctx.args[i] == "-w" || ctx.args[i] == "--width") && (i + 1U) < ctx.args.size()) {
      if (!parse_size(ctx.args[++i], &width) || width == 0U) return 1;
      width = std::min<size_t>(width, 240U);
      continue;
    }
    path = ctx.args[i];
  }
  std::string text;
  std::string label;
  if (!read_text(ctx, path, &text, &label)) return 1;
  for (const std::string& line : split_lines(text)) write_wrapped(ctx.state, line, width);
  return 0;
}

void shell_help_fmt(ShellState& state) {
  shell_write_line(state, "Usage: fmt [-w WIDTH] [FILE|-]");
}

int shell_cmd_fmt(ShellContext& ctx) {
  size_t width = 72U;
  std::string path = "-";
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_fmt(ctx.state);
      return 0;
    }
    if ((ctx.args[i] == "-w" || ctx.args[i] == "--width") && (i + 1U) < ctx.args.size()) {
      if (!parse_size(ctx.args[++i], &width) || width == 0U) return 1;
      continue;
    }
    path = ctx.args[i];
  }
  std::string text;
  std::string label;
  if (!read_text(ctx, path, &text, &label)) return 1;
  std::string words;
  for (char ch : text) words.push_back(std::isspace(static_cast<unsigned char>(ch)) ? ' ' : ch);
  size_t col = 0U;
  for (const std::string& word : split_fields(words)) {
    if (col != 0U && col + 1U + word.size() > width) {
      shell_write(ctx.state, "\n");
      col = 0U;
    } else if (col != 0U) {
      shell_write(ctx.state, " ");
      col++;
    }
    shell_write(ctx.state, word.c_str());
    col += word.size();
  }
  shell_write(ctx.state, "\n");
  return 0;
}

void shell_help_expand(ShellState& state) {
  shell_write_line(state, "Usage: expand [-t TABSTOP] [FILE|-]");
}

int shell_cmd_expand(ShellContext& ctx) {
  size_t tabstop = 8U;
  std::string path = "-";
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_expand(ctx.state);
      return 0;
    }
    if ((ctx.args[i] == "-t" || ctx.args[i] == "--tabs") && (i + 1U) < ctx.args.size()) {
      if (!parse_size(ctx.args[++i], &tabstop) || tabstop == 0U) return 1;
      continue;
    }
    path = ctx.args[i];
  }
  std::string text;
  std::string label;
  if (!read_text(ctx, path, &text, &label)) return 1;
  size_t col = 0U;
  for (char ch : text) {
    if (ch == '\t') {
      const size_t spaces = tabstop - (col % tabstop);
      for (size_t i = 0; i < spaces; ++i) shell_write(ctx.state, " ");
      col += spaces;
    } else {
      char s[2] = {ch, '\0'};
      shell_write(ctx.state, s);
      col = (ch == '\n') ? 0U : (col + 1U);
    }
  }
  return 0;
}

void shell_help_unexpand(ShellState& state) {
  shell_write_line(state, "Usage: unexpand [-t TABSTOP] [FILE|-]");
}

int shell_cmd_unexpand(ShellContext& ctx) {
  size_t tabstop = 8U;
  std::string path = "-";
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_unexpand(ctx.state);
      return 0;
    }
    if ((ctx.args[i] == "-t" || ctx.args[i] == "--tabs") && (i + 1U) < ctx.args.size()) {
      if (!parse_size(ctx.args[++i], &tabstop) || tabstop == 0U) return 1;
      continue;
    }
    path = ctx.args[i];
  }
  std::string text;
  std::string label;
  if (!read_text(ctx, path, &text, &label)) return 1;
  for (const std::string& line : split_lines(text)) {
    size_t leading = 0U;
    while (leading < line.size() && line[leading] == ' ') leading++;
    size_t remaining = leading;
    while (remaining >= tabstop) {
      shell_write(ctx.state, "\t");
      remaining -= tabstop;
    }
    for (size_t i = 0; i < remaining; ++i) shell_write(ctx.state, " ");
    shell_write(ctx.state, line.c_str() + std::min(line.size(), leading));
  }
  return 0;
}

void shell_help_column(ShellState& state) {
  shell_write_line(state, "Usage: column [-t] [FILE|-]");
}

int shell_cmd_column(ShellContext& ctx) {
  std::string path = "-";
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_column(ctx.state);
      return 0;
    }
    if (ctx.args[i] != "-t") path = ctx.args[i];
  }
  std::string text;
  std::string label;
  if (!read_text(ctx, path, &text, &label)) return 1;
  std::vector<std::vector<std::string>> rows;
  std::vector<size_t> widths;
  for (const std::string& line : split_lines(text)) {
    rows.push_back(split_fields(line));
    if (widths.size() < rows.back().size()) widths.resize(rows.back().size(), 0U);
    for (size_t i = 0; i < rows.back().size(); ++i) widths[i] = std::max(widths[i], rows.back()[i].size());
  }
  for (const auto& row : rows) {
    for (size_t i = 0; i < row.size(); ++i) {
      shell_write(ctx.state, row[i].c_str());
      if ((i + 1U) < row.size()) {
        for (size_t pad = row[i].size(); pad < widths[i] + 2U; ++pad) shell_write(ctx.state, " ");
      }
    }
    shell_write(ctx.state, "\n");
  }
  return 0;
}

void shell_help_bc(ShellState& state) {
  shell_write_line(state, "Usage: bc [EXPR]");
  shell_write_line(state, "Evaluate + - * / and parentheses. Reads stdin when EXPR is omitted.");
}

int shell_cmd_bc(ShellContext& ctx) {
  std::string expr = ctx.args.size() > 1U ? join_args(ctx.args, 1U) : (ctx.stdin_buffer != nullptr ? *ctx.stdin_buffer : "");
  if (expr == "--help" || expr.empty()) {
    shell_help_bc(ctx.state);
    return expr.empty() ? 1 : 0;
  }
  double value = 0.0;
  ExprParser parser(expr);
  if (!parser.parse(&value) || !std::isfinite(value)) {
    shell_write_line(ctx.state, "bc: parse error");
    return 1;
  }
  shell_printf(ctx.state, "%.8g\n", value);
  return 0;
}

void shell_help_units(ShellState& state) {
  shell_write_line(state, "Usage: units VALUE FROM TO");
  shell_write_line(state, "Families: length, angle, time, voltage, current, freq, torque.");
}

int shell_cmd_units(ShellContext& ctx) {
  if (ctx.args.size() != 4U || ctx.args[1] == "--help") {
    shell_help_units(ctx.state);
    return ctx.args.size() == 2U && ctx.args[1] == "--help" ? 0 : 1;
  }
  char* end = nullptr;
  const double value = std::strtod(ctx.args[1].c_str(), &end);
  if (end == ctx.args[1].c_str()) return 1;
  const UnitDef* from = find_unit(ctx.args[2]);
  const UnitDef* to = find_unit(ctx.args[3]);
  if (from == nullptr || to == nullptr || std::strcmp(from->family, to->family) != 0) {
    shell_write_line(ctx.state, "units: incompatible or unknown units");
    return 1;
  }
  shell_printf(ctx.state, "%.8g %s\n", value * from->factor / to->factor, to->name);
  return 0;
}

void shell_help_pushd(ShellState& state) {
  shell_write_line(state, "Usage: pushd DIR");
}

int shell_cmd_pushd(ShellContext& ctx) {
  if (ctx.args.size() != 2U || ctx.args[1] == "--help") {
    shell_help_pushd(ctx.state);
    return ctx.args.size() == 2U ? 0 : 1;
  }
  if (ctx.state.dir_stack.size() >= kDirStackMax) ctx.state.dir_stack.erase(ctx.state.dir_stack.begin());
  ctx.state.dir_stack.push_back(ctx.state.cwd);
  if (!change_dir(ctx, ctx.args[1])) {
    ctx.state.dir_stack.pop_back();
    return 1;
  }
  shell_cmd_dirs(ctx);
  return 0;
}

void shell_help_popd(ShellState& state) {
  shell_write_line(state, "Usage: popd");
}

int shell_cmd_popd(ShellContext& ctx) {
  if (ctx.args.size() > 1U && ctx.args[1] == "--help") {
    shell_help_popd(ctx.state);
    return 0;
  }
  if (ctx.state.dir_stack.empty()) {
    shell_write_line(ctx.state, "popd: directory stack empty");
    return 1;
  }
  const std::string next = ctx.state.dir_stack.back();
  ctx.state.dir_stack.pop_back();
  if (!change_dir(ctx, next)) return 1;
  shell_cmd_dirs(ctx);
  return 0;
}

void shell_help_dirs(ShellState& state) {
  shell_write_line(state, "Usage: dirs");
}

int shell_cmd_dirs(ShellContext& ctx) {
  if (ctx.args.size() > 1U && ctx.args[1] == "--help") {
    shell_help_dirs(ctx.state);
    return 0;
  }
  shell_write(ctx.state, visible_path(ctx.state, ctx.state.cwd).c_str());
  for (auto it = ctx.state.dir_stack.rbegin(); it != ctx.state.dir_stack.rend(); ++it) {
    shell_write(ctx.state, " ");
    shell_write(ctx.state, visible_path(ctx.state, *it).c_str());
  }
  shell_write(ctx.state, "\n");
  return 0;
}

}  // namespace mros::shell
