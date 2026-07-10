#include "src/shell/mros_shell_internal.h"

#include <esp_heap_caps.h>
#include <esp_chip_info.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include "src/comm_interfaces/spi/spi_c3_master.h"
#include "src/comm_interfaces/spi/spi_t41_link.h"
#include "src/comm_interfaces/uart/uart_cobs.h"
#include "src/drivers/i2c_pca9685.h"
#include "src/drivers/storage/logger_driver.h"
#include "src/net/mcp_service.h"
#include "src/net/ssh_service.h"
#include "src/platform/mros_system.h"
#include "src/platform/mros_time.h"
#include "src/security/ssh_identity.h"
#include "src/security/user_profile.h"
#include "src/web/server/web_server.h"
#include "src/web/server/websocket_handler.h"
#include "src/web/server/wifi_manager.h"

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

namespace mros::shell {
namespace {

bool get_storage_usage(uint64_t* total_bytes, uint64_t* used_bytes) {
  if (total_bytes == nullptr || used_bytes == nullptr) {
    return false;
  }
  if (!logger_storage_ready()) {
    *total_bytes = 0U;
    *used_bytes = 0U;
    return false;
  }
  return logger_storage_info(total_bytes, used_bytes);
}

struct FsSnapshot {
  std::string label;
  std::string fstype;
  std::string mount;
  uint64_t total = 0U;
  uint64_t used = 0U;
  bool usage_known = false;
};

struct StaticLibVersion {
  const char* name;
  const char* version;
};

struct TaskRuntimeSummary {
  bool available = false;
  uint32_t task_count = 0U;
  uint32_t core0_percent = 0U;
  uint32_t core1_percent = 0U;
  uint32_t min_stack_words = 0U;
  uint32_t low_stack_task_count = 0U;
};

struct RecoverySnapshot {
  bool present = false;
  bool valid = false;
  std::string label = "recovery";
  std::string version = "-";
  uint32_t size_kb = 0U;
  uint32_t used_kb = 0U;
  uint32_t free_kb = 0U;
};

enum class LogoMode {
  None,
  Small,
  Big,
};

const char* safe_text(const char* text, const char* fallback) {
  return (text != nullptr && text[0] != '\0') ? text : fallback;
}

std::string format_size(const uint64_t bytes) {
  char buffer[32] = {};
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

uint32_t percent_or_zero(const uint64_t used, const uint64_t total) {
  return total == 0U ? 0U : static_cast<uint32_t>((used * 100U) / total);
}

std::string format_yyyymmdd_from_build_date() {
  static constexpr const char* kMonths[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };
  const char* raw = __DATE__;
  if (raw == nullptr || std::strlen(raw) < 11U) {
    return "00000000";
  }

  int month = 1;
  for (size_t i = 0U; i < (sizeof(kMonths) / sizeof(kMonths[0])); ++i) {
    if (std::strncmp(raw, kMonths[i], 3U) == 0) {
      month = static_cast<int>(i + 1U);
      break;
    }
  }
  const int day = std::atoi(raw + 4);
  const int year = std::atoi(raw + 7);
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d", year, month, day);
  return buffer;
}

std::string normalize_firmware_version_text(const char* raw_version) {
  const std::string input = safe_text(raw_version, "v0");
  if (input.empty()) {
    return "v0";
  }
  if ((input[0] == 'v' || input[0] == 'V') && input.size() > 1U) {
    return "v" + input.substr(1U);
  }

  const size_t version_pos = input.rfind("_V");
  if (version_pos != std::string::npos && (version_pos + 2U) < input.size()) {
    const std::string tail = input.substr(version_pos + 2U);
    bool all_digits = !tail.empty();
    for (const char ch : tail) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        all_digits = false;
        break;
      }
    }
    if (all_digits) {
      return "v" + tail;
    }
  }
  return input;
}

std::string chip_series_name() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  return "ESP32S3";
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
  return "ESP32P4";
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  return "ESP32C6";
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  return "ESP32C3";
#else
  esp_chip_info_t chip_info {};
  esp_chip_info(&chip_info);
  switch (chip_info.model) {
    case CHIP_ESP32S3:
      return "ESP32S3";
    case CHIP_ESP32C3:
      return "ESP32C3";
#ifdef CHIP_ESP32C6
    case CHIP_ESP32C6:
      return "ESP32C6";
#endif
#ifdef CHIP_ESP32P4
    case CHIP_ESP32P4:
      return "ESP32P4";
#endif
    default:
      return "ESP32";
  }
#endif
}

uint32_t bytes_to_mb_rounded(const uint64_t bytes) {
  return bytes == 0U ? 0U : static_cast<uint32_t>((bytes + (512U * 1024U)) / (1024U * 1024U));
}

uint32_t canonical_capacity_mb(const uint64_t bytes) {
  static constexpr std::array<uint32_t, 8> kCapacityStepsMb = {
      1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U,
  };
  const uint32_t rounded = bytes_to_mb_rounded(bytes);
  if (rounded == 0U) {
    return 0U;
  }
  for (const uint32_t step : kCapacityStepsMb) {
    if (rounded <= step) {
      return step;
    }
  }
  return rounded;
}

uint64_t physical_psram_total_bytes() {
  return static_cast<uint64_t>(canonical_capacity_mb(static_cast<uint64_t>(mros::platform::mros_system_psram_total()))) *
         1024ULL * 1024ULL;
}

std::string detected_board_model() {
  const std::string series = chip_series_name();
  const uint32_t flash_mb = canonical_capacity_mb(static_cast<uint64_t>(mros::platform::mros_system_flash_total()));
  const uint32_t psram_mb = canonical_capacity_mb(static_cast<uint64_t>(mros::platform::mros_system_psram_total()));
  std::string model = series;
  if (flash_mb > 0U) {
    model += "N";
    model += std::to_string(flash_mb);
  }
  if (psram_mb > 0U) {
    model += "R";
    model += std::to_string(psram_mb);
  }
  return model;
}

std::string device_name_text() {
  const mros::ssh::IdentityConfig identity = mros::ssh::identity_get();
  if (identity.device_name.length() > 0) {
    return identity.device_name.c_str();
  }
  return "DEUSCARA-S3V";
}

uint32_t estimate_partition_used_bytes(const esp_partition_t* partition) {
  if (partition == nullptr || partition->size == 0U) {
    return 0U;
  }
  std::array<uint8_t, 1024> buffer = {};
  uint32_t highest_used = 0U;
  for (uint32_t offset = 0U; offset < partition->size; offset += buffer.size()) {
    const size_t read_len =
        std::min<size_t>(buffer.size(), static_cast<size_t>(partition->size - offset));
    if (esp_partition_read(partition, offset, buffer.data(), read_len) != ESP_OK) {
      break;
    }
    for (size_t i = read_len; i > 0U; --i) {
      if (buffer[i - 1U] != 0xFFU) {
        highest_used = offset + static_cast<uint32_t>(i);
        break;
      }
    }
  }
  return highest_used;
}

