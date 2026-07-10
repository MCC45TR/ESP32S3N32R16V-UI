#include "src/shell/mros_shell_internal.h"
#include "src/platform/mros_time.h"

#include <mros_update_shared.h>

#include <dirent.h>
#include <errno.h>
#include <esp_app_desc.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/md.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace mros::shell {
namespace {

constexpr const char* kFirmwarePrefix = "mros_esp32s3n32r16v_";
constexpr const char* kRecoveryLabel = mros::update::kRecoveryLabel;
constexpr const char* kAppLabel = mros::update::kAppLabel;
constexpr const char* kFirmwareRootRelativePath = "ESPUSER/firmware";
constexpr const char* kCurrentRootRelativePath = "ESPUSER/firmware/current";
constexpr uint32_t kFirmwareScanCacheTtlMs = 5000U;

struct UpdateSystemOptions {
  bool list_only = false;
  bool dry_run = false;
  bool verify = false;
  bool no_reboot = false;
  bool prepare_only = false;
  std::string target;
};

struct FirmwareCandidate {
  std::string actual_path;
  std::string display_path;
  std::string basename;
  std::string version_token;
  std::string timestamp_key;
  std::string sha256;
  uint64_t size_bytes = 0U;
  long long mtime = 0LL;
  int score = 0;
};

std::vector<FirmwareCandidate> g_firmware_scan_cache;
std::string g_firmware_scan_cache_key;
uint32_t g_firmware_scan_cache_ms = 0U;

std::string to_lower_copy(const std::string& text) {
  std::string out = text;
  std::transform(
      out.begin(),
      out.end(),
      out.begin(),
      [](const unsigned char ch) -> char { return static_cast<char>(std::tolower(ch)); });
  return out;
}

bool is_digit_string(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (const char ch : text) {
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
      return false;
    }
  }
  return true;
}

std::string basename_of(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1U);
}

bool ends_with_bin(const std::string& path) {
  const std::string lower = to_lower_copy(path);
  return lower.size() >= 4U && lower.substr(lower.size() - 4U) == ".bin";
}

std::string display_storage_path(const ShellState& state, const std::string& actual_path) {
  const std::string mount_point =
      state.config.storage_mount_point != nullptr ? std::string(state.config.storage_mount_point) : std::string("/littlefs");
  if (actual_path == mount_point) {
    return kShellStorageAlias;
  }
  if (actual_path.size() > mount_point.size() &&
      actual_path.compare(0U, mount_point.size(), mount_point) == 0) {
    return std::string(kShellStorageAlias) + actual_path.substr(mount_point.size());
  }
  return actual_path;
}

std::string storage_mount_point(const ShellState& state) {
  return state.config.storage_mount_point != nullptr ? std::string(state.config.storage_mount_point)
                                                     : std::string("/littlefs");
}

std::string join_storage_path(const std::string& root, const char* relative) {
  if (relative == nullptr || relative[0] == '\0') {
    return root;
  }
  if (!root.empty() && root.back() == '/') {
    return root + relative;
  }
  return root + "/" + relative;
}

