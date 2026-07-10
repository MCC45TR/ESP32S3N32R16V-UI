#include "src/shell/mros_shell_internal.h"

#include <mros_update_shared.h>

#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <string>

#include "src/drivers/storage/logger_driver.h"
#include "src/platform/mros_ota.h"
#include "src/web/server/wifi_manager.h"

namespace mros::shell {
namespace {

constexpr const char* kRecoveryLabel = "recovery";
constexpr const char* kAppLabel = "app0";
constexpr const char* kUserNvsLabel = "nvs_sys_usr";
constexpr const char* kManifestMountPoint = "/littlefs";

bool is_help_arg(const std::string& arg) {
  return arg == "--help" || arg == "-h";
}

const char* safe_label(const esp_partition_t* partition) {
  return partition != nullptr ? partition->label : "(none)";
}

const char* safe_type(const esp_partition_t* partition) {
  if (partition == nullptr) {
    return "(none)";
  }
  if (partition->type == ESP_PARTITION_TYPE_APP) {
    return "app";
  }
  if (partition->type == ESP_PARTITION_TYPE_DATA) {
    return "data";
  }
  return "unknown";
}

const esp_partition_t* find_app_partition(const char* label) {
  return mros::platform::mros_ota_find_app_partition(label);
}

const esp_partition_t* find_data_partition(const char* label) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
}

bool partition_has_valid_app(const esp_partition_t* partition) {
  return mros::platform::mros_ota_partition_has_valid_app(partition);
}

bool storage_ready() {
  return logger_storage_ready();
}

void print_partition_line(ShellState& state, const char* name, const esp_partition_t* partition) {
  if (partition == nullptr) {
    shell_printf(state, "%-14s : (none)\n", name);
    return;
  }
  shell_printf(state,
               "%-14s : label=%s type=%s subtype=0x%02X offset=0x%06lX size=%luK\n",
               name,
               safe_label(partition),
               safe_type(partition),
               static_cast<unsigned>(partition->subtype),
               static_cast<unsigned long>(partition->address),
               static_cast<unsigned long>(partition->size / 1024U));
}

void print_app_desc(ShellState& state, const char* label, const esp_partition_t* partition) {
  shell_printf(state, "%s image    : ", label);
  if (partition == nullptr) {
    shell_write_line(state, "(partition missing)");
    return;
  }
  esp_app_desc_t desc {};
  if (esp_ota_get_partition_description(partition, &desc) != ESP_OK) {
    shell_write_line(state, "(no valid image)");
    return;
  }
  shell_printf(state, "%s version=%s built=%s %s\n",
               desc.project_name,
               desc.version,
               desc.date,
               desc.time);
}

void print_manifest_state(ShellState& state) {
  const std::string manifest_path =
      mros::update::manifest_path_for_mount(kManifestMountPoint);
  struct stat info {};
  if (::stat(manifest_path.c_str(), &info) != 0) {
    shell_write_line(state, "manifest      : (none)");
    return;
  }
  shell_printf(state, "manifest      : %s (%llu bytes)\n",
               manifest_path.c_str(),
               static_cast<unsigned long long>(info.st_size));
}

void print_plan(ShellState& state) {
  shell_write_line(state, "MROS DEUSCARA recovery updater plan");
  shell_write_line(state, "--------------------------------");
  shell_write_line(state, "1. Normal app runs from app0 (factory).");
  shell_write_line(state, "2. update-system selects MROS_ESP32S3N32R16V_*.BIN from LittleFS.");
  shell_write_line(state, "3. Newest filename wins; preferred format is YYYYMMDD_HHMM.");
  shell_write_line(state, "4. update-system writes /ESPUSER/firmware/update.json with SHA-256.");
  shell_write_line(state, "5. If recovery contains a valid image, next boot is set to recovery.");
  shell_write_line(state, "6. Recovery flashes the selected BIN into app0, verifies, then boots app0.");
  shell_write_line(state, "7. Normal app must confirm health; otherwise recovery can restore backup.");
  shell_write_line(state, "");
  shell_write_line(state, "Recovery idle LED       : purple -> orange -> blue -> green -> wait -> loop");
  shell_write_line(state, "Recovery failure LED    : red blink x5");
  shell_write_line(state, "Recovery success LED    : green -> gap -> green -> gap -> blue");
  shell_write_line(state, "");
  shell_write_line(state, "Future optional web UI  : static page under LittleFS, not enabled yet.");
}

}  // namespace

void shell_help_mros7dofs3_update(ShellState& state) {
  shell_write_line(state, "Usage: mros-deuscara-update <check|status|plan|verify|install|rollback|clear-manifest>");
  shell_write_line(state, "Alias: mros7dofs3-update <...>");
  shell_write_line(state, "Inspect the recovery/app0 based update pipeline.");
  shell_write_line(state, "  check          verify partition prerequisites");
  shell_write_line(state, "  status         show running/boot/recovery/app0 and manifest status");
  shell_write_line(state, "  plan           show the recovery install and LED behavior plan");
  shell_write_line(state, "  verify FILE    check a local firmware file path/size");
  shell_write_line(state, "  install FILE   staged handoff to recovery updater");
  shell_write_line(state, "  rollback       staged recovery rollback command");
  shell_write_line(state, "  clear-manifest remove staged update manifest from LittleFS");
}