RecoverySnapshot collect_recovery_snapshot() {
  RecoverySnapshot out {};
  const esp_partition_t* recovery = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "recovery");
  if (recovery == nullptr) {
    return out;
  }
  out.present = true;
  out.label = recovery->label[0] != '\0' ? recovery->label : "recovery";
  out.size_kb = recovery->size / 1024U;

  esp_app_desc_t desc {};
  if (esp_ota_get_partition_description(recovery, &desc) == ESP_OK) {
    out.valid = true;
    out.version = safe_text(desc.version, "-");
  }

  const uint32_t used = estimate_partition_used_bytes(recovery);
  out.used_kb = (used + 1023U) / 1024U;
  out.free_kb = out.size_kb > out.used_kb ? (out.size_kb - out.used_kb) : 0U;
  return out;
}

std::string board_summary_text() {
  return detected_board_model();
}

std::string cpu_summary_text() {
  WebServerDiagSnapshot diag {};
  web_server_get_diag_snapshot(&diag);
  esp_chip_info_t chip_info {};
  esp_chip_info(&chip_info);
  if (chip_info.cores <= 1U) {
    return "Core0: " + std::to_string(static_cast<unsigned long>(diag.cpu_freq_core0_mhz)) + "MHz";
  }
  return "Core0: " + std::to_string(static_cast<unsigned long>(diag.cpu_freq_core0_mhz)) +
         "MHz  Core1: " + std::to_string(static_cast<unsigned long>(diag.cpu_freq_core1_mhz)) + "MHz";
}

std::string memory_usage_text(const uint64_t total, const uint64_t free) {
  const uint64_t used = total >= free ? (total - free) : 0U;
  return format_size(free) + " free / " + format_size(total) + " total (" +
         std::to_string(percent_or_zero(used, total)) + "% used)";
}

std::string storage_usage_text(const uint64_t total, const uint64_t used) {
  return format_size(used) + " / " + format_size(total) + " (" +
         std::to_string(percent_or_zero(used, total)) + "%)";
}

bool fetch_tr() { return mros::profile::is_turkish_locale(); }

std::string fetch_label(const char* key) {
  const std::string k = key != nullptr ? std::string(key) : std::string();
  if (!fetch_tr()) {
    if (k == "board") return "Board";
    if (k == "os") return "OS";
    if (k == "firmware") return "Firmware";
    if (k == "host") return "Host";
    if (k == "uptime") return "Uptime";
    if (k == "shell") return "Shell";
    if (k == "kernel") return "Kernel";
    if (k == "conn") return "Connection";
    if (k == "tasks") return "Tasks";
    if (k == "cpu") return "CPU";
    if (k == "sram") return "SRAM";
    if (k == "psram") return "PSRAM";
    if (k == "app") return "Application";
    if (k == "recovery") return "Recovery";
    if (k == "storage") return "Storage";
    if (k == "sdk") return "SDK";
    if (k == "services") return "Services";
    return k;
  }
  if (k == "board") return "Kart";
  if (k == "os") return "İS";
  if (k == "firmware") return "Yazılım";
  if (k == "host") return "Üstlenici";
  if (k == "uptime") return "Uyanık Zaman";
  if (k == "shell") return "Kabuk";
  if (k == "kernel") return "Çekirdek";
  if (k == "conn") return "Bağlantı";
  if (k == "tasks") return "İşlemler";
  if (k == "cpu") return "CPU";
  if (k == "sram") return "SRAM";
  if (k == "psram") return "PSRAM";
  if (k == "app") return "Uygulama";
  if (k == "recovery") return "Kurtarma";
  if (k == "storage") return "Depolama";
  if (k == "sdk") return "SDK";
  if (k == "services") return "Servisler";
  return k;
}

size_t terminal_display_width(const std::string& value);

std::string centered_text(const std::string& text, const size_t width) {
  const size_t text_width = terminal_display_width(text);
  if (text_width >= width) {
    return text;
  }
  const size_t total_padding = width - text_width;
  const size_t left_padding = total_padding / 2U;
  const size_t right_padding = total_padding - left_padding;
  return std::string(left_padding, ' ') + text + std::string(right_padding, ' ');
}

size_t terminal_display_width(const std::string& value) {
  size_t width = 0U;
  for (size_t i = 0U; i < value.size();) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    uint32_t cp = c;
    size_t step = 1U;
    if ((c & 0xE0U) == 0xC0U && i + 1U < value.size()) {
      cp = ((c & 0x1FU) << 6U) | (static_cast<unsigned char>(value[i + 1U]) & 0x3FU);
      step = 2U;
    } else if ((c & 0xF0U) == 0xE0U && i + 2U < value.size()) {
      cp = ((c & 0x0FU) << 12U) |
           ((static_cast<unsigned char>(value[i + 1U]) & 0x3FU) << 6U) |
           (static_cast<unsigned char>(value[i + 2U]) & 0x3FU);
      step = 3U;
    } else if ((c & 0xF8U) == 0xF0U && i + 3U < value.size()) {
      cp = ((c & 0x07U) << 18U) |
           ((static_cast<unsigned char>(value[i + 1U]) & 0x3FU) << 12U) |
           ((static_cast<unsigned char>(value[i + 2U]) & 0x3FU) << 6U) |
           (static_cast<unsigned char>(value[i + 3U]) & 0x3FU);
      step = 4U;
    }

    if (cp == '\t') {
      width += 4U;
    } else if (cp >= 0x20U && cp != 0x7FU && !(cp >= 0x0300U && cp <= 0x036FU)) {
      const bool wide =
          (cp >= 0x1100U && cp <= 0x115FU) ||
          (cp >= 0x2329U && cp <= 0x232AU) ||
          (cp >= 0x2E80U && cp <= 0xA4CFU) ||
          (cp >= 0xAC00U && cp <= 0xD7A3U) ||
          (cp >= 0xF900U && cp <= 0xFAFFU) ||
          (cp >= 0xFE10U && cp <= 0xFE19U) ||
          (cp >= 0xFE30U && cp <= 0xFE6FU) ||
          (cp >= 0xFF00U && cp <= 0xFF60U) ||
          (cp >= 0xFFE0U && cp <= 0xFFE6U);
      width += wide ? 2U : 1U;
    }
    i += step;
  }
  return width;
}

std::vector<std::string> make_chip_banner() {
  const std::string title = chip_series_name();
  const std::string subtitle = "MROS Robotics Project";
  const size_t inner_width = std::max(terminal_display_width(title), terminal_display_width(subtitle)) + 6U;

  std::vector<std::string> lines;
  lines.push_back("." + std::string(inner_width + 2U, '-') + ".");
  lines.push_back("| " + centered_text(title, inner_width) + " |");
  lines.push_back("| " + centered_text(subtitle, inner_width) + " |");
  lines.push_back("'" + std::string(inner_width + 2U, '-') + "'");
  return lines;
}

