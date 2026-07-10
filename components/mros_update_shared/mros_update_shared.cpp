#include "mros_update_shared.h"

extern "C" {
#include <cJSON.h>
}

#include <esp_err.h>
#include <mbedtls/md.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <algorithm>
#include <cctype>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

namespace mros::update {
namespace {

#if defined(MROS_PRODUCTION_BUILD) && !defined(MROS_UPDATE_MANIFEST_HMAC_KEY)
#error "Production update builds require MROS_UPDATE_MANIFEST_HMAC_KEY"
#endif

constexpr const char* kGuardKeyArmed = "armed";
constexpr const char* kGuardKeyState = "state";
constexpr const char* kGuardKeyCandidateSha256 = "candidate_sha";
constexpr const char* kGuardKeyBackupPath = "backup_path";
constexpr const char* kGuardKeyBootCount = "boot_count";
constexpr const char* kGuardKeyMaxBoots = "max_boots";
constexpr const char* kGuardKeyConfirmTimeoutSec = "confirm_sec";

void set_error(std::string* error, const std::string& text) {
  if (error != nullptr) {
    *error = text;
  }
}

const char* manifest_hmac_key() {
#ifdef MROS_UPDATE_MANIFEST_HMAC_KEY
  return MROS_UPDATE_MANIFEST_HMAC_KEY;
#else
  return nullptr;
#endif
}

bool hmac_key_usable(const char* key) {
  if (key == nullptr) {
    return false;
  }
  const size_t len = ::strlen(key);
  if (len < 16U || len > 256U) {
    return false;
  }
  return ::strstr(key, "CHANGE_ME") == nullptr &&
         ::strstr(key, "REPLACE") == nullptr &&
         ::strstr(key, "PLACEHOLDER") == nullptr;
}

bool hex_string_len(const std::string& value, const size_t expected_len) {
  if (value.size() != expected_len) {
    return false;
  }
  for (const char ch : value) {
    if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
      return false;
    }
  }
  return true;
}

std::string lower_hex_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool constant_time_equal(const std::string& left, const std::string& right) {
  uint8_t diff = left.size() == right.size() ? 0U : 1U;
  const size_t max_len = left.size() > right.size() ? left.size() : right.size();
  for (size_t i = 0; i < max_len; ++i) {
    const uint8_t a = i < left.size() ? static_cast<uint8_t>(left[i]) : 0U;
    const uint8_t b = i < right.size() ? static_cast<uint8_t>(right[i]) : 0U;
    diff |= static_cast<uint8_t>(a ^ b);
  }
  return diff == 0U;
}

std::string bytes_to_hex(const uint8_t* data, const size_t size) {
  std::string out;
  out.reserve(size * 2U);
  char byte_hex[3] = {};
  for (size_t i = 0; i < size; ++i) {
    std::snprintf(byte_hex, sizeof(byte_hex), "%02x", data[i]);
    out.append(byte_hex);
  }
  return out;
}

void append_canonical_field(std::string* out,
                            const char* name,
                            const std::string& value) {
  if (out == nullptr || name == nullptr) {
    return;
  }
  out->append(name);
  out->push_back(':');
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
  out->push_back('\n');
}

void append_canonical_u64(std::string* out, const char* name, const uint64_t value) {
  append_canonical_field(out, name, std::to_string(value));
}

bool manifest_hmac_hex(const UpdateManifest& manifest,
                       std::string* out_hex,
                       std::string* error) {
  if (out_hex == nullptr) {
    set_error(error, "invalid hmac output");
    return false;
  }
  const char* key = manifest_hmac_key();
  if (!hmac_key_usable(key)) {
    set_error(error, "manifest signing key missing or placeholder");
    return false;
  }
  const std::string payload = manifest_canonical_payload(manifest);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) {
    set_error(error, "sha256 unavailable");
    return false;
  }
  uint8_t digest[32] = {};
  const int rc = mbedtls_md_hmac(info,
                                 reinterpret_cast<const unsigned char*>(key),
                                 ::strlen(key),
                                 reinterpret_cast<const unsigned char*>(payload.data()),
                                 payload.size(),
                                 digest);
  if (rc != 0) {
    set_error(error, "manifest hmac failed");
    return false;
  }
  *out_hex = bytes_to_hex(digest, sizeof(digest));
  return true;
}

