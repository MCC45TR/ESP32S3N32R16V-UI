#include "src/platform/mros_fs.h"

#include <cerrno>
#include <sys/stat.h>

#include "src/platform/esp_littlefs_compat.h"

namespace mros::platform {

namespace {

bool g_fs_mounted = false;
std::string g_base_path = "/littlefs";
std::string g_partition_label;

std::string normalize_path(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    return g_base_path;
  }

  std::string raw(path);
  if (raw.rfind(g_base_path, 0) == 0) {
    return raw;
  }
  if (raw.front() != '/') {
    return g_base_path + "/" + raw;
  }
  return g_base_path + raw;
}

}  // namespace

bool mros_fs_mount(const FsMountConfig& config) {
  if (g_fs_mounted) {
    return true;
  }

  const char* base_path = (config.base_path != nullptr && config.base_path[0] != '\0')
                              ? config.base_path
                              : "/littlefs";
  esp_vfs_littlefs_conf_t littlefs = {};
  littlefs.base_path = base_path;
  littlefs.partition_label = config.partition_label;
  littlefs.partition = nullptr;
  littlefs.format_if_mount_failed = config.format_if_mount_failed ? 1U : 0U;
  littlefs.read_only = 0U;
  littlefs.dont_mount = 0U;
  littlefs.grow_on_mount = 0U;

  if (esp_vfs_littlefs_register(&littlefs) != ESP_OK) {
    return false;
  }
  const char* mounted_label =
      (config.partition_label != nullptr) ? config.partition_label : "littlefs";
  if (!esp_littlefs_mounted(mounted_label)) {
    (void)esp_vfs_littlefs_unregister(mounted_label);
    return false;
  }

  g_base_path = base_path;
  g_partition_label = mounted_label;
  g_fs_mounted = true;
  return true;
}

void mros_fs_unmount() {
  if (!g_fs_mounted) {
    return;
  }
  (void)esp_vfs_littlefs_unregister(mros_fs_partition_label());
  g_fs_mounted = false;
}

bool mros_fs_is_mounted() { return g_fs_mounted; }

const char* mros_fs_base_path() { return g_base_path.c_str(); }

const char* mros_fs_partition_label() {
  return g_partition_label.empty() ? nullptr : g_partition_label.c_str();
}

std::string mros_fs_vfs_path(const char* path) { return normalize_path(path); }

FILE* mros_fs_open(const char* path, const char* mode) {
  if (!g_fs_mounted || mode == nullptr) {
    return nullptr;
  }
  const std::string vfs_path = normalize_path(path);
  return std::fopen(vfs_path.c_str(), mode);
}

bool mros_fs_exists(const char* path) {
  if (!g_fs_mounted) {
    return false;
  }
  struct stat info = {};
  const std::string vfs_path = normalize_path(path);
  return ::stat(vfs_path.c_str(), &info) == 0;
}

bool mros_fs_mkdir(const char* path) {
  if (!g_fs_mounted) {
    return false;
  }
  const std::string vfs_path = normalize_path(path);
  if (::mkdir(vfs_path.c_str(), 0755) == 0) {
    return true;
  }
  return errno == EEXIST;
}

bool mros_fs_remove(const char* path) {
  if (!g_fs_mounted) {
    return false;
  }
  const std::string vfs_path = normalize_path(path);
  return ::remove(vfs_path.c_str()) == 0;
}

bool mros_fs_rename(const char* from, const char* to) {
  if (!g_fs_mounted) {
    return false;
  }
  const std::string from_vfs = normalize_path(from);
  const std::string to_vfs = normalize_path(to);
  return ::rename(from_vfs.c_str(), to_vfs.c_str()) == 0;
}

bool mros_fs_file_size(const char* path, size_t* out_size) {
  if (!g_fs_mounted || out_size == nullptr) {
    return false;
  }
  struct stat info = {};
  const std::string vfs_path = normalize_path(path);
  if (::stat(vfs_path.c_str(), &info) != 0) {
    return false;
  }
  *out_size = static_cast<size_t>(info.st_size);
  return true;
}

bool mros_fs_info(uint64_t* total_bytes, uint64_t* used_bytes) {
  if (!g_fs_mounted || total_bytes == nullptr || used_bytes == nullptr) {
    return false;
  }

  size_t total = 0U;
  size_t used = 0U;
  const char* partition = mros_fs_partition_label();
  if (partition != nullptr &&
      esp_littlefs_info(partition, &total, &used) == ESP_OK) {
    *total_bytes = total;
    *used_bytes = used;
    return true;
  }
  return false;
}

}  // namespace mros::platform
