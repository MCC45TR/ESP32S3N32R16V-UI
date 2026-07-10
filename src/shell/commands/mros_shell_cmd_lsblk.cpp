#include "src/shell/mros_shell_internal.h"
#include "src/shell/mshell_remote.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "src/drivers/storage/logger_driver.h"

namespace mros::shell {
namespace {

struct LsblkFlags {
  bool human_readable = false;
  bool filesystems_only = false;
};

struct LsblkEntry {
  std::string name;
  std::string type;
  std::string subtype;
  std::string fstype;
  std::string mount;
  std::string role;
  std::string detail;
  uint32_t address = 0U;
  uint64_t size_bytes = 0U;
  uint64_t used_bytes = 0U;
  uint64_t avail_bytes = 0U;
  bool has_usage = false;
};

std::string format_size(const uint64_t bytes, const bool human_readable) {
  char buffer[32] = {};
  if (!human_readable) {
    std::snprintf(buffer, sizeof(buffer), "%lluK", static_cast<unsigned long long>((bytes + 1023U) / 1024U));
    return buffer;
  }

  static const char* units[] = {"B", "K", "M", "G"};
  double scaled = static_cast<double>(bytes);
  size_t unit_index = 0U;
  while (scaled >= 1024.0 && unit_index < 3U) {
    scaled /= 1024.0;
    ++unit_index;
  }
  if (scaled >= 10.0 || unit_index == 0U) {
    std::snprintf(buffer, sizeof(buffer), "%.0f%s", scaled, units[unit_index]);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f%s", scaled, units[unit_index]);
  }
  return buffer;
}

std::string format_hex32(const uint32_t value) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%06lX", static_cast<unsigned long>(value));
  return buffer;
}

const char* partition_type_name(const esp_partition_type_t type) {
  switch (type) {
    case ESP_PARTITION_TYPE_APP:
      return "app";
    case ESP_PARTITION_TYPE_DATA:
      return "data";
    default:
      return "unknown";
  }
}

const char* app_subtype_name(const esp_partition_subtype_t subtype) {
  switch (subtype) {
    case ESP_PARTITION_SUBTYPE_APP_FACTORY:
      return "factory";
    case ESP_PARTITION_SUBTYPE_APP_OTA_0:
      return "ota_0";
    case ESP_PARTITION_SUBTYPE_APP_OTA_1:
      return "ota_1";
    case ESP_PARTITION_SUBTYPE_APP_OTA_2:
      return "ota_2";
    case ESP_PARTITION_SUBTYPE_APP_OTA_3:
      return "ota_3";
    case ESP_PARTITION_SUBTYPE_APP_OTA_4:
      return "ota_4";
    case ESP_PARTITION_SUBTYPE_APP_OTA_5:
      return "ota_5";
    case ESP_PARTITION_SUBTYPE_APP_OTA_6:
      return "ota_6";
    case ESP_PARTITION_SUBTYPE_APP_OTA_7:
      return "ota_7";
    case ESP_PARTITION_SUBTYPE_APP_OTA_8:
      return "ota_8";
    case ESP_PARTITION_SUBTYPE_APP_OTA_9:
      return "ota_9";
    case ESP_PARTITION_SUBTYPE_APP_OTA_10:
      return "ota_10";
    case ESP_PARTITION_SUBTYPE_APP_OTA_11:
      return "ota_11";
    case ESP_PARTITION_SUBTYPE_APP_OTA_12:
      return "ota_12";
    case ESP_PARTITION_SUBTYPE_APP_OTA_13:
      return "ota_13";
    case ESP_PARTITION_SUBTYPE_APP_OTA_14:
      return "ota_14";
    case ESP_PARTITION_SUBTYPE_APP_OTA_15:
      return "ota_15";
    case ESP_PARTITION_SUBTYPE_APP_TEST:
      return "test";
    default:
      return nullptr;
  }
}

const char* data_subtype_name(const esp_partition_subtype_t subtype) {
  switch (subtype) {
    case ESP_PARTITION_SUBTYPE_DATA_OTA:
      return "ota";
    case ESP_PARTITION_SUBTYPE_DATA_PHY:
      return "phy";
    case ESP_PARTITION_SUBTYPE_DATA_NVS:
      return "nvs";
    case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:
      return "coredump";
    case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:
      return "nvs_keys";
    case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM:
      return "efuse_em";
    case ESP_PARTITION_SUBTYPE_DATA_UNDEFINED:
      return "undefined";
    case ESP_PARTITION_SUBTYPE_DATA_FAT:
      return "fat";
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
      return "spiffs";
    case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS:
      return "littlefs";
    default:
      return nullptr;
  }
}

std::string partition_subtype_name(const esp_partition_t* partition) {
  if (partition == nullptr) {
    return "--";
  }
  const esp_partition_subtype_t subtype = static_cast<esp_partition_subtype_t>(partition->subtype);
  const char* name = nullptr;
  if (partition->type == ESP_PARTITION_TYPE_APP) {
    name = app_subtype_name(subtype);
  } else if (partition->type == ESP_PARTITION_TYPE_DATA) {
    name = data_subtype_name(subtype);
  }
  if (name != nullptr) {
    return name;
  }
  char buffer[12] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%02X", static_cast<unsigned>(partition->subtype));
  return buffer;
}

const char* fs_type_name(const esp_partition_subtype_t subtype) {
  switch (subtype) {
    case ESP_PARTITION_SUBTYPE_DATA_FAT:
      return "fat16/fat32";
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
      return "spiffs";
    case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS:
      return "littlefs";
    default:
      return nullptr;
  }
}

std::string partition_role(const esp_partition_t* partition,
                           const esp_partition_t* running,
                           const esp_partition_t* boot) {
  if (partition == nullptr) {
    return "-";
  }
  std::string role;
  if (running != nullptr && partition->address == running->address) {
    role += "running";
  }
  if (boot != nullptr && partition->address == boot->address) {
    if (!role.empty()) role += ",";
    role += "boot";
  }
  if (std::strcmp(partition->label, "recovery") == 0) {
    if (!role.empty()) role += ",";
    role += "recovery";
  } else if (std::strcmp(partition->label, "app0") == 0) {
    if (!role.empty()) role += ",";
    role += "main";
  } else if (std::strcmp(partition->label, "littlefs") == 0) {
    if (!role.empty()) role += ",";
    role += "storage";
  } else if (std::strcmp(partition->label, "nvs_sys_usr") == 0) {
    if (!role.empty()) role += ",";
    role += "user-nvs";
  }
  return role.empty() ? "-" : role;
}

std::string app_partition_detail(const esp_partition_t* partition) {
  if (partition == nullptr || partition->type != ESP_PARTITION_TYPE_APP) {
    return "-";
  }
  esp_app_desc_t desc {};
  if (esp_ota_get_partition_description(partition, &desc) != ESP_OK) {
    return "no-app";
  }
  std::string detail = "ver=";
  detail += desc.version[0] != '\0' ? desc.version : "-";
  return detail;
}

bool parse_lsblk_args(ShellContext& ctx, LsblkFlags* flags, bool* help_requested) {
  if (flags == nullptr || help_requested == nullptr) {
    return false;
  }

  *help_requested = false;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      *help_requested = true;
      return true;
    }
    if (arg == "--all") {
      continue;
    }
    if (arg == "--fs" || arg == "--filesystems") {
      flags->filesystems_only = true;
      continue;
    }
    if (arg == "--human-readable") {
      flags->human_readable = true;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'a':
            break;
          case 'f':
            flags->filesystems_only = true;
            break;
          case 'h':
            flags->human_readable = true;
            break;
          default:
            shell_printf(ctx.state, "lsblk: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }
    shell_printf(ctx.state, "lsblk: unexpected operand '%s'\n", arg.c_str());
    return false;
  }
  return true;
}

}  // namespace

