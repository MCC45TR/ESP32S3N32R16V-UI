#pragma once

#include <cstdint>

namespace mros::platform {

uint32_t mros_system_heap_free();
uint32_t mros_system_heap_min_free();
uint32_t mros_system_flash_total();
uint32_t mros_system_psram_total();
uint32_t mros_system_psram_free();
uint32_t mros_system_app_image_size();
uint32_t mros_system_cpu_freq_mhz();
const char* mros_system_sdk_version();
bool mros_system_has_psram();
void mros_system_restart();

}  // namespace mros::platform