bool ensure_dir(const std::string& path) {
  struct stat info {};
  if (::stat(path.c_str(), &info) == 0) {
    return (info.st_mode & S_IFDIR) != 0;
  }
  return ::mkdir(path.c_str(), 0775) == 0 || errno == EEXIST;
}

bool read_file(const char* path, std::string* out_data, std::string* error) {
  if (path == nullptr || out_data == nullptr) {
    set_error(error, "invalid file read args");
    return false;
  }

  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    set_error(error, std::string("open failed: ") + path);
    return false;
  }

  if (std::fseek(file, 0L, SEEK_END) != 0) {
    std::fclose(file);
    set_error(error, "seek end failed");
    return false;
  }
  const long length = std::ftell(file);
  if (length < 0L) {
    std::fclose(file);
    set_error(error, "ftell failed");
    return false;
  }
  if (std::fseek(file, 0L, SEEK_SET) != 0) {
    std::fclose(file);
    set_error(error, "seek set failed");
    return false;
  }

  out_data->assign(static_cast<size_t>(length), '\0');
  if (length > 0L) {
    const size_t read_count =
        std::fread(out_data->data(), 1U, static_cast<size_t>(length), file);
    if (read_count != static_cast<size_t>(length)) {
      std::fclose(file);
      set_error(error, "read failed");
      return false;
    }
  }
  std::fclose(file);
  return true;
}

bool write_file(const char* path, const std::string& data, std::string* error) {
  if (path == nullptr) {
    set_error(error, "invalid file write path");
    return false;
  }

  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    set_error(error, std::string("open for write failed: ") + path);
    return false;
  }

  const bool ok = std::fwrite(data.data(), 1U, data.size(), file) == data.size();
  std::fclose(file);
  if (!ok) {
    set_error(error, std::string("short write: ") + path);
    return false;
  }
  return true;
}

std::string get_json_string(cJSON* root, const char* key, const char* fallback = "") {
  cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return item->valuestring;
  }
  return fallback != nullptr ? fallback : "";
}

uint32_t get_json_u32(cJSON* root, const char* key, const uint32_t fallback) {
  cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (cJSON_IsNumber(item) && item->valuedouble >= 0.0) {
    return static_cast<uint32_t>(item->valuedouble);
  }
  return fallback;
}

uint64_t get_json_u64(cJSON* root, const char* key, const uint64_t fallback) {
  cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (cJSON_IsNumber(item) && item->valuedouble >= 0.0) {
    return static_cast<uint64_t>(item->valuedouble);
  }
  return fallback;
}

esp_err_t init_partition_if_needed(const char* partition_label) {
  if (partition_label != nullptr && partition_label[0] != '\0') {
    return nvs_flash_init_partition(partition_label);
  }
  return nvs_flash_init();
}

bool open_guard_nvs(const bool read_only, nvs_handle_t* out_handle, std::string* error) {
  if (out_handle == nullptr) {
    set_error(error, "nvs handle pointer missing");
    return false;
  }

  const nvs_open_mode_t mode = read_only ? NVS_READONLY : NVS_READWRITE;
  const char* partitions[] = {kUserNvsPartition, kLegacyUserNvsPartition, nullptr};
  esp_err_t last_err = ESP_FAIL;

  for (const char* partition_label : partitions) {
    const esp_err_t init_err = init_partition_if_needed(partition_label);
    if (init_err != ESP_OK && init_err != ESP_ERR_NVS_NO_FREE_PAGES &&
        init_err != ESP_ERR_NVS_NEW_VERSION_FOUND &&
        init_err != ESP_ERR_NVS_PART_NOT_FOUND) {
      last_err = init_err;
      continue;
    }

    esp_err_t open_err = ESP_FAIL;
    if (partition_label != nullptr) {
      if (init_err == ESP_OK) {
        open_err = nvs_open_from_partition(partition_label, kBootGuardNamespace, mode, out_handle);
      } else {
        open_err = init_err;
      }
    } else {
      if (init_err == ESP_OK || init_err == ESP_ERR_NVS_NO_FREE_PAGES ||
          init_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (init_err == ESP_OK) {
          open_err = nvs_open(kBootGuardNamespace, mode, out_handle);
        } else {
          open_err = init_err;
        }
      }
    }

    if (open_err == ESP_OK) {
      return true;
    }
    last_err = open_err;
  }

  set_error(error, std::string("guard nvs open failed: ") + esp_err_to_name(last_err));
  return false;
}

