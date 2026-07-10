#include "src/shell/mros_shell_internal.h"
#include "src/platform/mros_file.h"
#include "src/platform/mros_time.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <miniz.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace mros::shell {
namespace {

constexpr size_t kIoChunk = 512U;
constexpr size_t kMaxMemoryFetchBytes = 128U * 1024U;
constexpr size_t kMaxGzipOutputBytes = 4U * 1024U * 1024U;

struct TarHeader {
  char name[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12];
  char mtime[12];
  char checksum[8];
  char typeflag;
  char linkname[100];
  char magic[6];
  char version[2];
  char uname[32];
  char gname[32];
  char devmajor[8];
  char devminor[8];
  char prefix[155];
  char pad[12];
};

static_assert(sizeof(TarHeader) == 512, "ustar header must be 512 bytes");

std::string visible_path(const ShellState& state, const std::string& normalized) {
  const std::string mount = shell_storage_mount_path(state);
  if (normalized == mount) return "/fs";
  if (normalized.rfind(mount + "/", 0U) == 0U) return "/fs" + normalized.substr(mount.size());
  return normalized;
}

bool parse_size_arg(const std::string& text, size_t* out) {
  if (out == nullptr || text.empty()) return false;
  char* end = nullptr;
  const unsigned long value = std::strtoul(text.c_str(), &end, 0);
  if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
  *out = static_cast<size_t>(value);
  return true;
}

bool read_file_bytes(ShellContext& ctx, const std::string& path, std::vector<uint8_t>* out) {
  if (out == nullptr) return false;
  out->clear();
  const std::string normalized = shell_normalize_path(ctx.state, path);
  std::string actual;
  std::string error;
  if (!shell_openable_file_path(ctx.state, normalized, &actual, &error)) {
    shell_printf(ctx.state, "%s: %s\n", path.c_str(), error.c_str());
    return false;
  }
  FILE* file = std::fopen(actual.c_str(), "rb");
  if (file == nullptr) {
    shell_printf(ctx.state, "%s: unable to open file\n", path.c_str());
    return false;
  }
  uint8_t buffer[kIoChunk] = {};
  while (true) {
    const size_t n = std::fread(buffer, 1U, sizeof(buffer), file);
    if (n == 0U) break;
    out->insert(out->end(), buffer, buffer + n);
  }
  std::fclose(file);
  return true;
}

bool writable_actual_path(ShellState& state, const std::string& requested, std::string* normalized, std::string* actual) {
  if (normalized == nullptr || actual == nullptr) return false;
  std::string candidate = requested;
  if (!candidate.empty() && candidate.front() != '/' && state.cwd == "/" && shell_is_storage_mounted(state)) {
    candidate = shell_storage_user_root(state) + "/" + candidate;
  }
  *normalized = shell_normalize_path(state, candidate);
  if (!shell_is_user_writable_path(state, *normalized)) {
    shell_printf(state, "%s: target must be inside /ESPUSER\n", requested.c_str());
    return false;
  }
  *actual = *normalized;
  return true;
}

bool write_atomic(ShellState& state, const std::string& requested, const std::vector<uint8_t>& bytes) {
  std::string normalized;
  std::string actual;
  if (!writable_actual_path(state, requested, &normalized, &actual)) return false;
  const std::string temp = actual + ".tmp-mshell-" +
                           std::to_string(static_cast<unsigned long>(mros::platform::mros_millis()));
  const std::string_view payload(
      reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!mros::platform::mros_file_write_all_atomic(actual.c_str(), temp.c_str(), payload)) {
    shell_printf(state, "%s: rename failed\n", requested.c_str());
    return false;
  }
  shell_printf(state, "%s: wrote %lu bytes\n", visible_path(state, normalized).c_str(),
               static_cast<unsigned long>(bytes.size()));
  return true;
}

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320UL : 0UL);
  }
  return ~crc;
}

void put_u32_le(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 16U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 24U) & 0xFFU));
}

uint32_t get_u32_le(const std::vector<uint8_t>& bytes, size_t off) {
  return static_cast<uint32_t>(bytes[off]) |
         (static_cast<uint32_t>(bytes[off + 1U]) << 8U) |
         (static_cast<uint32_t>(bytes[off + 2U]) << 16U) |
         (static_cast<uint32_t>(bytes[off + 3U]) << 24U);
}

struct DeflateAppendContext {
  std::vector<uint8_t>* out = nullptr;
};