std::vector<std::string> make_small_logo_lines() {
  return {
      " ____  ____  ____  ____  ____ ",
      "(  __)/ ___)(  _ \\( __ \\(___ \\",
      " ) _) \\___ \\ ) __/ (__ ( / __/",
      "(____)(____/(__)  (____/(____)",
      " ____  ____  __ _  ____  ____ ",
      "/ ___)( __ \\(  ( \\( __ \\(___ \\",
      "\\___ \\ (__ (/    / (__ ( / __/",
      "(____/(____/\\_)__)(____/(____)",
      "",
      chip_series_name(),
      "MROS Robotics Project",
  };
}

std::vector<std::string> build_logo_block(const bool include_mros_logo, const LogoMode mode) {
  if (mode == LogoMode::None) {
    return {};
  }
  if (mode == LogoMode::Small) {
    return make_small_logo_lines();
  }

  static const char* kMrosDeuscaraLogo[] = {
      "  __  __ ____   ___  ____ _____ ____  ____  _____ ",
      " |  \\/  |  _ \\ / _ \\/ ___|___  |  _ \\|  _ \\|  ___|",
      " | |\\/| | |_) | | | \\___ \\ / /| | | | |_) | |_   ",
      " | |  | |  _ <| |_| |___) / /_| |_| |  _ <|  _|  ",
      " |_|  |_|_| \\_\\\\___/|____/____|____/|_| \\_\\_|    ",
      "                 MROS DEUSCARA                   ",
      "",
  };

  std::vector<std::string> lines;
  if (include_mros_logo) {
    for (const char* line : kMrosDeuscaraLogo) {
      lines.emplace_back(line);
    }
  }
  const std::vector<std::string> chip_banner = make_chip_banner();
  lines.insert(lines.end(), chip_banner.begin(), chip_banner.end());
  return lines;
}

void append_kv_line(std::vector<std::string>* lines, const char* key, const std::string& value) {
  if (lines == nullptr || key == nullptr) {
    return;
  }
  std::string clean_key = key;
  while (!clean_key.empty() && clean_key.back() == ' ') {
    clean_key.pop_back();
  }
  lines->push_back(fetch_label(clean_key.c_str()) + ": " + value);
}

std::vector<std::string> align_info_lines(const std::vector<std::string>& lines) {
  size_t key_width = 0U;
  for (const std::string& line : lines) {
    const size_t separator = line.find(':');
    if (separator != std::string::npos) {
      key_width = std::max(key_width, terminal_display_width(line.substr(0U, separator)));
    }
  }
  std::vector<std::string> out;
  out.reserve(lines.size());
  for (const std::string& line : lines) {
    const size_t separator = line.find(':');
    if (separator == std::string::npos) {
      out.push_back(line);
      continue;
    }
    const std::string key = line.substr(0U, separator);
    const size_t visible_key_width = terminal_display_width(key);
    out.push_back(key +
                  std::string(visible_key_width < key_width ? key_width - visible_key_width : 0U, ' ') +
                  line.substr(separator));
  }
  return out;
}

std::string format_logo_line(const ShellState& state, const std::string& line) {
  if (line.empty()) {
    return line;
  }
  if (line == chip_series_name()) {
    return shell_ansi_wrap(state, "38;5;81;1", line);
  }
  if (line == "MROS Robotics Project") {
    return shell_ansi_wrap(state, "38;5;223", line);
  }
  return shell_ansi_wrap(state, "38;5;112;1", line);
}

std::string format_info_line(const ShellState& state, const std::string& line) {
  const size_t separator = line.find(':');
  if (separator == std::string::npos) {
    return shell_ansi_wrap(state, "38;5;223", line);
  }
  const std::string key = line.substr(0U, separator);
  const std::string value = (separator + 1U) < line.size() ? line.substr(separator + 1U) : std::string();
  return shell_ansi_wrap(state, "38;5;246", key) +
         shell_ansi_wrap(state, "38;5;244", ":") +
         shell_ansi_wrap(state, "38;5;223", value);
}

bool can_render_side_by_side(
    const ShellState& state,
    const std::vector<std::string>& logo_lines,
    const std::vector<std::string>& info_lines) {
  if (logo_lines.empty() || info_lines.empty()) {
    return false;
  }
  size_t left_width = 0U;
  size_t right_width = 0U;
  for (const std::string& line : logo_lines) {
    left_width = std::max(left_width, terminal_display_width(line));
  }
  for (const std::string& line : info_lines) {
    right_width = std::max(right_width, terminal_display_width(line));
  }
  return shell_terminal_columns(state) >= (left_width + right_width + 3U);
}