bool set_or_erase_str(nvs_handle_t handle, const char* key, const std::string& value) {
  if (value.empty()) {
    const esp_err_t err = nvs_erase_key(handle, key);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
  }
  return nvs_set_str(handle, key, value.c_str()) == ESP_OK;
}

bool get_nvs_string(nvs_handle_t handle, const char* key, std::string* out_value) {
  if (out_value == nullptr) {
    return false;
  }

  size_t required = 0U;
  esp_err_t err = nvs_get_str(handle, key, nullptr, &required);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    out_value->clear();
    return true;
  }
  if (err != ESP_OK || required == 0U) {
    return false;
  }

  std::vector<char> buffer(required, '\0');
  err = nvs_get_str(handle, key, buffer.data(), &required);
  if (err != ESP_OK) {
    return false;
  }
  out_value->assign(buffer.data());
  return true;
}

}  // namespace

bool manifest_signature_key_available() {
  return hmac_key_usable(manifest_hmac_key());
}

std::string manifest_canonical_payload(const UpdateManifest& manifest) {
  std::string payload;
  payload.reserve(512U);
  append_canonical_field(&payload, "schema", "mros-update-manifest-v1");
  append_canonical_field(&payload, "target", manifest.target);
  append_canonical_field(&payload, "target_display", manifest.target_display);
  append_canonical_field(&payload, "sha256", lower_hex_copy(manifest.sha256));
  append_canonical_u64(&payload, "size", manifest.size);
  append_canonical_field(&payload, "version", manifest.version);
  append_canonical_field(&payload, "target_partition", manifest.target_partition);
  append_canonical_field(&payload, "recovery_partition", manifest.recovery_partition);
  append_canonical_u64(&payload, "max_attempts", manifest.max_attempts);
  append_canonical_u64(&payload, "max_boots", manifest.max_boots);
  append_canonical_u64(&payload, "confirm_timeout_sec", manifest.confirm_timeout_sec);
  return payload;
}

bool sign_manifest(UpdateManifest* manifest, std::string* error) {
  if (manifest == nullptr) {
    set_error(error, "invalid manifest sign args");
    return false;
  }
  std::string signature_hex;
  if (!manifest_hmac_hex(*manifest, &signature_hex, error)) {
    return false;
  }
  manifest->signature_algorithm = kManifestSignatureAlgorithm;
  manifest->signature = signature_hex;
  manifest->signed_by = "device-hmac";
  return true;
}

bool verify_manifest_signature(const UpdateManifest& manifest, std::string* error) {
  if (manifest.signature_algorithm != kManifestSignatureAlgorithm) {
    set_error(error, "manifest signature algorithm missing or unsupported");
    return false;
  }
  if (!hex_string_len(manifest.signature, 64U)) {
    set_error(error, "manifest signature missing or invalid");
    return false;
  }
  std::string expected_hex;
  if (!manifest_hmac_hex(manifest, &expected_hex, error)) {
    return false;
  }
  if (!constant_time_equal(lower_hex_copy(manifest.signature), expected_hex)) {
    set_error(error, "manifest signature mismatch");
    return false;
  }
  return true;
}

std::string storage_actual_path(const char* mount_point, const char* relative_path) {
  const std::string base =
      (mount_point != nullptr && mount_point[0] != '\0') ? mount_point : "/littlefs";
  if (relative_path == nullptr || relative_path[0] == '\0') {
    return base;
  }
  return base + "/" + relative_path;
}

std::string manifest_path_for_mount(const char* mount_point) {
  return storage_actual_path(mount_point, kManifestRelativePath);
}