mz_bool append_deflate_bytes(const void* data, int len, void* user) {
  auto* ctx = static_cast<DeflateAppendContext*>(user);
  if (ctx == nullptr || ctx->out == nullptr || len < 0) return MZ_FALSE;
  const auto* bytes = static_cast<const uint8_t*>(data);
  ctx->out->insert(ctx->out->end(), bytes, bytes + len);
  return MZ_TRUE;
}

struct InflateAppendContext {
  std::vector<uint8_t>* out = nullptr;
  size_t max_bytes = 0U;
};

int append_inflated_bytes(const void* data, int len, void* user) {
  auto* ctx = static_cast<InflateAppendContext*>(user);
  if (ctx == nullptr || ctx->out == nullptr || len < 0) return 0;
  if (ctx->out->size() + static_cast<size_t>(len) > ctx->max_bytes) return 0;
  const auto* bytes = static_cast<const uint8_t*>(data);
  ctx->out->insert(ctx->out->end(), bytes, bytes + len);
  return 1;
}

std::vector<uint8_t> gzip_store(const std::vector<uint8_t>& input) {
  std::vector<uint8_t> out;
  out.reserve(input.size() + 32U);
  const uint8_t header[] = {0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x03};
  out.insert(out.end(), header, header + sizeof(header));
  DeflateAppendContext deflate_ctx {&out};
  if (!tdefl_compress_mem_to_output(input.data(),
                                    input.size(),
                                    append_deflate_bytes,
                                    &deflate_ctx,
                                    TDEFL_DEFAULT_MAX_PROBES)) {
    out.clear();
    return out;
  }
  put_u32_le(&out, crc32_update(0U, input.data(), input.size()));
  put_u32_le(&out, static_cast<uint32_t>(input.size()));
  return out;
}

bool gzip_unstore(const std::vector<uint8_t>& input, std::vector<uint8_t>* output, std::string* error) {
  if (output == nullptr) return false;
  output->clear();
  if (input.size() < 18U || input[0] != 0x1F || input[1] != 0x8B || input[2] != 0x08) {
    if (error) *error = "not a gzip deflate stream";
    return false;
  }
  size_t pos = 10U;
  const uint8_t flags = input[3];
  if ((flags & 0xE0U) != 0U) {
    if (error) *error = "reserved gzip flags are set";
    return false;
  }
  if ((flags & 0x04U) != 0U) {
    if (pos + 2U > input.size()) return false;
    const size_t xlen = input[pos] | (static_cast<size_t>(input[pos + 1U]) << 8U);
    pos += 2U + xlen;
  }
  if ((flags & 0x08U) != 0U) while (pos < input.size() && input[pos++] != 0) {}
  if ((flags & 0x10U) != 0U) while (pos < input.size() && input[pos++] != 0) {}
  if ((flags & 0x02U) != 0U) pos += 2U;
  if (pos + 8U > input.size()) return false;
  const size_t trailer = input.size() - 8U;
  const uint32_t expected_crc = get_u32_le(input, trailer);
  const uint32_t expected_size = get_u32_le(input, trailer + 4U);
  if (expected_size > kMaxGzipOutputBytes) {
    if (error) *error = "gzip output exceeds embedded memory cap";
    return false;
  }
  const size_t deflate_len = trailer - pos;
  output->reserve(std::min<size_t>(expected_size, 1024U * 1024U));
  InflateAppendContext inflate_ctx {output, static_cast<size_t>(expected_size)};
  size_t consumed = deflate_len;
  const int ok = tinfl_decompress_mem_to_callback(input.data() + pos,
                                                  &consumed,
                                                  append_inflated_bytes,
                                                  &inflate_ctx,
                                                  0);
  if (ok != 1 || consumed != deflate_len) {
    output->clear();
    if (error) *error = "deflate decode failed";
    return false;
  }
  if (expected_crc != crc32_update(0U, output->data(), output->size()) ||
      expected_size != static_cast<uint32_t>(output->size())) {
    if (error) *error = "gzip crc/size mismatch";
    return false;
  }
  return true;
}

uint64_t parse_tar_octal(const char* field, size_t n) {
  uint64_t value = 0U;
  for (size_t i = 0; i < n && field[i] != '\0'; ++i) {
    if (field[i] >= '0' && field[i] <= '7') value = (value << 3U) + static_cast<uint64_t>(field[i] - '0');
  }
  return value;
}

