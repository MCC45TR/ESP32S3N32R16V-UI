#include "src/platform/mros_system.h"

#include <esp_private/esp_clk.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_psram.h>
#include <esp_system.h>

namespace mros::platform {

uint32_t mros_system_heap_free() {
  return static_cast<uint32_t>(esp_get_free_heap_size());
}

uint32_t mros_system_heap_min_free() {
  return static_cast<uint32_t>(esp_get_minimum_free_heap_size());
}

uint32_t mros_system_flash_total() {
#if defined(CONFIG_ESPTOOLPY_FLASHSIZE_1MB)
  return 1U * 1024U * 1024U;
#elif defined(CONFIG_ESPTOOLPY_FLASHSIZE_2MB)
  return 2U * 1024U * 1024U;
#elif defined(CONFIG_ESPTOOLPY_FLASHSIZE_4MB)
  return 4U * 1024U * 1024U;
#elif defined(CONFIG_ESPTOOLPY_FLASHSIZE_8MB)
  return 8U * 1024U * 1024U;
#elif defined(CONFIG_ESPTOOLPY_FLASHSIZE_16MB)
  return 16U * 1024U * 1024U;
#elif defined(CONFIG_ESPTOOLPY_FLASHSIZE_32MB)
  return 32U * 1024U * 1024U;
#elif defined(CONFIG_ESPTOOLPY_FLASHSIZE_64MB)
  return 64U * 1024U * 1024U;
#elif defined(CONFIG_ESPTOOLPY_FLASHSIZE_128MB)
  return 128U * 1024U * 1024U;
#else
  return 0U;
#endif
}

uint32_t mros_system_psram_total() {
  return static_cast<uint32_t>(esp_psram_get_size());
}

uint32_t mros_system_psram_free() {
  return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

uint32_t mros_system_app_image_size() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr) {
    return 0U;
  }
  esp_partition_pos_t position = {};
  position.offset = running->address;
  position.size = running->size;
  esp_image_metadata_t metadata = {};
  if (esp_image_get_metadata(&position, &metadata) != ESP_OK) {
    return 0U;
  }
  return metadata.image_len;
}

uint32_t mros_system_cpu_freq_mhz() {
  return static_cast<uint32_t>(esp_clk_cpu_freq() / 1000000U);
}

const char* mros_system_sdk_version() {
  return esp_get_idf_version();
}

bool mros_system_has_psram() {
  return mros_system_psram_total() > 0U;
}

void mros_system_restart() {
  esp_restart();
}

}  // namespace mros::platform
