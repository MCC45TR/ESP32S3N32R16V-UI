#include "ssh_service.h"

#include <esp_log.h>
#include <esp_heap_caps.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#ifndef MROS_ENABLE_WOLFSSH_BACKEND
#define MROS_ENABLE_WOLFSSH_BACKEND 0
#endif

#if MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>)
#include <fcntl.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <mbedtls/base64.h>
#if defined(ARDUINO)
#define MROS_SSH_ARDUINO_WAS_DEFINED ARDUINO
#undef ARDUINO
#endif
#include <wolfssh/ssh.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>
#if defined(MROS_SSH_ARDUINO_WAS_DEFINED)
#define ARDUINO MROS_SSH_ARDUINO_WAS_DEFINED
#undef MROS_SSH_ARDUINO_WAS_DEFINED
#endif
#endif

#include "src/drivers/sd_logger.h"
#include "src/platform/mros_file.h"
#include "src/platform/mros_fs.h"
#include "src/platform/mros_time.h"
#include "src/security/ssh_identity.h"
#include "src/shell/mros_shell.h"
#include "src/web/server/wifi_manager.h"

namespace mros::ssh {
namespace {

constexpr const char* kSshTag = "SSH";

#if MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>)
constexpr int kInvalidFd = -1;
constexpr size_t kRxBufferSize = 256U;
constexpr size_t kMaxLineSize = 240U;
constexpr uint32_t kSessionIdlePollMs = 25U;
constexpr uint32_t kListenerPollMs = 100U;
constexpr const char* kSessionBanner = "\nMROS DEUSCARA-S3V mshell\n";

enum class SessionPhase : uint8_t {
  Handshake,
  Shell,
};

struct ActiveSession {
  WOLFSSH* ssh = nullptr;
  mros::shell::ShellSession* shell = nullptr;
  int fd = kInvalidFd;
  uint32_t id = 0U;
  uint32_t login_ms = 0U;
  uint32_t last_cmd_ms = 0U;
  std::string remote_ip;
  String user;
  SessionPhase phase = SessionPhase::Handshake;
  std::string line;
};

WOLFSSH_CTX* g_ctx = nullptr;
int g_listener_fd = kInvalidFd;
uint16_t g_listener_port = 0U;
ActiveSession g_session;
uint32_t g_next_session_id = 1U;
String g_auth_user;
#endif

TaskHandle_t g_task_handle = nullptr;
bool g_initialized = false;
bool g_listener_active = false;
uint32_t g_last_backend_log_ms = 0U;

void log_backend_missing_once() {
  const uint32_t now = mros::platform::mros_millis();
  if (now - g_last_backend_log_ms < 5000U) {
    return;
  }
  g_last_backend_log_ms = now;
#if !(MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>))
  ESP_LOGW(kSshTag, "wolfSSH backend is not linked; server stays disabled.");
#endif
}

bool backend_available() {
#if MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>)
  return true;
#else
  return false;
#endif
}

#if MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>)
String host_key_path() {
  return logger_user_path("ssh/host_key_ecc.der");
}

bool ensure_ssh_dir() {
  if (!logger_storage_ready()) {
    logger_init();
  }
  const String root = logger_user_path("ssh");
  if (!mros::platform::mros_fs_exists(root.c_str())) {
    return mros::platform::mros_fs_mkdir(root.c_str());
  }
  return true;
}

bool set_nonblocking(const int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void close_fd(int* fd) {
  if (fd != nullptr && *fd != kInvalidFd) {
    close(*fd);
    *fd = kInvalidFd;
  }
}

bool read_file_bytes(const String& path, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  std::string raw;
  if (!mros::platform::mros_file_read_all(path.c_str(), &raw)) {
    return false;
  }
  const size_t size = raw.size();
  if (size == 0U || size > 2048U) {
    return false;
  }
  out->assign(raw.begin(), raw.end());
  return true;
}

bool write_file_bytes(const String& path, const uint8_t* data, const size_t size) {
  if (data == nullptr || size == 0U || !ensure_ssh_dir()) {
    return false;
  }
  std::string raw(reinterpret_cast<const char*>(data), size);
  return mros::platform::mros_file_write_all(path.c_str(), raw);
}

bool generate_host_key(std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return false;
  }

  WC_RNG rng {};
  ecc_key key {};
  uint8_t der[512] = {};
  bool ok = false;

  if (wc_InitRng(&rng) != 0) {
    return false;
  }
  wc_ecc_init(&key);
  if (wc_ecc_make_key_ex(&rng, 32, &key, ECC_SECP256R1) == 0) {
    const int der_size = wc_EccPrivateKeyToDer(&key, der, sizeof(der));
    if (der_size > 0 && write_file_bytes(host_key_path(), der, static_cast<size_t>(der_size))) {
      out->assign(der, der + der_size);
      ok = true;
    }
  }
  wc_ecc_free(&key);
  wc_FreeRng(&rng);
  return ok;
}

bool load_or_create_host_key(std::vector<uint8_t>* out) {
  if (out == nullptr || !ensure_ssh_dir()) {
    return false;
  }
  if (read_file_bytes(host_key_path(), out)) {
    return true;
  }
  return generate_host_key(out);
}

bool base64_decode_blob(const std::string& text, std::vector<uint8_t>* out) {
  if (out == nullptr || text.empty()) {
    return false;
  }
  size_t needed = 0U;
  const int probe = mbedtls_base64_decode(
      nullptr,
      0U,
      &needed,
      reinterpret_cast<const unsigned char*>(text.data()),
      text.size());
  if (probe != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || needed == 0U || needed > 2048U) {
    return false;
  }
  out->assign(needed, 0U);
  size_t written = 0U;
  if (mbedtls_base64_decode(
          out->data(),
          out->size(),
          &written,
          reinterpret_cast<const unsigned char*>(text.data()),
          text.size()) != 0) {
    out->clear();
    return false;
  }
  out->resize(written);
  return true;
}

bool authorized_key_matches(const String& username, const uint8_t* key, const size_t key_size) {
  if (key == nullptr || key_size == 0U) {
    return false;
  }
  const IdentityConfig cfg = identity_get();
  if (username != cfg.username) {
    return false;
  }

  std::vector<std::string> lines;
  if (!mros::platform::mros_file_read_lines(authorized_keys_path().c_str(), &lines)) {
    return false;
  }

  bool matched = false;
  for (std::string line : lines) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const size_t trim_left = line.find_first_not_of(" \t");
    if (trim_left == std::string::npos) {
      continue;
    }
    const size_t trim_right = line.find_last_not_of(" \t");
    line = line.substr(trim_left, trim_right - trim_left + 1U);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const size_t first_space = line.find(' ');
    if (first_space == std::string::npos || first_space == 0U) {
      continue;
    }
    const size_t second_space = line.find(' ', first_space + 1U);
    const std::string type = line.substr(0U, first_space);
    const std::string blob64 = second_space != std::string::npos
                                   ? line.substr(first_space + 1U, second_space - first_space - 1U)
                                   : line.substr(first_space + 1U);
    if (type != "ssh-rsa" && type != "ssh-ed25519" && type.rfind("ecdsa-sha2-", 0U) != 0U) {
      continue;
    }
    std::vector<uint8_t> decoded;
    if (!base64_decode_blob(blob64, &decoded)) {
      continue;
    }
    matched = decoded.size() == key_size && std::memcmp(decoded.data(), key, key_size) == 0;
  }
  return matched;
}

String auth_text(const uint8_t* data, const uint32_t size) {
  if (data == nullptr || size == 0U) {
    return String();
  }
  String out;
  out.reserve(size + 1U);
  for (uint32_t i = 0U; i < size; ++i) {
    out += static_cast<char>(data[i]);
  }
  return out;
}

int user_auth_callback(byte auth_type, WS_UserAuthData* auth_data, void* /*ctx*/) {
  if (auth_data == nullptr || auth_data->username == nullptr || auth_data->usernameSz == 0U) {
    return WOLFSSH_USERAUTH_INVALID_USER;
  }

  const String username = auth_text(auth_data->username, auth_data->usernameSz);
  if (username == root_username()) {
    return WOLFSSH_USERAUTH_INVALID_USER;
  }

  if (auth_type == WOLFSSH_USERAUTH_PASSWORD) {
    const String password = auth_text(
        auth_data->sf.password.password,
        auth_data->sf.password.passwordSz);
    if (verify_password(username, password)) {
      g_auth_user = username;
      return WOLFSSH_USERAUTH_SUCCESS;
    }
    return WOLFSSH_USERAUTH_INVALID_PASSWORD;
  }

  if (auth_type == WOLFSSH_USERAUTH_PUBLICKEY) {
    if (authorized_key_matches(
            username,
            auth_data->sf.publicKey.publicKey,
            auth_data->sf.publicKey.publicKeySz)) {
      g_auth_user = username;
      return WOLFSSH_USERAUTH_SUCCESS;
    }
    return WOLFSSH_USERAUTH_INVALID_PUBLICKEY;
  }

  return WOLFSSH_USERAUTH_FAILURE;
}

int channel_shell_callback(WOLFSSH_CHANNEL* /*channel*/, void* /*ctx*/) {
  return 0;
}

int channel_exec_callback(WOLFSSH_CHANNEL* /*channel*/, void* /*ctx*/) {
  return 1;
}

int channel_subsystem_callback(WOLFSSH_CHANNEL* /*channel*/, void* /*ctx*/) {
  return 1;
}

bool ensure_context() {
  if (g_ctx != nullptr) {
    return true;
  }

  wolfSSH_Init();
  g_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, nullptr);
  if (g_ctx == nullptr) {
    ESP_LOGE(kSshTag, "failed to allocate wolfSSH context");
    return false;
  }
  wolfSSH_SetUserAuth(g_ctx, user_auth_callback);
  wolfSSH_CTX_SetBanner(g_ctx, "MROS-DEUSCARA-S3V SSH");
  (void)wolfSSH_CTX_SetChannelReqShellCb(g_ctx, channel_shell_callback);
  (void)wolfSSH_CTX_SetChannelReqExecCb(g_ctx, channel_exec_callback);
  (void)wolfSSH_CTX_SetChannelReqSubsysCb(g_ctx, channel_subsystem_callback);