void write_tar_octal(char* field, size_t n, uint64_t value) {
  std::snprintf(field, n, "%0*llo", static_cast<int>(n - 1U), static_cast<unsigned long long>(value));
}

std::string tar_name(const TarHeader& h) {
  std::string name(h.name, strnlen(h.name, sizeof(h.name)));
  std::string prefix(h.prefix, strnlen(h.prefix, sizeof(h.prefix)));
  if (!prefix.empty()) return prefix + "/" + name;
  return name;
}

bool append_tar_file(ShellContext& ctx, std::vector<uint8_t>* archive, const std::string& path_arg) {
  const std::string normalized = shell_normalize_path(ctx.state, path_arg);
  bool is_dir = false;
  struct stat st {};
  std::string error;
  if (!shell_path_exists(ctx.state, normalized, &is_dir, &st, &error) || is_dir) {
    shell_printf(ctx.state, "tar: %s: %s\n", path_arg.c_str(), is_dir ? "directories are not expanded in v1" : error.c_str());
    return false;
  }
  std::vector<uint8_t> bytes;
  if (!read_file_bytes(ctx, normalized, &bytes)) return false;
  TarHeader h {};
  std::string name = visible_path(ctx.state, normalized);
  if (!name.empty() && name.front() == '/') name.erase(name.begin());
  if (name.size() >= sizeof(h.name)) {
    shell_printf(ctx.state, "tar: %s: path too long\n", path_arg.c_str());
    return false;
  }
  std::memcpy(h.name, name.c_str(), name.size());
  write_tar_octal(h.mode, sizeof(h.mode), 0644);
  write_tar_octal(h.uid, sizeof(h.uid), 0);
  write_tar_octal(h.gid, sizeof(h.gid), 0);
  write_tar_octal(h.size, sizeof(h.size), bytes.size());
  write_tar_octal(h.mtime, sizeof(h.mtime), static_cast<uint64_t>(st.st_mtime));
  std::memset(h.checksum, ' ', sizeof(h.checksum));
  h.typeflag = '0';
  std::memcpy(h.magic, "ustar", 5U);
  std::memcpy(h.version, "00", 2U);
  std::memcpy(h.uname, "mros", 4U);
  std::memcpy(h.gname, "mros", 4U);
  uint32_t sum = 0U;
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&h);
  for (size_t i = 0; i < sizeof(h); ++i) sum += raw[i];
  std::snprintf(h.checksum, sizeof(h.checksum), "%06lo", static_cast<unsigned long>(sum));
  h.checksum[6] = '\0';
  h.checksum[7] = ' ';
  const uint8_t* header = reinterpret_cast<const uint8_t*>(&h);
  archive->insert(archive->end(), header, header + sizeof(h));
  archive->insert(archive->end(), bytes.begin(), bytes.end());
  const size_t pad = (512U - (bytes.size() % 512U)) % 512U;
  archive->insert(archive->end(), pad, 0U);
  return true;
}

