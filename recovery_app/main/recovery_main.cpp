#include <mros_update_shared.h>

#include "../../src/config/production_security.h"
#include "recovery_web.h"

extern "C" {
#include <esp_littlefs.h>
}

#include <esp_app_desc.h>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/md.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {

constexpr const char* kTag = "recovery";
constexpr const char* kMountPoint = "/littlefs";
constexpr size_t kCopyChunkBytes = 4096U;
constexpr mbedtls_md_type_t kManifestDigestType = MBEDTLS_MD_SHA256;
constexpr uint32_t kIdleAutobootDelayMs = 15000U;

const esp_partition_t* g_app0_partition = nullptr;
bool g_littlefs_mounted = false;

std::string digest_to_hex(const uint8_t* data, size_t size) {
  std::string out;
  out.reserve(size * 2U);
  char byte_hex[3] = {};
  for (size_t i = 0; i < size; ++i) {
    std::snprintf(byte_hex, sizeof(byte_hex), "%02x", data[i]);
    out.append(byte_hex);
  }
  return out;
}

const std::string& manifest_digest_hex(const mros::update::UpdateManifest& manifest) {
  // Keep schema compatibility: upstream manifest currently stores digest in sha256.
  return manifest.sha256;
}

void set_guard_candidate_digest(mros::update::UpdateBootGuard* guard, const std::string& digest_hex) {
  if (guard == nullptr) {
    return;
  }
  // Keep schema compatibility: boot guard currently stores digest in candidate_sha256.
  guard->candidate_sha256 = digest_hex;
}

const esp_partition_t* find_app_partition(const char* label) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
}

bool partition_has_valid_app(const esp_partition_t* partition) {
  if (partition == nullptr) {
    return false;
  }
  esp_app_desc_t desc {};
  return esp_ota_get_partition_description(partition, &desc) == ESP_OK;
}

std::string sanitize_token(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-') {
      out.push_back(ch);
    } else {
      out.push_back('_');
    }
  }
  return out.empty() ? "latest" : out;
}

bool boot_partition_and_restart(const esp_partition_t* partition, const char* why) {
  if (partition == nullptr) {
    ESP_LOGE(kTag, "Cannot boot null partition (%s)", why != nullptr ? why : "unknown");
    return false;
  }

  const esp_err_t err = esp_ota_set_boot_partition(partition);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "Failed to select boot partition '%s': %s",
             partition->label, esp_err_to_name(err));
    return false;
  }

  ESP_LOGI(kTag, "Rebooting to %s (%s)", partition->label, why != nullptr ? why : "next stage");
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
  return true;
}

bool mount_littlefs() {
  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "littlefs");
  const char* label = "littlefs";

  if (partition == nullptr) {
    partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, nullptr);
    if (partition != nullptr) {
      label = partition->label;
    }
  }
  if (partition == nullptr || label == nullptr) {
    ESP_LOGE(kTag, "LittleFS partition not found");
    return false;
  }

  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = kMountPoint;
  conf.partition_label = label;
  conf.format_if_mount_failed = false;
  conf.dont_mount = false;

  const esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "LittleFS mount failed on '%s': %s", label, esp_err_to_name(err));
    return false;
  }

  size_t total = 0U;
  size_t used = 0U;
  if (esp_littlefs_info(label, &total, &used) == ESP_OK) {
    ESP_LOGI(kTag, "LittleFS mounted on '%s' (%u/%u bytes used)",
             label,
             static_cast<unsigned>(used),
             static_cast<unsigned>(total));
  }
  return true;
}

bool copy_partition_to_file(const esp_partition_t* partition, const std::string& path) {
  if (partition == nullptr) {
    return false;
  }

  mros::recovery::web_set_phase("installing", "backup", "Creating app0 backup", 5, -1, true);
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    ESP_LOGE(kTag, "Backup open failed: %s", path.c_str());
    return false;
  }

  std::vector<uint8_t> buffer(kCopyChunkBytes, 0U);
  const size_t partition_size = static_cast<size_t>(partition->size);
  for (size_t offset = 0U; offset < partition_size; offset += buffer.size()) {
    const size_t chunk = std::min(buffer.size(), partition_size - offset);
    const esp_err_t err = esp_partition_read(partition, offset, buffer.data(), chunk);
    if (err != ESP_OK) {
      std::fclose(file);
      ESP_LOGE(kTag, "Partition read failed at 0x%zx: %s", offset, esp_err_to_name(err));
      return false;
    }
    if (std::fwrite(buffer.data(), 1U, chunk, file) != chunk) {
      std::fclose(file);
      ESP_LOGE(kTag, "Backup write failed at 0x%zx", offset);
      return false;
    }
    const uint32_t progress =
        5U + static_cast<uint32_t>((static_cast<uint64_t>(offset + chunk) * 20ULL) /
                                   static_cast<uint64_t>(partition_size));
    mros::recovery::web_set_phase("installing", "backup", "Creating app0 backup", progress, -1, true);
  }

  std::fclose(file);
  return true;
}