bool is_existing_directory(const std::string& path) {
  struct stat info {};
  return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string parse_timestamp_key(const std::string& basename, std::string* version_token) {
  const std::string lower = to_lower_copy(basename);
  if (lower.rfind(kFirmwarePrefix, 0U) != 0 || !ends_with_bin(lower)) {
    return {};
  }

  std::string token = basename.substr(strlen(kFirmwarePrefix));
  token = token.substr(0U, token.size() - 4U);
  if (version_token != nullptr) {
    *version_token = token;
  }
  if (token.size() < 13U || token[8] != '_') {
    return {};
  }

  const std::string date = token.substr(0U, 8U);
  const std::string time = token.substr(9U, 4U);
  if (!is_digit_string(date) || !is_digit_string(time)) {
    return {};
  }

  const int first4 = std::atoi(date.substr(0U, 4U).c_str());
  if (first4 >= 2020 && first4 <= 2099) {
    return date + time;
  }

  const std::string dd = date.substr(0U, 2U);
  const std::string mm = date.substr(2U, 2U);
  const std::string yyyy = date.substr(4U, 4U);
  const int year = std::atoi(yyyy.c_str());
  if (year < 2020 || year > 2099) {
    return {};
  }
  return yyyy + mm + dd + time;
}

int firmware_candidate_score(FirmwareCandidate& candidate) {
  const std::string lower_path = to_lower_copy(candidate.display_path);
  candidate.basename = basename_of(candidate.actual_path);
  candidate.timestamp_key = parse_timestamp_key(candidate.basename, &candidate.version_token);

  int score = 0;
  if (!candidate.timestamp_key.empty()) {
    score += 1000;
  }
  if (lower_path.find("/assets/") != std::string::npos) {
    score -= 300;
  }
  if (lower_path.find("/firmware/") != std::string::npos ||
      lower_path.find("/update/") != std::string::npos ||
      lower_path.find("/ota/") != std::string::npos) {
    score += 250;
  }
  if (lower_path.find("mros_esp32s3n32r16v_") != std::string::npos) {
    score += 250;
  } else if (lower_path.find("mros") != std::string::npos || lower_path.find("bridge") != std::string::npos) {
    score += 80;
  }
  if (lower_path.find("data.bin") != std::string::npos ||
      lower_path.find("littlefs") != std::string::npos) {
    score -= 200;
  }
  if (candidate.size_bytes >= (256U * 1024U)) {
    score += 40;
  }
  return score;
}

bool parse_update_system_args(ShellContext& ctx, UpdateSystemOptions* options, bool* help_requested) {
  if (options == nullptr || help_requested == nullptr) {
    return false;
  }

  *help_requested = false;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help" || arg == "-h") {
      *help_requested = true;
      return true;
    }
    if (arg == "--list") {
      options->list_only = true;
      continue;
    }
    if (arg == "--dry-run") {
      options->dry_run = true;
      continue;
    }
    if (arg == "--verify") {
      options->verify = true;
      continue;
    }
    if (arg == "--no-reboot") {
      options->no_reboot = true;
      continue;
    }
    if (arg == "--prepare-only") {
      options->prepare_only = true;
      options->no_reboot = true;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      shell_printf(ctx.state, "update-system: unknown option '%s'\n", arg.c_str());
      return false;
    }
    if (!options->target.empty()) {
      shell_printf(ctx.state, "update-system: extra operand '%s'\n", arg.c_str());
      return false;
    }
    options->target = arg;
  }
  return true;
}

void scan_firmware_candidates(
    const ShellState& state,
    const std::string& actual_dir,
    std::vector<FirmwareCandidate>* out_candidates) {
  if (out_candidates == nullptr) {
    return;
  }

  DIR* dir = ::opendir(actual_dir.c_str());
  if (dir == nullptr) {
    return;
  }

  struct dirent* entry = nullptr;
  while ((entry = ::readdir(dir)) != nullptr) {
    const char* name = entry->d_name;
    if (name == nullptr || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }

    const std::string child_actual =
        actual_dir == "/" ? std::string("/") + name : actual_dir + "/" + name;
    struct stat info {};
    if (::stat(child_actual.c_str(), &info) != 0) {
      continue;
    }

    if (S_ISDIR(info.st_mode)) {
      scan_firmware_candidates(state, child_actual, out_candidates);
      continue;
    }
    if (!S_ISREG(info.st_mode) || !ends_with_bin(name)) {
      continue;
    }

    FirmwareCandidate candidate {};
    candidate.actual_path = child_actual;
    candidate.display_path = display_storage_path(state, child_actual);
    candidate.size_bytes = static_cast<uint64_t>(info.st_size);
    candidate.mtime = static_cast<long long>(info.st_mtime);
    candidate.score = firmware_candidate_score(candidate);
    out_candidates->push_back(std::move(candidate));
  }

  ::closedir(dir);
}

