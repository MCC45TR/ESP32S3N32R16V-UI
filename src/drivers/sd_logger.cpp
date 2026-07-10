#include "sd_logger.h"
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_err.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <ctype.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <string.h>
#include "src/core/state/event_bus.h"
#include "src/drivers/utils/mros_console.h"
#include "src/platform/mros_file.h"
#include "src/platform/mros_fs.h"
#include "src/platform/mros_nvs.h"
#include "src/platform/mros_system.h"
#include "src/platform/mros_time.h"
#if defined(__has_include)
#if __has_include(<esp_littlefs.h>)
extern "C" {
#include <esp_littlefs.h>
}
#define MROS_HAS_ESP_LITTLEFS 1
#else
#define MROS_HAS_ESP_LITTLEFS 0
#endif
#else
extern "C" {
#include <esp_littlefs.h>
}
#define MROS_HAS_ESP_LITTLEFS 1
#endif

static bool g_fs_mounted = false;
static bool g_fs_warned_unavailable = false;
static TaskHandle_t g_storage_task_handle = nullptr;
static bool storage_ready();

static inline void increment_counter(volatile uint32_t* counter) {
  if (counter == nullptr) {
    return;
  }
  *counter = *counter + 1U;
}

namespace {

using mros::platform::NvsNamespace;
using mros::platform::NvsPartitionMode;

constexpr size_t kWriteBatchSlots = 8;
constexpr size_t kWriteBatchPathMax = 128;
constexpr size_t kWriteBatchPayloadMax = 12288;
constexpr size_t kStorageCommandKeyMax = 128;
constexpr UBaseType_t kStorageQueueLength = 24;
constexpr unsigned long kWriteBatchFlushPeriodMs = 1500UL;
constexpr size_t kLargeTolerantPsramOnlyThreshold = 4096;

// CSV log batching: accumulate in RAM, flush to flash periodically
constexpr size_t kCsvLogBufSize = 4096;
constexpr unsigned long kCsvLogFlushIntervalMs = 3000UL;
constexpr size_t kCsvLogMaxFileSize = 1024 * 50; // 50KB rotation limit
constexpr uint8_t kWifiCfgVersion = 1;
constexpr uint8_t kCredCfgVersion = 1;
constexpr const char *kUserNvsPartition = "nvs_sys_usr";
constexpr const char *kLegacyUserNvsPartition = "nvs_usr";
constexpr const char *kTokenNamespace = "sys_cfg";
constexpr const char *kCredNamespace = "cred_cfg";
constexpr const char *kUserRootPath = "/ESPUSER";
constexpr const char *kConfigJsonPath = "/ESPUSER/config.json";
constexpr const char *kPidCfgPath = "/ESPUSER/pid.cfg";
constexpr const char *kLogsCsvPath = "/ESPUSER/logs.csv";

std::string to_std_string(const String &value) {
  return std::string(value.c_str(), value.length());
}

String to_arduino_string(const std::string &value) {
  return String(value.c_str());
}

bool open_default_nvs(NvsNamespace *nvs, const char *name, const bool read_only) {
  return nvs != nullptr &&
         nvs->open(name, read_only, NvsPartitionMode::DefaultOnly);
}

bool open_user_nvs(NvsNamespace *nvs, const char *name, const bool read_only) {
  return nvs != nullptr &&
         nvs->open(name, read_only, NvsPartitionMode::UserPartitionsThenDefault);
}

static char *g_csv_log_buf = nullptr;
static size_t g_csv_log_len = 0;
static bool g_csv_log_ready = false;
static bool g_csv_log_psram = false;
static unsigned long g_csv_log_last_flush_ms = 0;

struct BatchedWriteSlot {
  bool used = false;
  bool dirty = false;
  size_t len = 0;
  char path[kWriteBatchPathMax] = {0};
  char *payload = nullptr;
};

static BatchedWriteSlot g_write_batch[kWriteBatchSlots];
static SemaphoreHandle_t g_write_batch_mutex = nullptr;
static bool g_write_batch_ready = false;
static bool g_write_batch_psram = false;
static bool g_write_batch_warned_full = false;
static unsigned long g_last_batch_flush_ms = 0;
static bool g_boot_credential_migration_done = false;
static void run_boot_credential_migrations_once();
static bool save_credentials_user_nvs(const String &user, const String &pass_hash);
static bool load_credentials_user_nvs(String &user, String &pass_hash);
static QueueHandle_t g_storage_queue = nullptr;
static volatile uint32_t g_storage_enqueue_count = 0;
static volatile uint32_t g_storage_drop_count = 0;
static volatile uint32_t g_storage_processed_count = 0;
static volatile uint32_t g_storage_queue_high_watermark = 0;
static volatile uint32_t g_storage_batched_write_count = 0;
static volatile uint32_t g_storage_batch_flush_count = 0;
static volatile uint32_t g_storage_direct_write_fallback_count = 0;
static volatile uint32_t g_storage_csv_flush_count = 0;
static volatile uint32_t g_storage_atomic_write_count = 0;
static volatile uint32_t g_storage_atomic_write_fail_count = 0;
static bool g_storage_last_atomic_write_ok = false;
static char g_storage_last_atomic_write_error[32] = "none";
static bool g_credential_migration_attempted = false;
static bool g_credential_migration_migrated = false;
static char g_credential_migration_source[32] = "none";
static char g_credential_migration_error[32] = "not_run";

static void set_atomic_write_status(const bool ok, const char* error) {
  g_storage_last_atomic_write_ok = ok;
  std::snprintf(g_storage_last_atomic_write_error,
                sizeof(g_storage_last_atomic_write_error),
                "%s",
                error != nullptr ? error : (ok ? "ok" : "failed"));
}

static void set_credential_migration_status(const char* source,
                                            const bool migrated,
                                            const char* error) {
  g_credential_migration_attempted = true;
  g_credential_migration_migrated = migrated;
  std::snprintf(g_credential_migration_source,
                sizeof(g_credential_migration_source),
                "%s",
                source != nullptr ? source : "none");
  std::snprintf(g_credential_migration_error,
                sizeof(g_credential_migration_error),
                "%s",
                error != nullptr ? error : (migrated ? "none" : "not_found"));
}

static bool ensure_user_root_exists() {
  if (!mros::platform::mros_fs_exists(kUserRootPath)) {
    if (!mros::platform::mros_fs_mkdir(kUserRootPath)) {
      mros_console.printf("[STORAGE] Failed to create %s\n", kUserRootPath);
      return false;
    }
    mros_console.printf("[STORAGE] Created %s\n", kUserRootPath);
  }
  return true;
}

static void migrate_legacy_file_to_user_root(const char *legacy_path,
                                             const char *user_path) {
  if (legacy_path == nullptr || user_path == nullptr) {
    return;
  }
  if (!mros::platform::mros_fs_exists(legacy_path) ||
      mros::platform::mros_fs_exists(user_path)) {
    return;
  }
  if (mros::platform::mros_fs_rename(legacy_path, user_path)) {
    mros_console.printf("[STORAGE] Migrated %s -> %s\n", legacy_path, user_path);
  } else {
    mros_console.printf("[STORAGE] Failed to migrate %s -> %s\n", legacy_path,
                        user_path);
  }
}

static void migrate_legacy_popup_settings_files() {
  DIR *dir = opendir("/littlefs");
  if (dir == nullptr) {
    return;
  }

  dirent *entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    const char *name = entry->d_name;
    if (strncmp(name, "ui_settings_", 12) != 0) {
      continue;
    }
    const size_t len = strlen(name);
    if (len < 18 || strcmp(name + (len - 5), ".json") != 0) {
      continue;
    }
    const String old_path = String("/") + name;
    const String new_path = String(kUserRootPath) + "/" + name;
    if (mros::platform::mros_fs_exists(new_path.c_str())) {
      continue;
    }
    if (mros::platform::mros_fs_rename(old_path.c_str(), new_path.c_str())) {
      mros_console.printf("[STORAGE] Migrated %s -> %s\n", old_path.c_str(),
                          new_path.c_str());
    }
  }

