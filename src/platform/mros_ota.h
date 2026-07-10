#pragma once

#include <esp_err.h>
#include <esp_partition.h>

namespace mros::platform {

const esp_partition_t* mros_ota_find_app_partition(const char* label);
bool mros_ota_partition_has_valid_app(const esp_partition_t* partition);
esp_err_t mros_ota_set_boot_partition(const esp_partition_t* partition);
bool mros_ota_boot_partition_and_restart(const esp_partition_t* partition);

}  // namespace mros::platform
