#pragma once

#include <stdint.h>

struct AppState {
  float turret_deg = 0.0f;
  float joints_deg[6] = {0};
  uint16_t error_code = 0;
  bool connected = false;
};

const AppState &app_state_get();
void app_state_set(const AppState &state);