TaskStatus_t* alloc_task_snapshot_buffer(const size_t count) {
  if (count == 0U) {
    return nullptr;
  }
  return static_cast<TaskStatus_t*>(
      heap_caps_malloc(sizeof(TaskStatus_t) * count,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

TaskRuntimeSummary collect_task_runtime_summary() {
  TaskRuntimeSummary summary {};
  const UBaseType_t task_count = uxTaskGetNumberOfTasks();
  if (task_count == 0U) {
    return summary;
  }

  const size_t snapshot_capacity = static_cast<size_t>(task_count + 8U);
  TaskStatus_t* tasks = alloc_task_snapshot_buffer(snapshot_capacity);
  if (tasks == nullptr) {
    return summary;
  }

  uint32_t total_runtime = 0U;
  const UBaseType_t actual_count = uxTaskGetSystemState(tasks, snapshot_capacity, &total_runtime);
  if (actual_count == 0U) {
    heap_caps_free(tasks);
    return summary;
  }

  uint64_t core0_runtime = 0U;
  uint64_t core1_runtime = 0U;
  uint32_t min_stack_words = UINT32_MAX;
  uint32_t low_stack_tasks = 0U;

  for (UBaseType_t i = 0U; i < actual_count; ++i) {
    const TaskStatus_t& task = tasks[i];
    if (static_cast<uint32_t>(task.usStackHighWaterMark) < min_stack_words) {
      min_stack_words = static_cast<uint32_t>(task.usStackHighWaterMark);
    }
    if (static_cast<uint32_t>(task.usStackHighWaterMark) < 512U) {
      ++low_stack_tasks;
    }
#if defined(INCLUDE_xTaskGetCoreID) && (INCLUDE_xTaskGetCoreID == 1)
    const BaseType_t core_id = xTaskGetCoreID(task.xHandle);
    if (core_id == 0) {
      core0_runtime += task.ulRunTimeCounter;
    } else if (core_id == 1) {
      core1_runtime += task.ulRunTimeCounter;
    }
#endif
  }

  summary.available = total_runtime > 0U;
  summary.task_count = static_cast<uint32_t>(actual_count);
  summary.min_stack_words = (min_stack_words == UINT32_MAX) ? 0U : min_stack_words;
  summary.low_stack_task_count = low_stack_tasks;
  if (total_runtime > 0U) {
    summary.core0_percent =
        static_cast<uint32_t>((core0_runtime * 100U) / static_cast<uint64_t>(total_runtime));
    summary.core1_percent =
        static_cast<uint32_t>((core1_runtime * 100U) / static_cast<uint64_t>(total_runtime));
  }
  heap_caps_free(tasks);
  return summary;
}

std::string connected_links_text(const ShellState& state) {
  std::vector<std::string> links;
  if (state.config.is_spi_connected_callback != nullptr &&
      state.config.is_spi_connected_callback(state.config.user_data)) {
    links.emplace_back("SPI");
  }
  if (state.config.is_espnow_connected_callback != nullptr &&
      state.config.is_espnow_connected_callback(state.config.user_data)) {
    links.emplace_back("ESPNOW");
  }
  if (links.empty()) {
    return {};
  }

  std::string output;
  for (size_t i = 0U; i < links.size(); ++i) {
    if (i > 0U) {
      output += ", ";
    }
    output += links[i];
  }
  return output;
}

std::string arduino_version_text() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && defined(ESP_ARDUINO_VERSION_MINOR) && defined(ESP_ARDUINO_VERSION_PATCH)
  char buffer[24] = {};
  std::snprintf(
      buffer,
      sizeof(buffer),
      "%d.%d.%d",
      ESP_ARDUINO_VERSION_MAJOR,
      ESP_ARDUINO_VERSION_MINOR,
      ESP_ARDUINO_VERSION_PATCH);
  return buffer;
#elif defined(ESP_ARDUINO_VERSION_STR)
  return ESP_ARDUINO_VERSION_STR;
#elif defined(ARDUINO_ESP32_RELEASE)
  return ARDUINO_ESP32_RELEASE;
#else
  return "unknown";
#endif
}

std::string freertos_version_text() {
#if defined(tskKERNEL_VERSION_NUMBER)
  return std::string("FreeRTOS ") + tskKERNEL_VERSION_NUMBER;
#elif defined(tskKERNEL_VERSION_MAJOR) && defined(tskKERNEL_VERSION_MINOR) && defined(tskKERNEL_VERSION_BUILD)
  char buffer[32] = {};
  std::snprintf(
      buffer,
      sizeof(buffer),
      "FreeRTOS V%d.%d.%d",
      tskKERNEL_VERSION_MAJOR,
      tskKERNEL_VERSION_MINOR,
      tskKERNEL_VERSION_BUILD);
  return buffer;
#else
  return "FreeRTOS unknown";
#endif
}

std::vector<std::string> collect_lib_versions() {
  std::vector<std::string> lines;
  static constexpr std::array<StaticLibVersion, 6> kPinnedComponents = {{
      {"joltwallet/littlefs", "1.20.4"},
      {"ESP32Async/AsyncTCP", "3.4.10"},
      {"ESP32Async/ESPAsyncWebServer", "3.10.3"},
      {"espressif/esp-dsp", "1.8.0"},
      {"espressif/esp-modbus", "2.1.2"},
      {"mshell", kShellVersion},
  }};

  append_kv_line(&lines, "idf                ", safe_text(mros::platform::mros_system_sdk_version(), "unknown"));
  append_kv_line(&lines, "arduino-esp32      ", arduino_version_text());
  append_kv_line(&lines, "freertos           ", freertos_version_text());
  for (const StaticLibVersion& item : kPinnedComponents) {
    append_kv_line(&lines, item.name, item.version);
  }
  return lines;
}

std::string uptime_text() {
  uint32_t seconds = mros::platform::mros_millis() / 1000U;
  const uint32_t days = seconds / 86400U;
  seconds %= 86400U;
  const uint32_t hours = seconds / 3600U;
  seconds %= 3600U;
  const uint32_t minutes = seconds / 60U;
  seconds %= 60U;
  char buffer[48] = {};
  if (days > 0U) {
    std::snprintf(buffer, sizeof(buffer), "%lud %luh %lum",
                  static_cast<unsigned long>(days),
                  static_cast<unsigned long>(hours),
                  static_cast<unsigned long>(minutes));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%luh %lum %lus",
                  static_cast<unsigned long>(hours),
                  static_cast<unsigned long>(minutes),
                  static_cast<unsigned long>(seconds));
  }
  return buffer;
}

const char* fs_name_for_subtype(const esp_partition_subtype_t subtype) {
  switch (subtype) {
    case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS:
      return "littlefs";
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
      return "spiffs";
    case ESP_PARTITION_SUBTYPE_DATA_FAT:
      return "fat16/fat32";
    default:
      return nullptr;
  }
}

std::vector<FsSnapshot> collect_fs_partitions() {
  std::vector<FsSnapshot> snapshots;
  uint64_t fs_total = 0U;
  uint64_t fs_used = 0U;
  const bool fs_ready = get_storage_usage(&fs_total, &fs_used);

  for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, nullptr);
       it != nullptr;
       it = esp_partition_next(it)) {
    const esp_partition_t* partition = esp_partition_get(it);
    if (partition == nullptr) {
      continue;
    }

    if (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS ||
        partition->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS) {
      continue;
    }

    const char* fs_name = fs_name_for_subtype(static_cast<esp_partition_subtype_t>(partition->subtype));
    if (fs_name == nullptr) {
      continue;
    }

    FsSnapshot snapshot {};
    snapshot.label = partition->label[0] != '\0' ? partition->label : "data";
    snapshot.fstype = fs_name;
    snapshot.mount = "-";
    snapshot.total = partition->size;
    if (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_LITTLEFS && fs_ready) {
      snapshot.mount = "/fs";
      snapshot.total = fs_total;
      snapshot.used = fs_used;
      snapshot.usage_known = true;
    }
    snapshots.push_back(snapshot);
  }

  std::sort(snapshots.begin(), snapshots.end(), [](const FsSnapshot& lhs, const FsSnapshot& rhs) {
    return lhs.label < rhs.label;
  });
  return snapshots;
}

std::string json_escape(const std::string& text) {
  std::string out;
  for (const char ch : text) {
    if (ch == '"' || ch == '\\') out.push_back('\\');
    if (ch == '\n') out += "\\n";
    else if (ch != '\r') out.push_back(ch);
  }
  return out;
}

void print_partition_table(ShellState& state) {
  shell_write_line(state, "label       type  subtype  offset    size");
  shell_write_line(state, "----------  ----  -------  --------  --------");
  for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
       it != nullptr;
       it = esp_partition_next(it)) {
    const esp_partition_t* p = esp_partition_get(it);
    if (p == nullptr) continue;
    shell_printf(state, "%-10s  %-4s  0x%02X     0x%06lX  %luK\n",
                 p->label,
                 p->type == ESP_PARTITION_TYPE_APP ? "app" : "data",
                 static_cast<unsigned>(p->subtype),
                 static_cast<unsigned long>(p->address),
                 static_cast<unsigned long>(p->size / 1024U));
  }
}

