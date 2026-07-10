#include "ssh_identity.h"

#include <mbedtls/md.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "src/drivers/sd_logger.h"
#include "src/platform/mros_file.h"
#include "src/platform/mros_fs.h"
#include "src/platform/mros_nvs.h"

#ifndef MROS_ENABLE_WOLFSSH_BACKEND
#define MROS_ENABLE_WOLFSSH_BACKEND 0
#endif

namespace mros::ssh {
namespace {

using mros::platform::NvsNamespace;
using mros::platform::NvsPartitionMode;

constexpr const char* kUserNvsPartition = "nvs_sys_usr";
constexpr const char* kLegacyUserNvsPartition = "nvs_usr";
constexpr const char* kSshNamespace = "ssh_cfg";
constexpr const char* kUserNamespace = "user_cfg";
constexpr const char* kExtraUsersKey = "extra_users_v1";
constexpr const char* kDefaultUser = "mros";
constexpr const char* kRootUser = "root";
constexpr const char* kDefaultDeviceName = "DEUSCARA-S3V";
constexpr const char* kDefaultDisplayName = "MROS Operator";
constexpr uint16_t kDefaultPort = 6755;
constexpr uint8_t kDefaultMaxSessions = 1;
constexpr uint32_t kDefaultAuthFlags = 0x03U;
constexpr size_t kMaxNormalUsers = 4U;

// SHA-256("MROS-s3_9vQ7-pK2x-4LaT"). The plaintext bootstrap password is not
// persisted to NVS; users should rotate it after enabling SSH.
constexpr const char* kBootstrapPasswordHash =
    "dfebf72de6706e5aae89dca35f22f1641eb7e1ee61777bf915f583dda9c86eae";

IdentityConfig g_config;
std::vector<UserAccount> g_extra_users;
bool g_initialized = false;

bool open_user_nvs(NvsNamespace* pref, const char* name, const bool read_only) {
  return pref != nullptr &&
         pref->open(name, read_only, NvsPartitionMode::UserPartitionsThenDefault);
}

String clean_string(String value) {
  value.replace("\r", "");
  value.replace("\n", "");
  value.trim();
  return value;
}

bool ensure_storage_ready() {
  if (!logger_storage_ready()) {
    logger_init();
  }
  return true;
}

String users_file_path() {
  return logger_user_path("auth/users.db");
}

bool is_hash_hex(const String& value) {
  if (value.length() != 64U) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value[i];
    const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                    (ch >= 'A' && ch <= 'F');
    if (!ok) {
      return false;
    }
  }
  return true;
}

void ensure_loaded() {
  if (!g_initialized) {
    identity_init();
  }
}

void save_extra_users();

void save_ssh_config() {
  NvsNamespace pref;
  if (!open_user_nvs(&pref, kSshNamespace, false)) {
    return;
  }
  pref.set_bool("enabled", g_config.enabled);
  pref.set_u16("port", g_config.port);
  pref.set_u8("max_sess", g_config.max_sessions);
  pref.set_u32("auth_flags", g_config.auth_flags);
  pref.set_string("dev_name", std::string(g_config.device_name.c_str(),
                                          g_config.device_name.length()));
}

void save_user_config() {
  NvsNamespace pref;
  if (!open_user_nvs(&pref, kUserNamespace, false)) {
    return;
  }
  pref.set_string("username",
                  std::string(g_config.username.c_str(), g_config.username.length()));
  pref.set_string("display", std::string(g_config.display_name.c_str(),
                                         g_config.display_name.length()));
  pref.set_string("user_hash",
                  std::string(g_config.user_hash.c_str(), g_config.user_hash.length()));
  pref.set_string("root_hash",
                  std::string(g_config.root_hash.c_str(), g_config.root_hash.length()));
  pref.set_bool("user_admin", g_config.user_admin);
  pref.set_bool("user_sudo", g_config.user_sudo);
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

bool append_extra_user_from_line(const std::string& raw_line) {
  String raw = String(raw_line.c_str());
  raw = clean_string(raw);
  if (raw.length() == 0U || raw[0] == '#') {
    return true;
  }

  int p1 = raw.indexOf('|');
  int p2 = p1 >= 0 ? raw.indexOf('|', p1 + 1) : -1;
  int p3 = p2 >= 0 ? raw.indexOf('|', p2 + 1) : -1;
  int t41 = p3 >= 0 ? raw.indexOf('|', p3 + 1) : -1;
  if (p1 <= 0 || p2 <= p1 || p3 <= p2 || t41 <= p3) {
    return false;
  }

  UserAccount account;
  account.username = clean_string(raw.substring(0, p1));
  account.display_name = clean_string(raw.substring(p1 + 1, p2));
  account.password_hash = clean_string(raw.substring(p2 + 1, p3));
  account.admin = raw.substring(p3 + 1, t41) == "1";
  account.sudo = raw.substring(t41 + 1) == "1";
  account.primary = false;
  account.root = false;

  if (!is_valid_username(account.username) || account.display_name.length() == 0U ||
      !is_hash_hex(account.password_hash) || account.username == kRootUser ||
      account.username == g_config.username || g_extra_users.size() >= (kMaxNormalUsers - 1U)) {
    return false;
  }
  g_extra_users.push_back(account);
  return true;
}

bool load_extra_users_from_payload(const std::string& payload) {
  g_extra_users.clear();
  size_t cursor = 0U;
  while (cursor <= payload.size()) {
    const size_t next = payload.find('\n', cursor);
    const std::string line =
        payload.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor);
    (void)append_extra_user_from_line(line);
    if (next == std::string::npos) break;
    cursor = next + 1U;
  }
  return true;
}

bool load_extra_users_from_nvs() {
  NvsNamespace pref;
  if (!open_user_nvs(&pref, kUserNamespace, true)) {
    return false;
  }
  std::string payload;
  if (!pref.get_string(kExtraUsersKey, &payload)) {
    return false;
  }
  return load_extra_users_from_payload(payload);
}

bool load_extra_users_from_legacy_file() {
  g_extra_users.clear();
  if (!ensure_storage_ready()) {
    return false;
  }
  std::vector<std::string> lines;
  if (!mros::platform::mros_file_read_lines(users_file_path().c_str(), &lines)) {
    return false;
  }
  for (const std::string& raw_line : lines) {
    (void)append_extra_user_from_line(raw_line);
  }
  return true;
}

std::string extra_users_payload() {
  std::string payload;
  for (const UserAccount& account : g_extra_users) {
    payload += account.username.c_str();
    payload.push_back('|');
    payload += account.display_name.c_str();
    payload.push_back('|');
    payload += account.password_hash.c_str();
    payload.push_back('|');
    payload.push_back(account.admin ? '1' : '0');
    payload.push_back('|');
    payload.push_back(account.sudo ? '1' : '0');
    payload.push_back('\n');
  }
  return payload;
}

void cleanup_legacy_extra_users_file() {
  if (!logger_storage_ready()) {
    logger_init();
  }
  (void)mros::platform::mros_fs_remove(users_file_path().c_str());
}

void load_extra_users() {
  g_extra_users.clear();
  if (load_extra_users_from_nvs()) {
    cleanup_legacy_extra_users_file();
    return;
  }
  if (load_extra_users_from_legacy_file()) {
    save_extra_users();
  }
}

void save_extra_users() {
  NvsNamespace pref;
  if (!open_user_nvs(&pref, kUserNamespace, false)) {
    return;
  }
  const std::string payload = extra_users_payload();
  const bool ok = payload.empty() ? pref.erase_key(kExtraUsersKey)
                                  : pref.set_string(kExtraUsersKey, payload);
  if (ok) {
    cleanup_legacy_extra_users_file();
  }
}

UserAccount build_root_account() {
  UserAccount account;
  account.username = kRootUser;
  account.display_name = "root";
  account.password_hash = g_config.root_hash;
  account.admin = true;
  account.sudo = true;
  account.root = true;
  return account;
}

UserAccount build_primary_account() {
  UserAccount account;
  account.username = g_config.username;
  account.display_name = g_config.display_name;
  account.password_hash = g_config.user_hash;
  account.admin = g_config.user_admin;
  account.sudo = g_config.user_sudo;
  account.primary = true;
  return account;
}

bool find_extra_user_index(const String& username, size_t* index) {
  for (size_t i = 0U; i < g_extra_users.size(); ++i) {
    if (g_extra_users[i].username == username) {
      if (index != nullptr) {
        *index = i;
      }
      return true;
    }
  }
  return false;
}

}  // namespace

