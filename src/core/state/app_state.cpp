#include "app_state.h"

static AppState g_state;

const AppState &app_state_get() { return g_state; }
void app_state_set(const AppState &state) { g_state = state; }