void print_bus_table(ShellState& state) {
  WifiManagerSnapshot wifi = {};
  wifi_manager_get_snapshot(&wifi);
  WebServerDiagSnapshot diag {};
  web_server_get_diag_snapshot(&diag);
  shell_write_line(state, "bus   peer       status      detail");
  shell_write_line(state, "----  ---------  ----------  --------------------------------");
  shell_printf(state, "QSPI  TEENSY4.1  %-10s  crc=%lu marker=%lu seq=%u\n",
               spi_s3_is_connected() ? "connected" : "down",
               static_cast<unsigned long>(spi_s3_get_crc_errors()),
               static_cast<unsigned long>(spi_s3_get_marker_errors()),
               static_cast<unsigned>(spi_s3_get_last_rx_seq()));
  shell_printf(state, "SPI   ESP32-C3   %-10s  hz=%u crc=%lu marker=%lu\n",
               spi_c3_is_connected() ? "connected" : "down",
               static_cast<unsigned>(spi_c3_get_loop_hz()),
               static_cast<unsigned long>(spi_c3_get_crc_errors()),
               static_cast<unsigned long>(spi_c3_get_marker_errors()));
  shell_printf(state, "I2C   PCA9685    %-10s  freq=%.1fHz oe=%s\n",
               pca9685_is_ready() ? "ready" : "down",
               pca9685_get_frequency(),
               pca9685_get_output_enable() ? "on" : "off");
  shell_printf(state, "WiFi  station    %-10s  ip=%s rssi=%ld\n",
               wifi.state.sta_connected ? "connected" : "down",
               wifi.ip.length() > 0 ? wifi.ip.c_str() : "-",
               static_cast<long>(wifi.state.rssi));
  shell_printf(state, "WS    runtime    %-10s  total=%lu legacy=%lu tele=%lu shell=%lu debug=%lu\n",
               diag.ws_clients_total > 0U ? "active" : "idle",
               static_cast<unsigned long>(diag.ws_clients_total),
               static_cast<unsigned long>(diag.ws_clients_legacy),
               static_cast<unsigned long>(diag.ws_clients_telemetry),
               static_cast<unsigned long>(diag.ws_clients_shell),
               static_cast<unsigned long>(diag.ws_clients_debug));
}

int print_health(ShellState& state) {
  uint64_t fs_total = 0U;
  uint64_t fs_used = 0U;
  const bool heap_ok = mros::platform::mros_system_heap_free() > 64U * 1024U;
  const bool psram_ok = mros::platform::mros_system_psram_total() > 0U &&
                        mros::platform::mros_system_psram_free() > 512U * 1024U;
  const bool fs_ok = get_storage_usage(&fs_total, &fs_used);
  const bool teensy_qspi_ok = spi_s3_is_connected();
  const bool c3_ok = spi_c3_is_connected();
  const bool pca_ok = pca9685_is_ready();
  const bool ok = heap_ok && psram_ok && fs_ok && teensy_qspi_ok && c3_ok && pca_ok;
  shell_printf(state, "overall : %s\n", ok ? "OK" : "WARN");
  shell_printf(state, "heap    : %s (%lu free)\n", heap_ok ? "OK" : "WARN",
               static_cast<unsigned long>(mros::platform::mros_system_heap_free()));
  shell_printf(state, "psram   : %s (%lu free)\n", psram_ok ? "OK" : "WARN",
               static_cast<unsigned long>(mros::platform::mros_system_psram_free()));
  shell_printf(state, "littlefs: %s\n", fs_ok ? "OK" : "WARN");
  shell_printf(state, "t41-qspi: %s\n", teensy_qspi_ok ? "OK" : "WARN");
  shell_printf(state, "c3-spi  : %s\n", c3_ok ? "OK" : "WARN");
  shell_printf(state, "pca9685 : %s\n", pca_ok ? "OK" : "WARN");
  return ok ? 0 : 2;
}

void print_boot(ShellState& state) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  const RecoverySnapshot recovery = collect_recovery_snapshot();
  shell_printf(state, "reset_reason : %d\n", static_cast<int>(esp_reset_reason()));
  shell_printf(state, "running      : %s\n", running != nullptr ? running->label : "(none)");
  shell_printf(state, "boot         : %s\n", boot != nullptr ? boot->label : "(none)");
  shell_printf(state, "next_update  : %s\n", next != nullptr ? next->label : "(none)");
  shell_printf(state, "recovery     : %s version=%s used=%luK free=%luK size=%luK\n",
               recovery.present ? (recovery.valid ? "installed" : "empty/invalid") : "missing",
               recovery.version.c_str(),
               static_cast<unsigned long>(recovery.used_kb),
               static_cast<unsigned long>(recovery.free_kb),
               static_cast<unsigned long>(recovery.size_kb));
}

void print_metric_table_row(
    ShellState& state,
    const char* metric,
    const char* status,
    const std::string& value,
    const std::string& detail) {
  shell_printf(
      state,
      "%-15s %-5s %-34s %s\n",
      metric != nullptr ? metric : "-",
      status != nullptr ? status : "-",
      value.c_str(),
      detail.c_str());
}