String sha256_hex(const String& text) {
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md_info == nullptr) {
    return String();
  }
  uint8_t digest[32] = {};
  if (mbedtls_md(md_info,
                 reinterpret_cast<const unsigned char*>(text.c_str()),
                 text.length(),
                 digest) != 0) {
    return String();
  }

  char out[65] = {};
  for (size_t i = 0; i < sizeof(digest); ++i) {
    std::snprintf(out + (i * 2U), sizeof(out) - (i * 2U), "%02x", digest[i]);
  }
  return String(out);
}

void identity_init() {
  if (g_initialized) {
    return;
  }

  g_config.enabled = false;
  g_config.port = kDefaultPort;
  g_config.max_sessions = kDefaultMaxSessions;
  g_config.auth_flags = kDefaultAuthFlags;
  g_config.device_name = kDefaultDeviceName;
  g_config.username = kDefaultUser;
  g_config.display_name = kDefaultDisplayName;
  g_config.user_hash = kBootstrapPasswordHash;
  g_config.root_hash = kBootstrapPasswordHash;
  g_config.user_admin = false;
  g_config.user_sudo = false;

  NvsNamespace ssh_pref;
  if (open_user_nvs(&ssh_pref, kSshNamespace, true)) {
    bool enabled = g_config.enabled;
    uint16_t port = g_config.port;
    uint8_t max_sessions = g_config.max_sessions;
    uint32_t auth_flags = g_config.auth_flags;
    std::string device_name;
    (void)ssh_pref.get_bool("enabled", &enabled);
    (void)ssh_pref.get_u16("port", &port);
    (void)ssh_pref.get_u8("max_sess", &max_sessions);
    (void)ssh_pref.get_u32("auth_flags", &auth_flags);
    if (ssh_pref.get_string("dev_name", &device_name)) {
      g_config.device_name = String(device_name.c_str());
      if (g_config.device_name == "MROS7DOFS3") {
        g_config.device_name = kDefaultDeviceName;
      }
    }
    g_config.enabled = enabled;
    g_config.port = port;
    g_config.max_sessions = max_sessions;
    g_config.auth_flags = auth_flags;
  }

  NvsNamespace user_pref;
  if (open_user_nvs(&user_pref, kUserNamespace, true)) {
    std::string username;
    std::string display_name;
    std::string user_hash;
    std::string root_hash;
    bool user_admin = g_config.user_admin;
    bool user_sudo = g_config.user_sudo;
    if (user_pref.get_string("username", &username)) {
      g_config.username = String(username.c_str());
    }
    if (user_pref.get_string("display", &display_name)) {
      g_config.display_name = String(display_name.c_str());
    }
    if (user_pref.get_string("user_hash", &user_hash)) {
      g_config.user_hash = String(user_hash.c_str());
    }
    if (user_pref.get_string("root_hash", &root_hash)) {
      g_config.root_hash = String(root_hash.c_str());
    }
    (void)user_pref.get_bool("user_admin", &user_admin);
    (void)user_pref.get_bool("user_sudo", &user_sudo);
    g_config.user_admin = user_admin;
    g_config.user_sudo = user_sudo;
  }

  if (!is_valid_username(g_config.username)) {
    g_config.username = kDefaultUser;
  }
  if (!is_valid_device_name(g_config.device_name)) {
    g_config.device_name = kDefaultDeviceName;
  }
  if (!is_hash_hex(g_config.user_hash)) {
    g_config.user_hash = kBootstrapPasswordHash;
  }
  if (!is_hash_hex(g_config.root_hash)) {
    g_config.root_hash = kBootstrapPasswordHash;
  }
  if (g_config.port == 0U) {
    g_config.port = kDefaultPort;
  }
  if (g_config.max_sessions == 0U || g_config.max_sessions > 2U) {
    g_config.max_sessions = kDefaultMaxSessions;
  }

  g_initialized = true;
  load_extra_users();
  save_ssh_config();
  save_user_config();
  save_extra_users();
}