int shell_cmd_mros7dofs3_update(ShellContext& ctx) {
  if (ctx.args.size() < 2U || is_help_arg(ctx.args[1])) {
    shell_help_mros7dofs3_update(ctx.state);
    return ctx.args.size() < 2U ? 1 : 0;
  }

  const std::string& sub = ctx.args[1];
  if (sub == "status") {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* recovery = find_app_partition(kRecoveryLabel);
    const esp_partition_t* app0 = find_app_partition(kAppLabel);
    const esp_partition_t* otadata = find_data_partition("otadata");
    const esp_partition_t* nvs_sys_usr = find_data_partition(kUserNvsLabel);
    const esp_partition_t* coredump = find_data_partition("coredump");

    shell_write_line(ctx.state, "MROS DEUSCARA update status");
    shell_write_line(ctx.state, "------------------------");
    print_partition_line(ctx.state, "running", running);
    print_partition_line(ctx.state, "boot", boot);
    print_partition_line(ctx.state, "recovery", recovery);
    print_partition_line(ctx.state, "app0", app0);
    print_partition_line(ctx.state, "otadata", otadata);
    print_partition_line(ctx.state, "nvs_sys_usr", nvs_sys_usr);
    print_partition_line(ctx.state, "coredump", coredump);
    print_app_desc(ctx.state, "running", running);
    print_app_desc(ctx.state, "recovery", recovery);
    print_manifest_state(ctx.state);
    shell_printf(ctx.state, "wifi          : %s\n", wifi_manager_is_connected() ? wifi_manager_ip() : "down");
    shell_printf(ctx.state, "littlefs      : %s\n", storage_ready() ? "mounted" : "not mounted");
    return 0;
  }

  if (sub == "check") {
    const esp_partition_t* recovery = find_app_partition(kRecoveryLabel);
    const esp_partition_t* app0 = find_app_partition(kAppLabel);
    const esp_partition_t* otadata = find_data_partition("otadata");
    const esp_partition_t* nvs_sys_usr = find_data_partition(kUserNvsLabel);
    bool ok = true;

    shell_write_line(ctx.state, "MROS DEUSCARA update check");
    shell_write_line(ctx.state, "-----------------------");
    shell_printf(ctx.state, "recovery partition : %s\n", recovery != nullptr ? "present" : "missing");
    shell_printf(ctx.state, "recovery image     : %s\n", partition_has_valid_app(recovery) ? "valid" : "missing/not flashed");
    shell_printf(ctx.state, "app0 partition     : %s\n", app0 != nullptr ? "present" : "missing");
    shell_printf(ctx.state, "otadata            : %s\n", otadata != nullptr ? "present" : "missing");
    shell_printf(ctx.state, "nvs_sys_usr        : %s\n", nvs_sys_usr != nullptr ? "present" : "missing");
    shell_printf(ctx.state, "littlefs           : %s\n", storage_ready() ? "mounted" : "not mounted");
    ok = ok && recovery != nullptr && app0 != nullptr && otadata != nullptr && nvs_sys_usr != nullptr;
    ok = ok && storage_ready();
    if (!partition_has_valid_app(recovery)) {
      shell_write_line(ctx.state, "note: recovery boot is intentionally blocked until a valid recovery image is flashed");
    }
    return ok ? 0 : 1;
  }

  if (sub == "plan") {
    print_plan(ctx.state);
    return 0;
  }

  if (sub == "verify") {
    if (ctx.args.size() < 3U) {
      shell_write_line(ctx.state, "mros-deuscara-update verify: file required");
      return 1;
    }
    const std::string path = shell_normalize_path(ctx.state, ctx.args[2]);
    bool is_dir = false;
    struct stat info {};
    std::string error;
    if (!shell_path_exists(ctx.state, path, &is_dir, &info, &error) || is_dir) {
      shell_printf(ctx.state, "mros-deuscara-update verify: %s\n", error.empty() ? "file not found" : error.c_str());
      return 1;
    }
    shell_printf(ctx.state, "verify file      : %s\n", path.c_str());
    shell_printf(ctx.state, "size             : %llu bytes\n", static_cast<unsigned long long>(info.st_size));
    shell_write_line(ctx.state, "image header     : staged");
    shell_write_line(ctx.state, "sha256 manifest  : staged");
    return 2;
  }

  if (sub == "install") {
    shell_write_line(ctx.state, "install staged: recovery manifest handoff is not armed from shell yet.");
    shell_write_line(ctx.state, "current safe path: update-system FILE");
    return 2;
  }

  if (sub == "rollback") {
    shell_write_line(ctx.state, "rollback staged: recovery backup/rollback metadata is not active yet.");
    return 2;
  }

  if (sub == "clear-manifest") {
    const std::string manifest_path =
        mros::update::manifest_path_for_mount(kManifestMountPoint);
    if (::remove(manifest_path.c_str()) == 0) {
      shell_write_line(ctx.state, "mros-deuscara-update: manifest removed");
      return 0;
    }
    shell_printf(ctx.state, "mros-deuscara-update: manifest remove failed: %s\n", strerror(errno));
    return 1;
  }

  shell_printf(ctx.state, "mros-deuscara-update: unknown command '%s'\n", sub.c_str());
  return 1;
}

}  // namespace mros::shell
