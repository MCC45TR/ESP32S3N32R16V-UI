#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace mros::platform {

struct FsMountConfig {
  const char* base_path = "/littlefs";
  size_t max_open_files = 10U;
  bool format_if_mount_failed = false;
  const char* partition_label = nullptr;
};

bool mros_fs_mount(const FsMountConfig& config);
void mros_fs_unmount();
bool mros_fs_is_mounted();
const char* mros_fs_base_path();
const char* mros_fs_partition_label();

std::string mros_fs_vfs_path(const char* path);
FILE* mros_fs_open(const char* path, const char* mode);
bool mros_fs_exists(const char* path);
bool mros_fs_mkdir(const char* path);
bool mros_fs_remove(const char* path);
bool mros_fs_rename(const char* from, const char* to);
bool mros_fs_file_size(const char* path, size_t* out_size);
bool mros_fs_info(uint64_t* total_bytes, uint64_t* used_bytes);

}  // namespace mros::platform