bool ensure_update_dirs(const char* mount_point, std::string* error) {
  const std::string espuser = storage_actual_path(mount_point, "ESPUSER");
  const std::string firmware = storage_actual_path(mount_point, kFirmwareRootRelativePath);
  const std::string current = storage_actual_path(mount_point, kCurrentRootRelativePath);
  const std::string previous = storage_actual_path(mount_point, kPreviousRootRelativePath);
  const std::string inbox = storage_actual_path(mount_point, kInboxRootRelativePath);
  const std::string staging = storage_actual_path(mount_point, kStagingRootRelativePath);

  if (!ensure_dir(espuser) || !ensure_dir(firmware) || !ensure_dir(current) ||
      !ensure_dir(previous) || !ensure_dir(inbox) || !ensure_dir(staging)) {
    set_error(error, "failed to create update directories");
    return false;
  }
  return true;
}

bool load_manifest(const char* manifest_path, UpdateManifest* out_manifest, std::string* error) {
  if (manifest_path == nullptr || out_manifest == nullptr) {
    set_error(error, "invalid manifest load args");
    return false;
  }

  std::string raw;
  if (!read_file(manifest_path, &raw, error)) {
    return false;
  }

  cJSON* root = cJSON_ParseWithLength(raw.c_str(), raw.size());
  if (root == nullptr) {
    set_error(error, "manifest json parse failed");
    return false;
  }

  UpdateManifest manifest {};
  manifest.state = get_json_string(root, "state", "");
  manifest.target = get_json_string(root, "target", "");
  manifest.target_display = get_json_string(root, "target_display", "");
  manifest.backup_path = get_json_string(root, "backup_path", "");
  if (manifest.backup_path.empty()) {
    manifest.backup_path = get_json_string(root, "backup", "");
  }
  manifest.sha256 = get_json_string(root, "sha256", "");
  manifest.size = get_json_u64(root, "size", 0U);
  manifest.version = get_json_string(root, "version", "");
  manifest.target_partition = get_json_string(root, "target_partition", kAppLabel);
  manifest.recovery_partition = get_json_string(root, "recovery_partition", kRecoveryLabel);
  manifest.running_partition = get_json_string(root, "running_partition", "");
  manifest.signature_algorithm = get_json_string(root, "signature_algorithm", "");
  manifest.signature = get_json_string(root, "signature", "");
  manifest.signed_by = get_json_string(root, "signed_by", "");
  manifest.attempt = get_json_u32(root, "attempt", 0U);
  manifest.max_attempts = get_json_u32(root, "max_attempts", kDefaultMaxAttempts);
  manifest.max_boots = get_json_u32(root, "max_boots", kDefaultMaxBoots);
  manifest.confirm_timeout_sec =
      get_json_u32(root, "confirm_timeout_sec", kDefaultConfirmTimeoutSec);

  cJSON_Delete(root);
  *out_manifest = std::move(manifest);
  return true;
}

bool save_manifest(const char* manifest_path, const UpdateManifest& manifest, std::string* error) {
  if (manifest_path == nullptr) {
    set_error(error, "invalid manifest save path");
    return false;
  }

  cJSON* root = cJSON_CreateObject();
  if (root == nullptr) {
    set_error(error, "manifest root alloc failed");
    return false;
  }

  cJSON_AddStringToObject(root, "state", manifest.state.c_str());
  cJSON_AddStringToObject(root, "target", manifest.target.c_str());
  cJSON_AddStringToObject(root, "target_display", manifest.target_display.c_str());
  cJSON_AddStringToObject(root, "backup_path", manifest.backup_path.c_str());
  cJSON_AddStringToObject(root, "sha256", manifest.sha256.c_str());
  cJSON_AddNumberToObject(root, "size", static_cast<double>(manifest.size));
  cJSON_AddStringToObject(root, "version", manifest.version.c_str());
  cJSON_AddStringToObject(root, "target_partition", manifest.target_partition.c_str());
  cJSON_AddStringToObject(root, "recovery_partition", manifest.recovery_partition.c_str());
  cJSON_AddStringToObject(root, "running_partition", manifest.running_partition.c_str());
  cJSON_AddStringToObject(root, "signature_algorithm", manifest.signature_algorithm.c_str());
  cJSON_AddStringToObject(root, "signature", manifest.signature.c_str());
  cJSON_AddStringToObject(root, "signed_by", manifest.signed_by.c_str());
  cJSON_AddNumberToObject(root, "attempt", manifest.attempt);
  cJSON_AddNumberToObject(root, "max_attempts", manifest.max_attempts);
  cJSON_AddNumberToObject(root, "max_boots", manifest.max_boots);
  cJSON_AddNumberToObject(root, "confirm_timeout_sec", manifest.confirm_timeout_sec);

  char* printed = cJSON_Print(root);
  cJSON_Delete(root);
  if (printed == nullptr) {
    set_error(error, "manifest print failed");
    return false;
  }

  std::string payload(printed);
  cJSON_free(printed);
  payload.push_back('\n');
  return write_file(manifest_path, payload, error);
}

