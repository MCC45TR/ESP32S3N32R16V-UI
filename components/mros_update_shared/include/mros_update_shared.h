#pragma once

#include <cstdint>
#include <string>

namespace mros::update {

constexpr const char* kRecoveryLabel = "recovery";
constexpr const char* kAppLabel = "app0";
constexpr const char* kManifestRelativePath = "ESPUSER/firmware/update.json";
constexpr const char* kFirmwareRootRelativePath = "ESPUSER/firmware";
constexpr const char* kCurrentRootRelativePath = "ESPUSER/firmware/current";
constexpr const char* kPreviousRootRelativePath = "ESPUSER/firmware/previous";
constexpr const char* kInboxRootRelativePath = "ESPUSER/firmware/inbox";
constexpr const char* kStagingRootRelativePath = "ESPUSER/firmware/staging";
constexpr const char* kUserNvsPartition = "nvs_sys_usr";
constexpr const char* kLegacyUserNvsPartition = "nvs_usr";
constexpr const char* kBootGuardNamespace = "sys_update";
constexpr const char* kManifestSignatureAlgorithm = "hmac-sha256";
constexpr uint32_t kDefaultMaxAttempts = 2U;
constexpr uint32_t kDefaultMaxBoots = 2U;
constexpr uint32_t kDefaultConfirmTimeoutSec = 30U;
constexpr uint32_t kHealthConfirmDelaySec = 20U;

struct UpdateManifest {
  std::string state;
  std::string target;
  std::string target_display;
  std::string backup_path;
  std::string sha256;
  uint64_t size = 0U;
  std::string version;
  std::string target_partition = kAppLabel;
  std::string recovery_partition = kRecoveryLabel;
  std::string running_partition;
  std::string signature_algorithm;
  std::string signature;
  std::string signed_by;
  uint32_t attempt = 0U;
  uint32_t max_attempts = kDefaultMaxAttempts;
  uint32_t max_boots = kDefaultMaxBoots;
  uint32_t confirm_timeout_sec = kDefaultConfirmTimeoutSec;
};

struct UpdateBootGuard {
  bool armed = false;
  std::string state;
  std::string candidate_sha256;
  std::string backup_path;
  uint32_t boot_count = 0U;
  uint32_t max_boots = kDefaultMaxBoots;
  uint32_t confirm_timeout_sec = kDefaultConfirmTimeoutSec;
};

std::string storage_actual_path(const char* mount_point, const char* relative_path);
std::string manifest_path_for_mount(const char* mount_point);
bool ensure_update_dirs(const char* mount_point, std::string* error = nullptr);

bool load_manifest(const char* manifest_path, UpdateManifest* out_manifest, std::string* error = nullptr);
bool save_manifest(const char* manifest_path, const UpdateManifest& manifest, std::string* error = nullptr);
bool load_manifest_from_mount(const char* mount_point, UpdateManifest* out_manifest, std::string* error = nullptr);
bool save_manifest_to_mount(const char* mount_point, const UpdateManifest& manifest, std::string* error = nullptr);
bool manifest_signature_key_available();
std::string manifest_canonical_payload(const UpdateManifest& manifest);
bool sign_manifest(UpdateManifest* manifest, std::string* error = nullptr);
bool verify_manifest_signature(const UpdateManifest& manifest, std::string* error = nullptr);

bool load_boot_guard(UpdateBootGuard* out_guard, std::string* error = nullptr);
bool save_boot_guard(const UpdateBootGuard& guard, std::string* error = nullptr);
bool clear_boot_guard(std::string* error = nullptr);

}  // namespace mros::update