bool http_fetch(ShellContext& ctx,
                const std::string& url,
                const std::string& method,
                uint32_t timeout_ms,
                std::vector<uint8_t>* body,
                int* status_code) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = static_cast<int>(timeout_ms);
  config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    shell_write_line(ctx.state, "curl: esp_http_client_init failed");
    return false;
  }
  if (method == "HEAD") esp_http_client_set_method(client, HTTP_METHOD_HEAD);
  else esp_http_client_set_method(client, HTTP_METHOD_GET);
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    shell_printf(ctx.state, "curl: open failed: %s\n", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }
  (void)esp_http_client_fetch_headers(client);
  if (status_code != nullptr) *status_code = esp_http_client_get_status_code(client);
  if (body != nullptr) body->clear();
  if (method != "HEAD" && body != nullptr) {
    uint8_t buffer[384] = {};
    while (true) {
      const int n = esp_http_client_read(client, reinterpret_cast<char*>(buffer), sizeof(buffer));
      if (n < 0) {
        shell_write_line(ctx.state, "curl: read failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
      }
      if (n == 0) break;
      if (body->size() + static_cast<size_t>(n) > kMaxMemoryFetchBytes) {
        shell_write_line(ctx.state, "curl: response truncated at 128KB");
        break;
      }
      body->insert(body->end(), buffer, buffer + n);
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return true;
}

std::string basename_from_url(const std::string& url) {
  size_t end = url.find('?');
  std::string clean = url.substr(0, end);
  size_t slash = clean.find_last_of('/');
  std::string base = slash == std::string::npos ? clean : clean.substr(slash + 1U);
  if (base.empty()) base = "index.html";
  return base;
}

int curl_like(ShellContext& ctx, bool wget_mode) {
  bool head = false;
  bool fail = false;
  bool remote_name = wget_mode;
  uint32_t timeout_ms = 10000U;
  std::string output_path;
  std::string url;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      wget_mode ? shell_help_wget(ctx.state) : shell_help_curl(ctx.state);
      return 0;
    }
    if (arg == "-I" || arg == "--head" || arg == "--spider") {
      head = true;
      continue;
    }
    if (arg == "--fail") {
      fail = true;
      continue;
    }
    if (arg == "-O") {
      remote_name = true;
      continue;
    }
    if ((arg == "-o" || arg == "-O-") && (i + 1U) < ctx.args.size()) {
      output_path = ctx.args[++i];
      continue;
    }
    if ((arg == "--max-time" || arg == "-T") && (i + 1U) < ctx.args.size()) {
      size_t seconds = 0U;
      if (!parse_size_arg(ctx.args[++i], &seconds)) return 1;
      timeout_ms = static_cast<uint32_t>(std::min<size_t>(seconds * 1000U, 60000U));
      continue;
    }
    if (arg == "-L") continue;
    if (arg.rfind("http://", 0U) == 0U || arg.rfind("https://", 0U) == 0U) {
      url = arg;
      continue;
    }
  }
  if (url.empty()) {
    shell_write_line(ctx.state, wget_mode ? "wget: URL required" : "curl: URL required");
    return 1;
  }
  std::vector<uint8_t> body;
  int status = 0;
  if (!http_fetch(ctx, url, head ? "HEAD" : "GET", timeout_ms, &body, &status)) return 1;
  if (fail && (status < 200 || status >= 400)) {
    shell_printf(ctx.state, "curl: HTTP %d\n", status);
    return 1;
  }
  if (head) {
    shell_printf(ctx.state, "HTTP %d\n", status);
    return 0;
  }
  if (output_path.empty() && remote_name) output_path = basename_from_url(url);
  if (!output_path.empty()) return write_atomic(ctx.state, output_path, body) ? 0 : 1;
  std::string text(body.begin(), body.end());
  shell_write(ctx.state, text.c_str());
  if (!text.empty() && text.back() != '\n') shell_write(ctx.state, "\n");
  return 0;
}

}  // namespace

void shell_help_tar(ShellState& state) {
  shell_write_line(state, "Usage: tar -cf ARCHIVE FILE... | tar -tf ARCHIVE | tar -xf ARCHIVE [-C DIR]");
  shell_write_line(state, "BusyBox-style ustar subset; v1 stores regular files only.");
}

int shell_cmd_tar(ShellContext& ctx) {
  if (ctx.args.size() < 3U || ctx.args[1] == "--help") {
    shell_help_tar(ctx.state);
    return ctx.args.size() >= 2U && ctx.args[1] == "--help" ? 0 : 1;
  }
  const std::string mode = ctx.args[1];
  if (mode == "-cf" || mode == "cf") {
    if (ctx.args.size() < 4U) {
      shell_write_line(ctx.state, "tar: files required");
      return 1;
    }
    std::vector<uint8_t> archive;
    bool ok = true;
    for (size_t i = 3U; i < ctx.args.size(); ++i) ok &= append_tar_file(ctx, &archive, ctx.args[i]);
    archive.insert(archive.end(), 1024U, 0U);
    return ok && write_atomic(ctx.state, ctx.args[2], archive) ? 0 : 1;
  }
  if (mode == "-tf" || mode == "tf" || mode == "-xf" || mode == "xf") {
    std::vector<uint8_t> archive;
    if (!read_file_bytes(ctx, ctx.args[2], &archive)) return 1;
    std::string dest_dir = shell_storage_user_root(ctx.state);
    for (size_t i = 3U; i + 1U < ctx.args.size(); ++i) {
      if (ctx.args[i] == "-C") dest_dir = shell_normalize_path(ctx.state, ctx.args[i + 1U]);
    }
    size_t pos = 0U;
    while (pos + sizeof(TarHeader) <= archive.size()) {
      const TarHeader* h = reinterpret_cast<const TarHeader*>(archive.data() + pos);
      bool empty = true;
      for (size_t i = 0; i < sizeof(TarHeader); ++i) {
        if (archive[pos + i] != 0U) {
          empty = false;
          break;
        }
      }
      if (empty) break;
      const std::string name = tar_name(*h);
      const size_t size = static_cast<size_t>(parse_tar_octal(h->size, sizeof(h->size)));
      pos += sizeof(TarHeader);
      if (mode == "-tf" || mode == "tf") {
        shell_printf(ctx.state, "%s\n", name.c_str());
      } else if (h->typeflag == '0' || h->typeflag == '\0') {
        const std::string out = dest_dir + "/" + name;
        if (!write_atomic(ctx.state, out, std::vector<uint8_t>(archive.begin() + pos, archive.begin() + pos + size))) {
          return 1;
        }
      }
      pos += ((size + 511U) / 512U) * 512U;
    }
    return 0;
  }
  shell_help_tar(ctx.state);
  return 1;
}

