#include "src/platform/mros_ota.h"

#include <esp_app_desc.h>
#include <esp_ota_ops.h>

#include "src/platform/mros_system.h"

namespace mros::platform {

const esp_partition_t* mros_ota_find_app_partition(const char* label) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  ESP_PARTITION_SUBTYPE_ANY, label);
}

bool mros_ota_partition_has_valid_app(const esp_partition_t* partition) {
  if (partition == nullptr) {
    return false;
  }
  esp_app_desc_t desc = {};
  return esp_ota_get_partition_description(partition, &desc) == ESP_OK;
}

esp_err_t mros_ota_set_boot_partition(const esp_partition_t* partition) {
  if (partition == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return esp_ota_set_boot_partition(partition);
}

bool mros_ota_boot_partition_and_restart(const esp_partition_t* partition) {
  if (partition == nullptr) {
    return false;
  }
  if (mros_ota_set_boot_partition(partition) != ESP_OK) {
    return false;
  }
  mros_system_restart();
  return true;
}

}  // namespace mros::platform