  closedir(dir);
}

static void migrate_legacy_user_files() {
  if (!ensure_user_root_exists()) {
    return;
  }
  migrate_legacy_file_to_user_root("/config.json", kConfigJsonPath);
  migrate_legacy_file_to_user_root("/pid.cfg", kPidCfgPath);
  migrate_legacy_file_to_user_root("/logs.csv", kLogsCsvPath);
  migrate_legacy_popup_settings_files();
}

enum class StorageCommandType : uint8_t {
  SaveWifi = 0,
  SaveWifiRuntime,
  SaveToken,
  SaveCredentials,
  SavePid,
  WriteTextFile,
};

struct StorageCommand {
  StorageCommandType type = StorageCommandType::SaveToken;
  char key[kStorageCommandKeyMax] = {0};
  char value[96] = {0};
  uint8_t aux_u8 = 0;
  char *payload = nullptr;
  size_t payload_len = 0;
  float pid[5] = {0.0f};
};

static bool ensure_storage_queue_ready() {
  if (g_storage_queue != nullptr) {
    return true;
  }
  g_storage_queue = xQueueCreate(kStorageQueueLength, sizeof(StorageCommand));
  return g_storage_queue != nullptr;
}

static void *alloc_tolerant_buffer(size_t bytes, bool *is_psram = nullptr) {
  if (is_psram) *is_psram = false;
  if (bytes == 0) return nullptr;

  const bool has_psram = mros::platform::mros_system_psram_total() > 0U;
  if (has_psram) {
    void *ptr =
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) {
      if (is_psram) *is_psram = true;
      return ptr;
    }
    // Keep large tolerant buffers out of internal SRAM when PSRAM exists.
    if (bytes > kLargeTolerantPsramOnlyThreshold) {
      return nullptr;
    }
  }

  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static char *alloc_payload_copy(const String &text) {
  void *buf = alloc_tolerant_buffer(text.length() + 1U);
  if (buf == nullptr) {
    return nullptr;
  }
  char *payload = static_cast<char *>(buf);
  memcpy(payload, text.c_str(), text.length());
  payload[text.length()] = '\0';
  return payload;
}

static bool enqueue_storage_command(StorageCommand *command) {
  if (command == nullptr) {
    return false;
  }
  if (!ensure_storage_queue_ready()) {
    if (command->payload != nullptr) {
      heap_caps_free(command->payload);
      command->payload = nullptr;
    }
    increment_counter(&g_storage_drop_count);
    return false;
  }
  if (xQueueSend(g_storage_queue, command, 0) != pdTRUE) {
    if (command->payload != nullptr) {
      heap_caps_free(command->payload);
      command->payload = nullptr;
    }
    increment_counter(&g_storage_drop_count);
    return false;
  }
  increment_counter(&g_storage_enqueue_count);
  const UBaseType_t depth = uxQueueMessagesWaiting(g_storage_queue);
  if (depth > g_storage_queue_high_watermark) {
    g_storage_queue_high_watermark = depth;
  }
  event_bus_publish(BIT_STORAGE_DIRTY);
  if (g_storage_task_handle != nullptr) {
    xTaskNotifyGive(g_storage_task_handle);
  }
  return true;
}

static bool lock_write_batch(TickType_t timeout_ticks = pdMS_TO_TICKS(20)) {
  if (g_write_batch_mutex == nullptr) {
    return false;
  }
  return xSemaphoreTake(g_write_batch_mutex, timeout_ticks) == pdTRUE;
}

static void unlock_write_batch() {
  if (g_write_batch_mutex != nullptr) {
    xSemaphoreGive(g_write_batch_mutex);
  }
}