void print_full_metrics_table(ShellState& state) {
  const TaskRuntimeSummary task_summary = collect_task_runtime_summary();
  WebServerDiagSnapshot diag {};
  web_server_get_diag_snapshot(&diag);
  WifiManagerSnapshot wifi {};
  wifi_manager_get_snapshot(&wifi);

  const uint64_t psram_physical_total = physical_psram_total_bytes();
  const uint64_t psram_heap_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const uint64_t psram_free = static_cast<uint64_t>(mros::platform::mros_system_psram_free());
  const uint64_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  uint64_t littlefs_total = 0U;
  uint64_t littlefs_used = 0U;
  (void)get_storage_usage(&littlefs_total, &littlefs_used);
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  const RecoverySnapshot recovery = collect_recovery_snapshot();

  shell_write_line(state, "metric          st    value                              detail");
  shell_write_line(state, "--------------- ----- ---------------------------------- ----------------------------------------");

  if (task_summary.available) {
    print_metric_table_row(
        state,
        "cpu load",
        "ok",
        "Core0 " + std::to_string(task_summary.core0_percent) + "% Core1 " +
            std::to_string(task_summary.core1_percent) + "%",
        "tasks=" + std::to_string(task_summary.task_count));
  } else {
    print_metric_table_row(state, "cpu load", "n/a", "runtime counters unavailable", "FreeRTOS snapshot disabled");
  }

  print_metric_table_row(
      state,
      "task jitter",
      diag.pid_cycle_peak_ms <= 4U ? "ok" : "warn",
      "avg=" + std::to_string(diag.pid_cycle_avg_ms).substr(0, 4) + "ms last=" +
          std::to_string(static_cast<unsigned long>(diag.pid_cycle_last_ms)) + "ms",
      "exec=" + std::to_string(static_cast<unsigned long>(diag.pid_cycle_exec_ms)) +
          "ms peak=" + std::to_string(static_cast<unsigned long>(diag.pid_cycle_peak_ms)) + "ms");

  print_metric_table_row(
      state,
      "stack margin",
      task_summary.min_stack_words >= 512U ? "ok" : "warn",
      "min=" + std::to_string(task_summary.min_stack_words) + " words",
      std::to_string(task_summary.low_stack_task_count) + " task(s) under 512 words");

  if (wifi.state.sta_connected) {
    print_metric_table_row(
        state,
        "wifi quality",
        "ok",
        "rssi=" + std::to_string(static_cast<long>(wifi.state.rssi)) + "dBm ssid=" +
            std::string(wifi.ssid.c_str()),
        "ip=" + std::string(wifi.ip.c_str()) + " retry=" +
            std::to_string(static_cast<unsigned long>(wifi.state.reconnect_attempts)));
  } else {
    print_metric_table_row(
        state,
        "wifi quality",
        wifi.state.ap_active ? "warn" : "n/a",
        "phase=" + std::string(wifi.phase.c_str()),
        "backoff=" + std::to_string(static_cast<unsigned long>(wifi.reconnect_backoff_ms)) + "ms");
  }

  print_metric_table_row(
      state,
      "ws stats",
      "ok",
      "total=" + std::to_string(static_cast<unsigned long>(diag.ws_clients_total)) +
          " auth=" + std::to_string(static_cast<unsigned long>(diag.ws_clients_auth)),
      "legacy=" + std::to_string(static_cast<unsigned long>(diag.ws_clients_legacy)) +
          " tele=" + std::to_string(static_cast<unsigned long>(diag.ws_clients_telemetry)) +
          " shell=" + std::to_string(static_cast<unsigned long>(diag.ws_clients_shell)) +
          " debug=" + std::to_string(static_cast<unsigned long>(diag.ws_clients_debug)));

  print_metric_table_row(
      state,
      "services",
      "ok",
      "ssh=" + std::string(mros::ssh::identity_get().enabled ? "on" : "off") +
          " mcp=" + std::string(mros::mcp::service_is_enabled() ? "on" : "off"),
      "ssh_backend=" + std::string(mros::ssh::backend_name()) +
          " mcp_shell=" + std::string(mros::mcp::service_allow_shell() ? "yes" : "no"));

  print_metric_table_row(
      state,
      "storage io",
      (littlefs_total > 0U && diag.storage_ready) ? "ok" : "warn",
      format_size(littlefs_used) + " / " + format_size(littlefs_total),
      littlefs_total > 0U
          ? std::to_string(percent_or_zero(littlefs_used, littlefs_total)) + "% used, flush queue n/a"
          : "LittleFS not mounted");

  print_metric_table_row(
      state,
      "spi health",
      (spi_s3_get_crc_errors() == 0U && spi_c3_get_crc_errors() == 0U &&
       spi_s3_get_marker_errors() == 0U && spi_c3_get_marker_errors() == 0U)
          ? "ok"
          : "warn",
      "t41 c=" + std::to_string(static_cast<unsigned long>(spi_s3_get_crc_errors())) +
          " m=" + std::to_string(static_cast<unsigned long>(spi_s3_get_marker_errors())),
      "c3 c=" + std::to_string(static_cast<unsigned long>(spi_c3_get_crc_errors())) +
          " m=" + std::to_string(static_cast<unsigned long>(spi_c3_get_marker_errors())));

  print_metric_table_row(
      state,
      "uart health",
      uart1_cobs_get_log_version() > 0U ? "ok" : "n/a",
      "log_rev=" + std::to_string(static_cast<unsigned long>(uart1_cobs_get_log_version())),
      "overflow/framing/cobs counters not exposed");

  print_metric_table_row(
      state,
      "update state",
      "ok",
      "storage=" + std::string(diag.storage_ready ? "ready" : "down") +
          " pca=" + std::string(diag.pca_ready ? "ready" : "down"),
      "debug_sub=" +
          std::to_string(static_cast<unsigned long>(diag.ws_clients_debug_subscribed)) +
          " scene_sub=" +
          std::to_string(static_cast<unsigned long>(diag.ws_clients_scene)));

  print_metric_table_row(
      state,
      "power",
      esp_reset_reason() == ESP_RST_BROWNOUT ? "warn" : "ok",
      "reset=" + std::to_string(static_cast<int>(esp_reset_reason())),
      "motor=" + std::to_string(static_cast<unsigned>(spi_s3_get_motor_state())) + " vin sensor n/a");

  print_metric_table_row(
      state,
      "thermal",
      "n/a",
      "temperature sensor not exposed",
      "no throttling telemetry available");

  print_metric_table_row(
      state,
      "psram alloc",
      psram_physical_total > 0U ? "ok" : "n/a",
      "free=" + format_size(psram_free) + " largest=" + format_size(psram_largest),
      "physical=" + format_size(psram_physical_total) + " alloc=" + format_size(psram_heap_total));

  print_metric_table_row(
      state,
      "ota/update",
      "ok",
      "run=" + std::string(running != nullptr ? running->label : "-") + " boot=" +
          std::string(boot != nullptr ? boot->label : "-"),
      "next=" + std::string(next != nullptr ? next->label : "-") + " sketch=" +
          format_size(static_cast<uint64_t>(mros::platform::mros_system_app_image_size())));

  print_metric_table_row(
      state,
      "recovery",
      recovery.present && recovery.valid ? "ok" : "warn",
      std::string(recovery.present ? (recovery.valid ? "installed" : "invalid") : "missing") +
          " version=" + recovery.version,
      "used=" + std::to_string(static_cast<unsigned long>(recovery.used_kb)) +
          "K free=" + std::to_string(static_cast<unsigned long>(recovery.free_kb)) +
          "K size=" + std::to_string(static_cast<unsigned long>(recovery.size_kb)) + "K");
}

}  // namespace

void shell_help_espfetch(ShellState& state) {
  shell_write_line(state, "Usage: mfetch [--libs|--full|--health|--buses|--partitions|--boot|--json|--compact|--dev-name] [--logo [small|big|none]]");
  shell_write_line(state, "Show project-specific device, runtime and partition information.");
  shell_write_line(state, "Default output follows a fastfetch-like logo + module summary layout.");
  shell_write_line(state, "  --libs        Show build-time framework/component versions only.");
  shell_write_line(state, "  --full        Show default card plus metrics table, boot, buses and health.");
  shell_write_line(state, "  --health      Show OK/WARN health summary.");
  shell_write_line(state, "  --buses       Show SPI/UART/I2C/WiFi status.");
  shell_write_line(state, "  --partitions  Show partition table.");
  shell_write_line(state, "  --boot        Show reset and OTA partition status.");
  shell_write_line(state, "  --json        Print compact JSON summary.");
  shell_write_line(state, "  --compact     Print one-line summary.");
  shell_write_line(state, "  --dev-name    Print the configured device name only.");
  shell_write_line(state, "  --logo        Select logo style: small (default), big or none.");
}