bool restore_file_to_partition(const std::string& path, const esp_partition_t* partition) {
  if (partition == nullptr) {
    return false;
  }

  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    ESP_LOGE(kTag, "Backup file missing: %s", path.c_str());
    return false;
  }
  if (static_cast<size_t>(info.st_size) > partition->size) {
    ESP_LOGE(kTag, "Backup file too large for partition: %s", path.c_str());
    return false;
  }

  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    ESP_LOGE(kTag, "Backup restore open failed: %s", path.c_str());
    return false;
  }

  esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
  if (err != ESP_OK) {
    std::fclose(file);
    ESP_LOGE(kTag, "Partition erase failed before restore: %s", esp_err_to_name(err));
    return false;
  }

  std::vector<uint8_t> buffer(kCopyChunkBytes, 0U);
  size_t offset = 0U;
  while (true) {
    const size_t read_size = std::fread(buffer.data(), 1U, buffer.size(), file);
    if (read_size > 0U) {
      err = esp_partition_write(partition, offset, buffer.data(), read_size);
      if (err != ESP_OK) {
        std::fclose(file);
        ESP_LOGE(kTag, "Partition write failed at 0x%zx: %s", offset, esp_err_to_name(err));
        return false;
      }
      offset += read_size;
    }
    if (read_size < buffer.size()) {
      if (std::ferror(file) != 0) {
        std::fclose(file);
        ESP_LOGE(kTag, "Backup file read failed");
        return false;
      }
      break;
    }
  }

  std::fclose(file);
  return partition_has_valid_app(partition);
}

bool install_file_to_partition(const std::string& path,
                               const esp_partition_t* partition,
                               const std::string& expected_digest_hex) {
  if (partition == nullptr) {
    return false;
  }

  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    ESP_LOGE(kTag, "Target firmware open failed: %s", path.c_str());
    return false;
  }
  struct stat file_info {};
  const uint64_t file_size = (::stat(path.c_str(), &file_info) == 0 && file_info.st_size > 0)
                                 ? static_cast<uint64_t>(file_info.st_size)
                                 : 0ULL;

  esp_ota_handle_t ota_handle = 0;
  esp_err_t err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &ota_handle);
  if (err != ESP_OK) {
    std::fclose(file);
    ESP_LOGE(kTag, "esp_ota_begin failed: %s", esp_err_to_name(err));
    return false;
  }

  mbedtls_md_context_t md_ctx;
  mbedtls_md_init(&md_ctx);

  bool ok = true;
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(kManifestDigestType);
  if (md_info == nullptr) {
    ESP_LOGE(kTag, "mbedtls_md_info_from_type failed");
    ok = false;
  }
  if (ok) {
    const int md_ret = mbedtls_md_setup(&md_ctx, md_info, 0);
    if (md_ret != 0) {
      ESP_LOGE(kTag, "mbedtls_md_setup failed: %d", md_ret);
      ok = false;
    }
  }
  if (ok) {
    const int md_ret = mbedtls_md_starts(&md_ctx);
    if (md_ret != 0) {
      ESP_LOGE(kTag, "mbedtls_md_starts failed: %d", md_ret);
      ok = false;
    }
  }

  std::vector<uint8_t> buffer(kCopyChunkBytes, 0U);
  while (ok) {
    const size_t read_size = std::fread(buffer.data(), 1U, buffer.size(), file);
    if (read_size > 0U) {
      const int md_ret = mbedtls_md_update(&md_ctx, buffer.data(), read_size);
      if (md_ret != 0) {
        ESP_LOGE(kTag, "mbedtls_md_update failed: %d", md_ret);
        ok = false;
      }
      if (ok) {
        err = esp_ota_write(ota_handle, buffer.data(), read_size);
        if (err != ESP_OK) {
          ESP_LOGE(kTag, "esp_ota_write failed: %s", esp_err_to_name(err));
          ok = false;
        }
      }
      if (file_size > 0ULL) {
        const long pos = std::ftell(file);
        const uint64_t done = pos > 0 ? static_cast<uint64_t>(pos) : 0ULL;
        const uint32_t progress =
            30U + static_cast<uint32_t>((std::min<uint64_t>(done, file_size) * 60ULL) / file_size);
        const int32_t eta = progress >= 90U ? 2 : static_cast<int32_t>((90U - progress) / 2U + 2U);
        mros::recovery::web_set_phase("installing", "write", "Writing new firmware to app0", progress, eta, true);
      }
    }
    if (read_size < buffer.size()) {
      if (std::ferror(file) != 0) {
        ESP_LOGE(kTag, "Firmware read failed");
        ok = false;
      }
      break;
    }
  }

  std::fclose(file);

  std::string digest_hex;
  if (ok) {
    const unsigned char digest_size = mbedtls_md_get_size(md_info);
    if (digest_size == 0U) {
      ESP_LOGE(kTag, "mbedtls_md_get_size returned zero");
      ok = false;
    } else {
      std::vector<uint8_t> digest(digest_size, 0U);
      const int md_ret = mbedtls_md_finish(&md_ctx, digest.data());
      if (md_ret != 0) {
        ESP_LOGE(kTag, "mbedtls_md_finish failed: %d", md_ret);
        ok = false;
      } else {
        digest_hex = digest_to_hex(digest.data(), digest.size());
      }
    }
  }
  mbedtls_md_free(&md_ctx);

  if (ok) {
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "esp_ota_end failed: %s", esp_err_to_name(err));
      ok = false;
    }
  } else {
    (void)esp_ota_abort(ota_handle);
  }

  if (ok && !expected_digest_hex.empty() && expected_digest_hex != digest_hex) {
    ESP_LOGE(kTag, "Digest mismatch: expected %s got %s",
             expected_digest_hex.c_str(), digest_hex.c_str());
    mros::recovery::web_set_phase("failed", "verify", "Firmware digest mismatch", 90, -1, false);
    ok = false;
  }

  if (ok && !partition_has_valid_app(partition)) {
    ESP_LOGE(kTag, "Installed partition has no valid app description");
    mros::recovery::web_set_phase("failed", "verify", "Installed image has no valid app description", 90, -1, false);
    ok = false;
  }
  return ok;
}