IdentityConfig identity_get() {
  ensure_loaded();
  return g_config;
}

std::vector<UserAccount> list_users() {
  ensure_loaded();
  std::vector<UserAccount> users;
  users.push_back(build_root_account());
  users.push_back(build_primary_account());
  users.insert(users.end(), g_extra_users.begin(), g_extra_users.end());
  return users;
}

bool get_user(const String& username, UserAccount* out_user) {
  ensure_loaded();
  const String clean_user = clean_string(username);
  if (clean_user == kRootUser) {
    if (out_user != nullptr) {
      *out_user = build_root_account();
    }
    return true;
  }
  if (clean_user == g_config.username) {
    if (out_user != nullptr) {
      *out_user = build_primary_account();
    }
    return true;
  }
  size_t index = 0U;
  if (find_extra_user_index(clean_user, &index)) {
    if (out_user != nullptr) {
      *out_user = g_extra_users[index];
    }
    return true;
  }
  return false;
}

bool user_exists(const String& username) {
  return get_user(username, nullptr);
}

bool verify_password(const String& user, const String& password) {
  UserAccount account;
  if (!get_user(user, &account)) {
    return false;
  }
  return sha256_hex(password).equalsIgnoreCase(account.password_hash);
}

bool set_enabled(const bool enabled) {
  ensure_loaded();
  g_config.enabled = enabled;
  save_ssh_config();
  return true;
}