int shell_cmd_espfetch(ShellContext& ctx) {
  bool show_libs_only = false;
  bool show_full = false;
  bool show_health = false;
  bool show_buses = false;
  bool show_partitions = false;
  bool show_boot = false;
  bool show_json = false;
  bool show_compact = false;
  bool show_dev_name = false;
  LogoMode logo_mode = LogoMode::Small;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--help" || ctx.args[i] == "-h") {
      shell_help_espfetch(ctx.state);
      return 0;
    }
    if (ctx.args[i] == "--libs") {
      show_libs_only = true;
      continue;
    }
    if (ctx.args[i] == "--full") {
      show_full = true;
      continue;
    }
    if (ctx.args[i] == "--health") {
      show_health = true;
      continue;
    }
    if (ctx.args[i] == "--buses") {
      show_buses = true;
      continue;
    }
    if (ctx.args[i] == "--partitions") {
      show_partitions = true;
      continue;
    }
    if (ctx.args[i] == "--boot") {
      show_boot = true;
      continue;
    }
    if (ctx.args[i] == "--json") {
      show_json = true;
      continue;
    }
    if (ctx.args[i] == "--compact") {
      show_compact = true;
      continue;
    }
    if (ctx.args[i] == "--dev-name" || ctx.args[i] == "--devname" || ctx.args[i] == "--device-name") {
      show_dev_name = true;
      continue;
    }
    if (ctx.args[i] == "--logo") {
      if ((i + 1U) < ctx.args.size() && !ctx.args[i + 1U].empty() && ctx.args[i + 1U].front() != '-') {
        ++i;
        if (ctx.args[i] == "small") {
          logo_mode = LogoMode::Small;
          continue;
        }
        if (ctx.args[i] == "big") {
          logo_mode = LogoMode::Big;
          continue;
        }
        if (ctx.args[i] == "none") {
          logo_mode = LogoMode::None;
          continue;
        }
        shell_printf(ctx.state, "mfetch: unsupported logo mode '%s'\n", ctx.args[i].c_str());
        return 1;
      }
      logo_mode = LogoMode::Small;
      continue;
    }
    shell_printf(ctx.state, "mfetch: unsupported option '%s'\n", ctx.args[i].c_str());
    return 1;
  }

  if (show_dev_name && !show_json && !show_compact && !show_full) {
    shell_write_line(ctx.state, device_name_text().c_str());
    return 0;
  }

  if (show_libs_only) {
    const std::vector<std::string> libs = collect_lib_versions();
    for (const std::string& line : libs) {
      shell_write_line(ctx.state, line.c_str());
    }
    return 0;
  }

  if (show_json) {
    const std::string board_model = board_summary_text();
    const std::string firmware_text =
        std::string(safe_text(ctx.state.config.firmware_name, "MROS")) + " " +
        normalize_firmware_version_text(web_server_system_version()) + " (" +
        format_yyyymmdd_from_build_date() + ")";
    const uint64_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint64_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint64_t psram_total = physical_psram_total_bytes();
    const uint64_t psram_free = static_cast<uint64_t>(mros::platform::mros_system_psram_free());
    uint64_t fs_total = 0U;
    uint64_t fs_used = 0U;
    (void)get_storage_usage(&fs_total, &fs_used);
    WebServerDiagSnapshot diag {};
    web_server_get_diag_snapshot(&diag);
    WifiManagerSnapshot wifi = {};
    wifi_manager_get_snapshot(&wifi);
    const RecoverySnapshot recovery = collect_recovery_snapshot();
    shell_printf(ctx.state,
                 "{\"board\":\"%s\",\"dev_name\":\"%s\",\"firmware\":\"%s\",\"freertos\":\"%s\","
                 "\"os\":\"MROS DEUSCARA Bridge\",\"kernel\":\"%s\",\"uptime\":\"%s\","
                 "\"sram_free\":%lu,\"sram_total\":%lu,"
                 "\"psram_free\":%lu,\"psram_total\":%lu,\"cpu_core0_mhz\":%lu,\"cpu_core1_mhz\":%lu,"
                 "\"teensy_qspi\":%s,\"t41_qspi\":%s,\"c3_spi\":%s,\"pca9685\":%s,\"wifi\":\"%s\","
                 "\"ws_total\":%lu,\"ws_auth\":%lu,\"ws_legacy\":%lu,\"ws_telemetry\":%lu,"
                 "\"ws_shell\":%lu,\"ws_debug\":%lu,\"storage_ready\":%s,"
                 "\"littlefs_used\":%lu,\"littlefs_total\":%lu,"
                 "\"recovery_present\":%s,\"recovery_valid\":%s,\"recovery_version\":\"%s\","
                 "\"recovery_used_kb\":%lu,\"recovery_free_kb\":%lu,\"recovery_size_kb\":%lu,"
                 "\"ssh_enabled\":%s,\"ssh_backend\":\"%s\",\"mcp_enabled\":%s,\"mcp_allow_shell\":%s}\n",
                 json_escape(board_model).c_str(),
                 json_escape(device_name_text()).c_str(),
                 json_escape(firmware_text).c_str(),
                 json_escape(freertos_version_text()).c_str(),
                 json_escape(safe_text(mros::platform::mros_system_sdk_version(), "unknown")).c_str(),
                 json_escape(uptime_text()).c_str(),
                 static_cast<unsigned long>(sram_free),
                 static_cast<unsigned long>(sram_total),
                 static_cast<unsigned long>(psram_free),
                 static_cast<unsigned long>(psram_total),
                 static_cast<unsigned long>(diag.cpu_freq_core0_mhz),
                 static_cast<unsigned long>(diag.cpu_freq_core1_mhz),
                 spi_s3_is_connected() ? "true" : "false",
                 spi_s3_is_connected() ? "true" : "false",
                 spi_c3_is_connected() ? "true" : "false",
                 pca9685_is_ready() ? "true" : "false",
                 wifi.state.sta_connected && wifi.ip.length() > 0 ? wifi.ip.c_str() : "down",
                 static_cast<unsigned long>(diag.ws_clients_total),
                 static_cast<unsigned long>(diag.ws_clients_auth),
                 static_cast<unsigned long>(diag.ws_clients_legacy),
                 static_cast<unsigned long>(diag.ws_clients_telemetry),
                 static_cast<unsigned long>(diag.ws_clients_shell),
                 static_cast<unsigned long>(diag.ws_clients_debug),
                 diag.storage_ready ? "true" : "false",
                 static_cast<unsigned long>(fs_used),
                 static_cast<unsigned long>(fs_total),
                 recovery.present ? "true" : "false",
                 recovery.valid ? "true" : "false",
                 json_escape(recovery.version).c_str(),
                 static_cast<unsigned long>(recovery.used_kb),
                 static_cast<unsigned long>(recovery.free_kb),
                 static_cast<unsigned long>(recovery.size_kb),
                 mros::ssh::identity_get().enabled ? "true" : "false",
                 json_escape(mros::ssh::backend_name()).c_str(),
                 mros::mcp::service_is_enabled() ? "true" : "false",
                 mros::mcp::service_allow_shell() ? "true" : "false");
    return 0;
  }

  if (show_compact) {
    WebServerDiagSnapshot diag {};
    web_server_get_diag_snapshot(&diag);
    WifiManagerSnapshot wifi = {};
    wifi_manager_get_snapshot(&wifi);
    uint64_t fs_total = 0U;
    uint64_t fs_used = 0U;
    (void)get_storage_usage(&fs_total, &fs_used);
    const RecoverySnapshot recovery = collect_recovery_snapshot();
    shell_printf(ctx.state, "%s dev=%s fw=%s %s sram=%luK psram=%luK t41-qspi=%s c3=%s pca=%s wifi=%s fs=%lu/%luK recovery=%s:%s free=%luK\n",
                 detected_board_model().c_str(),
                 device_name_text().c_str(),
                 normalize_firmware_version_text(web_server_system_version()).c_str(),
                 cpu_summary_text().c_str(),
                 static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
                 static_cast<unsigned long>(mros::platform::mros_system_psram_free() / 1024U),
                 spi_s3_is_connected() ? "ok" : "down",
                 spi_c3_is_connected() ? "ok" : "down",
                 pca9685_is_ready() ? "ok" : "down",
                 wifi.state.sta_connected && wifi.ip.length() > 0 ? wifi.ip.c_str() : "down",
                 static_cast<unsigned long>(fs_used / 1024U),
                 static_cast<unsigned long>(fs_total / 1024U),
                 recovery.present ? (recovery.valid ? "ok" : "invalid") : "missing",
                 recovery.version.c_str(),
                 static_cast<unsigned long>(recovery.free_kb));
    return 0;
  }

  if (show_json || show_compact) {
    logo_mode = LogoMode::None;
  }

  if (show_health && !show_full) return print_health(ctx.state);
  if (show_buses && !show_full) {
    print_bus_table(ctx.state);
    return 0;
  }
  if (show_partitions && !show_full) {
    print_partition_table(ctx.state);
    return 0;
  }
  if (show_boot && !show_full) {
    print_boot(ctx.state);
    return 0;
  }

  std::vector<std::string> info;
  const uint64_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint64_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint64_t psram_total = physical_psram_total_bytes();
  const uint64_t psram_free = static_cast<uint64_t>(mros::platform::mros_system_psram_free());
  append_kv_line(&info, "board", board_summary_text());
  append_kv_line(&info, "os", "MROS DEUSCARA Bridge");
  append_kv_line(
      &info,
      "firmware",
      std::string(safe_text(ctx.state.config.firmware_name, "MROS")) + " " +
          "(" + format_yyyymmdd_from_build_date() + ")");
  append_kv_line(&info, "host", safe_text(ctx.state.config.hostname, "mros"));
  append_kv_line(&info, "uptime", uptime_text());
  append_kv_line(&info, "shell", std::string(kShellName) + " " + kShellVersion);
  append_kv_line(&info, "kernel", freertos_version_text());
  const std::string links = connected_links_text(ctx.state);
  append_kv_line(&info, "conn", links.empty() ? "WEB" : links);
  append_kv_line(&info, "tasks", std::to_string(static_cast<unsigned long>(uxTaskGetNumberOfTasks())));
  append_kv_line(&info, "cpu", cpu_summary_text());
  append_kv_line(&info, "sram", memory_usage_text(sram_total, sram_free));
  append_kv_line(&info, "psram", memory_usage_text(psram_total, psram_free));

  const esp_partition_t* running = esp_ota_get_running_partition();
  const RecoverySnapshot recovery = collect_recovery_snapshot();
  const uint64_t app_total = running != nullptr ? running->size : 0U;
  const uint64_t app_used = static_cast<uint64_t>(mros::platform::mros_system_app_image_size());
  if (app_total > 0U) {
    append_kv_line(
        &info,
        "app",
        format_size(app_used) + " / " + format_size(app_total) + " (" +
            std::to_string(percent_or_zero(app_used, app_total)) + "%)");
  } else {
    append_kv_line(&info, "app", "unknown");
  }

  append_kv_line(
      &info,
      "recovery",
      recovery.version + " (" +
          std::string(recovery.present ? (recovery.valid ? "installed" : "invalid") : "missing") +
          ") free=" +
          std::to_string(static_cast<unsigned long>(recovery.free_kb)) + "K / " +
          std::to_string(static_cast<unsigned long>(recovery.size_kb)) + "K");

  const std::vector<FsSnapshot> filesystems = collect_fs_partitions();
  if (filesystems.empty()) {
    append_kv_line(&info, "storage", "no littlefs/spiffs/fat partitions");
  } else {
    for (const FsSnapshot& fs : filesystems) {
      if (fs.usage_known) {
        append_kv_line(
            &info,
            "storage",
            storage_usage_text(fs.total, fs.used) + " " + fs.fstype + " @" + fs.mount);
      } else {
        append_kv_line(&info, "storage", format_size(fs.total) + " total " + fs.fstype);
      }
    }
  }

  append_kv_line(
      &info,
      "sdk",
      std::string(safe_text(mros::platform::mros_system_sdk_version(), "unknown")) + " (IDF)");
  const std::vector<std::string> logo_lines = build_logo_block(show_full, logo_mode);
  const std::vector<std::string> aligned_info = align_info_lines(info);
  if (can_render_side_by_side(ctx.state, logo_lines, aligned_info)) {
    const size_t logo_count = logo_lines.size();
    const size_t line_count = std::max(logo_count, aligned_info.size());
    size_t left_width = 0U;
    for (const std::string& line : logo_lines) {
      left_width = std::max(left_width, line.size());
    }
    left_width += 2U;
    for (size_t i = 0U; i < line_count; ++i) {
      std::string left_plain = i < logo_count ? logo_lines[i] : "";
      if (left_plain.size() < left_width) {
        left_plain += std::string(left_width - left_plain.size(), ' ');
      }
      const std::string left = format_logo_line(ctx.state, left_plain);
      const std::string right = i < aligned_info.size() ? format_info_line(ctx.state, aligned_info[i]) : "";
      shell_write(ctx.state, left.c_str());
      if (!right.empty()) {
        shell_write(ctx.state, " ");
        shell_write(ctx.state, right.c_str());
      }
      shell_write(ctx.state, "\n");
    }
  } else {
    for (const std::string& line : logo_lines) {
      shell_write_line(ctx.state, format_logo_line(ctx.state, line).c_str());
    }
    if (!logo_lines.empty()) {
      shell_write_line(ctx.state, "");
    }
    for (const std::string& line : aligned_info) {
      shell_write_line(ctx.state, format_info_line(ctx.state, line).c_str());
    }
  }
  if (show_full) {
    shell_write_line(ctx.state, "");
    print_full_metrics_table(ctx.state);
    shell_write_line(ctx.state, "");
    print_boot(ctx.state);
    shell_write_line(ctx.state, "");
    print_bus_table(ctx.state);
    shell_write_line(ctx.state, "");
    print_health(ctx.state);
  }
  return 0;
}

}  // namespace mros::shell