static int find_write_batch_slot(const char *path) {
  for (size_t i = 0; i < kWriteBatchSlots; i++) {
    if (g_write_batch[i].used && strncmp(g_write_batch[i].path, path, kWriteBatchPathMax) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

static int find_free_write_batch_slot() {
  for (size_t i = 0; i < kWriteBatchSlots; i++) {
    if (!g_write_batch[i].used || !g_write_batch[i].dirty) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

static bool write_file_direct_locked(const char *path, const char *data, size_t len) {
  return mros::platform::mros_file_write_all(path, std::string_view(data, len));
}

static bool flush_write_batch_locked() {
  if (!g_write_batch_ready) {
    return false;
  }
  bool any_flushed = false;
  for (size_t i = 0; i < kWriteBatchSlots; i++) {
    BatchedWriteSlot &slot = g_write_batch[i];
    if (!slot.used || !slot.dirty) {
      continue;
    }
    if (!write_file_direct_locked(slot.path, slot.payload, slot.len)) {
      mros_console.printf("[STORAGE] Batched flush failed for %s\n", slot.path);
      continue;
    }
    slot.dirty = false;
    any_flushed = true;
  }
  if (any_flushed) {
    g_last_batch_flush_ms = mros::platform::mros_millis();
    increment_counter(&g_storage_batch_flush_count);
  }
  return any_flushed;
}

static bool init_write_batch_buffers() {
  if (g_write_batch_ready) {
    return true;
  }

  if (g_write_batch_mutex == nullptr) {
    g_write_batch_mutex = xSemaphoreCreateMutex();
    if (g_write_batch_mutex == nullptr) {
      return false;
    }
  }

  g_write_batch_psram = (mros::platform::mros_system_psram_total() > 0U);

  for (size_t i = 0; i < kWriteBatchSlots; i++) {
    g_write_batch[i].used = false;
    g_write_batch[i].dirty = false;
    g_write_batch[i].len = 0;
    g_write_batch[i].path[0] = '\0';

    bool slot_psram = false;
    void *buf = alloc_tolerant_buffer(kWriteBatchPayloadMax + 1, &slot_psram);
    if (buf == nullptr) {
      for (size_t j = 0; j < i; j++) {
        if (g_write_batch[j].payload != nullptr) {
          heap_caps_free(g_write_batch[j].payload);
          g_write_batch[j].payload = nullptr;
        }
      }
      mros_console.println("[STORAGE] Batched write buffer allocation failed.");
      return false;
    }
    if (!slot_psram) {
      g_write_batch_psram = false;
    }
    g_write_batch[i].payload = static_cast<char *>(buf);
    g_write_batch[i].payload[0] = '\0';
  }

  g_write_batch_warned_full = false;
  g_last_batch_flush_ms = mros::platform::mros_millis();
  g_write_batch_ready = true;
  mros_console.printf("[STORAGE] LittleFS write batching ready (%s, slots=%u, payload=%u)\n",
                      g_write_batch_psram ? "PSRAM" : "HEAP",
                      static_cast<unsigned>(kWriteBatchSlots),
                      static_cast<unsigned>(kWriteBatchPayloadMax));
  return true;
}

static bool enqueue_batched_write(const char *path, const std::string &data) {
  if (!g_write_batch_ready || path == nullptr) {
    return false;
  }
  if (strlen(path) >= kWriteBatchPathMax) {
    return false;
  }
  if (data.length() > kWriteBatchPayloadMax) {
    return false;
  }
  if (!lock_write_batch()) {
    return false;
  }

  int slot_index = find_write_batch_slot(path);
  if (slot_index < 0) {
    slot_index = find_free_write_batch_slot();
  }
  if (slot_index < 0) {
    flush_write_batch_locked();
    slot_index = find_free_write_batch_slot();
  }
  if (slot_index < 0) {
    if (!g_write_batch_warned_full) {
      g_write_batch_warned_full = true;
      mros_console.println("[STORAGE] Batched write slots full, falling back to direct writes.");
    }
    unlock_write_batch();
    return false;
  }

  BatchedWriteSlot &slot = g_write_batch[slot_index];
  slot.used = true;
  slot.dirty = true;
  slot.len = data.length();
  strncpy(slot.path, path, kWriteBatchPathMax - 1);
  slot.path[kWriteBatchPathMax - 1] = '\0';
  memcpy(slot.payload, data.c_str(), slot.len);
  slot.payload[slot.len] = '\0';
  increment_counter(&g_storage_batched_write_count);

  unlock_write_batch();
  return true;
}

static bool write_file_direct(const char *path, const std::string &data) {
  if (!storage_ready()) {
    return false;
  }
  return mros::platform::mros_file_write_all(path, data);
}

} // namespace

static const esp_partition_t *detect_fs_partition(const char **out_label) {
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "littlefs");
  if (part != nullptr) {
    if (out_label != nullptr) {
      *out_label = "littlefs";
    }
    return part;
  }

  part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, nullptr);
  if (part != nullptr) {
    if (out_label != nullptr) {
      *out_label = part->label;
    }
    return part;
  }

  if (out_label != nullptr) {
    *out_label = nullptr;
  }
  return nullptr;
}

static bool format_fs_partition(const char *partition_label,
                                const esp_partition_t *partition) {
  if (partition_label == nullptr || partition == nullptr) {
    return false;
  }

#if MROS_HAS_ESP_LITTLEFS
  esp_err_t fmt_result = esp_littlefs_format(partition_label);
  if (fmt_result == ESP_OK) {
    mros_console.printf("[STORAGE] LittleFS format completed on '%s'\n",
                        partition_label);
    return true;
  }
  mros_console.printf("[STORAGE] LittleFS format failed on '%s': %s\n",
                      partition_label, esp_err_to_name(fmt_result));
#endif

  esp_err_t erase_result = esp_partition_erase_range(partition, 0, partition->size);
  if (erase_result == ESP_OK) {
    mros_console.printf("[STORAGE] Raw partition erase completed on '%s'\n",
                        partition_label);
    return true;
  }
  mros_console.printf("[STORAGE] Raw partition erase failed on '%s': %s\n",
                      partition_label, esp_err_to_name(erase_result));
  return false;
}

static void migrate_fs_layout_if_needed(const char *partition_label,
                                        const esp_partition_t *partition) {
  if (partition_label == nullptr || partition == nullptr) {
    return;
  }

  NvsNamespace layout_prefs;
  if (!open_default_nvs(&layout_prefs, "fs_layout", false)) {
    return;
  }

  // Mark mount as in-progress. If reboot happens before a successful mount,
  // next boot will force erase to escape potential LittleFS assert loops.
  bool previous_mount_ok = true;
  (void)layout_prefs.get_bool("mount_ok", &previous_mount_ok);
  layout_prefs.set_bool("mount_ok", false);

  String layout_key = String(partition_label) + ":" + String(partition->size);
  std::string saved_layout_std;
  (void)layout_prefs.get_string("layout", &saved_layout_std);
  const String saved_layout = to_arduino_string(saved_layout_std);
  const bool layout_changed = (saved_layout != layout_key);
  const bool recover_unclean = !previous_mount_ok;

  if (layout_changed || recover_unclean) {
    if (layout_changed) {
      mros_console.printf(
          "[STORAGE] FS layout change detected ('%s' -> '%s'). Erasing partition...\n",
          saved_layout.c_str(), layout_key.c_str());
    } else {
      mros_console.println(
          "[STORAGE] Previous LittleFS mount was interrupted. Erasing partition for recovery...");
    }

    if (format_fs_partition(partition_label, partition)) {
      layout_prefs.set_string("layout", to_std_string(layout_key));
      mros_console.println("[STORAGE] FS recovery completed.");
    } else {
      mros_console.println("[STORAGE] FS recovery operation failed.");
    }
  }
}

static void mark_fs_mount_ok() {
  NvsNamespace layout_prefs;
  if (!open_default_nvs(&layout_prefs, "fs_layout", false)) {
    return;
  }
  layout_prefs.set_bool("mount_ok", true);
}

static bool storage_ready() {
  if (g_fs_mounted) {
    return true;
  }
  if (!g_fs_warned_unavailable) {
    g_fs_warned_unavailable = true;
    mros_console.println(
        "[STORAGE] LittleFS not mounted. File operations are skipped.");
  }
  return false;
}

static void init_csv_log_buffer() {
  if (g_csv_log_ready) return;
  g_csv_log_buf = nullptr;
  g_csv_log_psram = false;
  g_csv_log_buf = static_cast<char *>(
      alloc_tolerant_buffer(kCsvLogBufSize + 1, &g_csv_log_psram));
  if (!g_csv_log_buf) {
    mros_console.println("[STORAGE] CSV log buffer allocation failed.");
    return;
  }
  g_csv_log_buf[0] = '\0';
  g_csv_log_len = 0;
  g_csv_log_last_flush_ms = mros::platform::mros_millis();
  g_csv_log_ready = true;
  mros_console.printf("[STORAGE] CSV log batching ready (%s, buf=%u)\n",
                      g_csv_log_psram ? "PSRAM" : "HEAP",
                      static_cast<unsigned>(kCsvLogBufSize));
}

static void csv_log_flush_to_flash() {
  if (!g_csv_log_ready || g_csv_log_len == 0 || !g_csv_log_buf) return;
  if (!storage_ready()) return;

  // Check rotation before writing
  size_t cur_size = 0;
  (void)mros::platform::mros_fs_file_size(kLogsCsvPath, &cur_size);
  if (cur_size + g_csv_log_len > kCsvLogMaxFileSize) {
    (void)mros::platform::mros_fs_remove(kLogsCsvPath);
  }

  (void)mros::platform::mros_file_append_all(
      kLogsCsvPath, std::string_view(g_csv_log_buf, g_csv_log_len));
  increment_counter(&g_storage_csv_flush_count);
  g_csv_log_len = 0;
  g_csv_log_buf[0] = '\0';
  g_csv_log_last_flush_ms = mros::platform::mros_millis();
}

void logger_init() {
  if (g_fs_mounted) {
    return;
  }
  g_fs_mounted = false;
  g_fs_warned_unavailable = false;

  const char *partition_label = nullptr;
  const esp_partition_t *partition = detect_fs_partition(&partition_label);
  if (partition_label == nullptr || partition == nullptr) {
    mros_console.println(
        "[STORAGE] No FS partition found. Expected label: littlefs or spiffs");
    return;
  }

  // Prevent bootloop when partition layout changes (e.g. LittleFS size shrink).
  migrate_fs_layout_if_needed(partition_label, partition);

  mros::platform::FsMountConfig mount_config = {};
  mount_config.base_path = "/littlefs";
  mount_config.max_open_files = 10U;
  mount_config.partition_label = partition_label;

  bool mounted = mros::platform::mros_fs_mount(mount_config);
  if (!mounted) {
    mros_console.printf(
        "[STORAGE] Mount failed on '%s'. Running explicit recovery format...\n",
        partition_label);
    mros::platform::mros_fs_unmount();
    format_fs_partition(partition_label, partition);
    mounted = mros::platform::mros_fs_mount(mount_config);
  }

  if (!mounted) {
    mros_console.printf(
        "[STORAGE] Recovery mount failed on '%s'. Trying format-on-fail mount...\n",
        partition_label);
    mros::platform::mros_fs_unmount();
    mount_config.format_if_mount_failed = true;
    mounted = mros::platform::mros_fs_mount(mount_config);
  }

  if (!mounted) {
    mros_console.printf("[STORAGE] LittleFS mount failed on '%s'\n",
                        partition_label);
    return;
  }

  g_fs_mounted = true;
  mark_fs_mount_ok();
  mros_console.printf("[STORAGE] LittleFS mounted on '%s'\n", partition_label);
  migrate_legacy_user_files();
  run_boot_credential_migrations_once();
  if (!init_write_batch_buffers()) {
    mros_console.println("[STORAGE] LittleFS write batching disabled.");
  }
  init_csv_log_buffer();
}

void logger_service(unsigned long now_ms) {
  // Flush CSV log buffer periodically
  if (g_csv_log_ready && g_csv_log_len > 0 &&
      (now_ms - g_csv_log_last_flush_ms) >= kCsvLogFlushIntervalMs) {
    csv_log_flush_to_flash();
  }
  if (!g_write_batch_ready || !storage_ready()) {
    return;
  }
  if (now_ms - g_last_batch_flush_ms < kWriteBatchFlushPeriodMs) {
    return;
  }
  logger_flush_pending();
}

void logger_set_task_handle(TaskHandle_t task_handle) {
  g_storage_task_handle = task_handle;
  (void)ensure_storage_queue_ready();
}

void logger_notify_task() {
  if (g_storage_task_handle != nullptr) {
    xTaskNotifyGive(g_storage_task_handle);
  }
}

void logger_process_pending() {
  StorageCommand command = {};
  while (g_storage_queue != nullptr &&
         xQueueReceive(g_storage_queue, &command, 0) == pdTRUE) {
    increment_counter(&g_storage_processed_count);
    switch (command.type) {
      case StorageCommandType::SaveWifi: {
        NvsNamespace prefs;
        if (open_default_nvs(&prefs, "wifi_cfg", false)) {
          prefs.set_u8("version", kWifiCfgVersion);
          prefs.set_string("ssid", command.key);
          prefs.set_string("pass", command.value);
        }
        break;
      }
      case StorageCommandType::SaveWifiRuntime: {
        NvsNamespace prefs;
        if (open_default_nvs(&prefs, "wifi_cfg", false)) {
          prefs.set_u8("version", kWifiCfgVersion);
          prefs.set_string("last_ssid", command.key);
          prefs.set_string("last_bssid", command.value);
          prefs.set_u8("last_ch", command.aux_u8);
          if (command.payload != nullptr) {
            prefs.set_string("last_pass", command.payload);
          }
        }
        break;
      }
      case StorageCommandType::SaveToken:
        {
          NvsNamespace prefs;
          if (open_user_nvs(&prefs, kTokenNamespace, false)) {
            prefs.set_string("token", command.key);
          }
        }
        break;
      case StorageCommandType::SaveCredentials:
        (void)save_credentials_user_nvs(String(command.key), String(command.value));
        break;
      case StorageCommandType::SavePid:
        if (storage_ready()) {
          char buffer[128] = {};
          const int written = std::snprintf(
              buffer, sizeof(buffer), "%.6g\n%.6g\n%.6g\n%.6g\n%.6g\n",
              static_cast<double>(command.pid[0]),
              static_cast<double>(command.pid[1]),
              static_cast<double>(command.pid[2]),
              static_cast<double>(command.pid[3]),
              static_cast<double>(command.pid[4]));
          if (written > 0 && static_cast<size_t>(written) < sizeof(buffer)) {
            (void)mros::platform::mros_file_write_all(
                kPidCfgPath,
                std::string_view(buffer, static_cast<size_t>(written)));
          }
        }
        break;
      case StorageCommandType::WriteTextFile:
        if (command.payload != nullptr) {
          const std::string path(command.key);
          const std::string payload(command.payload, command.payload_len);
          if (!enqueue_batched_write(path.c_str(), payload)) {
            increment_counter(&g_storage_direct_write_fallback_count);
            (void)write_file_direct(path.c_str(), payload);
          }
        }
        break;
    }

    if (command.payload != nullptr) {
      heap_caps_free(command.payload);
      command.payload = nullptr;
    }
  }

  logger_service(mros::platform::mros_millis());
}

void logger_flush_pending() {
  // Flush CSV log buffer first
  if (g_csv_log_ready && g_csv_log_len > 0) {
    csv_log_flush_to_flash();
  }
  if (!g_write_batch_ready || !storage_ready()) {
    return;
  }
  if (!lock_write_batch(pdMS_TO_TICKS(100))) {
    return;
  }
  flush_write_batch_locked();
  unlock_write_batch();
}

void logger_append_csv(const String &data) {
  if (!storage_ready()) {
    return;
  }

  // Use RAM-batched CSV logging to reduce flash I/O
  if (g_csv_log_ready && g_csv_log_buf) {
    const size_t needed = data.length() + 1; // +1 for newline
    // If this entry would overflow the buffer, flush first
    if (g_csv_log_len + needed >= kCsvLogBufSize) {
      csv_log_flush_to_flash();
    }
    // Append to RAM buffer
    if (g_csv_log_len + needed < kCsvLogBufSize) {
      memcpy(g_csv_log_buf + g_csv_log_len, data.c_str(), data.length());
      g_csv_log_len += data.length();
      g_csv_log_buf[g_csv_log_len++] = '\n';
      g_csv_log_buf[g_csv_log_len] = '\0';
      return;
    }
  }

  // Fallback: direct write if batching unavailable
  increment_counter(&g_storage_direct_write_fallback_count);
  size_t file_size = 0U;
  if (mros::platform::mros_fs_file_size(kLogsCsvPath, &file_size) &&
      file_size > kCsvLogMaxFileSize) {
    (void)mros::platform::mros_fs_remove(kLogsCsvPath);
  }
  std::string line = to_std_string(data);
  line.push_back('\n');
  (void)mros::platform::mros_file_append_all(kLogsCsvPath, line);
}

String logger_read_csv() {
  if (!storage_ready()) {
    return "";
  }
  // Flush pending CSV entries before reading
  if (g_csv_log_ready && g_csv_log_len > 0) {
    csv_log_flush_to_flash();
  }
  size_t fsize = 0U;
  if (!mros::platform::mros_fs_file_size(kLogsCsvPath, &fsize) || fsize == 0U) {
    return "";
  }
  FILE *file = mros::platform::mros_fs_open(kLogsCsvPath, "rb");
  if (file == nullptr) {
    return "";
  }
  // Read file via PSRAM-backed buffer to avoid 50KB internal SRAM allocation
  char *buf = static_cast<char *>(alloc_tolerant_buffer(fsize + 1));
  if (!buf) {
    std::string data;
    std::fclose(file);
    if (!mros::platform::mros_file_read_all(kLogsCsvPath, &data)) {
      return "";
    }
    return to_arduino_string(data);
  }
  const size_t rd = std::fread(buf, 1U, fsize, file);
  std::fclose(file);
  buf[rd] = '\0';
  String result = String(buf);
  heap_caps_free(buf);
  return result;
}

String logger_read_csv_tail(size_t max_bytes) {
  if (!storage_ready() || max_bytes == 0U) {
    return "";
  }
  if (g_csv_log_ready && g_csv_log_len > 0) {
    csv_log_flush_to_flash();
  }
  size_t fsize = 0U;
  if (!mros::platform::mros_fs_file_size(kLogsCsvPath, &fsize) || fsize == 0U) {
    return "";
  }
  const size_t read_size = std::min(fsize, max_bytes);
  FILE *file = mros::platform::mros_fs_open(kLogsCsvPath, "rb");
  if (file == nullptr) {
    return "";
  }
  const long offset = static_cast<long>(fsize - read_size);
  if (offset > 0L && std::fseek(file, offset, SEEK_SET) != 0) {
    std::fclose(file);
    return "";
  }
  char *buf = static_cast<char *>(alloc_tolerant_buffer(read_size + 1U));
  if (buf == nullptr) {
    std::fclose(file);
    return "";
  }
  const size_t rd = std::fread(buf, 1U, read_size, file);
  std::fclose(file);
  buf[rd] = '\0';
  String result(buf);
  heap_caps_free(buf);
  return result;
}

void logger_clear_csv() {
  if (!storage_ready()) {
    return;
  }
  (void)mros::platform::mros_fs_remove(kLogsCsvPath);
}

void prefs_save_wifi(const String &ssid, const String &pass) {
  StorageCommand command = {};
  command.type = StorageCommandType::SaveWifi;
  ssid.substring(0, sizeof(command.key) - 1).toCharArray(command.key, sizeof(command.key));
  pass.substring(0, sizeof(command.value) - 1).toCharArray(command.value, sizeof(command.value));
  if (enqueue_storage_command(&command)) {
    return;
  }
  NvsNamespace prefs;
  if (!open_default_nvs(&prefs, "wifi_cfg", false)) {
    return;
  }
  prefs.set_u8("version", kWifiCfgVersion);
  prefs.set_string("ssid", to_std_string(ssid));
  prefs.set_string("pass", to_std_string(pass));
}

bool prefs_load_wifi(String &ssid, String &pass) {
  NvsNamespace prefs;
  if (!open_default_nvs(&prefs, "wifi_cfg", true)) {
    ssid = "";
    pass = "";
    return false;
  }
  std::string stored_ssid;
  std::string stored_pass;
  if (!prefs.get_string("ssid", &stored_ssid) ||
      !prefs.get_string("pass", &stored_pass)) {
    ssid = "";
    pass = "";
    return false;
  }
  ssid = to_arduino_string(stored_ssid);
  pass = to_arduino_string(stored_pass);
  return (ssid.length() > 0);
}

void prefs_save_wifi_runtime(const String &ssid, const String &bssid,
                             uint8_t channel, const String &pass) {
  StorageCommand command = {};
  command.type = StorageCommandType::SaveWifiRuntime;
  ssid.substring(0, sizeof(command.key) - 1)
      .toCharArray(command.key, sizeof(command.key));
  bssid.substring(0, sizeof(command.value) - 1)
      .toCharArray(command.value, sizeof(command.value));
  command.aux_u8 = channel;
  command.payload = alloc_payload_copy(pass);
  command.payload_len = pass.length();
  if (enqueue_storage_command(&command)) {
    return;
  }
  NvsNamespace prefs;
  if (!open_default_nvs(&prefs, "wifi_cfg", false)) {
    return;
  }
  prefs.set_u8("version", kWifiCfgVersion);
  prefs.set_string("last_ssid", to_std_string(ssid));
  prefs.set_string("last_bssid", to_std_string(bssid));
  prefs.set_u8("last_ch", channel);
  prefs.set_string("last_pass", to_std_string(pass));
}

bool prefs_load_wifi_runtime(String &ssid, String &bssid, uint8_t &channel,
                             String *pass) {
  channel = 0;
  NvsNamespace prefs;
  if (!open_default_nvs(&prefs, "wifi_cfg", true)) {
    ssid = "";
    bssid = "";
    if (pass != nullptr) {
      *pass = "";
    }
    return false;
  }
  std::string stored_ssid;
  std::string stored_bssid;
  if (!prefs.get_string("last_ssid", &stored_ssid) ||
      !prefs.get_string("last_bssid", &stored_bssid) ||
      !prefs.get_u8("last_ch", &channel)) {
    ssid = "";
    bssid = "";
    channel = 0;
    if (pass != nullptr) {
      *pass = "";
    }
    return false;
  }
  ssid = to_arduino_string(stored_ssid);
  bssid = to_arduino_string(stored_bssid);
  if (pass != nullptr) {
    std::string stored_pass;
    *pass = prefs.get_string("last_pass", &stored_pass)
                ? to_arduino_string(stored_pass)
                : "";
  }
  return (ssid.length() > 0 && bssid.length() > 0 && channel > 0);
}

void prefs_save_token(const String &token) {
  StorageCommand command = {};
  command.type = StorageCommandType::SaveToken;
  token.substring(0, sizeof(command.key) - 1).toCharArray(command.key, sizeof(command.key));
  if (enqueue_storage_command(&command)) {
    return;
  }
  NvsNamespace prefs;
  if (!open_user_nvs(&prefs, kTokenNamespace, false)) {
    return;
  }
  prefs.set_string("token", to_std_string(token));
}

String prefs_load_token() {
  NvsNamespace prefs;
  if (!open_user_nvs(&prefs, kTokenNamespace, true)) {
    return "";
  }
  std::string token;
  if (!prefs.get_string("token", &token)) {
    return "";
  }
  return to_arduino_string(token);
}

void config_write_json(const String &json_data) {
  const std::string json = to_std_string(json_data);
  if (logger_write_text_file_atomic(kConfigJsonPath, json_data)) {
    return;
  }
  if (logger_enqueue_text_file_write(kConfigJsonPath, json_data)) {
    return;
  }
  if (!storage_ready()) {
    return;
  }
  if (enqueue_batched_write(kConfigJsonPath, json)) {
    return;
  }
  increment_counter(&g_storage_direct_write_fallback_count);
  if (!write_file_direct(kConfigJsonPath, json)) {
    mros_console.printf("[STORAGE] Direct write failed for %s\n", kConfigJsonPath);
  }
}

String config_read_json() {
  if (!storage_ready()) {
    return "{}";
  }
  if (g_write_batch_ready && lock_write_batch()) {
    const int slot_index = find_write_batch_slot(kConfigJsonPath);
    if (slot_index >= 0 && g_write_batch[slot_index].dirty) {
      String pending = String(g_write_batch[slot_index].payload);
      unlock_write_batch();
      return pending;
    }
    unlock_write_batch();
  }
  size_t fsize = 0U;
  if (!mros::platform::mros_fs_file_size(kConfigJsonPath, &fsize) || fsize == 0U) {
    return "{}"; // Empty default JSON
  }
  FILE *file = mros::platform::mros_fs_open(kConfigJsonPath, "rb");
  if (file == nullptr) {
    return "{}";
  }
  // Read via PSRAM-backed buffer to protect internal SRAM
  char *buf = static_cast<char *>(alloc_tolerant_buffer(fsize + 1));
  if (!buf) {
    std::string data;
    std::fclose(file);
    if (!mros::platform::mros_file_read_all(kConfigJsonPath, &data)) {
      return "{}";
    }
    return to_arduino_string(data);
  }
  const size_t rd = std::fread(buf, 1U, fsize, file);
  std::fclose(file);
  buf[rd] = '\0';
  String result = String(buf);
  heap_caps_free(buf);
  return result;
}

// ---- Credential Storage (NVS user partition primary + legacy migrations) ----
// pass_hash must be the hex-encoded SHA-256 digest of the plaintext password.

namespace {

static const char *kCredFilePath = "/auth_credentials.dat";
static const char *kCredTmpPath = "/auth_credentials.tmp";
static const char *kCredLegacyUserFilePath = "/ESPUSER/auth_credentials.dat";
static const char *kCredLegacyUserTmpPath = "/ESPUSER/auth_credentials.tmp";

static bool is_valid_hash_hex(const String &value) {
  if (value.length() != 64) {
    return false;
  }
  for (size_t i = 0; i < value.length(); i++) {
    if (!isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

static bool is_valid_credential_hash(const String &value) {
  if (is_valid_hash_hex(value)) {
    return true;
  }
  if (!value.startsWith("pbkdf2_sha256$") || value.length() > 160U) {
    return false;
  }
  const int p1 = value.indexOf('$');
  const int p2 = value.indexOf('$', p1 + 1);
  const int p3 = value.indexOf('$', p2 + 1);
  if (p1 <= 0 || p2 <= p1 + 1 || p3 <= p2 + 1 ||
      value.indexOf('$', p3 + 1) >= 0) {
    return false;
  }
  const String iterations = value.substring(p1 + 1, p2);
  const String salt_hex = value.substring(p2 + 1, p3);
  const String digest_hex = value.substring(p3 + 1);
  if (iterations.length() < 4U || iterations.length() > 8U ||
      salt_hex.length() < 16U || salt_hex.length() > 64U ||
      (salt_hex.length() % 2U) != 0U || digest_hex.length() != 64U) {
    return false;
  }
  for (size_t i = 0; i < iterations.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(iterations[i]))) {
      return false;
    }
  }
  for (size_t i = 0; i < salt_hex.length(); ++i) {
    if (!isxdigit(static_cast<unsigned char>(salt_hex[i]))) {
      return false;
    }
  }
  return is_valid_hash_hex(digest_hex);
}

static bool save_credentials_user_nvs(const String &user,
                                      const String &pass_hash) {
  NvsNamespace p;
  if (!open_user_nvs(&p, kCredNamespace, false)) {
    return false;
  }

  bool ok = p.set_string("user", to_std_string(user));
  ok = p.set_string("hash", to_std_string(pass_hash)) && ok;
  uint8_t ver = 0;
  (void)p.get_u8("version", &ver);
  if (ver < kCredCfgVersion) {
    ok = p.set_u8("version", kCredCfgVersion) && ok;
  }
  return ok;
}

static bool load_credentials_user_nvs(String &user, String &pass_hash) {
  NvsNamespace p;
  if (!open_user_nvs(&p, kCredNamespace, true)) {
    return false;
  }
  std::string stored_user;
  std::string stored_hash;
  if (!p.get_string("user", &stored_user) || !p.get_string("hash", &stored_hash)) {
    return false;
  }
  String user_string = to_arduino_string(stored_user);
  String hash_string = to_arduino_string(stored_hash);
  user_string.trim();
  hash_string.trim();
  hash_string.toLowerCase();

  if (user_string.length() == 0 || !is_valid_credential_hash(hash_string)) {
    return false;
  }

  user = user_string;
  pass_hash = hash_string;
  return true;
}

[[maybe_unused]] static bool save_credentials_file(const String &user,
                                                   const String &pass_hash) {
  char payload[256] = {};
  const int written = std::snprintf(payload, sizeof(payload), "%u\n%s\n%s\n",
                                    static_cast<unsigned>(user.length()),
                                    user.c_str(), pass_hash.c_str());
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(payload)) {
    return false;
  }
  if (!mros::platform::mros_file_write_all_atomic(kCredFilePath, kCredTmpPath,
                                                  std::string_view(payload,
                                                                   static_cast<size_t>(written)))) {
    return false;
  }
  return mros::platform::mros_fs_exists(kCredFilePath);
}

static bool load_credentials_file_path(const char *path, String &user, String &pass_hash) {
  if (path == nullptr || !mros::platform::mros_fs_exists(path)) {
    return false;
  }
  std::vector<std::string> lines;
  if (!mros::platform::mros_file_read_lines(path, &lines, 3U) ||
      lines.size() < 3U) {
    return false;
  }
  String len_line = to_arduino_string(lines[0]);
  String stored_user = to_arduino_string(lines[1]);
  String stored_hash = to_arduino_string(lines[2]);

  len_line.trim();
  stored_user.trim();
  stored_hash.trim();
  stored_hash.toLowerCase();

  if (stored_user.length() == 0 || !is_valid_credential_hash(stored_hash)) {
    return false;
  }

  const int declared_len = len_line.toInt();
  if (declared_len > 0 && static_cast<size_t>(declared_len) != stored_user.length()) {
    return false;
  }

  user = stored_user;
  pass_hash = stored_hash;
  return true;
}

static bool load_credentials_file(String &user, String &pass_hash) {
  return load_credentials_file_path(kCredFilePath, user, pass_hash) ||
         load_credentials_file_path(kCredLegacyUserFilePath, user, pass_hash);
}

static void cleanup_legacy_credential_files() {
  (void)mros::platform::mros_fs_remove(kCredFilePath);
  (void)mros::platform::mros_fs_remove(kCredTmpPath);
  (void)mros::platform::mros_fs_remove(kCredLegacyUserFilePath);
  (void)mros::platform::mros_fs_remove(kCredLegacyUserTmpPath);
}

static bool load_credentials_legacy_nvs(String &user, String &pass_hash) {
  NvsNamespace p;
  if (!open_default_nvs(&p, kCredNamespace, true)) {
    return false;
  }
  std::string legacy_user;
  std::string legacy_hash;
  if (!p.get_string("user", &legacy_user) || !p.get_string("hash", &legacy_hash)) {
    return false;
  }
  String legacy_user_string = to_arduino_string(legacy_user);
  String legacy_hash_string = to_arduino_string(legacy_hash);

  legacy_user_string.trim();
  legacy_hash_string.trim();
  legacy_hash_string.toLowerCase();
  if (legacy_user_string.length() == 0 || !is_valid_credential_hash(legacy_hash_string)) {
    return false;
  }

  user = legacy_user_string;
  pass_hash = legacy_hash_string;
  return true;
}

static bool has_wifi_legacy_payload() {
  NvsNamespace p;
  if (!open_default_nvs(&p, "wifi_cfg", true)) {
    return false;
  }
  return p.contains("ssid") && p.contains("pass");
}

static bool has_cred_legacy_payload() {
  NvsNamespace p;
  if (!open_default_nvs(&p, kCredNamespace, true)) {
    return false;
  }
  return p.contains("user") && p.contains("hash");
}

static void migrate_wifi_cfg_version_nvs_once() {
  if (!has_wifi_legacy_payload()) {
    return;
  }
  NvsNamespace p;
  if (!open_default_nvs(&p, "wifi_cfg", false)) {
    return;
  }
  uint8_t ver = 0;
  (void)p.get_u8("version", &ver);
  if (ver < kWifiCfgVersion) {
    p.set_u8("version", kWifiCfgVersion);
    mros_console.printf("[STORAGE] Migrated wifi_cfg version -> %u\n",
                        static_cast<unsigned>(kWifiCfgVersion));
  }
}

static void migrate_credentials_to_user_partition_once() {
  String user, hash;
  if (load_credentials_user_nvs(user, hash)) {
    cleanup_legacy_credential_files();
    set_credential_migration_status("nvs_sys_usr", false, "already_current");
    return;
  }

  bool migrated = false;
  if (storage_ready() && load_credentials_file(user, hash)) {
    migrated = save_credentials_user_nvs(user, hash);
    if (migrated) {
      set_credential_migration_status("littlefs", true, "none");
      mros_console.println(
          "[STORAGE] Migrated credentials from LittleFS to nvs_sys_usr.");
    } else {
      set_credential_migration_status("littlefs", false, "save_failed");
    }
  }
  if (!migrated && has_cred_legacy_payload() &&
      load_credentials_legacy_nvs(user, hash)) {
    migrated = save_credentials_user_nvs(user, hash);
    if (migrated) {
      set_credential_migration_status("default_nvs", true, "none");
      mros_console.println(
          "[STORAGE] Migrated credentials from default NVS to nvs_sys_usr.");
    } else {
      set_credential_migration_status("default_nvs", false, "save_failed");
    }
  }
  if (!migrated) {
    if (g_credential_migration_error[0] == '\0' ||
        strcmp(g_credential_migration_error, "not_run") == 0) {
      set_credential_migration_status("none", false, "not_found");
    }
  }
  if (migrated) {
    cleanup_legacy_credential_files();
  }

  NvsNamespace p;
  if (open_user_nvs(&p, kCredNamespace, false)) {
    uint8_t ver = 0;
    (void)p.get_u8("version", &ver);
    if (ver < kCredCfgVersion) {
      p.set_u8("version", kCredCfgVersion);
      mros_console.printf("[STORAGE] Migrated cred_cfg version -> %u\n",
                          static_cast<unsigned>(kCredCfgVersion));
    }
  }
}

static void migrate_session_token_to_user_partition_once() {
  NvsNamespace user_part;
  if (!open_user_nvs(&user_part, kTokenNamespace, false)) {
    return;
  }
  const bool has_user_token = user_part.contains("token");
  if (has_user_token) {
    return;
  }

  NvsNamespace legacy_part;
  if (!open_default_nvs(&legacy_part, kTokenNamespace, true)) {
    return;
  }
  std::string legacy_token_std;
  if (!legacy_part.get_string("token", &legacy_token_std)) {
    return;
  }
  String legacy_token = to_arduino_string(legacy_token_std);
  if (legacy_token.length() == 0) {
    return;
  }

  if (open_user_nvs(&user_part, kTokenNamespace, false)) {
    user_part.set_string("token", to_std_string(legacy_token));
    mros_console.println("[STORAGE] Migrated session token to nvs_sys_usr.");
  }
}

static void run_boot_credential_migrations_once() {
  if (g_boot_credential_migration_done) {
    return;
  }
  g_boot_credential_migration_done = true;
  migrate_wifi_cfg_version_nvs_once();
  migrate_credentials_to_user_partition_once();
  migrate_session_token_to_user_partition_once();
}

} // namespace

const char *logger_user_root_path() { return kUserRootPath; }

String logger_user_path(const char *relative_name) {
  if (relative_name == nullptr || relative_name[0] == '\0') {
    return String(kUserRootPath);
  }
  String out = String(kUserRootPath);
  if (relative_name[0] != '/') {
    out += '/';
  }
  out += relative_name;
  return out;
}

void prefs_save_credentials(const String &user, const String &pass_hash) {
  String clean_user = user;
  String clean_hash = pass_hash;
  clean_user.replace("\r", "");
  clean_user.replace("\n", "");
  clean_user.trim();
  clean_hash.trim();
  clean_hash.toLowerCase();

  if (clean_user.length() == 0 || !is_valid_credential_hash(clean_hash)) {
    return;
  }

  // Authentication changes must be visible to the next HTTP request immediately.
  // The generic storage queue can lag long enough to make setup/login look failed.
  if (!save_credentials_user_nvs(clean_user, clean_hash)) {
    mros_console.println("[STORAGE] Failed to save credentials in nvs_sys_usr.");
  }
}

bool prefs_load_credentials(String &user, String &pass_hash) {
  user = "";
  pass_hash = "";
  run_boot_credential_migrations_once();
  if (load_credentials_user_nvs(user, pass_hash)) {
    return true;
  }
  return false;
}

bool prefs_clear_credentials() {
  bool ok = true;
  NvsNamespace user_part;
  if (open_user_nvs(&user_part, kCredNamespace, false)) {
    ok = user_part.erase_key("user") && ok;
    ok = user_part.erase_key("hash") && ok;
    ok = user_part.erase_key("version") && ok;
  } else {
    ok = false;
  }

  NvsNamespace legacy_part;
  if (open_default_nvs(&legacy_part, kCredNamespace, false)) {
    ok = legacy_part.erase_key("user") && ok;
    ok = legacy_part.erase_key("hash") && ok;
    ok = legacy_part.erase_key("version") && ok;
  }

  cleanup_legacy_credential_files();
  return ok;
}

// ---- PID Gain Storage (LittleFS) ----

void prefs_save_pid(float kp, float ki, float kd, float imax, float dspc) {
  StorageCommand command = {};
  command.type = StorageCommandType::SavePid;
  command.pid[0] = kp;
  command.pid[1] = ki;
  command.pid[2] = kd;
  command.pid[3] = imax;
  command.pid[4] = dspc;
  if (enqueue_storage_command(&command)) {
    return;
  }
  if (!storage_ready()) return;
  char payload[160] = {};
  const int written = std::snprintf(payload, sizeof(payload),
                                    "%.7g\n%.7g\n%.7g\n%.7g\n%.7g\n",
                                    static_cast<double>(kp),
                                    static_cast<double>(ki),
                                    static_cast<double>(kd),
                                    static_cast<double>(imax),
                                    static_cast<double>(dspc));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(payload) ||
      !mros::platform::mros_file_write_all(
          kPidCfgPath,
          std::string_view(payload, static_cast<size_t>(written)))) {
    mros_console.printf("[STORAGE] Failed to write %s\n", kPidCfgPath);
    return;
  }
  mros_console.println("[STORAGE] PID configuration saved.");
}

bool prefs_load_pid(float &kp, float &ki, float &kd, float &imax, float &dspc) {
  if (!storage_ready() || !mros::platform::mros_fs_exists(kPidCfgPath)) {
    return false;
  }
  std::vector<std::string> lines;
  if (!mros::platform::mros_file_read_lines(kPidCfgPath, &lines, 5U) ||
      lines.size() < 5U) {
    return false;
  }
  kp = std::strtof(lines[0].c_str(), nullptr);
  ki = std::strtof(lines[1].c_str(), nullptr);
  kd = std::strtof(lines[2].c_str(), nullptr);
  imax = std::strtof(lines[3].c_str(), nullptr);
  dspc = std::strtof(lines[4].c_str(), nullptr);
  mros_console.println("[STORAGE] PID configuration loaded from LittleFS.");
  return true;
}

bool logger_storage_ready() {
  return storage_ready();
}

bool logger_storage_info(uint64_t *total_bytes, uint64_t *used_bytes) {
  if (!storage_ready()) {
    if (total_bytes != nullptr) {
      *total_bytes = 0U;
    }
    if (used_bytes != nullptr) {
      *used_bytes = 0U;
    }
    return false;
  }
  return mros::platform::mros_fs_info(total_bytes, used_bytes);
}

void logger_get_diag_snapshot(LoggerDiagSnapshot *snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->queue_capacity = kStorageQueueLength;
  snapshot->queue_depth =
      (g_storage_queue != nullptr) ? uxQueueMessagesWaiting(g_storage_queue) : 0U;
  snapshot->queue_high_watermark = g_storage_queue_high_watermark;
  snapshot->enqueue_count = g_storage_enqueue_count;
  snapshot->drop_count = g_storage_drop_count;
  snapshot->processed_count = g_storage_processed_count;
  snapshot->batched_write_count = g_storage_batched_write_count;
  snapshot->batch_flush_count = g_storage_batch_flush_count;
  snapshot->direct_write_fallback_count = g_storage_direct_write_fallback_count;
  snapshot->csv_flush_count = g_storage_csv_flush_count;
  snapshot->atomic_write_count = g_storage_atomic_write_count;
  snapshot->atomic_write_fail_count = g_storage_atomic_write_fail_count;
  snapshot->last_atomic_write_ok = g_storage_last_atomic_write_ok;
  std::snprintf(snapshot->last_atomic_write_error,
                sizeof(snapshot->last_atomic_write_error),
                "%s",
                g_storage_last_atomic_write_error);
  snapshot->migration_attempted = g_credential_migration_attempted;
  snapshot->migration_migrated = g_credential_migration_migrated;
  std::snprintf(snapshot->migration_source,
                sizeof(snapshot->migration_source),
                "%s",
                g_credential_migration_source);
  std::snprintf(snapshot->migration_error,
                sizeof(snapshot->migration_error),
                "%s",
                g_credential_migration_error);
}

bool logger_write_text_file_atomic(const String &path, const String &payload) {
  if (path.length() == 0 || path.length() >= kStorageCommandKeyMax) {
    increment_counter(&g_storage_atomic_write_fail_count);
    set_atomic_write_status(false, "bad_path");
    return false;
  }
  if (!storage_ready()) {
    increment_counter(&g_storage_atomic_write_fail_count);
    set_atomic_write_status(false, "not_ready");
    return false;
  }
  const String tmp_path = path + ".tmp-" + String(mros::platform::mros_millis());
  const std::string data = to_std_string(payload);
  if (!mros::platform::mros_file_write_all(
          tmp_path.c_str(), std::string_view(data.data(), data.size()))) {
    (void)mros::platform::mros_fs_remove(tmp_path.c_str());
    increment_counter(&g_storage_atomic_write_fail_count);
    set_atomic_write_status(false, "write_failed");
    return false;
  }
  if (!mros::platform::mros_fs_rename(tmp_path.c_str(), path.c_str())) {
    (void)mros::platform::mros_fs_remove(tmp_path.c_str());
    increment_counter(&g_storage_atomic_write_fail_count);
    set_atomic_write_status(false, "rename_failed");
    return false;
  }
  increment_counter(&g_storage_atomic_write_count);
  set_atomic_write_status(true, "ok");
  return true;
}

bool logger_enqueue_text_file_write(const String &path, const String &payload) {
  if (path.length() == 0 || path.length() >= kStorageCommandKeyMax) {
    return false;
  }
  StorageCommand command = {};
  command.type = StorageCommandType::WriteTextFile;
  path.toCharArray(command.key, sizeof(command.key));
  command.payload = alloc_payload_copy(payload);
  if (command.payload == nullptr) {
    return false;
  }
  command.payload_len = payload.length();
  return enqueue_storage_command(&command);
}