void shell_help_lsblk(ShellState& state) {
  shell_write_line(state, "Usage: lsblk [OPTION]...");
  shell_write_line(state, "List ESP-IDF partition table entries.");
  shell_write_line(state, "  -a, --all                accepted for compatibility; default already shows all");
  shell_write_line(state, "  -f, --fs, --filesystems  show filesystem partitions only");
  shell_write_line(state, "  -h, --human-readable     show sizes in a human-readable form");
}

int shell_cmd_lsblk(ShellContext& ctx) {
  LsblkFlags flags {};
  bool help_requested = false;
  if (!parse_lsblk_args(ctx, &flags, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_lsblk(ctx.state);
    return 0;
  }

  const bool fs_ready = shell_is_storage_mounted(ctx.state);
  uint64_t fs_total = 0U;
  uint64_t fs_used = 0U;
  if (fs_ready) {
    (void)logger_storage_info(&fs_total, &fs_used);
  }
  const uint64_t fs_free = (fs_total >= fs_used) ? (fs_total - fs_used) : 0U;

  const esp_partition_t* running_part = esp_ota_get_running_partition();
  const esp_partition_t* boot_part = esp_ota_get_boot_partition();
  std::vector<LsblkEntry> entries;
  const esp_partition_type_t types[] = {
      ESP_PARTITION_TYPE_APP,
      ESP_PARTITION_TYPE_DATA,
  };
  for (const esp_partition_type_t type : types) {
    for (esp_partition_iterator_t it = esp_partition_find(type, ESP_PARTITION_SUBTYPE_ANY, nullptr);
         it != nullptr;
         it = esp_partition_next(it)) {
      const esp_partition_t* partition = esp_partition_get(it);
      if (partition == nullptr) {
        continue;
      }

      const esp_partition_subtype_t subtype = static_cast<esp_partition_subtype_t>(partition->subtype);
      const char* fs_name = (partition->type == ESP_PARTITION_TYPE_DATA) ? fs_type_name(subtype) : nullptr;
      if (flags.filesystems_only && fs_name == nullptr) {
        continue;
      }

      LsblkEntry entry {};
      entry.name = (partition->label[0] != '\0') ? partition->label : partition_type_name(partition->type);
      entry.type = partition_type_name(partition->type);
      entry.subtype = partition_subtype_name(partition);
      entry.fstype = (fs_name != nullptr) ? fs_name : "-";
      entry.mount = "-";
      entry.role = partition_role(partition, running_part, boot_part);
      entry.detail = app_partition_detail(partition);
      entry.address = partition->address;
      entry.size_bytes = static_cast<uint64_t>(partition->size);

      if (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_LITTLEFS && fs_ready) {
        entry.mount = ctx.state.config.storage_mount_point != nullptr ? ctx.state.config.storage_mount_point : "/fs";
        entry.has_usage = true;
        entry.used_bytes = fs_used;
        entry.avail_bytes = fs_free;
      }

      entries.push_back(entry);
    }
  }

  std::sort(entries.begin(), entries.end(), [](const LsblkEntry& lhs, const LsblkEntry& rhs) {
    return lhs.address < rhs.address;
  });

  for (const auto mount : {remote::FsMount::T41, remote::FsMount::T41Sdcard}) {
    remote::FsMountSnapshot snap {};
    remote::fs_snapshot(mount, &snap);
    LsblkEntry entry {};
    entry.name = snap.provider;
    entry.type = "remote";
    entry.subtype = snap.name;
    entry.fstype = mount == remote::FsMount::T41Sdcard ? "sdcard" : "remote-mshell";
    entry.mount = snap.mounted ? snap.root : "-";
    entry.role = snap.writable ? "rw" : "ro";
    entry.detail = snap.mounted ? snap.root : snap.peer_status;
    entry.size_bytes = 0U;
    entry.address = 0xFFFFFFFFU;
    entries.push_back(entry);
  }

  shell_printf(
      ctx.state,
      "%-14s %-5s %-10s %-11s %10s %10s %10s %-12s %-16s %s\n",
      "NAME",
      "TYPE",
      "SUBTYPE",
      "OFFSET",
      "SIZE",
      "USED",
      "AVAIL",
      "FSTYPE",
      "ROLE",
      "MOUNT/DETAIL");

  if (entries.empty()) {
    shell_write_line(ctx.state, "lsblk: no matching partitions found");
  } else {
    for (const LsblkEntry& entry : entries) {
      std::string mount_or_detail = entry.mount;
      if (entry.mount == "-" && entry.detail != "-") {
        mount_or_detail = entry.detail;
      }
      shell_printf(
          ctx.state,
          "%-14s %-5s %-10s %-11s %10s %10s %10s %-12s %-16s %s\n",
          entry.name.c_str(),
          entry.type.c_str(),
          entry.subtype.c_str(),
          format_hex32(entry.address).c_str(),
          format_size(entry.size_bytes, flags.human_readable).c_str(),
          entry.has_usage ? format_size(entry.used_bytes, flags.human_readable).c_str() : "-",
          entry.has_usage ? format_size(entry.avail_bytes, flags.human_readable).c_str() : "-",
          entry.fstype.c_str(),
          entry.role.c_str(),
          mount_or_detail.c_str());
    }
  }

  shell_write_line(ctx.state, "supported fs: littlefs, spiffs, fat16/fat32; app details show valid image version when readable");
  return 0;
}

}  // namespace mros::shell
