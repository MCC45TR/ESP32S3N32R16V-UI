#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace mros_sync {
inline SemaphoreHandle_t create_mutex() { return xSemaphoreCreateMutex(); }
}
