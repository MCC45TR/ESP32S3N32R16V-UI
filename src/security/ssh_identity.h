#pragma once

#include <cstdint>
#include <vector>
#include "WString.h"

namespace mros::ssh {

struct UserAccount {
  String username;
  String display_name;
  String password_hash;
  bool admin = false;
  bool sudo = false;
  bool primary = false;
  bool root = false;
};

struct IdentityConfig {
  bool enabled = false;
  uint16_t port = 6755;
  uint8_t max_sessions = 1;
  uint32_t auth_flags = 0x03U;
  String device_name = "DEUSCARA-S3V";
  String username = "mros";
  String display_name = "MROS Operator";
  String user_hash;
  String root_hash;
  bool user_admin = false;
  bool user_sudo = false;
};

void identity_init();
IdentityConfig identity_get();

String sha256_hex(const String& text);
bool verify_password(const String& user, const String& password);

bool set_enabled(bool enabled);
bool set_port(uint16_t port);
bool set_max_sessions(uint8_t max_sessions);
bool set_display_name(const String& display_name);
bool set_username(const String& username);
bool set_device_name(const String& device_name);
bool set_primary_user_role(bool admin, bool sudo_enabled);
bool set_password_for_user(const String& user, const String& password);
bool add_user(
    const String& display_name,
    const String& username,
    const String& password,
    bool admin,
    bool sudo_enabled);
bool disable_user(const String& username);
bool reset_identity_for_initial_setup();
std::vector<UserAccount> list_users();
bool get_user(const String& username, UserAccount* out_user);
bool user_exists(const String& username);
bool append_authorized_key(const String& key);
String authorized_keys_path();

bool is_valid_username(const String& username);
bool is_valid_device_name(const String& device_name);

const char* default_username();
const char* root_username();
const char* backend_name();

}  // namespace mros::ssh
