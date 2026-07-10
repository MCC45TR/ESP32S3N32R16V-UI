#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <nvs.h>

namespace mros::platform {

enum class NvsPartitionMode : uint8_t {
  DefaultOnly = 0,
  UserPartitionsThenDefault,
};

class NvsNamespace {
 public:
  NvsNamespace();
  ~NvsNamespace();

  NvsNamespace(const NvsNamespace&) = delete;
  NvsNamespace& operator=(const NvsNamespace&) = delete;

  bool open(const char* name, bool read_only,
            NvsPartitionMode partition_mode = NvsPartitionMode::DefaultOnly);
  void close();
  bool is_open() const;

  bool contains(const char* key) const;

  bool get_bool(const char* key, bool* value) const;
  bool get_u8(const char* key, uint8_t* value) const;
  bool get_u16(const char* key, uint16_t* value) const;
  bool get_u32(const char* key, uint32_t* value) const;
  bool get_string(const char* key, std::string* value) const;
  bool get_blob(const char* key, void* data, size_t len) const;
  size_t get_blob_size(const char* key) const;

  bool set_bool(const char* key, bool value);
  bool set_u8(const char* key, uint8_t value);
  bool set_u16(const char* key, uint16_t value);
  bool set_u32(const char* key, uint32_t value);
  bool set_string(const char* key, const std::string& value);
  bool set_blob(const char* key, const void* data, size_t len);
  bool erase_key(const char* key);

  const char* partition_label() const;

 private:
  bool open_default(const char* name, nvs_open_mode_t mode);
  bool open_from_partition(const char* partition, const char* name,
                           nvs_open_mode_t mode);
  bool commit();

  nvs_handle_t handle_;
  bool open_;
  std::string partition_label_;
};

}  // namespace mros::platform