bool persist_manifest(const mros::update::UpdateManifest& manifest) {
  std::string error;
  if (!mros::update::save_manifest_to_mount(kMountPoint, manifest, &error)) {
    ESP_LOGE(kTag, "Manifest save failed: %s", error.c_str());
    return false;
  }
  return true;
}

bool persist_guard(const mros::update::UpdateBootGuard& guard) {
  std::string error;
  if (!mros::update::save_boot_guard(guard, &error)) {
    ESP_LOGE(kTag, "Guard save failed: %s", error.c_str());
    return false;
  }
  return true;
}

bool clear_guard() {
  std::string error;
  if (!mros::update::clear_boot_guard(&error)) {
    ESP_LOGE(kTag, "Guard clear failed: %s", error.c_str());
    return false;
  }
  return true;
}

std::string build_backup_path(const mros::update::UpdateManifest& manifest) {
  const std::string& digest_hex = manifest_digest_hex(manifest);
  const std::string token = sanitize_token(
      manifest.version.empty() ? digest_hex.substr(0U, std::min<size_t>(8U, digest_hex.size()))
                               : manifest.version);
  return mros::update::storage_actual_path(
      kMountPoint,
      (std::string(mros::update::kPreviousRootRelativePath) + "/app0_backup_" + token + ".bin").c_str());
}

bool restore_backup_and_boot(mros::update::UpdateManifest* manifest,
                             mros::update::UpdateBootGuard* guard,
                             const esp_partition_t* app0) {
  if (manifest == nullptr || guard == nullptr) {
    return false;
  }

  const std::string backup_path =
      !guard->backup_path.empty() ? guard->backup_path : manifest->backup_path;
  if (backup_path.empty()) {
    ESP_LOGE(kTag, "Rollback requested but no backup path is recorded");
    return false;
  }

  manifest->backup_path = backup_path;
  manifest->state = "rolling_back";
  (void)persist_manifest(*manifest);

  if (!restore_file_to_partition(backup_path, app0)) {
    ESP_LOGE(kTag, "Rollback restore failed from %s", backup_path.c_str());
    manifest->state = "rollback_pending";
    (void)persist_manifest(*manifest);
    return false;
  }

  guard->armed = false;
  guard->state = "rolled_back";
  guard->boot_count = 0U;
  (void)persist_guard(*guard);
  (void)clear_guard();

  manifest->state = "rolled_back";
  (void)persist_manifest(*manifest);
  return boot_partition_and_restart(app0, "rollback complete");
}

