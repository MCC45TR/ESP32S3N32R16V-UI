#include "update_runtime.h"

#include <mros_update_shared.h>

#include "src/drivers/storage/logger_driver.h"
#include "src/drivers/utils/mros_console.h"
#include "src/platform/mros_ota.h"
#include "src/platform/mros_system.h"
#include "src/platform/mros_time.h"

#include <esp_app_desc.h>
#include <esp_partition.h>
#include <esp_rom_sys.h>

#include <string>

namespace mros::update {
namespace {

constexpr const char* kMountPoint = "/littlefs";

bool g_monitor_active = false;
bool g_app_ready = false;
unsigned long g_app_ready_ms = 0UL;
UpdateBootGuard g_guard {};

bool recovery_partition_has_valid_image(const esp_partition_t* partition) {
  return mros::platform::mros_ota_partition_has_valid_app(partition);
}

void log_boot_guard(const char* message) {
  if (message != nullptr) {
    esp_rom_printf("%s\n", message);
  }
}

void update_manifest_state_if_possible(const char* state) {
  if (!logger_storage_ready() || state == nullptr) {
    return;
  }

  UpdateManifest manifest {};
  std::string error;
  if (!load_manifest_from_mount(kMountPoint, &manifest, &error)) {
    return;
  }
  manifest.state = state;
  if (std::string(state) == "confirmed") {
    manifest.attempt = manifest.attempt + 1U;
  }
  if (!save_manifest_to_mount(kMountPoint, manifest, &error)) {
    mros_console.printf("[UPDATE] Manifest state save failed: %s\n", error.c_str());
  }
}

bool boot_recovery_now(const char* reason) {
  const esp_partition_t* recovery =
      mros::platform::mros_ota_find_app_partition(kRecoveryLabel);
  if (!recovery_partition_has_valid_image(recovery)) {
    mros_console.printf("[UPDATE] Recovery boot requested but no valid recovery image is present (%s)\n",
                        reason != nullptr ? reason : "unknown");
    return false;
  }

  const esp_err_t err = mros::platform::mros_ota_set_boot_partition(recovery);
  if (err != ESP_OK) {
    mros_console.printf("[UPDATE] Failed to select recovery partition: %s\n",
                        esp_err_to_name(err));
    return false;
  }
  mros_console.printf("[UPDATE] Rebooting into recovery (%s)\n",
                      reason != nullptr ? reason : "requested");
  mros::platform::mros_delay_ms(50);
  mros::platform::mros_system_restart();
  return true;
}

void arm_rollback_and_reboot(const char* reason) {
  std::string error;
  g_guard.armed = true;
  g_guard.state = "rollback_pending";
  if (!save_boot_guard(g_guard, &error)) {
    mros_console.printf("[UPDATE] Failed to persist rollback guard: %s\n", error.c_str());
  }
  update_manifest_state_if_possible("rollback_pending");
  (void)boot_recovery_now(reason);
}

}  // namespace

void update_runtime_boot_guard() {
  g_monitor_active = false;
  g_app_ready = false;
  g_app_ready_ms = 0UL;
  g_guard = {};

  std::string error;
  UpdateBootGuard guard {};
  if (!load_boot_guard(&guard, &error)) {
    log_boot_guard("[UPDATE] Boot guard unavailable, continuing normal boot");
    return;
  }
  if (!guard.armed || guard.state != "pending_healthcheck") {
    return;
  }

  if (guard.max_boots == 0U) {
    guard.max_boots = kDefaultMaxBoots;
  }
  if (guard.confirm_timeout_sec == 0U) {
    guard.confirm_timeout_sec = kDefaultConfirmTimeoutSec;
  }

  guard.boot_count += 1U;
  if (!save_boot_guard(guard, &error)) {
    log_boot_guard("[UPDATE] Failed to update boot guard counter");
    return;
  }

  g_guard = guard;
  g_monitor_active = true;

  char buffer[128] = {};
  snprintf(buffer,
           sizeof(buffer),
           "[UPDATE] Health guard boot %lu/%lu",
           static_cast<unsigned long>(guard.boot_count),
           static_cast<unsigned long>(guard.max_boots));
  log_boot_guard(buffer);

  if (guard.boot_count > guard.max_boots) {
    g_guard.state = "rollback_pending";
    if (!save_boot_guard(g_guard, &error)) {
      log_boot_guard("[UPDATE] Failed to arm rollback during early boot");
    }
    const esp_partition_t* recovery =
        mros::platform::mros_ota_find_app_partition(kRecoveryLabel);
    if (recovery_partition_has_valid_image(recovery) &&
        mros::platform::mros_ota_set_boot_partition(recovery) == ESP_OK) {
      log_boot_guard("[UPDATE] Health guard exceeded limit, rebooting to recovery");
      mros::platform::mros_system_restart();
    }
  }
}

void update_runtime_mark_app_ready() {
  if (!g_monitor_active) {
    return;
  }
  g_app_ready = true;
  g_app_ready_ms = mros::platform::mros_millis();
  mros_console.printf("[UPDATE] Health confirmation window started (%lus timeout)\n",
                      static_cast<unsigned long>(g_guard.confirm_timeout_sec));
}

void update_runtime_process() {
  if (!g_monitor_active || !g_app_ready) {
    return;
  }

  const unsigned long elapsed_ms =
      mros::platform::mros_millis() - g_app_ready_ms;
  const unsigned long confirm_timeout_ms =
      static_cast<unsigned long>(g_guard.confirm_timeout_sec) * 1000UL;
  if (confirm_timeout_ms > 0UL && elapsed_ms >= confirm_timeout_ms) {
    arm_rollback_and_reboot("health timeout");
    return;
  }

  const unsigned long health_delay_ms = kHealthConfirmDelaySec * 1000UL;
  if (elapsed_ms < health_delay_ms || !logger_storage_ready()) {
    return;
  }

  std::string error;
  if (!clear_boot_guard(&error)) {
    mros_console.printf("[UPDATE] Failed to clear health guard: %s\n", error.c_str());
    return;
  }

  update_manifest_state_if_possible("confirmed");
  g_monitor_active = false;
  g_app_ready = false;
  g_guard = {};
  mros_console.println("[UPDATE] Firmware health confirmed.");
}

}  // namespace mros::update
