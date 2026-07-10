#pragma once

#include <stddef.h>
#include <stdint.h>

// Copy this file to wifi_secrets.h for a local build. Replace every placeholder
// before enabling the setup access point or adding infrastructure networks.
#define WIFI_AP_SSID "<SET_DEVICE_SPECIFIC_AP_SSID>"
#define WIFI_AP_PASSWORD "<SET_RANDOM_AP_PASSWORD_12_PLUS>"

struct WifiCredential {
  uint16_t version;
  const char* ssid;
  const char* pass;
};

static constexpr uint16_t WIFI_SECRETS_VERSION = 1;
static const WifiCredential known_networks[] = {
    {WIFI_SECRETS_VERSION, "<SET_WIFI_SSID>", "<SET_WIFI_PASSWORD>"},
};
static constexpr size_t known_networks_count =
    sizeof(known_networks) / sizeof(known_networks[0]);