  std::vector<uint8_t> host_key;
  if (!load_or_create_host_key(&host_key) ||
      wolfSSH_CTX_UsePrivateKey_buffer(
          g_ctx,
          host_key.data(),
          static_cast<word32>(host_key.size()),
          WOLFSSH_FORMAT_ASN1) < 0) {
    ESP_LOGE(kSshTag, "failed to load host key");
    wolfSSH_CTX_free(g_ctx);
    g_ctx = nullptr;
    return false;
  }
  return true;
}

void close_session() {
  if (g_session.ssh != nullptr) {
    g_auth_user = "";
    wolfSSH_stream_exit(g_session.ssh, 0);
    (void)wolfSSH_shutdown(g_session.ssh);
    wolfSSH_free(g_session.ssh);
  }
  if (g_session.shell != nullptr) {
    mros::shell::destroy_session(g_session.shell);
  }
  close_fd(&g_session.fd);
  g_session = {};
  g_session.fd = kInvalidFd;
}

void close_listener() {
  close_fd(&g_listener_fd);
  g_listener_active = false;
  g_listener_port = 0U;
}

bool ensure_listener(const uint16_t port) {
  if (g_listener_fd != kInvalidFd && g_listener_port == port) {
    g_listener_active = true;
    return true;
  }
  close_listener();

  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (fd < 0) {
    ESP_LOGE(kSshTag, "socket failed errno=%d", errno);
    return false;
  }

  int opt = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(fd, 1) != 0 ||
      !set_nonblocking(fd)) {
    ESP_LOGE(kSshTag, "listen failed errno=%d", errno);
    int mutable_fd = fd;
    close_fd(&mutable_fd);
    return false;
  }

  g_listener_fd = fd;
  g_listener_port = port;
  g_listener_active = true;
  ESP_LOGI(kSshTag, "listening on port %u", static_cast<unsigned>(port));
  return true;
}