void sort_candidates(std::vector<FirmwareCandidate>* candidates) {
  if (candidates == nullptr) {
    return;
  }
  std::sort(
      candidates->begin(),
      candidates->end(),
      [](const FirmwareCandidate& left, const FirmwareCandidate& right) {
        if (left.timestamp_key != right.timestamp_key) {
          return left.timestamp_key > right.timestamp_key;
        }
        if (left.score != right.score) {
          return left.score > right.score;
        }
        if (left.mtime != right.mtime) {
          return left.mtime > right.mtime;
        }
        if (left.size_bytes != right.size_bytes) {
          return left.size_bytes > right.size_bytes;
        }
        return left.display_path < right.display_path;
      });
}

bool ensure_littlefs_ready(ShellState& state) {
  if (shell_is_storage_mounted(state)) {
    return true;
  }
  if (state.config.mount_storage_callback == nullptr) {
    shell_write_line(state, "update-system: LittleFS mount callback is not configured");
    return false;
  }
  char message[192] = {};
  const bool ok = state.config.mount_storage_callback(message, sizeof(message), state.config.user_data);
  shell_write_line(state, message[0] != '\0' ? message : (ok ? "LittleFS ready." : "LittleFS mount failed."));
  return ok;
}

bool resolve_explicit_target(ShellState& state, const std::string& raw_target, FirmwareCandidate* out_candidate) {
  if (out_candidate == nullptr) {
    return false;
  }

  const std::string normalized = shell_normalize_path(state, raw_target);
  if (!shell_is_storage_path(state, normalized)) {
    shell_printf(state, "update-system: target must be under %s\n", kShellStorageAlias);
    return false;
  }

  bool is_dir = false;
  struct stat info {};
  std::string error;
  if (!shell_path_exists(state, normalized, &is_dir, &info, &error)) {
    shell_printf(state, "update-system: cannot access '%s': %s\n", raw_target.c_str(), error.c_str());
    return false;
  }
  if (is_dir) {
    shell_printf(state, "update-system: '%s' is a directory\n", raw_target.c_str());
    return false;
  }
  if (!ends_with_bin(normalized)) {
    shell_write_line(state, "update-system: only .bin firmware images are supported");
    return false;
  }

  out_candidate->actual_path = normalized;
  out_candidate->display_path = display_storage_path(state, normalized);
  out_candidate->basename = basename_of(normalized);
  out_candidate->size_bytes = static_cast<uint64_t>(info.st_size);
  out_candidate->mtime = static_cast<long long>(info.st_mtime);
  out_candidate->score = firmware_candidate_score(*out_candidate);
  return true;
}

bool resolve_auto_target(ShellState& state, const UpdateSystemOptions& options, FirmwareCandidate* out_candidate) {
  if (out_candidate == nullptr) {
    return false;
  }

  std::vector<FirmwareCandidate> candidates;
  const std::string mount_point = storage_mount_point(state);
  const std::string firmware_root = join_storage_path(mount_point, kFirmwareRootRelativePath);
  const std::string cache_key = mount_point + "|" + firmware_root;
  const uint32_t now_ms = mros::platform::mros_millis();
  if (!g_firmware_scan_cache.empty() && g_firmware_scan_cache_key == cache_key &&
      (now_ms - g_firmware_scan_cache_ms) < kFirmwareScanCacheTtlMs) {
    candidates = g_firmware_scan_cache;
  } else {
    if (is_existing_directory(firmware_root)) {
      scan_firmware_candidates(state, firmware_root, &candidates);
    }
    if (candidates.empty()) {
      scan_firmware_candidates(state, mount_point, &candidates);
    }
    g_firmware_scan_cache = candidates;
    g_firmware_scan_cache_key = cache_key;
    g_firmware_scan_cache_ms = now_ms;
  }
  sort_candidates(&candidates);

  if (candidates.empty()) {
    shell_printf(state,
                 "update-system: no .bin image found under %s or LittleFS\n",
                 display_storage_path(state, firmware_root).c_str());
    return false;
  }

  if (options.list_only) {
    shell_printf(state,
                 "update-system: auto scan root %s (fallback: %s, cache=%ums)\n",
                 display_storage_path(state, firmware_root).c_str(),
                 kShellStorageAlias,
                 static_cast<unsigned>(kFirmwareScanCacheTtlMs));
    shell_write_line(state, "candidate                                       version        bytes      score");
    shell_write_line(state, "----------------------------------------------  ------------  ---------  -----");
    for (const FirmwareCandidate& candidate : candidates) {
      shell_printf(
          state,
          "%-46s  %-12s  %9llu  %5d\n",
          candidate.display_path.c_str(),
          candidate.version_token.empty() ? "-" : candidate.version_token.c_str(),
          static_cast<unsigned long long>(candidate.size_bytes),
          candidate.score);
    }
    return false;
  }

  if (candidates.front().score <= 0) {
    shell_write_line(state, "update-system: no MROS-like firmware candidate found; use an explicit /fs/path");
    return false;
  }

  *out_candidate = candidates.front();
  return true;
}

