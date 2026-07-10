#include "auth_handler.h"
#include "WString.h"
#include <esp_random.h>
#include <esp_system.h>

#include "src/platform/mros_time.h"

namespace {

String g_token;
String g_user;
unsigned long g_expiry_ms = 0;

static String make_random_token() {
  String token;
  token.reserve(33);
  for (int i = 0; i < 4; ++i) {
    uint32_t r = esp_random();
    char buf[9];
    snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(r));
    token += buf;
  }
  return token;
}

static bool token_alive() {
  if (g_token.length() == 0) return false;
  if (g_expiry_ms == 0) return true;
  return static_cast<long>(g_expiry_ms - mros::platform::mros_millis()) > 0;
}

}  // namespace

void auth_handler_init() {
  g_token = "";
  g_user = "";
  g_expiry_ms = 0;
}

String auth_handler_issue_token(const String &username, uint32_t ttl_ms) {
  g_user = username;
  g_token = make_random_token();
  g_expiry_ms = mros::platform::mros_millis() + ttl_ms;
  return g_token;
}

void auth_handler_revoke() {
  g_token = "";
  g_user = "";
  g_expiry_ms = 0;
}

bool auth_handler_validate_token(const String &token) {
  if (!token_alive()) {
    auth_handler_revoke();
    return false;
  }
  return token.length() > 0 && token == g_token;
}

const String &auth_handler_current_user() { return g_user; }