bool send_all(WOLFSSH* ssh, const char* text) {
  if (ssh == nullptr || text == nullptr || text[0] == '\0') {
    return true;
  }
  const uint8_t* data = reinterpret_cast<const uint8_t*>(text);
  size_t remaining = std::strlen(text);
  uint8_t retries = 0U;
  while (remaining > 0U && retries < 20U) {
    const int sent = wolfSSH_stream_send(
        ssh,
        const_cast<byte*>(data),
        static_cast<word32>(std::min<size_t>(remaining, 512U)));
    if (sent > 0) {
      data += sent;
      remaining -= static_cast<size_t>(sent);
      retries = 0U;
      continue;
    }
    const int err = wolfSSH_get_error(ssh);
    if (err == WS_WANT_READ || err == WS_WANT_WRITE || sent == WS_REKEYING) {
      ++retries;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    return false;
  }
  return remaining == 0U;
}

void send_prompt() {
  if (g_session.ssh != nullptr && g_session.shell != nullptr) {
    (void)send_all(g_session.ssh, mros::shell::session_prompt(g_session.shell));
  }
}

int on_terminal_resize(
    WOLFSSH* ssh,
    word32 columns,
    word32 rows,
    word32 width_pixels,
    word32 height_pixels,
    void* user_ctx) {
  (void)ssh;
  (void)width_pixels;
  (void)height_pixels;
  ActiveSession* session = static_cast<ActiveSession*>(user_ctx);
  if (session == nullptr || session->shell == nullptr) {
    return WS_SUCCESS;
  }
  mros::shell::session_set_terminal_size(
      session->shell,
      static_cast<uint16_t>(columns),
      static_cast<uint16_t>(rows));
  return WS_SUCCESS;
}

void run_shell_line(const char* line) {
  if (g_session.shell == nullptr || g_session.ssh == nullptr) {
    return;
  }
  std::string output;
  (void)mros::shell::execute_session_line(g_session.shell, line, &output, false);
  g_session.last_cmd_ms = mros::platform::mros_millis();
  if (!output.empty()) {
    (void)send_all(g_session.ssh, output.c_str());
  }
  if (mros::shell::session_close_requested(g_session.shell)) {
    close_session();
    return;
  }
  send_prompt();
}

void handle_shell_bytes(const uint8_t* data, const int size) {
  if (data == nullptr || size <= 0) {
    return;
  }
  for (int i = 0; i < size && g_session.ssh != nullptr; ++i) {
    const char ch = static_cast<char>(data[i]);
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      std::string line;
      line.swap(g_session.line);
      run_shell_line(line.c_str());
      continue;
    }
    if (ch == 0x03) {
      g_session.line.clear();
      (void)send_all(g_session.ssh, "^C\n");
      send_prompt();
      continue;
    }
    if (ch == '\b' || static_cast<uint8_t>(ch) == 127U) {
      if (!g_session.line.empty()) {
        g_session.line.pop_back();
      }
      continue;
    }
    if (static_cast<uint8_t>(ch) < 32U) {
      continue;
    }
    if (g_session.line.size() < kMaxLineSize) {
      g_session.line.push_back(ch);
    }
  }
}