bool handle_pending_install(mros::update::UpdateManifest* manifest,
                            const esp_partition_t* app0) {
  if (manifest == nullptr || app0 == nullptr) {
    return false;
  }

  std::string signature_error;
  if (!mros::update::verify_manifest_signature(*manifest, &signature_error)) {
    ESP_LOGE(kTag, "Manifest signature verification failed: %s", signature_error.c_str());
    manifest->state = "signature_rejected";
    (void)persist_manifest(*manifest);
    mros::recovery::web_set_phase("failed", "signature", "Manifest signature rejected", 0, -1, false);
    return false;
  }

  mros::recovery::web_set_phase("installing", "manifest", "Pending install manifest accepted", 2, -1, true);
  manifest->state = "installing";
  if (!persist_manifest(*manifest)) {
    mros::recovery::web_set_phase("failed", "manifest", "Manifest save failed", 0, -1, false);
    return false;
  }

  if (manifest->backup_path.empty()) {
    manifest->backup_path = build_backup_path(*manifest);
  }

  if (!copy_partition_to_file(app0, manifest->backup_path)) {
    ESP_LOGE(kTag, "Backup creation failed");
    manifest->state = "failed";
    (void)persist_manifest(*manifest);
    mros::recovery::web_set_phase("failed", "backup", "Backup creation failed", 0, -1, false);
    return false;
  }
  if (!persist_manifest(*manifest)) {
    mros::recovery::web_set_phase("failed", "manifest", "Manifest save failed after backup", 25, -1, false);
    return false;
  }

  const std::string expected_digest_hex = manifest_digest_hex(*manifest);

  mros::recovery::web_set_phase("installing", "write", "Installing firmware to app0", 30, -1, true);
  if (!install_file_to_partition(manifest->target, app0, expected_digest_hex)) {
    ESP_LOGE(kTag, "Install failed, restoring backup");
    mros::update::UpdateBootGuard rollback_guard {};
    rollback_guard.armed = true;
    rollback_guard.state = "rollback_pending";
    set_guard_candidate_digest(&rollback_guard, expected_digest_hex);
    rollback_guard.backup_path = manifest->backup_path;
    rollback_guard.boot_count = 0U;
    rollback_guard.max_boots = manifest->max_boots == 0U ? mros::update::kDefaultMaxBoots : manifest->max_boots;
    rollback_guard.confirm_timeout_sec =
        manifest->confirm_timeout_sec == 0U ? mros::update::kDefaultConfirmTimeoutSec : manifest->confirm_timeout_sec;
    (void)persist_guard(rollback_guard);
    manifest->state = "rollback_pending";
    (void)persist_manifest(*manifest);
    mros::recovery::web_set_phase("rollback", "restore", "Install failed; restoring backup", 35, -1, true);
    return restore_backup_and_boot(manifest, &rollback_guard, app0);
  }

  mros::update::UpdateBootGuard guard {};
  guard.armed = true;
  guard.state = "pending_healthcheck";
  set_guard_candidate_digest(&guard, expected_digest_hex);
  guard.backup_path = manifest->backup_path;
  guard.boot_count = 0U;
  guard.max_boots = manifest->max_boots == 0U ? mros::update::kDefaultMaxBoots : manifest->max_boots;
  guard.confirm_timeout_sec =
      manifest->confirm_timeout_sec == 0U ? mros::update::kDefaultConfirmTimeoutSec : manifest->confirm_timeout_sec;

  if (!persist_guard(guard)) {
    manifest->state = "failed";
    (void)persist_manifest(*manifest);
    mros::recovery::web_set_phase("failed", "guard", "Boot guard save failed", 92, -1, false);
    return false;
  }

  manifest->state = "pending_healthcheck";
  if (!persist_manifest(*manifest)) {
    mros::recovery::web_set_phase("failed", "manifest", "Manifest save failed before boot", 94, -1, false);
    return false;
  }
  mros::recovery::web_set_confirmation("waiting-healthcheck");
  mros::recovery::web_set_phase("ready", "verify", "Install complete; rebooting to app0 healthcheck", 100, 0, false);
  return boot_partition_and_restart(app0, "install complete");
}