bool load_manifest_from_mount(const char* mount_point, UpdateManifest* out_manifest, std::string* error) {
  const std::string path = manifest_path_for_mount(mount_point);
  return load_manifest(path.c_str(), out_manifest, error);
}

bool save_manifest_to_mount(const char* mount_point, const UpdateManifest& manifest, std::string* error) {
  if (!ensure_update_dirs(mount_point, error)) {
    return false;
  }
  const std::string path = manifest_path_for_mount(mount_point);
  return save_manifest(path.c_str(), manifest, error);
}

bool load_boot_guard(UpdateBootGuard* out_guard, std::string* error) {
  if (out_guard == nullptr) {
    set_error(error, "invalid guard output");
    return false;
  }

  nvs_handle_t handle = 0;
  if (!open_guard_nvs(true, &handle, error)) {
    return false;
  }

  UpdateBootGuard guard {};
  uint8_t armed = 0U;
  uint32_t value = 0U;
  (void)nvs_get_u8(handle, kGuardKeyArmed, &armed);
  guard.armed = armed != 0U;
  (void)get_nvs_string(handle, kGuardKeyState, &guard.state);
  (void)get_nvs_string(handle, kGuardKeyCandidateSha256, &guard.candidate_sha256);
  (void)get_nvs_string(handle, kGuardKeyBackupPath, &guard.backup_path);
  if (nvs_get_u32(handle, kGuardKeyBootCount, &value) == ESP_OK) {
    guard.boot_count = value;
  }
  if (nvs_get_u32(handle, kGuardKeyMaxBoots, &value) == ESP_OK) {
    guard.max_boots = value;
  }
  if (nvs_get_u32(handle, kGuardKeyConfirmTimeoutSec, &value) == ESP_OK) {
    guard.confirm_timeout_sec = value;
  }
  nvs_close(handle);
  *out_guard = std::move(guard);
  return true;
}

bool save_boot_guard(const UpdateBootGuard& guard, std::string* error) {
  nvs_handle_t handle = 0;
  if (!open_guard_nvs(false, &handle, error)) {
    return false;
  }

  bool ok = true;
  ok = ok && nvs_set_u8(handle, kGuardKeyArmed, guard.armed ? 1U : 0U) == ESP_OK;
  ok = ok && set_or_erase_str(handle, kGuardKeyState, guard.state);
  ok = ok && set_or_erase_str(handle, kGuardKeyCandidateSha256, guard.candidate_sha256);
  ok = ok && set_or_erase_str(handle, kGuardKeyBackupPath, guard.backup_path);
  ok = ok && nvs_set_u32(handle, kGuardKeyBootCount, guard.boot_count) == ESP_OK;
  ok = ok && nvs_set_u32(handle, kGuardKeyMaxBoots, guard.max_boots) == ESP_OK;
  ok = ok && nvs_set_u32(handle, kGuardKeyConfirmTimeoutSec, guard.confirm_timeout_sec) == ESP_OK;
  ok = ok && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);

  if (!ok) {
    set_error(error, "save guard failed");
  }
  return ok;
}

bool clear_boot_guard(std::string* error) {
  nvs_handle_t handle = 0;
  if (!open_guard_nvs(false, &handle, error)) {
    return false;
  }

  const char* keys[] = {
      kGuardKeyArmed,
      kGuardKeyState,
      kGuardKeyCandidateSha256,
      kGuardKeyBackupPath,
      kGuardKeyBootCount,
      kGuardKeyMaxBoots,
      kGuardKeyConfirmTimeoutSec,
  };
  for (const char* key : keys) {
    const esp_err_t err = nvs_erase_key(handle, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
      nvs_close(handle);
      set_error(error, "erase guard key failed");
      return false;
    }
  }

  const bool ok = nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!ok) {
    set_error(error, "guard commit failed");
  }
  return ok;
}

}  // namespace mros::update