void accept_client() {
  if (g_listener_fd == kInvalidFd || g_session.fd != kInvalidFd) {
    return;
  }
  sockaddr_in client_addr {};
  socklen_t addr_len = sizeof(client_addr);
  const int client_fd = accept(
      g_listener_fd,
      reinterpret_cast<sockaddr*>(&client_addr),
      &addr_len);
  if (client_fd < 0) {
    return;
  }
  if (!set_nonblocking(client_fd)) {
    int mutable_fd = client_fd;
    close_fd(&mutable_fd);
    return;
  }

  WOLFSSH* ssh = wolfSSH_new(g_ctx);
  if (ssh == nullptr) {
    int mutable_fd = client_fd;
    close_fd(&mutable_fd);
    return;
  }

  g_auth_user = "";
  wolfSSH_set_fd(ssh, client_fd);
  wolfSSH_SetUserAuthCtx(ssh, nullptr);
  (void)wolfSSH_SetChannelReqCtx(ssh, &g_session);

  g_session = {};
  g_session.fd = client_fd;
  g_session.ssh = ssh;
  g_session.id = g_next_session_id++;
  char ip_text[INET_ADDRSTRLEN] = {};
  if (inet_ntop(AF_INET, &client_addr.sin_addr, ip_text, sizeof(ip_text)) != nullptr) {
    g_session.remote_ip = ip_text;
  } else {
    g_session.remote_ip = "-";
  }
  g_session.phase = SessionPhase::Handshake;
}

void process_session() {
  if (g_session.ssh == nullptr) {
    return;
  }

  if (g_session.phase == SessionPhase::Handshake) {
    const int ret = wolfSSH_accept(g_session.ssh);
    if (ret == WS_SUCCESS) {
      g_session.shell = mros::shell::create_session(mros::shell::ShellTransport::Ssh);
      if (g_session.shell == nullptr) {
        close_session();
        return;
      }
      g_session.user = g_auth_user.length() > 0U ? g_auth_user : identity_get().username;
      g_session.login_ms = mros::platform::mros_millis();
      g_session.last_cmd_ms = g_session.login_ms;
      g_session.phase = SessionPhase::Shell;
      (void)send_all(g_session.ssh, kSessionBanner);
      send_prompt();
      return;
    }
    const int err = wolfSSH_get_error(g_session.ssh);
    if (err == WS_WANT_READ || err == WS_WANT_WRITE || err == WS_AUTH_PENDING) {
      return;
    }
    close_session();
    return;
  }

  uint8_t buffer[kRxBufferSize] = {};
  const int read_size = wolfSSH_stream_read(g_session.ssh, buffer, sizeof(buffer));
  if (read_size > 0) {
    handle_shell_bytes(buffer, read_size);
    return;
  }
  const int err = wolfSSH_get_error(g_session.ssh);
  if (err == WS_WANT_READ || err == WS_WANT_WRITE || read_size == WS_REKEYING) {
    return;
  }
  close_session();
}
#endif

}  // namespace