std::string sha256_file(ShellState& state, const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    shell_printf(state, "update-system: failed to open '%s' for hashing: %s\n", path.c_str(), strerror(errno));
    return {};
  }

  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md_info == nullptr) {
    std::fclose(file);
    shell_write_line(state, "update-system: sha256 md info unavailable");
    return {};
  }

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, md_info, 0) != 0 || mbedtls_md_starts(&ctx) != 0) {
    std::fclose(file);
    mbedtls_md_free(&ctx);
    shell_write_line(state, "update-system: sha256 init failed");
    return {};
  }

  uint8_t buffer[4096] = {};
  while (true) {
    const size_t read_size = std::fread(buffer, 1U, sizeof(buffer), file);
    if (read_size > 0U) {
      if (mbedtls_md_update(&ctx, buffer, read_size) != 0) {
        shell_write_line(state, "update-system: sha256 update failed");
        std::fclose(file);
        mbedtls_md_free(&ctx);
        return {};
      }
    }
    if (read_size < sizeof(buffer)) {
      if (std::ferror(file) != 0) {
        shell_write_line(state, "update-system: read error while hashing firmware");
        std::fclose(file);
        mbedtls_md_free(&ctx);
        return {};
      }
      break;
    }
  }
  std::fclose(file);

  uint8_t digest[32] = {};
  if (mbedtls_md_finish(&ctx, digest) != 0) {
    mbedtls_md_free(&ctx);
    shell_write_line(state, "update-system: sha256 finish failed");
    return {};
  }
  mbedtls_md_free(&ctx);

  char out[65] = {};
  for (size_t i = 0; i < sizeof(digest); ++i) {
    snprintf(out + (i * 2U), sizeof(out) - (i * 2U), "%02x", digest[i]);
  }
  return out;
}

const esp_partition_t* find_app_partition(const char* label) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
}

const esp_partition_t* find_data_partition(const char* label) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
}

bool partition_has_valid_app(const esp_partition_t* partition) {
  if (partition == nullptr) {
    return false;
  }
  esp_app_desc_t desc {};
  return esp_ota_get_partition_description(partition, &desc) == ESP_OK;
}

