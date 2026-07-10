#pragma once

#include <stddef.h>
#include <stdint.h>

// Public builds contain no infrastructure credentials. The setup access point
// remains disabled until a local wifi_secrets.h supplies a non-placeholder
// password accepted by wifi_manager.cpp.
#define WIFI_AP_SSID "MROS-DEUSCARA-SETUP"
#define WIFI_AP_PASSWORD ""

struct WifiCredential {
  uint16_t version;
  const char* ssid;
  const char* pass;
};

static constexpr uint16_t WIFI_SECRETS_VERSION = 1;
static const WifiCredential known_networks[] = {
    {WIFI_SECRETS_VERSION, nullptr, nullptr},
};
static constexpr size_t known_networks_count =
    sizeof(known_networks) / sizeof(known_networks[0]);