void service_init() {
  if (g_initialized) {
    return;
  }
  identity_init();
  g_initialized = true;
}

void service_set_task_handle(TaskHandle_t handle) { g_task_handle = handle; }

void service_notify() {
  if (g_task_handle != nullptr) {
    xTaskNotifyGive(g_task_handle);
  }
}

void service_process() {
  service_init();
  const IdentityConfig cfg = identity_get();
  const bool sta_ready = wifi_manager_is_connected();
  if (!cfg.enabled) {
 #if MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>)
    close_session();
    close_listener();
 #endif
    g_listener_active = false;
    return;
  }
  if (!sta_ready) {
 #if MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>)
    close_session();
    close_listener();
 #endif
    g_listener_active = false;
    return;
  }
  if (!backend_available()) {
    g_listener_active = false;
    log_backend_missing_once();
    return;
  }

#if MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>)
  if (!ensure_context() || !ensure_listener(cfg.port)) {
    g_listener_active = false;
    return;
  }
  accept_client();
  process_session();
#else
  g_listener_active = false;
#endif
}

uint32_t service_wait_timeout_ms() {
  const IdentityConfig cfg = identity_get();
  if (!cfg.enabled) {
    return 0U;
  }
#if MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>)
  if (g_session.fd != kInvalidFd) {
    return kSessionIdlePollMs;
  }
  return g_listener_active ? kListenerPollMs : 1000U;
#else
  return g_listener_active ? 50U : 1000U;
#endif
}

bool service_enable() {
  service_init();
  if (!set_enabled(true)) {
    return false;
  }
  service_notify();
  return true;
}

bool service_disable() {
  service_init();
  g_listener_active = false;
  if (!set_enabled(false)) {
    return false;
  }
  service_notify();
  return true;
}

bool service_set_port(const uint16_t port) {
  service_init();
  g_listener_active = false;
  if (!set_port(port)) {
    return false;
  }
  service_notify();
  return true;
}

String service_backend_text() {
  String out = backend_name();
  out += backend_available() ? " linked" : " unavailable";
  return out;
}

String service_status_text() {
  service_init();
  const IdentityConfig cfg = identity_get();
  String out;
  out.reserve(384);
  out += "ssh server: ";
  out += cfg.enabled ? "enabled" : "disabled";
  out += "\nbackend   : ";
  out += service_backend_text();
  out += "\nlistener  : ";
  out += g_listener_active ? "active" : "sleeping";
  out += "\nport      : ";
  out += String(cfg.port);
  out += "\nmax sess  : ";
  out += String(cfg.max_sessions);
  out += "\nsta       : ";
  out += wifi_manager_is_connected() ? "connected" : "down";
  out += "\nip        : ";
  out += wifi_manager_is_connected() ? String(wifi_manager_ip()) : String("-");
  out += "\ndevice    : ";
  out += cfg.device_name;
  out += "\nuser      : ";
  out += cfg.username;
  out += "\nroot      : direct-login disabled, use su";
  out += "\nheap      : internal ";
  out += String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  out += " psram ";
  out += String(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  out += "\nauth keys : ";
  out += authorized_keys_path();
#if MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>)
  out += "\nhost key  : ";
  out += host_key_path();
#endif
  return out;
}

String service_sessions_text() {
  service_init();
  String out;
  out.reserve(160);
  out += "id  ip              user   role  login_ms  last_cmd_ms\n";
  out += "--  --------------  -----  ----  --------  -----------\n";
#if MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>)
  if (g_session.fd != kInvalidFd) {
    out += String(g_session.id);
    out += "   ";
    out += g_session.remote_ip.c_str();
    out += "  ";
    out += g_session.user.length() > 0U ? g_session.user : identity_get().username;
    out += "  ";
    out += (g_session.shell != nullptr && mros::shell::session_is_root(g_session.shell)) ? "root" : "user";
    out += "  ";
    out += String(g_session.login_ms);
    out += "  ";
    out += String(g_session.last_cmd_ms);
    out += "\n";
    return out;
  }
#endif
  out += "no active SSH sessions\n";
  return out;
}

}  // namespace mros::ssh