bool write_update_manifest(ShellState& state, FirmwareCandidate& candidate) {
  if (candidate.sha256.empty()) {
    candidate.sha256 = sha256_file(state, candidate.actual_path);
    if (candidate.sha256.empty()) {
      return false;
    }
  }

  const std::string mount_point =
      state.config.storage_mount_point != nullptr ? std::string(state.config.storage_mount_point) : std::string("/littlefs");
  std::string error;
  if (!mros::update::ensure_update_dirs(mount_point.c_str(), &error)) {
    shell_printf(state, "update-system: failed to create update dirs: %s\n", error.c_str());
    return false;
  }

  if (!mros::update::clear_boot_guard(&error)) {
    shell_printf(state, "update-system: failed to clear previous boot guard: %s\n", error.c_str());
    return false;
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  mros::update::UpdateManifest manifest {};
  manifest.state = "pending_install";
  manifest.target = candidate.actual_path;
  manifest.target_display = candidate.display_path;
  manifest.backup_path.clear();
  manifest.sha256 = candidate.sha256;
  manifest.size = candidate.size_bytes;
  manifest.version = candidate.version_token;
  manifest.target_partition = kAppLabel;
  manifest.recovery_partition = kRecoveryLabel;
  manifest.running_partition = running != nullptr ? running->label : "";
  manifest.attempt = 0U;
  manifest.max_attempts = mros::update::kDefaultMaxAttempts;
  manifest.max_boots = mros::update::kDefaultMaxBoots;
  manifest.confirm_timeout_sec = mros::update::kDefaultConfirmTimeoutSec;
  if (!mros::update::sign_manifest(&manifest, &error)) {
    shell_printf(state, "update-system: manifest signature failed: %s\n", error.c_str());
    shell_write_line(state,
                     "update-system: define MROS_UPDATE_MANIFEST_HMAC_KEY before staging OTA.");
    return false;
  }

  if (!mros::update::save_manifest_to_mount(mount_point.c_str(), manifest, &error)) {
    shell_printf(state, "update-system: manifest save failed: %s\n", error.c_str());
    return false;
  }

  const std::string manifest_path = mros::update::manifest_path_for_mount(mount_point.c_str());
  shell_printf(state, "update-system: manifest written to %s\n",
               display_storage_path(state, manifest_path).c_str());
  return true;
}

bool verify_update_candidate(ShellState& state, FirmwareCandidate& candidate) {
  if (candidate.size_bytes == 0U) {
    shell_write_line(state, "update-system: verify failed, selected image is empty");
    return false;
  }

  const esp_partition_t* app0 = find_app_partition(kAppLabel);
  if (app0 == nullptr) {
    shell_write_line(state, "update-system: verify failed, app0 partition is missing");
    return false;
  }
  if (candidate.size_bytes > app0->size) {
    shell_printf(state,
                 "update-system: verify failed, image is %llu bytes but app0 is %lu bytes\n",
                 static_cast<unsigned long long>(candidate.size_bytes),
                 static_cast<unsigned long>(app0->size));
    return false;
  }

  candidate.sha256 = sha256_file(state, candidate.actual_path);
  if (candidate.sha256.empty()) {
    return false;
  }

  const unsigned long free_bytes =
      static_cast<unsigned long>(app0->size - candidate.size_bytes);
  shell_printf(state,
               "update-system: verify app0 fit: %llu/%lu bytes (%lu bytes free)\n",
               static_cast<unsigned long long>(candidate.size_bytes),
               static_cast<unsigned long>(app0->size),
               free_bytes);
  shell_printf(state, "update-system: verify sha256=%s\n", candidate.sha256.c_str());
  return true;
}

bool print_partition_safety(ShellState& state) {
  const esp_partition_t* recovery = find_app_partition(kRecoveryLabel);
  const esp_partition_t* app0 = find_app_partition(kAppLabel);
  const esp_partition_t* otadata = find_data_partition("otadata");
  const esp_partition_t* nvs_sys_usr = find_data_partition("nvs_sys_usr");

  shell_printf(state, "update-system: recovery partition : %s (%s)\n",
               recovery != nullptr ? "present" : "missing",
               partition_has_valid_app(recovery) ? "valid image" : "no valid image");
  shell_printf(state, "update-system: app0 partition     : %s\n", app0 != nullptr ? "present" : "missing");
  shell_printf(state, "update-system: otadata            : %s\n", otadata != nullptr ? "present" : "missing");
  shell_printf(state, "update-system: nvs_sys_usr        : %s\n", nvs_sys_usr != nullptr ? "present" : "missing");
  return recovery != nullptr && app0 != nullptr && otadata != nullptr && nvs_sys_usr != nullptr;
}

bool boot_recovery(ShellState& state, const bool no_reboot) {
  const esp_partition_t* recovery = find_app_partition(kRecoveryLabel);
  if (!partition_has_valid_app(recovery)) {
    shell_write_line(state,
                     "update-system: recovery partition has no valid image; refusing to boot empty recovery");
    shell_write_line(state,
                     "update-system: manifest is staged. Flash recovery firmware first, then rerun update.");
    return false;
  }
  const esp_err_t err = esp_ota_set_boot_partition(recovery);
  if (err != ESP_OK) {
    shell_printf(state, "update-system: failed to select recovery boot partition: %s\n", esp_err_to_name(err));
    return false;
  }
  shell_write_line(state, "update-system: recovery selected for next boot");
  if (no_reboot) {
    shell_write_line(state, "update-system: reboot skipped by option");
    return true;
  }
  if (state.config.system_action_callback == nullptr) {
    shell_write_line(state, "update-system: reboot callback is not configured");
    return true;
  }
  shell_write_line(state, "update-system: rebooting into recovery...");
  return state.config.system_action_callback(ShellSystemAction::Reboot, state.config.user_data);
}

}  // namespace

