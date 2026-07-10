#pragma once

#include <stdint.h>

class String;

void auth_handler_init();
String auth_handler_issue_token(const String &username, uint32_t ttl_ms = 12UL * 60UL * 60UL * 1000UL);
void auth_handler_revoke();
bool auth_handler_validate_token(const String &token);
const String &auth_handler_current_user();
