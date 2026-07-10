#include "event_bus.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

static EventGroupHandle_t g_events = nullptr;

void event_bus_init() {
  if (!g_events) g_events = xEventGroupCreate();
}

void event_bus_publish(uint32_t event_mask) {
  if (!g_events) event_bus_init();
  xEventGroupSetBits(g_events, event_mask);
}

void event_bus_clear(uint32_t event_mask) {
  if (!g_events) event_bus_init();
  xEventGroupClearBits(g_events, event_mask);
}

uint32_t event_bus_wait(uint32_t event_mask, uint32_t timeout_ms) {
  if (!g_events) event_bus_init();
  EventBits_t bits = xEventGroupWaitBits(g_events, event_mask, pdTRUE, pdFALSE,
                                         pdMS_TO_TICKS(timeout_ms));
  return static_cast<uint32_t>(bits);
}
