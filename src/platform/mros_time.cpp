#include "src/platform/mros_time.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace mros::platform {

uint64_t mros_micros() {
  return static_cast<uint64_t>(esp_timer_get_time());
}

uint32_t mros_millis() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void mros_delay_ms(const uint32_t delay_ms) {
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

}  // namespace mros::platform