bool set_port(const uint16_t port) {
  ensure_loaded();
  if (port == 0U) {
    return false;
  }
  g_config.port = port;
  save_ssh_config();
  return true;
}

bool set_max_sessions(const uint8_t max_sessions) {
  ensure_loaded();
  if (max_sessions == 0U || max_sessions > 2U) {
    return false;
  }
  g_config.max_sessions = max_sessions;
  save_ssh_config();
  return true;
}

bool set_display_name(const String& display_name) {
  ensure_loaded();
  String value = clean_string(display_name);
  if (value.length() == 0U || value.length() > 64U) {
    return false;
  }
  g_config.display_name = value;
  save_user_config();
  return true;
}

bool set_username(const String& username) {
  ensure_loaded();
  String value = clean_string(username);
  if (!is_valid_username(value) || value == kRootUser) {
    return false;
  }
  if (find_extra_user_index(value, nullptr)) {
    return false;
  }
  g_config.username = value;
  save_user_config();
  return true;
}

bool set_device_name(const String& device_name) {
  ensure_loaded();
  String value = clean_string(device_name);
  if (!is_valid_device_name(value)) {
    return false;
  }
  g_config.device_name = value;
  save_ssh_config();
  return true;
}

bool set_primary_user_role(const bool admin, const bool sudo_enabled) {
  ensure_loaded();
  g_config.user_admin = admin;
  g_config.user_sudo = sudo_enabled;
  save_user_config();
  return true;
}