void shell_help_update_system(ShellState& state) {
  shell_write_line(state, "Usage: update-system [OPTION]... [FILE]");
  shell_write_line(state, "Stage an ESP32-S3 firmware image from LittleFS for recovery-mode install.");
  shell_write_line(state, "Without FILE, mshell searches /ESPUSER/firmware first, then LittleFS fallback.");
  shell_write_line(state, "  --list                     list detected .bin files under /ESPUSER/firmware");
  shell_write_line(state, "  --dry-run                  resolve only and do not write manifest");
  shell_write_line(state, "  --verify                   hash selected image and verify it fits app0");
  shell_write_line(state, "  --prepare-only             write manifest but do not select/reboot recovery");
  shell_write_line(state, "  --no-reboot                select recovery but do not reboot");
  shell_write_line(state, "Signed manifest requires MROS_UPDATE_MANIFEST_HMAC_KEY.");
  shell_write_line(state, "Examples:");
  shell_write_line(state, "  update-system --dry-run --verify /fs/ESPUSER/firmware/app.bin");
}

int shell_cmd_update_system(ShellContext& ctx) {
  UpdateSystemOptions options {};
  bool help_requested = false;
  if (!parse_update_system_args(ctx, &options, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_update_system(ctx.state);
    return 0;
  }
  if (!ensure_littlefs_ready(ctx.state)) {
    return 1;
  }

  FirmwareCandidate candidate {};
  const bool resolved = options.target.empty() ? resolve_auto_target(ctx.state, options, &candidate)
                                               : resolve_explicit_target(ctx.state, options.target, &candidate);
  if (!resolved) {
    return options.list_only ? 0 : 1;
  }

  shell_printf(
      ctx.state,
      "update-system: selected %s (%llu bytes, version=%s)\n",
      candidate.display_path.c_str(),
      static_cast<unsigned long long>(candidate.size_bytes),
      candidate.version_token.empty() ? "-" : candidate.version_token.c_str());
  if (!print_partition_safety(ctx.state)) {
    shell_write_line(ctx.state, "update-system: partition table is not recovery-layout ready");
    return 1;
  }
  if (options.verify && !verify_update_candidate(ctx.state, candidate)) {
    return 1;
  }
  if (options.dry_run) {
    shell_write_line(ctx.state,
                     options.verify
                         ? "update-system: dry-run verify complete, manifest was not written"
                         : "update-system: dry-run only, manifest was not written");
    return 0;
  }
  if (!write_update_manifest(ctx.state, candidate)) {
    return 1;
  }
  shell_printf(ctx.state, "update-system: sha256=%s\n", candidate.sha256.c_str());
  if (options.prepare_only) {
    shell_write_line(ctx.state, "update-system: prepared only; recovery boot not selected");
    return 0;
  }
  return boot_recovery(ctx.state, options.no_reboot) ? 0 : 1;
}

}  // namespace mros::shell
