#include "src/platform/mros_nvs.h"

#include <array>

namespace mros::platform {

namespace {

constexpr const char* kDefaultPartitionLabel = "default";
constexpr std::array<const char*, 2> kUserPartitionFallback = {
    "nvs_sys_usr",
    "nvs_usr",
};

}  // namespace

NvsNamespace::NvsNamespace() : handle_(0), open_(false) {}

NvsNamespace::~NvsNamespace() { close(); }

bool NvsNamespace::open(const char* name, const bool read_only,
                        const NvsPartitionMode partition_mode) {
  close();
  if (name == nullptr || name[0] == '\0') {
    return false;
  }

  const nvs_open_mode_t mode = read_only ? NVS_READONLY : NVS_READWRITE;
  if (partition_mode == NvsPartitionMode::UserPartitionsThenDefault) {
    for (const char* partition : kUserPartitionFallback) {
      if (open_from_partition(partition, name, mode)) {
        return true;
      }
    }
  }

  return open_default(name, mode);
}

void NvsNamespace::close() {
  if (!open_) {
    return;
  }
  nvs_close(handle_);
  handle_ = 0;
  open_ = false;
  partition_label_.clear();
}

bool NvsNamespace::is_open() const { return open_; }

bool NvsNamespace::open_default(const char* name, const nvs_open_mode_t mode) {
  if (nvs_open(name, mode, &handle_) != ESP_OK) {
    return false;
  }
  open_ = true;
  partition_label_ = kDefaultPartitionLabel;
  return true;
}

bool NvsNamespace::open_from_partition(const char* partition, const char* name,
                                       const nvs_open_mode_t mode) {
  if (partition == nullptr || partition[0] == '\0') {
    return false;
  }
  if (nvs_open_from_partition(partition, name, mode, &handle_) != ESP_OK) {
    return false;
  }
  open_ = true;
  partition_label_ = partition;
  return true;
}

bool NvsNamespace::contains(const char* key) const {
  if (!open_ || key == nullptr || key[0] == '\0') {
    return false;
  }
  uint8_t u8 = 0U;
  uint16_t u16 = 0U;
  uint32_t u32 = 0U;
  if (nvs_get_u8(handle_, key, &u8) == ESP_OK ||
      nvs_get_u16(handle_, key, &u16) == ESP_OK ||
      nvs_get_u32(handle_, key, &u32) == ESP_OK) {
    return true;
  }

  size_t required = 0U;
  if (nvs_get_str(handle_, key, nullptr, &required) == ESP_OK || required > 0U) {
    return true;
  }
  required = 0U;
  if (nvs_get_blob(handle_, key, nullptr, &required) == ESP_OK || required > 0U) {
    return true;
  }
  return false;
}

bool NvsNamespace::get_bool(const char* key, bool* value) const {
  if (!open_ || value == nullptr) {
    return false;
  }
  uint8_t raw = 0;
  if (nvs_get_u8(handle_, key, &raw) != ESP_OK) {
    return false;
  }
  *value = (raw != 0U);
  return true;
}

bool NvsNamespace::get_u8(const char* key, uint8_t* value) const {
  if (!open_ || value == nullptr) {
    return false;
  }
  return nvs_get_u8(handle_, key, value) == ESP_OK;
}

bool NvsNamespace::get_u16(const char* key, uint16_t* value) const {
  if (!open_ || value == nullptr) {
    return false;
  }
  return nvs_get_u16(handle_, key, value) == ESP_OK;
}

bool NvsNamespace::get_u32(const char* key, uint32_t* value) const {
  if (!open_ || value == nullptr) {
    return false;
  }
  return nvs_get_u32(handle_, key, value) == ESP_OK;
}

bool NvsNamespace::get_string(const char* key, std::string* value) const {
  if (!open_ || value == nullptr) {
    return false;
  }
  size_t required = 0;
  if (nvs_get_str(handle_, key, nullptr, &required) != ESP_OK || required == 0U) {
    return false;
  }
  value->assign(required, '\0');
  size_t len = required;
  if (nvs_get_str(handle_, key, value->data(), &len) != ESP_OK) {
    value->clear();
    return false;
  }
  if (!value->empty() && value->back() == '\0') {
    value->pop_back();
  }
  return true;
}

bool NvsNamespace::get_blob(const char* key, void* data, const size_t len) const {
  if (!open_ || data == nullptr || len == 0U) {
    return false;
  }
  size_t required = len;
  return nvs_get_blob(handle_, key, data, &required) == ESP_OK && required == len;
}

size_t NvsNamespace::get_blob_size(const char* key) const {
  if (!open_) {
    return 0U;
  }
  size_t required = 0U;
  if (nvs_get_blob(handle_, key, nullptr, &required) != ESP_OK) {
    return 0U;
  }
  return required;
}

bool NvsNamespace::set_bool(const char* key, const bool value) {
  if (!open_ || nvs_set_u8(handle_, key, value ? 1U : 0U) != ESP_OK) {
    return false;
  }
  return commit();
}

bool NvsNamespace::set_u8(const char* key, const uint8_t value) {
  if (!open_ || nvs_set_u8(handle_, key, value) != ESP_OK) {
    return false;
  }
  return commit();
}

bool NvsNamespace::set_u16(const char* key, const uint16_t value) {
  if (!open_ || nvs_set_u16(handle_, key, value) != ESP_OK) {
    return false;
  }
  return commit();
}

bool NvsNamespace::set_u32(const char* key, const uint32_t value) {
  if (!open_ || nvs_set_u32(handle_, key, value) != ESP_OK) {
    return false;
  }
  return commit();
}

bool NvsNamespace::set_string(const char* key, const std::string& value) {
  if (!open_ || nvs_set_str(handle_, key, value.c_str()) != ESP_OK) {
    return false;
  }
  return commit();
}

bool NvsNamespace::set_blob(const char* key, const void* data, const size_t len) {
  if (!open_ || data == nullptr || len == 0U ||
      nvs_set_blob(handle_, key, data, len) != ESP_OK) {
    return false;
  }
  return commit();
}

bool NvsNamespace::erase_key(const char* key) {
  if (!open_) {
    return false;
  }
  const esp_err_t err = nvs_erase_key(handle_, key);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }
  return commit();
}

bool NvsNamespace::commit() { return open_ && nvs_commit(handle_) == ESP_OK; }

const char* NvsNamespace::partition_label() const {
  return partition_label_.empty() ? nullptr : partition_label_.c_str();
}

}  // namespace mros::platform