bool set_password_for_user(const String& user, const String& password) {
  ensure_loaded();
  const String clean_user = clean_string(user);
  if (password.length() < 8U || password.length() > 96U) {
    return false;
  }
  const String hash = sha256_hex(password);
  if (clean_user == g_config.username) {
    g_config.user_hash = hash;
    save_user_config();
    return true;
  }
  if (clean_user == kRootUser) {
    g_config.root_hash = hash;
    save_user_config();
    return true;
  }
  size_t index = 0U;
  if (!find_extra_user_index(clean_user, &index)) {
    return false;
  }
  g_extra_users[index].password_hash = hash;
  save_extra_users();
  return true;
}

bool add_user(
    const String& display_name,
    const String& username,
    const String& password,
    const bool admin,
    const bool sudo_enabled) {
  ensure_loaded();
  const String clean_name = clean_string(display_name);
  const String clean_user = clean_string(username);
  if (g_extra_users.size() >= (kMaxNormalUsers - 1U) ||
      clean_name.length() == 0U || clean_name.length() > 64U ||
      !is_valid_username(clean_user) || clean_user == kRootUser ||
      password.length() < 8U || password.length() > 96U || user_exists(clean_user)) {
    return false;
  }
  UserAccount account;
  account.username = clean_user;
  account.display_name = clean_name;
  account.password_hash = sha256_hex(password);
  account.admin = admin;
  account.sudo = sudo_enabled;
  g_extra_users.push_back(account);
  std::sort(
      g_extra_users.begin(),
      g_extra_users.end(),
      [](const UserAccount& lhs, const UserAccount& rhs) {
        return lhs.username < rhs.username;
      });
  save_extra_users();
  return true;
}

bool disable_user(const String& username) {
  ensure_loaded();
  const String clean_user = clean_string(username);
  if (clean_user == kRootUser || clean_user == g_config.username) {
    return false;
  }
  size_t index = 0U;
  if (!find_extra_user_index(clean_user, &index)) {
    return false;
  }
  g_extra_users.erase(g_extra_users.begin() + static_cast<std::vector<UserAccount>::difference_type>(index));
  save_extra_users();
  return true;
}

bool reset_identity_for_initial_setup() {
  ensure_loaded();
  g_config.enabled = false;
  g_config.port = kDefaultPort;
  g_config.max_sessions = kDefaultMaxSessions;
  g_config.auth_flags = kDefaultAuthFlags;
  g_config.device_name = kDefaultDeviceName;
  g_config.username = kDefaultUser;
  g_config.display_name = kDefaultDisplayName;
  g_config.user_hash = kBootstrapPasswordHash;
  g_config.root_hash = kBootstrapPasswordHash;
  g_config.user_admin = false;
  g_config.user_sudo = false;
  g_extra_users.clear();
  save_ssh_config();
  save_user_config();
  save_extra_users();
  cleanup_legacy_extra_users_file();
  return true;
}

String authorized_keys_path() {
  return logger_user_path("ssh/authorized_keys");
}

bool append_authorized_key(const String& key) {
  ensure_loaded();
  String value = clean_string(key);
  if (!value.startsWith("ssh-ed25519 ") && !value.startsWith("ssh-rsa ")) {
    return false;
  }
  if (!ensure_ssh_dir()) {
    return false;
  }
  std::string line = value.c_str();
  line.push_back('\n');
  return mros::platform::mros_file_append_all(authorized_keys_path().c_str(), line);
}

bool is_valid_username(const String& username) {
  if (username.length() < 1U || username.length() > 32U) {
    return false;
  }
  for (size_t i = 0; i < username.length(); ++i) {
    const char ch = username[i];
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool is_valid_device_name(const String& device_name) {
  if (device_name.length() < 3U || device_name.length() > 32U) {
    return false;
  }
  for (size_t i = 0; i < device_name.length(); ++i) {
    const char ch = device_name[i];
    const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

const char* default_username() { return kDefaultUser; }

const char* root_username() { return kRootUser; }

const char* backend_name() {
#if MROS_ENABLE_WOLFSSH_BACKEND && __has_include(<wolfssh/ssh.h>)
  return "wolfssh";
#else
  return "not-built";
#endif
}

}  // namespace mros::ssh