void idle_forever() {
  while (true) {
    ESP_LOGI(kTag, "Recovery idle - waiting for manual intervention");
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void recovery_worker_task(void*) {
  const esp_partition_t* app0 = g_app0_partition;
  if (app0 == nullptr) {
    mros::recovery::web_set_phase("failed", "partition", "Target app0 partition is missing", 0, -1, false);
    idle_forever();
  }

  const bool fs_ok = g_littlefs_mounted;

  mros::update::UpdateBootGuard guard {};
  std::string error;
  (void)mros::update::load_boot_guard(&guard, &error);

  mros::update::UpdateManifest manifest {};
  const bool manifest_ok = fs_ok && mros::update::load_manifest_from_mount(kMountPoint, &manifest, &error);

  if (guard.armed && guard.state == "rollback_pending") {
    ESP_LOGW(kTag, "Rollback guard is armed, restoring backup");
    mros::recovery::web_set_phase("rollback", "restore", "Rollback guard is armed; restoring backup", 15, -1, true);
    if (!manifest_ok) {
      manifest.state = "rollback_pending";
      manifest.backup_path = guard.backup_path;
    }
    if (restore_backup_and_boot(&manifest, &guard, app0)) {
      vTaskDelete(nullptr);
      return;
    }
    mros::recovery::web_set_phase("failed", "restore", "Rollback restore failed", 0, -1, false);
    idle_forever();
  }

  if (manifest_ok && manifest.state == "pending_install") {
    ESP_LOGI(kTag, "Pending install detected: %s", manifest.target.c_str());
    if (handle_pending_install(&manifest, app0)) {
      vTaskDelete(nullptr);
      return;
    }
    if (partition_has_valid_app(app0)) {
      mros::recovery::web_set_phase("fallback", "boot", "Install failed; booting previous valid app0", 0, 2, false);
      (void)boot_partition_and_restart(app0, "install failed fallback");
    }
    idle_forever();
  }

  if (partition_has_valid_app(app0)) {
    ESP_LOGI(kTag, "No recovery work pending, recovery portal idle");
    mros::recovery::web_set_confirmation("not-required");
    mros::recovery::web_set_phase("idle", "portal", "No pending work; app0 is valid", 0, kIdleAutobootDelayMs / 1000U, false);
    const TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(kIdleAutobootDelayMs)) {
      if (mros::recovery::web_client_seen()) {
        mros::recovery::web_set_phase("idle", "portal", "Operator connected; automatic app0 boot paused", 0, -1, false);
        idle_forever();
      }
      const uint32_t elapsed_ms = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
      const uint32_t remaining_ms = elapsed_ms >= kIdleAutobootDelayMs ? 0U : (kIdleAutobootDelayMs - elapsed_ms);
      mros::recovery::web_set_phase("idle",
                                    "portal",
                                    "No pending work; app0 auto boot countdown",
                                    0,
                                    static_cast<int32_t>(remaining_ms / 1000U),
                                    false);
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
    (void)boot_partition_and_restart(app0, "no pending work");
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGE(kTag, "No valid app0 image is available to boot");
  mros::recovery::web_set_phase("manual", "portal", "No valid app0 image; upload firmware manually", 0, -1, false);
  idle_forever();
}

}  // namespace

extern "C" void app_main(void) {
  ESP_LOGI(kTag, "Recovery updater booting");

  g_app0_partition = find_app_partition(mros::update::kAppLabel);
  g_littlefs_mounted = mount_littlefs();
  if (g_littlefs_mounted) {
    std::string dir_error;
    if (!mros::update::ensure_update_dirs(kMountPoint, &dir_error)) {
      ESP_LOGW(kTag, "Update directories could not be prepared: %s", dir_error.c_str());
    }
  }
  mros::recovery::web_set_filesystem_ready(g_littlefs_mounted);
  mros::recovery::web_start(g_app0_partition);
  if (g_app0_partition == nullptr) {
    ESP_LOGE(kTag, "Target app0 partition is missing");
    idle_forever();
  }

  const BaseType_t task_ok = xTaskCreatePinnedToCore(
      recovery_worker_task,
      "rec_worker",
      8192,
      nullptr,
      10,
      nullptr,
#if CONFIG_FREERTOS_UNICORE
      0
#else
      1
#endif
  );
  if (task_ok != pdPASS) {
    ESP_LOGE(kTag, "Recovery worker task creation failed");
    mros::recovery::web_set_phase("failed", "rtos", "Recovery worker task creation failed", 0, -1, false);
  }
  idle_forever();
}
