#include "mros_json_writer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace mros::utils {
namespace {

portMUX_TYPE g_json_metric_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t g_json_overflow_count = 0;

}  // namespace

void record_json_overflow() {
  portENTER_CRITICAL(&g_json_metric_mux);
  if (g_json_overflow_count != 0xFFFFFFFFUL) {
    ++g_json_overflow_count;
  }
  portEXIT_CRITICAL(&g_json_metric_mux);
}

uint32_t json_overflow_count() {
  portENTER_CRITICAL(&g_json_metric_mux);
  const uint32_t value = g_json_overflow_count;
  portEXIT_CRITICAL(&g_json_metric_mux);
  return value;
}

void reset_json_overflow_count() {
  portENTER_CRITICAL(&g_json_metric_mux);
  g_json_overflow_count = 0;
  portEXIT_CRITICAL(&g_json_metric_mux);
}

}  // namespace mros::utils