void shell_help_gzip(ShellState& state) {
  shell_write_line(state, "Usage: gzip [-c] FILE");
  shell_write_line(state, "Writes valid gzip stored-deflate output; v1 does not compress.");
}

int shell_cmd_gzip(ShellContext& ctx) {
  bool to_stdout = false;
  std::string path;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_gzip(ctx.state);
      return 0;
    }
    if (ctx.args[i] == "-c") {
      to_stdout = true;
      continue;
    }
    path = ctx.args[i];
  }
  if (path.empty()) {
    shell_help_gzip(ctx.state);
    return 1;
  }
  std::vector<uint8_t> input;
  if (!read_file_bytes(ctx, path, &input)) return 1;
  const std::vector<uint8_t> gz = gzip_store(input);
  if (gz.empty()) {
    shell_write_line(ctx.state, "gzip: deflate failed");
    return 1;
  }
  if (to_stdout) {
    shell_write_line(ctx.state, "gzip: binary stdout is not supported in v1; use output file");
    return 1;
  }
  return write_atomic(ctx.state, path + ".gz", gz) ? 0 : 1;
}

void shell_help_gunzip(ShellState& state) {
  shell_write_line(state, "Usage: gunzip [-c] FILE.gz");
}

int shell_cmd_gunzip(ShellContext& ctx) {
  bool to_stdout = false;
  std::string path;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help") {
      shell_help_gunzip(ctx.state);
      return 0;
    }
    if (ctx.args[i] == "-c") {
      to_stdout = true;
      continue;
    }
    path = ctx.args[i];
  }
  if (path.empty()) {
    shell_help_gunzip(ctx.state);
    return 1;
  }
  std::vector<uint8_t> input;
  std::vector<uint8_t> output;
  std::string error;
  if (!read_file_bytes(ctx, path, &input) || !gzip_unstore(input, &output, &error)) {
    shell_printf(ctx.state, "gunzip: %s\n", error.empty() ? "failed" : error.c_str());
    return 1;
  }
  if (to_stdout) {
    std::string text(output.begin(), output.end());
    shell_write(ctx.state, text.c_str());
    return 0;
  }
  std::string out_path = path;
  if (out_path.size() > 3U && out_path.substr(out_path.size() - 3U) == ".gz") out_path.resize(out_path.size() - 3U);
  else out_path += ".out";
  return write_atomic(ctx.state, out_path, output) ? 0 : 1;
}

void shell_help_zcat(ShellState& state) {
  shell_write_line(state, "Usage: zcat FILE.gz");
}

int shell_cmd_zcat(ShellContext& ctx) {
  std::vector<std::string> args = {"gunzip", "-c"};
  for (size_t i = 1U; i < ctx.args.size(); ++i) args.push_back(ctx.args[i]);
  ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
  return shell_cmd_gunzip(child);
}

void shell_help_curl(ShellState& state) {
  shell_write_line(state, "Usage: curl [-I] [-L] [--fail] [--max-time SEC] [-o FILE|-O] URL");
}

int shell_cmd_curl(ShellContext& ctx) { return curl_like(ctx, false); }

void shell_help_wget(ShellState& state) {
  shell_write_line(state, "Usage: wget [-O FILE] [--spider] [--max-time SEC] URL");
}

int shell_cmd_wget(ShellContext& ctx) { return curl_like(ctx, true); }

}  // namespace mros::shell
