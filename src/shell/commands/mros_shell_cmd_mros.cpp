#include "src/shell/mros_shell_internal.h"

#include "WString.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "src/comm_interfaces/spi/spi_c3_master.h"
#include "src/comm_interfaces/spi/spi_t41_link.h"
#include "src/comm_interfaces/uart/uart_cobs.h"
#include "src/control/control_manager.h"
#include "src/core/health/bearing_health.h"
#include "src/core/memory/memory_monitor.h"
#include "src/core/power/power_manager.h"
#include "src/core/rtos/device_process_manager.h"
#include "src/core/rtos/task_manager.h"
#include "src/drivers/i2c_pca9685.h"
#include "src/drivers/storage/logger_driver.h"
#include "src/net/ssh_service.h"
#include "src/platform/mros_file.h"
#include "src/platform/mros_fs.h"
#include "src/platform/mros_http_client.h"
#include "src/platform/mros_i2c.h"
#include "src/platform/mros_system.h"
#include "src/platform/mros_time.h"
#include "src/security/ssh_identity.h"
#include "src/shell/mshell_remote.h"
#include "src/shell/mshell_runtime.h"
#include "src/shell/shell_service.h"
#include "src/utils/mros_json_writer.h"
#include "src/web/server/web_server.h"
#include "src/web/server/wifi_manager.h"

namespace mros::shell {
namespace {

constexpr uint32_t kDefaultWatchIntervalMs = 5000U;
constexpr size_t kDefaultWatchCycles = 12U;
constexpr size_t kMaxWatchCycles = 120U;
constexpr uint32_t kMaxWatchIntervalMs = 60000U;
constexpr size_t kMemJsonBufferBytes = 8192U;

std::string lower_copy(std::string text);

bool is_help_arg(const std::string& arg) {
  return arg == "--help" || arg == "-h";
}

bool parse_u32(const std::string& text, uint32_t* value) {
  if (value == nullptr) return false;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
  *value = static_cast<uint32_t>(parsed);
  return true;
}

bool parse_size(const std::string& text, size_t* value) {
  uint32_t parsed = 0;
  if (!parse_u32(text, &parsed) || parsed == 0U) return false;
  *value = static_cast<size_t>(parsed);
  return true;
}

const char* yn(const bool value) {
  return value ? "yes" : "no";
}

const char* ok_warn(const bool ok) {
  return ok ? "OK" : "WARN";
}

WifiManagerSnapshot wifi_snapshot() {
  WifiManagerSnapshot snapshot {};
  wifi_manager_get_snapshot(&snapshot);
  return snapshot;
}

const char* wifi_status_label(const WifiManagerSnapshot& snapshot) {
  return snapshot.state.sta_connected ? "connected" : "down";
}

String wifi_ip_or(const WifiManagerSnapshot& snapshot, const char* fallback = "") {
  return snapshot.state.sta_connected ? snapshot.ip : String(fallback);
}

String wifi_ssid_or(const WifiManagerSnapshot& snapshot, const char* fallback = "(none)") {
  return snapshot.ssid.length() != 0U ? snapshot.ssid : String(fallback);
}

long wifi_rssi_or(const WifiManagerSnapshot& snapshot, const long fallback = 0L) {
  return snapshot.state.sta_connected ? static_cast<long>(snapshot.state.rssi) : fallback;
}

uint32_t age_ms_since(const uint32_t last_ms) {
  if (last_ms == 0U) return 0xFFFFFFFFU;
  return static_cast<uint32_t>(mros::platform::mros_millis() - last_ms);
}

void print_age(ShellState& state, const uint32_t last_ms) {
  if (last_ms == 0U) {
    shell_write(state, "never");
  } else {
    shell_printf(state, "%lums", static_cast<unsigned long>(age_ms_since(last_ms)));
  }
}

void print_rule(ShellState& state) {
  shell_write_line(state, "------------------------------------------------------------");
}

void print_connection_table(ShellState& state) {
  const bool t41 = spi_s3_is_connected();
  const bool c3 = spi_c3_is_connected();
  const bool pca = pca9685_is_ready();
  const WifiManagerSnapshot wifi = wifi_snapshot();

  shell_write_line(state, "name          bus    peer       state      rate/loop     errors       last");
  shell_write_line(state, "------------  -----  ---------  ---------  ------------  -----------  --------");
  shell_printf(state,
               "t41-qspi      SPI    TEENSY4.1  %-9s  ",
               t41 ? "connected" : "down");
  shell_printf(state, "%ums loop     ", static_cast<unsigned>(spi_s3_get_loop_ms()));
  shell_printf(state, "crc=%lu ", static_cast<unsigned long>(spi_s3_get_crc_errors()));
  shell_printf(state, "mrk=%lu  ", static_cast<unsigned long>(spi_s3_get_marker_errors()));
  shell_printf(state, "seq=%u\n", static_cast<unsigned>(spi_s3_get_last_rx_seq()));

  shell_printf(state,
               "c3-spi        SPI    ESP32-C3   %-9s  ",
               c3 ? "connected" : "down");
  shell_printf(state, "%uHz loop     ", static_cast<unsigned>(spi_c3_get_loop_hz()));
  shell_printf(state, "crc=%lu ", static_cast<unsigned long>(spi_c3_get_crc_errors()));
  shell_printf(state, "mrk=%lu  ", static_cast<unsigned long>(spi_c3_get_marker_errors()));
  print_age(state, spi_c3_get_last_good_rx_ms());
  shell_write(state, "\n");
  shell_printf(
      state,
      "t41-uart-log  UART   TEENSY4.1  %-9s  rev=%lu       n/a          logs\n",
      uart1_cobs_get_log_version() > 0U ? "active" : "idle",
      static_cast<unsigned long>(uart1_cobs_get_log_version()));
  shell_printf(
      state,
      "pca9685       I2C    0x40       %-9s  %.1fHz       oe=%s        n/a\n",
      pca ? "ready" : "down",
      pca9685_get_frequency(),
      yn(pca9685_get_output_enable()));
  shell_printf(
      state,
      "wifi          NET    station    %-9s  rssi=%ld      ip=%s\n",
      wifi_status_label(wifi),
      wifi_rssi_or(wifi),
      wifi_ip_or(wifi).c_str());
}

void print_connections_tree(ShellState& state) {
  const WifiManagerSnapshot wifi = wifi_snapshot();
  shell_write_line(state, "S3 bridge");
  shell_printf(state, "  |-- QSPI -> TEENSY4.1    [%s]\n", spi_s3_is_connected() ? "connected" : "down");
  shell_printf(state, "  |-- SPI -> ESP32-C3      [%s]\n", spi_c3_is_connected() ? "connected" : "down");
  shell_printf(state, "  |-- UART -> TEENSY logs  [rev=%lu]\n", static_cast<unsigned long>(uart1_cobs_get_log_version()));
  shell_printf(state, "  |-- I2C -> PCA9685 0x40  [%s]\n", pca9685_is_ready() ? "ready" : "down");
  shell_printf(state, "  `-- WiFi -> %s       [%s]\n",
               wifi_ssid_or(wifi).c_str(),
               wifi_ip_or(wifi, "down").c_str());
}

void print_bus_errors(ShellState& state) {
  shell_write_line(state, "source   crc    marker  timeout  detail");
  shell_write_line(state, "-------  -----  ------  -------  ----------------------------");
  shell_printf(state, "t41-qspi %-5lu  %-6lu  %-7s  err_code=%u dev=%d\n",
               static_cast<unsigned long>(spi_s3_get_crc_errors()),
               static_cast<unsigned long>(spi_s3_get_marker_errors()),
               "n/a",
               static_cast<unsigned>(spi_s3_get_error_code()),
               static_cast<int>(spi_s3_get_device_status_code()));
  shell_printf(state, "c3-spi   %-5lu  %-6lu  %-7s  total_rx=%lu hz=%u\n",
               static_cast<unsigned long>(spi_c3_get_crc_errors()),
               static_cast<unsigned long>(spi_c3_get_marker_errors()),
               "n/a",
               static_cast<unsigned long>(spi_c3_get_total_rx()),
               static_cast<unsigned>(spi_c3_get_loop_hz()));
  shell_printf(state, "i2c      %-5s  %-6s  %-7s  pca9685=%s\n",
               "n/a",
               "n/a",
               "n/a",
               pca9685_is_ready() ? "ready" : "down");
}

void print_spi_status(ShellState& state, const std::string& peer) {
  if (peer == "t41" || peer == "t41" || peer == "all") {
    shell_write_line(state, "TEENSY4.1 QSPI");
    shell_printf(state, "  state          : %s\n", spi_s3_is_connected() ? "connected" : "down");
    shell_printf(state, "  transactions   : %lu\n", static_cast<unsigned long>(spi_s3_get_total_transactions()));
    shell_printf(state, "  loop_ms        : %u\n", static_cast<unsigned>(spi_s3_get_loop_ms()));
    shell_printf(state, "  crc_errors     : %lu\n", static_cast<unsigned long>(spi_s3_get_crc_errors()));
    shell_printf(state, "  marker_errors  : %lu\n", static_cast<unsigned long>(spi_s3_get_marker_errors()));
    shell_printf(state, "  last_marker    : 0x%02X\n", static_cast<unsigned>(spi_s3_get_last_rx_marker()));
    shell_printf(state, "  last_seq       : %u\n", static_cast<unsigned>(spi_s3_get_last_rx_seq()));
    shell_printf(state, "  device_status  : %d\n", static_cast<int>(spi_s3_get_device_status_code()));
  }
  if (peer == "c3" || peer == "all") {
    shell_write_line(state, "C3 SPI");
    shell_printf(state, "  state          : %s\n", spi_c3_is_connected() ? "connected" : "down");
    shell_printf(state, "  total_rx       : %lu\n", static_cast<unsigned long>(spi_c3_get_total_rx()));
    shell_printf(state, "  loop_hz        : %u\n", static_cast<unsigned>(spi_c3_get_loop_hz()));
    shell_printf(state, "  crc_errors     : %lu\n", static_cast<unsigned long>(spi_c3_get_crc_errors()));
    shell_printf(state, "  marker_errors  : %lu\n", static_cast<unsigned long>(spi_c3_get_marker_errors()));
    shell_write(state, "  last_good_age  : ");
    print_age(state, spi_c3_get_last_good_rx_ms());
    shell_write(state, "\n");
    shell_printf(state, "  espnow         : active=%s connected=%s recent_cmd=%s\n",
                 yn(spi_c3_is_espnow_active()),
                 yn(spi_c3_is_espnow_connected()),
                 yn(spi_c3_has_recent_espnow_cmd()));
  }
}

void print_spi_list(ShellState& state) {
  shell_write_line(state, "name    role    peer       speed     mode  status");
  shell_write_line(state, "------  ------  ---------  --------  ----  ---------");
  shell_printf(state, "t41     slave   TEENSY4.1  n/a       n/a   %s\n", spi_s3_is_connected() ? "connected" : "down");
  shell_printf(state, "c3      master  ESP32-C3   n/a       n/a   %s\n", spi_c3_is_connected() ? "connected" : "down");
}

std::vector<std::string> tail_lines(const std::string& text, size_t count, const std::string& filter = "") {
  std::vector<std::string> lines;
  size_t start = 0U;
  while (start <= text.size()) {
    const size_t nl = text.find('\n', start);
    const bool has_nl = nl != std::string::npos;
    const size_t end = has_nl ? nl : text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && (filter.empty() || line.find(filter) != std::string::npos)) {
      lines.push_back(line);
      if (lines.size() > count) {
        lines.erase(lines.begin());
      }
    }
    if (!has_nl) break;
    start = nl + 1U;
  }
  return lines;
}

void write_json_escaped(FILE* file, const char* text) {
  if (file == nullptr || text == nullptr) {
    return;
  }
  for (const char* p = text; *p != '\0'; ++p) {
    switch (*p) {
      case '\\':
        std::fputs("\\\\", file);
        break;
      case '"':
        std::fputs("\\\"", file);
        break;
      case '\n':
        std::fputs("\\n", file);
        break;
      case '\r':
        std::fputs("\\r", file);
        break;
      case '\t':
        std::fputs("\\t", file);
        break;
      default:
        if (static_cast<unsigned char>(*p) < 0x20U) {
          std::fprintf(file, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(*p)));
        } else {
          std::fputc(*p, file);
        }
        break;
    }
  }
}

void write_report_string_field(FILE* file, const char* key, const char* value, bool* first) {
  if (file == nullptr || key == nullptr || first == nullptr) {
    return;
  }
  std::fputs(*first ? "\"" : ",\"", file);
  *first = false;
  write_json_escaped(file, key);
  std::fputs("\":\"", file);
  write_json_escaped(file, value != nullptr ? value : "");
  std::fputc('"', file);
}

void write_report_bool_field(FILE* file, const char* key, const bool value, bool* first) {
  if (file == nullptr || key == nullptr || first == nullptr) {
    return;
  }
  std::fputs(*first ? "\"" : ",\"", file);
  *first = false;
  write_json_escaped(file, key);
  std::fprintf(file, "\":%s", value ? "true" : "false");
}

void write_report_u32_field(FILE* file, const char* key, const uint32_t value, bool* first) {
  if (file == nullptr || key == nullptr || first == nullptr) {
    return;
  }
  std::fputs(*first ? "\"" : ",\"", file);
  *first = false;
  write_json_escaped(file, key);
  std::fprintf(file, "\":%lu", static_cast<unsigned long>(value));
}

bool write_support_report_file(ShellState& state, const char* path, const uint32_t now_ms) {
  if (path == nullptr) {
    return false;
  }
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    return false;
  }

  const bool storage = shell_is_storage_mounted(state);
  const bool spi_t41 = spi_s3_is_connected();
  const bool spi_c3 = spi_c3_is_connected();
  const WifiManagerSnapshot wifi = wifi_snapshot();
  remote::FsMountSnapshot t41 {};
  remote::FsMountSnapshot t41sd {};
  remote::fs_snapshot(remote::FsMount::T41, &t41);
  remote::fs_snapshot(remote::FsMount::T41Sdcard, &t41sd);
  MrosRtosAggregateSnapshot rtos {};
  app_rtos_get_aggregate_diag(&rtos);

  char* jobs_json = static_cast<char*>(
      heap_caps_malloc(4096U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (jobs_json == nullptr) {
    jobs_json = static_cast<char*>(
        heap_caps_malloc(4096U, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (jobs_json != nullptr) {
    jobs_json[0] = '\0';
    (void)runtime::jobs_json(jobs_json, 4096U);
  }
  const std::string audit = audit_report();

  std::fputc('{', file);
  bool first = true;
  write_report_bool_field(file, "ok", true, &first);
  write_report_string_field(file, "kind", "mros-support-report", &first);
  write_report_u32_field(file, "created_ms", now_ms, &first);
  write_report_string_field(file, "shell", kShellVersion, &first);

  std::fputs(",\"doctor\":{", file);
  bool doctor_first = true;
  write_report_bool_field(file, "storage", storage, &doctor_first);
  write_report_bool_field(file, "t41_qspi", spi_t41, &doctor_first);
  write_report_bool_field(file, "c3_spi", spi_c3, &doctor_first);
  write_report_bool_field(file, "wifi_sta_connected", wifi.state.sta_connected, &doctor_first);
  write_report_bool_field(file, "wifi_ap", wifi.ap_ip.length() > 0U, &doctor_first);
  write_report_string_field(file, "ssid", wifi.ssid.c_str(), &doctor_first);
  write_report_string_field(file, "ip", wifi.ip.c_str(), &doctor_first);
  std::fputs(doctor_first ? "\"rssi\":" : ",\"rssi\":", file);
  doctor_first = false;
  std::fprintf(file, "%ld", static_cast<long>(wifi.state.rssi));
  write_report_u32_field(file, "shell_sessions", static_cast<uint32_t>(active_session_count()), &doctor_first);
  write_report_u32_field(file, "shell_session_capacity", static_cast<uint32_t>(session_capacity()), &doctor_first);
  write_report_u32_field(file, "jobs_active", static_cast<uint32_t>(runtime::job_active_count()), &doctor_first);
  write_report_bool_field(file, "tx_active", runtime::tx_active(), &doctor_first);
  write_report_string_field(file, "uart_bridge", remote::bridge_mode_name(remote::bridge_mode()), &doctor_first);
  write_report_string_field(file, "t41_fs", t41.error_code, &doctor_first);
  write_report_string_field(file, "t41_sdcard", t41sd.error_code, &doctor_first);
  std::fputc('}', file);

  std::fprintf(file,
               ",\"perf\":{\"heap_free\":%lu,\"heap_min\":%lu,\"psram_free\":%lu,"
               "\"rtos_slips\":%lu,\"rtos_max_slip_ms\":%lu,\"rtos_max_slip_task\":\"",
               static_cast<unsigned long>(esp_get_free_heap_size()),
               static_cast<unsigned long>(esp_get_minimum_free_heap_size()),
               static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
               static_cast<unsigned long>(rtos.total_slip_count),
               static_cast<unsigned long>(rtos.max_slip_ms));
  write_json_escaped(file, rtos.max_slip_task != nullptr ? rtos.max_slip_task : "");
  std::fputs("\"}", file);

  std::fputs(",\"jobs\":", file);
  std::fputs((jobs_json != nullptr && jobs_json[0] != '\0') ? jobs_json : "{\"ok\":false}", file);
  if (jobs_json != nullptr) {
    heap_caps_free(jobs_json);
  }

  std::fputs(",\"audit\":\"", file);
  write_json_escaped(file, audit.c_str());
  std::fputs("\"}", file);
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok;
}

void print_log_tail(ShellState& state, const size_t count, const std::string& filter = "") {
  const size_t snapshot_size = uart1_cobs_get_system_logs_size();
  if (snapshot_size == 0U) {
    return;
  }
  constexpr size_t kMrosLogTailCopyMax = 64U * 1024U;
  size_t copy_size = snapshot_size;
  if (copy_size > kMrosLogTailCopyMax) {
    copy_size = kMrosLogTailCopyMax;
  }
  const size_t base_offset = uart1_cobs_get_system_logs_base_offset();
  const size_t start_offset = base_offset + snapshot_size - copy_size;
  char* snapshot = static_cast<char*>(
      heap_caps_malloc(copy_size + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (snapshot == nullptr) {
    snapshot = static_cast<char*>(
        heap_caps_malloc(copy_size + 1U, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (snapshot == nullptr) {
    shell_write_line(state, "log tail alloc failed");
    return;
  }
  size_t copied = 0U;
  if (!uart1_cobs_copy_system_logs_since(snapshot, copy_size + 1U, start_offset,
                                         copy_size, &copied)) {
    heap_caps_free(snapshot);
    shell_write_line(state, "log tail read failed");
    return;
  }
  const std::vector<std::string> lines =
      tail_lines(std::string(snapshot, copied), count, filter);
  heap_caps_free(snapshot);
  for (const std::string& line : lines) {
    shell_write_line(state, line.c_str());
  }
}

void print_uart_status(ShellState& state) {
  UartLogDiagSnapshot log_diag {};
  uart1_cobs_get_diag_snapshot(&log_diag);
  shell_write_line(state, "UART bridge");
  shell_printf(state, "  t41_uart_log_revision : %lu\n", static_cast<unsigned long>(uart1_cobs_get_log_version()));
  shell_printf(state, "  log_mode        : %s\n", uart1_cobs_log_noise_mode_name(uart1_cobs_get_log_noise_mode()));
  shell_printf(state, "  log_ring        : %lu/%lu bytes base=%lu\n",
               static_cast<unsigned long>(log_diag.log_size),
               static_cast<unsigned long>(log_diag.log_capacity),
               static_cast<unsigned long>(log_diag.base_offset));
  shell_printf(state, "  log_copies      : full=%lu since=%lu string=%lu lock_fail=%lu\n",
               static_cast<unsigned long>(log_diag.full_copy_count),
               static_cast<unsigned long>(log_diag.since_copy_count),
               static_cast<unsigned long>(log_diag.snapshot_string_count),
               static_cast<unsigned long>(log_diag.lock_fail_count));
  shell_write_line(state, "Encoder feedback via C3 SPI");
  shell_printf(state, "  position_deg    : %.2f\n", spi_c3_get_position_deg());
  shell_printf(state, "  speed_deg_s     : %.2f\n", spi_c3_get_speed_deg_s());
  shell_printf(state, "  accel_deg_s2    : %.2f\n", spi_c3_get_accel_deg_s2());
  shell_printf(state, "  c3_connected    : %s\n", yn(spi_c3_is_connected()));
}

void print_i2c_status(ShellState& state) {
  shell_write_line(state, "I2C");
  shell_printf(state, "  pca9685_ready   : %s\n", yn(pca9685_is_ready()));
  shell_printf(state, "  pca9685_freq_hz : %.1f\n", pca9685_get_frequency());
  shell_printf(state, "  pca9685_oe      : %s\n", yn(pca9685_get_output_enable()));
  shell_write_line(state, "  nack_count      : n/a (not exposed)");
}

void print_i2c_scan(ShellState& state) {
  shell_write_line(state, "addr  status");
  shell_write_line(state, "----  ----------------");
  if (!mros::platform::mros_i2c_is_ready()) {
    shell_write_line(state, "(I2C bus is not initialized)");
    return;
  }
  uint8_t found = 0U;
  for (uint8_t addr = 1U; addr < 127U; ++addr) {
    if (mros::platform::mros_i2c_probe(addr) == ESP_OK) {
      shell_printf(state, "0x%02X  found%s\n", static_cast<unsigned>(addr), addr == 0x40U ? " (PCA9685)" : "");
      ++found;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (found == 0U) {
    shell_write_line(state, "(no I2C devices found)");
  }
}

void print_pca_channels(ShellState& state) {
  shell_write_line(state, "ch  pwm/us  cal_angle_us       cal_speed_us");
  shell_write_line(state, "--  ------  -----------------  -----------------");
  for (uint8_t ch = 1U; ch <= PCA_TOTAL_CHANNELS; ++ch) {
    PCA9685_ChannelCal_t* cal = pca9685_get_cal(ch);
    if (cal == nullptr) {
      shell_printf(state, "%2u  n/a     n/a                n/a\n", static_cast<unsigned>(ch));
      continue;
    }
    shell_printf(state,
                 "%2u  n/a     %.0f..%.0f          %.0f/%.0f/%.0f\n",
                 static_cast<unsigned>(ch),
                 cal->angle_min_us,
                 cal->angle_max_us,
                 cal->speed_min_us,
                 cal->speed_center_us,
                 cal->speed_max_us);
  }
}

void print_power_status(ShellState& state) {
  mros::power::Status power {};
  mros::power::get_status(&power);
  char temp_text[20] = "unavailable";
  if (power.temperature_valid) {
    std::snprintf(temp_text, sizeof(temp_text), "%.1f C", power.temperature_c);
  }
  shell_write_line(state, "Power manager status");
  shell_printf(state, "  mode/pm/tickless   : %s / %s / %s\n",
               power.mode,
               yn(power.pm_enabled),
               yn(power.tickless_idle_enabled));
  shell_printf(state, "  cpu actual/target   : %lu / %lu MHz\n",
               static_cast<unsigned long>(power.actual_cpu_mhz),
               static_cast<unsigned long>(power.target_mhz));
  shell_printf(state, "  demand net/rt       : %lu / %lu MHz\n",
               static_cast<unsigned long>(power.net_demand_mhz),
               static_cast<unsigned long>(power.rt_demand_mhz));
  shell_printf(state, "  min/max/light-sleep : %lu / %lu MHz / %s\n",
               static_cast<unsigned long>(power.min_mhz),
               static_cast<unsigned long>(power.max_mhz),
               yn(power.light_sleep_enabled));
  shell_printf(state, "  wifi power save     : %s (last=%lu)\n",
               power.wifi_ps_mode,
               static_cast<unsigned long>(power.last_wifi_ps_result));
  shell_printf(state, "  temperature         : %s\n", temp_text);
  shell_printf(state, "  locks/count         : %s / %lu\n",
               power.active_locks && power.active_locks[0] ? power.active_locks : "-",
               static_cast<unsigned long>(power.active_lock_count));
  shell_printf(state, "  boost               : %s %lums\n",
               power.boost_reason && power.boost_reason[0] ? power.boost_reason : "-",
               static_cast<unsigned long>(power.boost_remaining_ms));
  shell_printf(state, "  internal free/large : %lu / %lu\n",
               static_cast<unsigned long>(power.internal_free),
               static_cast<unsigned long>(power.internal_largest_block));
  shell_write_line(state, "");
  shell_write_line(state, "Hardware power:");
  shell_printf(state, "motor_power      : %u\n", static_cast<unsigned>(spi_s3_get_motor_state()));
  shell_printf(state, "pca_oe           : %s\n", yn(pca9685_get_output_enable()));
  shell_printf(state, "pca_ready        : %s\n", yn(pca9685_is_ready()));
  shell_printf(state, "t41_error_code    : %u\n", static_cast<unsigned>(spi_s3_get_error_code()));
  shell_write_line(state, "vin/current      : n/a (sensor not exposed)");
  shell_printf(state, "reset_reason     : %d\n", static_cast<int>(esp_reset_reason()));
}

void print_power_locks(ShellState& state) {
  char buffer[2048] = {};
  if (mros::power::locks_json(buffer, sizeof(buffer))) {
    shell_write_line(state, buffer);
  } else {
    shell_write_line(state, "{\"ok\":false,\"error\":\"locks_overflow\"}");
  }
}

void print_power_trace(ShellState& state) {
  char buffer[3072] = {};
  if (mros::power::trace_json(buffer, sizeof(buffer))) {
    shell_write_line(state, buffer);
  } else {
    shell_write_line(state, "{\"ok\":false,\"error\":\"trace_overflow\"}");
  }
}

void print_sram_status(ShellState& state) {
  mros::power::Status power {};
  mros::power::get_status(&power);
  shell_write_line(state, "SRAM/PSRAM status");
  shell_printf(state, "  internal total/free/min/largest : %lu / %lu / %lu / %lu\n",
               static_cast<unsigned long>(power.internal_total),
               static_cast<unsigned long>(power.internal_free),
               static_cast<unsigned long>(power.internal_min_free),
               static_cast<unsigned long>(power.internal_largest_block));
  shell_printf(state, "  psram total/free/largest        : %lu / %lu / %lu\n",
               static_cast<unsigned long>(power.psram_total),
               static_cast<unsigned long>(power.psram_free),
               static_cast<unsigned long>(power.psram_largest_block));
  shell_printf(state, "  lazy-task reclaim target        : %lu bytes\n",
               static_cast<unsigned long>(power.lazy_tasks_saved_bytes));
  shell_printf(state, "  stack right-size reclaimed      : %lu bytes\n",
               static_cast<unsigned long>(power.stack_reclaimed_bytes));
  shell_printf(state, "  psram migrated estimate         : %lu bytes\n",
               static_cast<unsigned long>(power.psram_migrated_bytes));
  shell_printf(state, "  floor state                     : %s\n",
               power.sram_floor_state);
  shell_write_line(state, "  policy                          : stacks internal, large buffers PSRAM-first");
}

void print_sram_reclaim_plan(ShellState& state) {
  shell_write_line(state, "SRAM reclaim plan");
  shell_write_line(state, "  done    : web/shell/storage/dpm stack right-size pass");
  shell_write_line(state, "  done    : power JSON/report buffers use PSRAM-first bounded writers");
  shell_write_line(state, "  active  : PID idle tick reduces task wake load when motor/PCA idle");
  shell_write_line(state, "  next    : lazy-create ssh/mcp/fk/trajectory/experimental tasks after runtime soak");
  shell_write_line(state, "  rule    : task stacks stay internal; PSRAM stack is avoided during flash-cache-off paths");
}

char* alloc_psram_json_buffer(const size_t bytes) {
  char* buffer = static_cast<char*>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    buffer = static_cast<char*>(
        heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  return buffer;
}

int write_mem_json(ShellState& state, bool (*writer)(char*, size_t)) {
  char* buffer = alloc_psram_json_buffer(kMemJsonBufferBytes);
  if (buffer == nullptr) {
    shell_write_line(state, "{\"ok\":false,\"error\":\"no_buffer\"}");
    return 1;
  }
  const bool ok = writer(buffer, kMemJsonBufferBytes);
  shell_write_line(state, buffer);
  heap_caps_free(buffer);
  return ok ? 0 : 1;
}

void print_mem_status(ShellState& state, const bool detail) {
  mros::memory::Snapshot snap = mros::memory::capture("shell");
  shell_write_line(state, "Memory monitor");
  shell_printf(state, "  internal total/free/min/largest : %lu / %lu / %lu / %lu\n",
               static_cast<unsigned long>(snap.internal_total),
               static_cast<unsigned long>(snap.internal_free),
               static_cast<unsigned long>(snap.internal_min_free),
               static_cast<unsigned long>(snap.internal_largest_block));
  shell_printf(state, "  psram total/free/largest        : %lu / %lu / %lu\n",
               static_cast<unsigned long>(snap.psram_total),
               static_cast<unsigned long>(snap.psram_free),
               static_cast<unsigned long>(snap.psram_largest_block));
  shell_printf(state, "  floor/fragmentation             : %s / %lu%%\n",
               mros::memory::floor_state_name(snap.floor_state),
               static_cast<unsigned long>(snap.fragmentation_pct));
  shell_printf(state, "  watch                           : %s interval=%lums\n",
               yn(mros::memory::watch_enabled()),
               static_cast<unsigned long>(mros::memory::watch_interval_ms()));
  if (!detail) return;

  mros::memory::DropEvent drops[16] {};
  const size_t count = mros::memory::get_drop_events(drops, 16);
  shell_write_line(state, "");
  shell_write_line(state, "recent internal SRAM drops");
  shell_write_line(state, "seq   ms         scope                    drop    after   largest  state");
  shell_write_line(state, "----  ---------  -----------------------  ------  ------  -------  --------");
  if (count == 0U) {
    shell_write_line(state, "(no drops >= 4KB recorded)");
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    shell_printf(state, "%-4lu  %-9lu  %-23s  %-6lu  %-6lu  %-7lu  %s\n",
                 static_cast<unsigned long>(drops[i].seq),
                 static_cast<unsigned long>(drops[i].ms),
                 drops[i].scope,
                 static_cast<unsigned long>(drops[i].drop_bytes),
                 static_cast<unsigned long>(drops[i].after_free),
                 static_cast<unsigned long>(drops[i].largest_block),
                 mros::memory::floor_state_name(drops[i].floor_state));
  }
}

int handle_mem(ShellContext& ctx) {
  const std::string sub = ctx.args.size() >= 3U ? lower_copy(ctx.args[2]) : "status";
  const bool json = ctx.json_output ||
                    std::find(ctx.args.begin(), ctx.args.end(), "--json") != ctx.args.end();
  if (sub == "status") {
    if (json) return write_mem_json(ctx.state, mros::memory::status_json);
    const bool detail =
        std::find(ctx.args.begin(), ctx.args.end(), "--detail") != ctx.args.end();
    print_mem_status(ctx.state, detail);
    return 0;
  }
  if (sub == "watch") {
    const std::string action = ctx.args.size() >= 4U ? lower_copy(ctx.args[3]) : "status";
    if (action == "start") {
      uint32_t interval = 5000U;
      for (size_t i = 4U; i + 1U < ctx.args.size(); ++i) {
        if ((ctx.args[i] == "--interval-ms" || ctx.args[i] == "-n") &&
            parse_u32(ctx.args[i + 1U], &interval)) {
          break;
        }
      }
      (void)mros::memory::watch_start(interval);
      shell_printf(ctx.state, "mem watch=on interval=%lums\n",
                   static_cast<unsigned long>(mros::memory::watch_interval_ms()));
      return 0;
    }
    if (action == "stop") {
      mros::memory::watch_stop();
      shell_write_line(ctx.state, "mem watch=off");
      return 0;
    }
    shell_printf(ctx.state, "mem watch=%s interval=%lums\n",
                 mros::memory::watch_enabled() ? "on" : "off",
                 static_cast<unsigned long>(mros::memory::watch_interval_ms()));
    return 0;
  }
  if (sub == "snapshot") {
    const char* name = ctx.args.size() >= 4U ? ctx.args[3].c_str() : "manual";
    mros::memory::Snapshot snap {};
    (void)mros::memory::store_snapshot(name, &snap);
    shell_printf(ctx.state, "snapshot %s seq=%lu internal_free=%lu state=%s\n",
                 snap.name,
                 static_cast<unsigned long>(snap.seq),
                 static_cast<unsigned long>(snap.internal_free),
                 mros::memory::floor_state_name(snap.floor_state));
    return 0;
  }
  if (sub == "diff") {
    char* buffer = alloc_psram_json_buffer(kMemJsonBufferBytes);
    if (buffer == nullptr) {
      shell_write_line(ctx.state, "{\"ok\":false,\"error\":\"no_buffer\"}");
      return 1;
    }
    const char* left = ctx.args.size() >= 4U ? ctx.args[3].c_str() : nullptr;
    const char* right = ctx.args.size() >= 5U ? ctx.args[4].c_str() : nullptr;
    const bool ok = mros::memory::diff_json(left, right, buffer, kMemJsonBufferBytes);
    shell_write_line(ctx.state, buffer);
    heap_caps_free(buffer);
    return ok ? 0 : 1;
  }
  if (sub == "leaks") {
    return write_mem_json(ctx.state, mros::memory::leaks_json);
  }
  if (sub == "reset") {
    mros::memory::reset();
    shell_write_line(ctx.state, "mem monitor reset");
    return 0;
  }
  shell_write_line(ctx.state, "Usage: mros mem status [--detail|--json] | watch start|stop|status | snapshot [name] | diff [a b] | leaks [--json] | reset");
  return 1;
}

void print_robot_state(ShellState& state) {
  shell_printf(state, "turret target/actual : %.2f / %.2f deg\n", spi_s3_get_turret_deg(), spi_s3_get_turret_actual_deg());
  shell_printf(state, "pid out/error        : %.2f / %.2f\n", spi_s3_get_turret_pid_output(), spi_s3_get_turret_pid_error());
  shell_printf(state, "joints               : %.2f %.2f %.2f %.2f %.2f %.2f\n",
               spi_s3_get_joint_deg(0),
               spi_s3_get_joint_deg(1),
               spi_s3_get_joint_deg(2),
               spi_s3_get_joint_deg(3),
               spi_s3_get_joint_deg(4),
               spi_s3_get_joint_deg(5));
  shell_printf(state, "coord x/y/z/a        : %.1f %.1f %.1f %.1f\n",
               spi_s3_get_coord_x(),
               spi_s3_get_coord_y(),
               spi_s3_get_coord_z(),
               spi_s3_get_alpha());
  shell_printf(state, "gripper              : %u\n", static_cast<unsigned>(spi_s3_get_gripper()));
  shell_printf(state, "traj_active          : %s\n", yn(spi_s3_joint_traj_is_active()));
}

void print_robot_errors(ShellState& state) {
  shell_printf(state, "t41_error_code        : %u\n", static_cast<unsigned>(spi_s3_get_error_code()));
  shell_printf(state, "t41_device_status     : %d\n", static_cast<int>(spi_s3_get_device_status_code()));
  shell_printf(state, "t41_qspi_crc/marker    : %lu / %lu\n",
               static_cast<unsigned long>(spi_s3_get_crc_errors()),
               static_cast<unsigned long>(spi_s3_get_marker_errors()));
  shell_printf(state, "c3_spi_crc/marker    : %lu / %lu\n",
               static_cast<unsigned long>(spi_c3_get_crc_errors()),
               static_cast<unsigned long>(spi_c3_get_marker_errors()));
  shell_printf(state, "turret_pid_error     : %.2f\n", spi_s3_get_turret_pid_error());
}

void print_telemetry_status(ShellState& state) {
  WebServerDiagSnapshot diag {};
  web_server_get_diag_snapshot(&diag);
  shell_printf(state, "console_rev          : %lu\n", static_cast<unsigned long>(uart1_cobs_get_log_version()));
  shell_printf(state, "web_last_feedback_ms : %lu\n", static_cast<unsigned long>(diag.last_web_feedback_ms));
  shell_printf(state, "pid_avg/last/exec    : %.2f / %lu / %lu ms\n",
               diag.pid_cycle_avg_ms,
               static_cast<unsigned long>(diag.pid_cycle_last_ms),
               static_cast<unsigned long>(diag.pid_cycle_exec_ms));
  shell_printf(state, "pid_peak_ms          : %lu\n", static_cast<unsigned long>(diag.pid_cycle_peak_ms));
  shell_printf(state, "cpu_mhz c0/c1/target : %lu / %lu / %lu\n",
               static_cast<unsigned long>(diag.cpu_freq_core0_mhz),
               static_cast<unsigned long>(diag.cpu_freq_core1_mhz),
               static_cast<unsigned long>(diag.cpu_freq_target_mhz));
  shell_write_line(state, "websocket_clients    : n/a (not exposed)");
}

void print_perf_status(ShellState& state) {
  WebServerDiagSnapshot web {};
  web_server_get_diag_snapshot(&web);
  MrosRtosAggregateSnapshot rtos {};
  app_rtos_get_aggregate_diag(&rtos);
  mros::shell::service::ShellServiceMetrics shell_metrics {};
  mros::shell::service::get_metrics(&shell_metrics);
  mros::power::Status power {};
  mros::power::get_status(&power);
  char temp_text[20] = "n/a";
  if (power.temperature_valid) {
    std::snprintf(temp_text, sizeof(temp_text), "%.1fC", power.temperature_c);
  }
  shell_write_line(state, "Performance status");
  shell_printf(state, "  heap free/min       : %lu / %lu\n",
               static_cast<unsigned long>(mros::platform::mros_system_heap_free()),
               static_cast<unsigned long>(mros::platform::mros_system_heap_min_free()));
  shell_printf(state, "  psram free          : %lu\n",
               static_cast<unsigned long>(mros::platform::mros_system_psram_free()));
  shell_printf(state, "  power mode/cpu/temp : %s / %luMHz / %s\n",
               power.mode,
               static_cast<unsigned long>(power.actual_cpu_mhz),
               temp_text);
  shell_printf(state, "  sram floor/largest  : %s / %lu\n",
               power.sram_floor_state,
               static_cast<unsigned long>(power.internal_largest_block));
  shell_printf(state, "  pm locks/wifi-ps    : %s / %s\n",
               power.active_locks && power.active_locks[0] ? power.active_locks : "-",
               power.wifi_ps_mode);
  shell_printf(state, "  json_overflow       : %lu\n",
               static_cast<unsigned long>(mros::utils::json_overflow_count()));
  shell_printf(state, "  shell pool miss/drop: %lu / %lu\n",
               static_cast<unsigned long>(shell_metrics.response_pool_miss),
               static_cast<unsigned long>(shell_metrics.response_drop));
  shell_printf(state, "  mshell jobs         : %lu/%lu active, completed=%lu, drops=%lu\n",
               static_cast<unsigned long>(runtime::job_active_count()),
               static_cast<unsigned long>(runtime::job_capacity()),
               static_cast<unsigned long>(runtime::job_completed_count()),
               static_cast<unsigned long>(runtime::job_drop_count()));
  shell_printf(state, "  mshell job storage  : %lu bytes (%s)\n",
               static_cast<unsigned long>(runtime::job_storage_bytes()),
               runtime::job_storage_allocated()
                   ? (runtime::job_storage_uses_psram() ? "PSRAM" : "internal fallback")
                   : "lazy");
  shell_printf(state, "  mshell tx           : %s\n", runtime::tx_active() ? "active" : "idle");
  shell_printf(state, "  ws clients total/auth: %lu / %lu\n",
               static_cast<unsigned long>(web.ws_clients_total),
               static_cast<unsigned long>(web.ws_clients_auth));
  shell_printf(state, "  fk ms last/avg/max  : %.3f / %.3f / %.3f\n",
               web.fk_last_ms, web.fk_avg_ms, web.fk_max_ms);
  shell_printf(state, "  pid ms avg/last/exec: %.2f / %lu / %lu\n",
               web.pid_cycle_avg_ms,
               static_cast<unsigned long>(web.pid_cycle_last_ms),
               static_cast<unsigned long>(web.pid_cycle_exec_ms));
  shell_printf(state, "  rtos deadline/max   : %lu / %lu ms (%s)\n",
               static_cast<unsigned long>(rtos.total_slip_count),
               static_cast<unsigned long>(rtos.max_slip_ms),
               rtos.max_slip_task != nullptr ? rtos.max_slip_task : "-");
}

void print_rtos_status(ShellState& state) {
  MrosRtosAggregateSnapshot agg {};
  app_rtos_get_aggregate_diag(&agg);
  shell_write_line(state, "RTOS runtime status");
  shell_printf(state, "  tracked tasks       : %lu\n", static_cast<unsigned long>(agg.task_count));
  shell_printf(state, "  wakes/deadline miss : %lu / %lu\n",
               static_cast<unsigned long>(agg.total_wake_count),
               static_cast<unsigned long>(agg.total_slip_count));
  shell_printf(state, "  max deadline miss   : %lu ms (%s)\n",
               static_cast<unsigned long>(agg.max_slip_ms),
               agg.max_slip_task != nullptr ? agg.max_slip_task : "-");
  shell_printf(state, "  max last exec       : %lu ms (%s)\n",
               static_cast<unsigned long>(agg.max_exec_ms),
               agg.max_exec_task != nullptr ? agg.max_exec_task : "-");
  shell_write_line(state, "  detail              : use ps -l for per-task stack/runtime table");
}

void print_wifi_diag_status(ShellState& state) {
  WifiManagerSnapshot snap = wifi_snapshot();
  shell_write_line(state, "WiFi diagnostic summary");
  shell_printf(state, "  phase/enabled       : %s / %s\n",
               snap.phase.c_str(), wifi_manager_is_enabled() ? "yes" : "no");
  shell_printf(state, "  sta/ip/rssi/ch      : %s / %s / %ld / %u\n",
               snap.state.sta_connected ? "connected" : "down",
               snap.ip.length() ? snap.ip.c_str() : "-",
               static_cast<long>(snap.state.rssi),
               static_cast<unsigned>(snap.current_channel));
  shell_printf(state, "  last-good           : %s ch=%u bssid=%s\n",
               snap.last_good_ssid.length() ? snap.last_good_ssid.c_str() : "-",
               static_cast<unsigned>(snap.last_good_channel),
               snap.last_good_bssid.length() ? snap.last_good_bssid.c_str() : "-");
  shell_printf(state, "  fast-path ok/attempt: %lu / %lu\n",
               static_cast<unsigned long>(snap.fast_path_successes),
               static_cast<unsigned long>(snap.fast_path_attempts));
  shell_printf(state, "  scan age/backoff    : %lu / %lu ms\n",
               static_cast<unsigned long>(snap.scan_age_ms),
               static_cast<unsigned long>(snap.reconnect_backoff_ms));
  shell_printf(state, "  last connect ms     : %lu\n",
               static_cast<unsigned long>(snap.last_connect_duration_ms));
}

void print_pid_status(ShellState& state) {
  float kp = 0.0f;
  float ki = 0.0f;
  float kd = 0.0f;
  float imax = 0.0f;
  spi_s3_get_turret_pid(&kp, &ki, &kd, &imax);

  WebServerDiagSnapshot diag {};
  web_server_get_diag_snapshot(&diag);
  ControlManagerDiag ctrl_diag {};
  control_manager_get_diag(&ctrl_diag);
  shell_write_line(state, "PID status");
  shell_printf(state, "  target/actual deg   : %.2f / %.2f\n", spi_s3_get_turret_deg(), spi_s3_get_turret_actual_deg());
  shell_printf(state, "  error/output        : %.3f / %.3f\n", spi_s3_get_turret_pid_error(), spi_s3_get_turret_pid_output());
  shell_printf(state, "  kp/ki/kd/imax       : %.4f / %.4f / %.4f / %.4f\n", kp, ki, kd, imax);
  shell_printf(state, "  dspc                : %.4f\n", spi_s3_get_turret_dspc());
  shell_printf(state, "  output_lock         : %s\n", yn(spi_s3_get_turret_output_lock()));
  shell_printf(state, "  motor/pca_oe        : %u / %s\n",
               static_cast<unsigned>(spi_s3_get_motor_state()),
               yn(pca9685_get_output_enable()));
  shell_printf(state, "  loop avg/last/exec  : %.2f / %lu / %lu ms\n",
               diag.pid_cycle_avg_ms,
               static_cast<unsigned long>(diag.pid_cycle_last_ms),
               static_cast<unsigned long>(diag.pid_cycle_exec_ms));
  shell_printf(state, "  loop peak           : %lu ms\n", static_cast<unsigned long>(diag.pid_cycle_peak_ms));
  shell_printf(state, "  local sat/clamp/fault: %lu / %lu / %lu\n",
               static_cast<unsigned long>(ctrl_diag.turret_saturation_count),
               static_cast<unsigned long>(ctrl_diag.turret_integral_clamp_count),
               static_cast<unsigned long>(ctrl_diag.turret_finite_fault_count));
  shell_printf(state, "  local max_err/dt    : %.3f / %.4f s\n",
               ctrl_diag.turret_max_abs_error,
               ctrl_diag.turret_last_dt_s);
}

void print_fk_status(ShellState& state) {
  WebServerDiagSnapshot diag {};
  web_server_get_diag_snapshot(&diag);
  shell_write_line(state, "FK status");
  shell_printf(state, "  t41 coord x/y/z/a   : %.1f / %.1f / %.1f / %.1f\n",
               spi_s3_get_coord_x(),
               spi_s3_get_coord_y(),
               spi_s3_get_coord_z(),
               spi_s3_get_alpha());
  shell_printf(state, "  live web fk ms      : last=%.3f avg=%.3f max=%.3f samples=%lu\n",
               diag.fk_last_ms,
               diag.fk_avg_ms,
               diag.fk_max_ms,
               static_cast<unsigned long>(diag.fk_samples));
  shell_printf(state, "  active ik backend   : %s\n",
               spi_s3_is_connected() ? "T41-QSPI" : (spi_c3_is_espnow_connected() ? "T41-ESP-NOW" : "WEB"));
  shell_printf(state, "  live_preview        : %s\n", diag.fk_samples > 0U ? "active" : "waiting");
}

void print_ik_status(ShellState& state) {
  const char* pref = web_server_get_ik_compute_preference();
  const bool t41 = spi_s3_is_connected();
  const bool espnow = spi_c3_is_espnow_connected();
  const char* active = t41 ? "T41-QSPI" : (espnow ? "T41-ESP-NOW" : "WEB");
  shell_write_line(state, "IK status");
  shell_printf(state, "  preference          : %s\n", pref != nullptr ? pref : "auto");
  shell_printf(state, "  effective backend   : %s\n", active);
  shell_printf(state, "  t41_qspi/espnow     : %s / %s\n", yn(t41), yn(espnow));
  shell_printf(state, "  target coord x/y/z/a: %.1f / %.1f / %.1f / %.1f\n",
               spi_s3_get_coord_x(),
               spi_s3_get_coord_y(),
               spi_s3_get_coord_z(),
               spi_s3_get_alpha());
  shell_printf(state, "  traj_scale          : %.2f\n", spi_s3_get_joint_traj_time_scale());
  shell_write_line(state, "  web note            : calc=web runs in the browser; keep the web UI open for --apply");
}

void print_overview(ShellState& state) {
  const WifiManagerSnapshot wifi = wifi_snapshot();
  uint64_t fs_total = 0U;
  uint64_t fs_used = 0U;
  (void)logger_storage_info(&fs_total, &fs_used);
  shell_write_line(state, "MROS S3 bridge overview");
  print_rule(state);
  shell_printf(state, "uptime_ms       : %lu\n", static_cast<unsigned long>(mros::platform::mros_millis()));
  shell_printf(state, "wifi            : %s %s\n", wifi_status_label(wifi),
               wifi_ip_or(wifi).c_str());
  shell_printf(state, "heap_free       : %lu\n", static_cast<unsigned long>(mros::platform::mros_system_heap_free()));
  shell_printf(state, "psram_free      : %lu\n", static_cast<unsigned long>(mros::platform::mros_system_psram_free()));
  shell_printf(state, "littlefs        : %luK used / %luK total\n",
               static_cast<unsigned long>(fs_used / 1024U),
               static_cast<unsigned long>(fs_total / 1024U));
  print_rule(state);
  print_connection_table(state);
}

int print_health(ShellState& state) {
  const bool t41 = spi_s3_is_connected();
  const bool c3 = spi_c3_is_connected();
  const bool pca = pca9685_is_ready();
  const bool spi_clean = (spi_s3_get_crc_errors() == 0U && spi_s3_get_marker_errors() == 0U &&
                          spi_c3_get_crc_errors() == 0U && spi_c3_get_marker_errors() == 0U);
  shell_printf(state, "overall         : %s\n", (t41 && c3 && pca && spi_clean) ? "OK" : "WARN");
  shell_printf(state, "t41_qspi        : %s\n", ok_warn(t41));
  shell_printf(state, "c3_spi          : %s\n", ok_warn(c3));
  shell_printf(state, "pca9685         : %s\n", ok_warn(pca));
  shell_printf(state, "spi_errors      : %s\n", ok_warn(spi_clean));
  shell_printf(state, "motor_power     : %u\n", static_cast<unsigned>(spi_s3_get_motor_state()));
  mros::health::bearing::BearingHealthSnapshot bearing {};
  if (mros::health::bearing::get_snapshot(&bearing)) {
    shell_printf(state, "bearing_health  : %s specs=%lu placements=%lu update=%luHz\n",
                 bearing.status,
                 static_cast<unsigned long>(bearing.spec_count),
                 static_cast<unsigned long>(bearing.placement_count),
                 static_cast<unsigned long>(bearing.update_hz));
    if (bearing.configured) {
      shell_write_line(state, "");
      shell_write(state, mros::health::bearing::format_table().c_str());
    } else {
      shell_printf(state, "bearing_config  : %s\n", bearing.config_path);
    }
  }
  return (t41 && c3 && pca && spi_clean) ? 0 : 2;
}

struct WatchOptions {
  uint32_t interval_ms = kDefaultWatchIntervalMs;
  size_t cycles = kDefaultWatchCycles;
  size_t next_index = 0U;
};

bool parse_watch_options(ShellContext& ctx, size_t start, WatchOptions* opts) {
  if (opts == nullptr) return false;
  size_t i = start;
  while (i < ctx.args.size()) {
    const std::string& arg = ctx.args[i];
    if (arg == "-n" || arg == "--interval") {
      if ((i + 1U) >= ctx.args.size()) {
        shell_write_line(ctx.state, "mros: missing interval value");
        return false;
      }
      uint32_t seconds = 0;
      if (!parse_u32(ctx.args[i + 1U], &seconds) || seconds == 0U) {
        shell_write_line(ctx.state, "mros: invalid interval");
        return false;
      }
      opts->interval_ms = std::min(seconds * 1000U, kMaxWatchIntervalMs);
      i += 2U;
      continue;
    }
    if (arg == "--count" || arg == "--cycles") {
      if ((i + 1U) >= ctx.args.size() || !parse_size(ctx.args[i + 1U], &opts->cycles)) {
        shell_write_line(ctx.state, "mros: invalid cycle count");
        return false;
      }
      opts->cycles = std::min(opts->cycles, kMaxWatchCycles);
      i += 2U;
      continue;
    }
    break;
  }
  opts->next_index = i;
  return true;
}

int dispatch_mros(ShellContext& ctx);

int handle_ssh(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage:");
    shell_write_line(ctx.state, "  mros ssh status");
    shell_write_line(ctx.state, "  mros ssh enable");
    shell_write_line(ctx.state, "  mros ssh set disable");
    shell_write_line(ctx.state, "  mros ssh set port 5378");
    shell_write_line(ctx.state, "  mros ssh set passwd \"new-pass\"");
    shell_write_line(ctx.state, "  mros ssh set pubkey \"ssh-ed25519 ...\"");
    shell_write_line(ctx.state, "  mros ssh list");
    return ctx.args.size() < 3U ? 1 : 0;
  }

  const std::string& sub = ctx.args[2];
  if (sub == "status") {
    shell_write_line(ctx.state, mros::ssh::service_status_text().c_str());
    return 0;
  }
  if (sub == "enable") {
    if (!mros::ssh::service_enable()) {
      shell_write_line(ctx.state, "mros ssh: enable failed");
      return 1;
    }
    shell_write_line(ctx.state, "mros ssh: enabled and saved");
    shell_write_line(ctx.state, mros::ssh::service_status_text().c_str());
    return 0;
  }
  if (sub == "list") {
    shell_write_line(ctx.state, mros::ssh::service_sessions_text().c_str());
    return 0;
  }
  if (sub == "set") {
    if (ctx.args.size() < 4U) {
      shell_write_line(ctx.state, "mros ssh set: missing key");
      return 1;
    }
    const std::string& key = ctx.args[3];
    if (key == "disable") {
      if (!mros::ssh::service_disable()) {
        shell_write_line(ctx.state, "mros ssh: disable failed");
        return 1;
      }
      shell_write_line(ctx.state, "mros ssh: disabled and saved");
      return 0;
    }
    if (key == "port") {
      if (ctx.args.size() < 5U) {
        shell_write_line(ctx.state, "mros ssh set port: value required");
        return 1;
      }
      uint32_t parsed = 0;
      if (!parse_u32(ctx.args[4], &parsed) || parsed == 0U || parsed > 65535U) {
        shell_write_line(ctx.state, "mros ssh set port: invalid port");
        return 1;
      }
      if (!mros::ssh::service_set_port(static_cast<uint16_t>(parsed))) {
        shell_write_line(ctx.state, "mros ssh: port save failed");
        return 1;
      }
      shell_printf(ctx.state, "mros ssh: port saved as %u\n", static_cast<unsigned>(parsed));
      return 0;
    }
    if (key == "passwd") {
      if (ctx.args.size() < 5U) {
        shell_write_line(ctx.state, "mros ssh set passwd: password required");
        return 1;
      }
      const mros::ssh::IdentityConfig cfg = mros::ssh::identity_get();
      if (!mros::ssh::set_password_for_user(cfg.username, String(ctx.args[4].c_str()))) {
        shell_write_line(ctx.state, "mros ssh set passwd: password must be 8-96 chars");
        return 1;
      }
      shell_write_line(ctx.state, "mros ssh: user password hash saved");
      return 0;
    }
    if (key == "root-passwd") {
      if (!ctx.state.root_session) {
        shell_write_line(ctx.state, "mros ssh set root-passwd: run su first");
        return 1;
      }
      if (ctx.args.size() < 5U ||
          !mros::ssh::set_password_for_user(mros::ssh::root_username(), String(ctx.args[4].c_str()))) {
        shell_write_line(ctx.state, "mros ssh set root-passwd: password must be 8-96 chars");
        return 1;
      }
      shell_write_line(ctx.state, "mros ssh: root password hash saved");
      return 0;
    }
    if (key == "pubkey") {
      if (ctx.args.size() < 5U) {
        shell_write_line(ctx.state, "mros ssh set pubkey: public key required");
        return 1;
      }
      if (!mros::ssh::append_authorized_key(String(ctx.args[4].c_str()))) {
        shell_write_line(ctx.state, "mros ssh set pubkey: expected ssh-ed25519 or ssh-rsa key");
        return 1;
      }
      shell_printf(ctx.state, "mros ssh: public key appended to %s\n",
                   mros::ssh::authorized_keys_path().c_str());
      return 0;
    }
    shell_printf(ctx.state, "mros ssh set: unknown key '%s'\n", key.c_str());
    return 1;
  }

  shell_printf(ctx.state, "mros ssh: unknown command '%s'\n", sub.c_str());
  return 1;
}

int handle_security(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage:");
    shell_write_line(ctx.state, "  mros security status");
    shell_write_line(ctx.state, "  mros security logout-all");
    shell_write_line(ctx.state, "  mros security audit");
    return ctx.args.size() < 3U ? 1 : 0;
  }

  const std::string sub = lower_copy(ctx.args[2]);
  if (sub == "status") {
    WebSecuritySnapshot sec {};
    web_server_get_security_snapshot(&sec);
    shell_write_line(ctx.state, "Security:");
    shell_printf(ctx.state, "  http_session_active : %s\n", sec.auth_session_active ? "yes" : "no");
    shell_printf(ctx.state, "  login_fail_count    : %lu\n", static_cast<unsigned long>(sec.login_fail_count));
    shell_printf(ctx.state, "  login_lockout_ms    : %lu\n", static_cast<unsigned long>(sec.login_lockout_ms));
    shell_printf(ctx.state, "  serial_auth         : %s\n", mros::shell::serial_auth_mode_text());
    shell_printf(ctx.state, "  ws_auth             : telemetry=%lu shell=%lu debug=%lu\n",
                 static_cast<unsigned long>(sec.ws_auth_count),
                 static_cast<unsigned long>(sec.ws_shell_auth_count),
                 static_cast<unsigned long>(sec.ws_debug_auth_count));
    shell_printf(ctx.state, "  shell_sessions      : %lu/%lu active, root=%lu\n",
                 static_cast<unsigned long>(active_session_count()),
                 static_cast<unsigned long>(session_capacity()),
                 static_cast<unsigned long>(active_root_session_count()));
    shell_printf(ctx.state, "  capabilities        : %s\n", capabilities_text(ctx.state.capability_mask));
    shell_printf(ctx.state, "  uart_shell_bridge   : %s\n",
                 remote::bridge_mode_name(remote::bridge_mode()));
    return 0;
  }

  if (sub == "logout-all") {
    if (!ctx.state.root_session && !ctx.state.session_admin) {
      shell_write_line(ctx.state, "mros security logout-all: admin/root required");
      return 1;
    }
    web_server_logout_all();
    shell_write_line(ctx.state, "mros security: all HTTP/WS sessions invalidated");
    return 0;
  }

  if (sub == "audit") {
    const std::string audit = audit_report();
    shell_write(ctx.state, audit.c_str());
    return 0;
  }

  shell_printf(ctx.state, "mros security: unknown command '%s'\n", sub.c_str());
  return 1;
}

int handle_users(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage:");
    shell_write_line(ctx.state, "  mros users list");
    shell_write_line(ctx.state, "  mros users roles");
    shell_write_line(ctx.state, "  mros users add <display-name> <username> <password> [admin|user] [sudo|nosudo]");
    shell_write_line(ctx.state, "  mros users passwd <username> <password>");
    shell_write_line(ctx.state, "  mros users disable <username>");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  const std::string sub = lower_copy(ctx.args[2]);
  if (sub == "list" || sub == "roles") {
    const std::vector<mros::ssh::UserAccount> users = mros::ssh::list_users();
    shell_write_line(ctx.state, "user              role       admin sudo primary root");
    shell_write_line(ctx.state, "----------------  ---------  ----- ---- ------- ----");
    for (const mros::ssh::UserAccount& user : users) {
      const char* role = user.root ? "root" : (user.admin ? "admin" : "user");
      shell_printf(ctx.state, "%-16s  %-9s  %-5s %-4s %-7s %-4s\n",
                   user.username.c_str(),
                   role,
                   yn(user.admin),
                   yn(user.sudo),
                   yn(user.primary),
                   yn(user.root));
    }
    if (sub == "roles") {
      shell_write_line(ctx.state, "");
      shell_write_line(ctx.state, "Capabilities:");
      shell_write_line(ctx.state, "  user  : read,write,network");
      shell_write_line(ctx.state, "  admin : read,write,robot,network,update,debug");
      shell_write_line(ctx.state, "  root  : admin + root-only commands");
    }
    return 0;
  }

  const bool admin_context = ctx.state.root_session || ctx.state.session_admin;
  if (sub == "add") {
    if (!admin_context) {
      shell_write_line(ctx.state, "mros users add: admin/root required");
      return 1;
    }
    if (ctx.args.size() < 6U) {
      shell_write_line(ctx.state, "mros users add: expected display-name username password [admin|user] [sudo|nosudo]");
      return 1;
    }
    bool admin = false;
    bool sudo = false;
    if (ctx.args.size() >= 7U) {
      const std::string role = lower_copy(ctx.args[6]);
      admin = role == "admin" || role == "operator";
    }
    if (ctx.args.size() >= 8U) {
      const std::string sudo_arg = lower_copy(ctx.args[7]);
      sudo = sudo_arg == "sudo" || sudo_arg == "yes" || sudo_arg == "on";
    }
    if (!mros::ssh::add_user(String(ctx.args[3].c_str()),
                             String(ctx.args[4].c_str()),
                             String(ctx.args[5].c_str()),
                             admin,
                             sudo)) {
      shell_write_line(ctx.state, "mros users add: failed (limit is 4 normal users, check username/password)");
      return 1;
    }
    audit_record("user-add", ctx.args[4].c_str());
    shell_printf(ctx.state, "mros users: added %s\n", ctx.args[4].c_str());
    return 0;
  }
  if (sub == "passwd") {
    if (ctx.args.size() < 5U) {
      shell_write_line(ctx.state, "mros users passwd: expected username password");
      return 1;
    }
    const bool self_change = ctx.args[3] == ctx.state.session_username;
    if (!admin_context && !self_change) {
      shell_write_line(ctx.state, "mros users passwd: admin/root or self required");
      return 1;
    }
    if (ctx.args[3] == mros::ssh::root_username() && !ctx.state.root_session) {
      shell_write_line(ctx.state, "mros users passwd: root session required for root password");
      return 1;
    }
    if (!mros::ssh::set_password_for_user(String(ctx.args[3].c_str()), String(ctx.args[4].c_str()))) {
      shell_write_line(ctx.state, "mros users passwd: failed");
      return 1;
    }
    audit_record("user-passwd", ctx.args[3].c_str());
    shell_printf(ctx.state, "mros users: password updated for %s\n", ctx.args[3].c_str());
    return 0;
  }
  if (sub == "disable") {
    if (!admin_context) {
      shell_write_line(ctx.state, "mros users disable: admin/root required");
      return 1;
    }
    if (ctx.args.size() < 4U) {
      shell_write_line(ctx.state, "mros users disable: username required");
      return 1;
    }
    if (!mros::ssh::disable_user(String(ctx.args[3].c_str()))) {
      shell_write_line(ctx.state, "mros users disable: only extra users can be disabled");
      return 1;
    }
    audit_record("user-disable", ctx.args[3].c_str());
    shell_printf(ctx.state, "mros users: disabled %s\n", ctx.args[3].c_str());
    return 0;
  }
  shell_printf(ctx.state, "mros users: unknown command '%s'\n", sub.c_str());
  return 1;
}

int run_watch(ShellContext& ctx, const size_t command_index) {
  WatchOptions opts {};
  if (!parse_watch_options(ctx, command_index, &opts)) return 1;
  if (opts.next_index >= ctx.args.size()) {
    shell_write_line(ctx.state, "mros watch: command required");
    return 1;
  }

  std::vector<std::string> child_args;
  child_args.push_back("mros");
  for (size_t i = opts.next_index; i < ctx.args.size(); ++i) {
    child_args.push_back(ctx.args[i]);
  }
  if (child_args.size() > 1U && child_args[1] == "watch") {
    shell_write_line(ctx.state, "mros watch: nested watch is not allowed");
    return 1;
  }

  int result = 0;
  for (size_t cycle = 0U; cycle < opts.cycles; ++cycle) {
    shell_printf(ctx.state, "\n[mros watch] cycle=%u/%u interval=%lums command=",
                 static_cast<unsigned>(cycle + 1U),
                 static_cast<unsigned>(opts.cycles),
                 static_cast<unsigned long>(opts.interval_ms));
    for (size_t i = 1U; i < child_args.size(); ++i) {
      shell_printf(ctx.state, "%s%s", i > 1U ? " " : "", child_args[i].c_str());
    }
    shell_write(ctx.state, "\n");
    print_rule(ctx.state);
    ShellContext child {ctx.state, child_args, ctx.stdin_buffer, ctx.json_output};
    result = dispatch_mros(child);
    if ((cycle + 1U) < opts.cycles) {
      vTaskDelay(pdMS_TO_TICKS(opts.interval_ms));
    }
  }
  return result;
}

int handle_connections(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mros connections <list|status|tree|watch>");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  const std::string& sub = ctx.args[2];
  if (sub == "list" || sub == "status") {
    print_connection_table(ctx.state);
    return 0;
  }
  if (sub == "tree") {
    print_connections_tree(ctx.state);
    return 0;
  }
  if (sub == "watch") {
    std::vector<std::string> args = {"mros", "watch"};
    for (size_t i = 3U; i < ctx.args.size(); ++i) args.push_back(ctx.args[i]);
    args.push_back("connections");
    args.push_back("status");
    ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output};
    return dispatch_mros(child);
  }
  shell_printf(ctx.state, "mros connections: unknown subcommand '%s'\n", sub.c_str());
  return 1;
}

int handle_bus(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mros bus <summary|errors>");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  if (ctx.args[2] == "summary") {
    print_connection_table(ctx.state);
    return 0;
  }
  if (ctx.args[2] == "errors") {
    print_bus_errors(ctx.state);
    return 0;
  }
  shell_printf(ctx.state, "mros bus: unknown subcommand '%s'\n", ctx.args[2].c_str());
  return 1;
}

int handle_spi(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mros spi <list|status|errors|reset-stats|monitor>");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  const std::string& sub = ctx.args[2];
  if (sub == "list") {
    print_spi_list(ctx.state);
    return 0;
  }
  if (sub == "status") {
    const std::string peer = ctx.args.size() > 3U ? ctx.args[3] : "all";
    if (peer != "t41" && peer != "t41" && peer != "c3" && peer != "all") {
      shell_write_line(ctx.state, "mros spi status: peer must be t41, c3 or all");
      return 1;
    }
    print_spi_status(ctx.state, peer);
    return 0;
  }
  if (sub == "errors") {
    shell_printf(ctx.state, "t41 errors json: %s\n", spi_s3_get_error_log_json().c_str());
    shell_printf(ctx.state, "C3 counters: crc=%lu marker=%lu total_rx=%lu\n",
                 static_cast<unsigned long>(spi_c3_get_crc_errors()),
                 static_cast<unsigned long>(spi_c3_get_marker_errors()),
                 static_cast<unsigned long>(spi_c3_get_total_rx()));
    return 0;
  }
  if (sub == "reset-stats") {
    spi_s3_reset_error_counters();
    spi_c3_reset_error_counters();
    shell_write_line(ctx.state, "mros spi: T41/C3 error counters reset");
    return 0;
  }
  if (sub == "monitor") {
    std::vector<std::string> args = {"mros", "watch"};
    size_t i = 3U;
    std::string peer = "all";
    if (i < ctx.args.size() && ctx.args[i] != "-n" && ctx.args[i] != "--interval" &&
        ctx.args[i] != "--count" && ctx.args[i] != "--cycles") {
      peer = ctx.args[i++];
    }
    for (; i < ctx.args.size(); ++i) args.push_back(ctx.args[i]);
    args.push_back("spi");
    args.push_back("status");
    args.push_back(peer);
    ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output};
    return dispatch_mros(child);
  }
  shell_printf(ctx.state, "mros spi: unknown subcommand '%s'\n", sub.c_str());
  return 1;
}

int handle_uart(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mros uart <list|status|tail|monitor|log-mode|shell>");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  const std::string& sub = ctx.args[2];
  if (sub == "shell") {
    if (ctx.args.size() < 4U || is_help_arg(ctx.args[3])) {
      shell_write_line(ctx.state, "Usage: mros uart shell <status|reset>");
      return ctx.args.size() < 4U ? 1 : 0;
    }
    const std::string action = lower_copy(ctx.args[3]);
    if (action == "status") {
      shell_write(ctx.state, remote::devices_report(&ctx.state).c_str());
      return 0;
    }
    if (action == "reset") {
      remote::reset_bridge_state();
      audit_record("uart-shell-reset", "");
      shell_write_line(ctx.state, "mros uart shell: bridge state reset");
      return 0;
    }
    shell_printf(ctx.state, "mros uart shell: unknown action '%s'\n", action.c_str());
    return 1;
  }
  if (sub == "list") {
    shell_write_line(ctx.state, "name        peer      protocol     status");
    shell_write_line(ctx.state, "----------  --------  -----------  --------");
    shell_printf(ctx.state, "t41-log     TEENSY4.1 text/log     rev=%lu\n", static_cast<unsigned long>(uart1_cobs_get_log_version()));
    shell_printf(ctx.state, "encoder     ESP32-C3  via C3 SPI   %s\n", spi_c3_is_connected() ? "connected" : "down");
    return 0;
  }
  if (sub == "status") {
    print_uart_status(ctx.state);
    return 0;
  }
  if (sub == "log-mode") {
    if (ctx.args.size() < 4U) {
      shell_printf(ctx.state, "log-mode: %s\n",
                   uart1_cobs_log_noise_mode_name(uart1_cobs_get_log_noise_mode()));
      shell_write_line(ctx.state, "Usage: mros uart log-mode <quiet|normal|verbose>");
      return 0;
    }
    if (ctx.args[3] == "quiet") {
      uart1_cobs_set_log_noise_mode(UartLogNoiseMode::Quiet);
    } else if (ctx.args[3] == "normal") {
      uart1_cobs_set_log_noise_mode(UartLogNoiseMode::Normal);
    } else if (ctx.args[3] == "verbose") {
      uart1_cobs_set_log_noise_mode(UartLogNoiseMode::Verbose);
    } else {
      shell_printf(ctx.state, "mros uart log-mode: unknown mode '%s'\n", ctx.args[3].c_str());
      return 1;
    }
    shell_printf(ctx.state, "log-mode: %s\n",
                 uart1_cobs_log_noise_mode_name(uart1_cobs_get_log_noise_mode()));
    return 0;
  }
  if (sub == "tail") {
    size_t lines = 20U;
    for (size_t i = 3U; i < ctx.args.size(); ++i) {
      if (ctx.args[i] == "-n" && (i + 1U) < ctx.args.size()) {
        parse_size(ctx.args[i + 1U], &lines);
        ++i;
      }
    }
    print_log_tail(ctx.state, lines, "");
    return 0;
  }
  if (sub == "monitor") {
    std::vector<std::string> args = {"mros", "watch"};
    for (size_t i = 3U; i < ctx.args.size(); ++i) {
      if (ctx.args[i] != "encoder") args.push_back(ctx.args[i]);
    }
    args.push_back("uart");
    args.push_back("status");
    ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output};
    return dispatch_mros(child);
  }
  shell_printf(ctx.state, "mros uart: unknown subcommand '%s'\n", sub.c_str());
  return 1;
}

int handle_i2c(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mros i2c <scan|status>");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  if (ctx.args[2] == "scan") {
    print_i2c_scan(ctx.state);
    return 0;
  }
  if (ctx.args[2] == "status") {
    print_i2c_status(ctx.state);
    return 0;
  }
  shell_printf(ctx.state, "mros i2c: unknown subcommand '%s'\n", ctx.args[2].c_str());
  return 1;
}

int handle_pca(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mros pca9685 <status|channels>");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  if (ctx.args[2] == "status") {
    print_i2c_status(ctx.state);
    return 0;
  }
  if (ctx.args[2] == "channels") {
    print_pca_channels(ctx.state);
    return 0;
  }
  shell_printf(ctx.state, "mros pca9685: unknown subcommand '%s'\n", ctx.args[2].c_str());
  return 1;
}

int handle_log(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mros log <tail|follow> [-n LINES] [--cycles NUM] [--interval SEC]");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  size_t lines = 50U;
  WatchOptions opts {};
  opts.interval_ms = 1000U;
  opts.cycles = 30U;
  for (size_t i = 3U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "-n" && (i + 1U) < ctx.args.size()) {
      parse_size(ctx.args[i + 1U], &lines);
      ++i;
    } else if ((ctx.args[i] == "--cycles" || ctx.args[i] == "--count") && (i + 1U) < ctx.args.size()) {
      parse_size(ctx.args[i + 1U], &opts.cycles);
      opts.cycles = std::min(opts.cycles, kMaxWatchCycles);
      ++i;
    } else if ((ctx.args[i] == "--interval" || ctx.args[i] == "--interval-sec") && (i + 1U) < ctx.args.size()) {
      uint32_t sec = 0;
      if (parse_u32(ctx.args[i + 1U], &sec) && sec > 0U) {
        opts.interval_ms = std::min(sec * 1000U, kMaxWatchIntervalMs);
      }
      ++i;
    }
  }
  if (ctx.args[2] == "tail") {
    print_log_tail(ctx.state, lines, "");
    return 0;
  }
  if (ctx.args[2] == "follow") {
    for (size_t cycle = 0U; cycle < opts.cycles; ++cycle) {
      shell_printf(ctx.state, "\n[mros log follow] cycle=%u/%u\n",
                   static_cast<unsigned>(cycle + 1U),
                   static_cast<unsigned>(opts.cycles));
      print_log_tail(ctx.state, lines, "");
      if ((cycle + 1U) < opts.cycles) vTaskDelay(pdMS_TO_TICKS(opts.interval_ms));
    }
    return 0;
  }
  shell_printf(ctx.state, "mros log: unknown subcommand '%s'\n", ctx.args[2].c_str());
  return 1;
}

bool ensure_user_dir() {
  if (!mros::platform::mros_fs_exists("/ESPUSER")) {
    mros::platform::mros_fs_mkdir("/ESPUSER");
  }
  return mros::platform::mros_fs_exists("/ESPUSER");
}

void write_spi_csv_row(FILE* file) {
  if (file == nullptr) {
    return;
  }
  std::fprintf(file,
               "%lu,%u,%lu,%lu,%u,%lu,%lu,%lu,%u,%.2f,%.2f\n",
               static_cast<unsigned long>(mros::platform::mros_millis()),
               spi_s3_is_connected() ? 1U : 0U,
               static_cast<unsigned long>(spi_s3_get_total_transactions()),
               static_cast<unsigned long>(spi_s3_get_crc_errors()),
               static_cast<unsigned>(spi_s3_get_last_rx_seq()),
               static_cast<unsigned long>(spi_s3_get_marker_errors()),
               static_cast<unsigned long>(spi_c3_get_total_rx()),
               static_cast<unsigned long>(spi_c3_get_crc_errors()),
               static_cast<unsigned>(spi_c3_get_loop_hz()),
               spi_c3_get_position_deg(),
               spi_c3_get_speed_deg_s());
}

int handle_record(ShellContext& ctx) {
  if (ctx.args.size() < 5U || ctx.args[2] != "start" || ctx.args[3] != "spi") {
    shell_write_line(ctx.state, "Usage: mros record start spi [--seconds NUM]");
    return 1;
  }
  uint32_t seconds = 30U;
  for (size_t i = 4U; i < ctx.args.size(); ++i) {
    if (ctx.args[i] == "--seconds" && (i + 1U) < ctx.args.size()) {
      parse_u32(ctx.args[i + 1U], &seconds);
      ++i;
    }
  }
  seconds = std::max<uint32_t>(1U, std::min<uint32_t>(seconds, 30U));
  if (!logger_storage_ready() || !ensure_user_dir()) {
    shell_write_line(ctx.state, "mros record: LittleFS is not ready");
    return 1;
  }
  const char* path = "/ESPUSER/diag_spi.csv";
  FILE* file = mros::platform::mros_fs_open(path, "wb");
  if (file == nullptr) {
    shell_write_line(ctx.state, "mros record: unable to open /ESPUSER/diag_spi.csv");
    return 1;
  }
  std::fprintf(file, "ms,t41_connected,t41_qspi_txn,t41_qspi_crc,t41_qspi_seq,t41_qspi_marker,c3_rx,c3_crc,c3_hz,c3_pos,c3_speed\n");
  for (uint32_t i = 0U; i < seconds; ++i) {
    write_spi_csv_row(file);
    if ((i + 1U) < seconds) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  std::fclose(file);
  shell_printf(ctx.state, "mros record: wrote %lu samples to %s\n", static_cast<unsigned long>(seconds), path);
  return 0;
}

int handle_export(ShellContext& ctx) {
  if (ctx.args.size() < 3U || ctx.args[2] != "diagnostics") {
    shell_write_line(ctx.state, "Usage: mros export diagnostics");
    return 1;
  }
  if (!logger_storage_ready() || !ensure_user_dir()) {
    shell_write_line(ctx.state, "mros export: LittleFS is not ready");
    return 1;
  }
  const WifiManagerSnapshot wifi = wifi_snapshot();
  const char* path = "/ESPUSER/diagnostics.txt";
  FILE* file = mros::platform::mros_fs_open(path, "wb");
  if (file == nullptr) {
    shell_write_line(ctx.state, "mros export: unable to open diagnostics file");
    return 1;
  }
  std::fprintf(file, "uptime_ms=%lu\n", static_cast<unsigned long>(mros::platform::mros_millis()));
  std::fprintf(file, "wifi=%s ip=%s rssi=%ld\n",
               wifi_status_label(wifi),
               wifi_ip_or(wifi).c_str(),
               wifi_rssi_or(wifi));
  std::fprintf(file, "t41 connected=%u loop_ms=%u crc=%lu marker=%lu seq=%u\n",
               spi_s3_is_connected() ? 1U : 0U,
               static_cast<unsigned>(spi_s3_get_loop_ms()),
               static_cast<unsigned long>(spi_s3_get_crc_errors()),
               static_cast<unsigned long>(spi_s3_get_marker_errors()),
               static_cast<unsigned>(spi_s3_get_last_rx_seq()));
  std::fprintf(file, "c3 connected=%u hz=%u rx=%lu crc=%lu marker=%lu pos=%.2f speed=%.2f\n",
               spi_c3_is_connected() ? 1U : 0U,
               static_cast<unsigned>(spi_c3_get_loop_hz()),
               static_cast<unsigned long>(spi_c3_get_total_rx()),
               static_cast<unsigned long>(spi_c3_get_crc_errors()),
               static_cast<unsigned long>(spi_c3_get_marker_errors()),
               spi_c3_get_position_deg(),
               spi_c3_get_speed_deg_s());
  std::fprintf(file, "pca ready=%u freq=%.1f oe=%u\n",
               pca9685_is_ready() ? 1U : 0U,
               pca9685_get_frequency(),
               pca9685_get_output_enable() ? 1U : 0U);
  std::fprintf(file, "robot turret=%.2f actual=%.2f pid_out=%.2f pid_err=%.2f error_code=%u\n",
               spi_s3_get_turret_deg(),
               spi_s3_get_turret_actual_deg(),
               spi_s3_get_turret_pid_output(),
               spi_s3_get_turret_pid_error(),
               static_cast<unsigned>(spi_s3_get_error_code()));
  std::fclose(file);
  shell_printf(ctx.state, "mros export: wrote diagnostics to %s\n", path);
  print_overview(ctx.state);
  return 0;
}

int handle_diag(ShellContext& ctx) {
  if (ctx.args.size() < 3U || ctx.args[2] != "bundle") {
    shell_write_line(ctx.state, "Usage: mros diag bundle");
    return 1;
  }
  std::vector<std::string> args = {"mros", "export", "diagnostics"};
  ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output};
  return handle_export(child);
}

int handle_alerts(ShellContext& ctx) {
  if (ctx.args.size() >= 3U && is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mros alerts");
    return 0;
  }

  const WifiManagerSnapshot wifi = wifi_snapshot();
  shell_write_line(ctx.state, "severity  source       message");
  shell_write_line(ctx.state, "--------  -----------  ----------------------------------------");
  bool any = false;
  auto alert = [&](const char* severity, const char* source, const char* message) {
    shell_printf(ctx.state, "%-8s  %-11s  %s\n", severity, source, message);
    any = true;
  };

  if (!spi_s3_is_connected()) alert("WARN", "t41-qspi", "Teensy4.1 QSPI link is down");
  if (!spi_c3_is_connected()) alert("WARN", "c3-spi", "ESP32-C3 SPI link is down");
  if (!pca9685_is_ready()) alert("WARN", "i2c", "PCA9685 is not ready");
  if (spi_s3_get_crc_errors() > 0U || spi_s3_get_marker_errors() > 0U) {
    alert("WARN", "t41-qspi", "Teensy4.1 QSPI crc/marker counters are non-zero");
  }
  if (spi_c3_get_crc_errors() > 0U || spi_c3_get_marker_errors() > 0U) {
    alert("WARN", "c3-spi", "C3 SPI crc/marker counters are non-zero");
  }
  if (mros::platform::mros_system_heap_free() < 64U * 1024U) alert("WARN", "heap", "free heap is below 64K");
  if (!wifi.state.sta_connected) alert("INFO", "wifi", "station is not connected");

  if (!any) {
    shell_write_line(ctx.state, "OK        system       no active alerts");
  }
  return any ? 2 : 0;
}

int handle_config(ShellContext& ctx) {
  if (ctx.args.size() < 3U || ctx.args[2] != "diff") {
    shell_write_line(ctx.state, "Usage: mros config diff");
    return 1;
  }
  shell_write_line(ctx.state, "item                  expected          current           status");
  shell_write_line(ctx.state, "--------------------  ----------------  ----------------  ------");
  shell_printf(ctx.state, "%-20s  %-16s  %-16.1f  %s\n",
               "pca9685_freq_hz",
               "50.0",
               pca9685_get_frequency(),
               (pca9685_get_frequency() > 45.0F && pca9685_get_frequency() < 55.0F) ? "OK" : "CHECK");
  shell_printf(ctx.state, "%-20s  %-16s  %-16s  %s\n",
               "pca9685_ready",
               "yes",
               yn(pca9685_is_ready()),
               pca9685_is_ready() ? "OK" : "CHECK");
  shell_printf(ctx.state, "%-20s  %-16s  %-16s  %s\n",
               "t41_qspi_link",
               "connected",
               spi_s3_is_connected() ? "connected" : "down",
               spi_s3_is_connected() ? "OK" : "CHECK");
  shell_printf(ctx.state, "%-20s  %-16s  %-16s  %s\n",
               "c3_spi_link",
               "connected",
               spi_c3_is_connected() ? "connected" : "down",
               spi_c3_is_connected() ? "OK" : "CHECK");
  shell_write_line(ctx.state, "");
  shell_write_line(ctx.state, "Note: persistent expected config profiles are planned; this compares live defaults.");
  return 0;
}

int handle_test_one(ShellState& state, const std::string& target) {
  if (target == "spi") {
    const bool ok = spi_s3_is_connected() && spi_c3_is_connected();
    shell_printf(state, "spi: %s (t41=%s c3=%s)\n",
                 ok ? "PASS" : "FAIL",
                 spi_s3_is_connected() ? "connected" : "down",
                 spi_c3_is_connected() ? "connected" : "down");
    return ok ? 0 : 2;
  }
  if (target == "i2c") {
    const bool ok = pca9685_is_ready();
    shell_printf(state, "i2c: %s (pca9685=%s)\n", ok ? "PASS" : "FAIL", ok ? "ready" : "down");
    return ok ? 0 : 2;
  }
  if (target == "uart") {
    const bool ok = uart1_cobs_get_log_version() > 0U || spi_c3_is_connected();
    shell_printf(state, "uart: %s (t41_log_rev=%lu encoder_via_c3=%s)\n",
                 ok ? "PASS" : "WARN",
                 static_cast<unsigned long>(uart1_cobs_get_log_version()),
                 spi_c3_is_connected() ? "connected" : "down");
    return ok ? 0 : 2;
  }
  shell_printf(state, "mros test: unknown target '%s'\n", target.c_str());
  return 1;
}

int handle_test(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mros test <spi|uart|i2c|all>");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  const std::string& target = ctx.args[2];
  if (target != "all") {
    return handle_test_one(ctx.state, target);
  }
  int result = 0;
  for (const char* one : {"spi", "uart", "i2c"}) {
    result = std::max(result, handle_test_one(ctx.state, one));
  }
  return result;
}

int handle_audit(ShellContext& ctx) {
  const std::string sub = ctx.args.size() >= 3U ? lower_copy(ctx.args[2]) : "list";
  if (sub == "list" || sub == "export") {
    const std::string audit = audit_report();
    if (sub == "export") {
      if (!mros::platform::mros_fs_mkdir("/ESPUSER")) {
        shell_write_line(ctx.state, "mros audit export: /ESPUSER unavailable");
        return 1;
      }
      const char* path = "/ESPUSER/audit.txt";
      if (!mros::platform::mros_file_write_all(path, audit)) {
        shell_write_line(ctx.state, "mros audit export: write failed");
        return 1;
      }
      shell_printf(ctx.state, "mros audit: exported %s\n", path);
      return 0;
    }
    shell_write(ctx.state, audit.c_str());
    return 0;
  }
  if (sub == "clear") {
    if (!ctx.state.root_session && !ctx.state.session_admin) {
      shell_write_line(ctx.state, "mros audit clear: admin/root required");
      return 1;
    }
    audit_clear();
    audit_record("audit-clear", ctx.state.session_username.c_str());
    shell_write_line(ctx.state, "mros audit: cleared");
    return 0;
  }
  shell_write_line(ctx.state, "Usage: mros audit list|export|clear");
  return 1;
}

int handle_doctor(ShellContext& ctx) {
  const std::string target = ctx.args.size() >= 3U ? lower_copy(ctx.args[2]) : "quick";
  const bool json = ctx.json_output || (ctx.args.size() >= 4U && ctx.args[3] == "--json");
  const bool storage = shell_is_storage_mounted(ctx.state);
  const bool spi_t41 = spi_s3_is_connected();
  const bool spi_c3 = spi_c3_is_connected();
  WifiManagerSnapshot wifi = wifi_snapshot();
  const bool wifi_sta = wifi_manager_is_connected();
  const bool wifi_ap = wifi.ap_ip.length() > 0;
  remote::FsMountSnapshot t41 {};
  remote::FsMountSnapshot t41sd {};
  remote::fs_snapshot(remote::FsMount::T41, &t41);
  remote::fs_snapshot(remote::FsMount::T41Sdcard, &t41sd);
  if (json) {
    std::string out = "{\"ok\":true,\"target\":\"";
    out += target;
    out += "\",\"storage\":";
    out += storage ? "true" : "false";
    out += ",\"t41_qspi\":";
    out += spi_t41 ? "true" : "false";
    out += ",\"c3_spi\":";
    out += spi_c3 ? "true" : "false";
    out += ",\"wifi_sta_connected\":";
    out += wifi_sta ? "true" : "false";
    out += ",\"shell_sessions\":";
    out += std::to_string(active_session_count());
    out += ",\"shell_session_capacity\":";
    out += std::to_string(session_capacity());
    out += ",\"jobs_active\":";
    out += std::to_string(runtime::job_active_count());
    out += ",\"job_storage_bytes\":";
    out += std::to_string(runtime::job_storage_bytes());
    out += ",\"job_storage_allocated\":";
    out += runtime::job_storage_allocated() ? "true" : "false";
    out += ",\"job_storage_psram\":";
    out += runtime::job_storage_uses_psram() ? "true" : "false";
    out += ",\"tx_active\":";
    out += runtime::tx_active() ? "true" : "false";
    out += ",\"uart_bridge\":\"";
    out += remote::bridge_mode_name(remote::bridge_mode());
    out += "\",\"t41_fs\":\"";
    out += t41.error_code;
    out += "\",\"t41_sdcard\":\"";
    out += t41sd.error_code;
    out += "\"}";
    shell_write(ctx.state, out.c_str());
    shell_write_line(ctx.state, "");
    return 0;
  }
  shell_write_line(ctx.state, "MROS Doctor");
  shell_write_line(ctx.state, "check                    state      detail");
  shell_write_line(ctx.state, "-----------------------  ---------  ----------------------------------------");
  shell_printf(ctx.state, "%-23s  %-9s  %s\n", "storage", ok_warn(storage), storage ? "/ESPUSER ready" : "LittleFS unavailable");
  shell_printf(ctx.state, "%-23s  %-9s  t41=%s c3=%s\n", "spi peers", ok_warn(spi_t41 || spi_c3), yn(spi_t41), yn(spi_c3));
  shell_printf(ctx.state, "%-23s  %-9s  sta=%s ap=%s ssid=%s\n", "wifi", ok_warn(wifi_sta || wifi_ap), yn(wifi_sta), yn(wifi_ap), wifi.ssid.c_str());
  shell_printf(ctx.state, "%-23s  %-9s  %lu/%lu active root=%lu\n", "shell sessions", ok_warn(active_session_count() < session_capacity()), static_cast<unsigned long>(active_session_count()), static_cast<unsigned long>(session_capacity()), static_cast<unsigned long>(active_root_session_count()));
  const char* storage_state = runtime::job_storage_allocated()
                                  ? (runtime::job_storage_uses_psram() ? "psram" : "internal")
                                  : "lazy";
  shell_printf(ctx.state, "%-23s  %-9s  %lu/%lu active completed=%lu storage=%lu/%s\n", "jobs", ok_warn(runtime::job_active_count() < runtime::job_capacity()), static_cast<unsigned long>(runtime::job_active_count()), static_cast<unsigned long>(runtime::job_capacity()), static_cast<unsigned long>(runtime::job_completed_count()), static_cast<unsigned long>(runtime::job_storage_bytes()), storage_state);
  shell_printf(ctx.state, "%-23s  %-9s  mode=%s\n", "uart bridge", remote::bridge_mode() == remote::BridgeMode::On ? "OK" : "INFO", remote::bridge_mode_name(remote::bridge_mode()));
  shell_printf(ctx.state, "%-23s  %-9s  /t41=%s /t41-sdcard=%s\n", "remote fs", t41.mounted || t41sd.mounted ? "OK" : "INFO", t41.error_code, t41sd.error_code);
  if (target == "all" || target == "wifi") print_wifi_diag_status(ctx.state);
  if (target == "all" || target == "security") {
    const std::string audit = audit_report();
    shell_write_line(ctx.state, "");
    shell_write(ctx.state, audit.c_str());
  }
  return 0;
}

int handle_report(ShellContext& ctx) {
  const std::string sub = ctx.args.size() >= 3U ? lower_copy(ctx.args[2]) : "create";
  if (sub == "create") {
    mros::memory::sample("report-before");
    if (!mros::memory::heavy_diagnostic_allowed()) {
      shell_write_line(ctx.state, "mros report create: internal SRAM critical; run 'mros mem status --detail'");
      return 2;
    }
    if (!mros::platform::mros_fs_mkdir("/ESPUSER") ||
        !mros::platform::mros_fs_mkdir("/ESPUSER/reports")) {
      shell_write_line(ctx.state, "mros report create: /ESPUSER/reports unavailable");
      return 1;
    }
    const uint32_t now = mros::platform::mros_millis();
    char path[96] = {};
    std::snprintf(path, sizeof(path), "/ESPUSER/reports/report-%lu.json", static_cast<unsigned long>(now));
    if (!write_support_report_file(ctx.state, path, now)) {
      shell_write_line(ctx.state, "mros report create: write failed");
      return 1;
    }
    audit_record("report-create", path);
    mros::memory::sample("report-after");
    shell_printf(ctx.state, "mros report: created %s\n", path);
    return 0;
  }
  if (sub == "list") {
    return execute_line_on_state(ctx.state, "ls -l /ESPUSER/reports", false, ctx.transport) ? 0 : 1;
  }
  if (sub == "show") {
    if (ctx.args.size() < 4U) {
      shell_write_line(ctx.state, "mros report show: path required");
      return 1;
    }
    const std::string cmd = "cat " + ctx.args[3];
    return execute_line_on_state(ctx.state, cmd.c_str(), false, ctx.transport) ? 0 : 1;
  }
  if (sub == "delete") {
    if (ctx.args.size() < 4U || ctx.args[3].find("/ESPUSER/reports/") != 0U) {
      shell_write_line(ctx.state, "mros report delete: path under /ESPUSER/reports required");
      return 1;
    }
    const bool ok = mros::platform::mros_fs_remove(ctx.args[3].c_str());
    shell_printf(ctx.state, "mros report delete: %s\n", ok ? "OK" : "failed");
    return ok ? 0 : 1;
  }
  shell_write_line(ctx.state, "Usage: mros report create|list|show|delete");
  return 1;
}

std::string lower_copy(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

std::string basename_from_url(const std::string& url) {
  size_t end = url.find('?');
  if (end == std::string::npos) end = url.size();
  const size_t slash = url.rfind('/', end == 0U ? 0U : end - 1U);
  std::string name = slash == std::string::npos ? url.substr(0U, end) : url.substr(slash + 1U, end - slash - 1U);
  return name.empty() ? "download.bin" : name;
}

std::string resolve_downloader_target(ShellState& state, const std::string& url, const std::string& target_arg) {
  const std::string name = basename_from_url(url);
  const std::string lower_name = lower_copy(name);
  if (target_arg.empty() || target_arg == "auto") {
    const bool update_like = lower_name.find("update") != std::string::npos || lower_name.find(".bin") != std::string::npos;
    const std::string dir = shell_storage_user_root(state) + (update_like ? "/updates" : "/downloads");
    mkdir(dir.c_str(), 0775);
    return dir + "/" + name;
  }
  if (target_arg == "/user" || target_arg == "/user/") {
    return shell_storage_user_root(state) + "/" + name;
  }
  if (target_arg.rfind("/user/", 0U) == 0U) {
    return shell_storage_user_root(state) + target_arg.substr(std::strlen("/user"));
  }
  std::string normalized = shell_normalize_path(state, target_arg);
  bool is_dir = false;
  if (shell_path_exists(state, normalized, &is_dir, nullptr, nullptr) && is_dir) {
    return normalized + "/" + name;
  }
  return normalized;
}

bool ensure_parent_dir(ShellState& state, const std::string& path) {
  const std::string parent = shell_parent_path(path);
  bool is_dir = false;
  if (shell_path_exists(state, parent, &is_dir, nullptr, nullptr) && is_dir) {
    return true;
  }

  const std::string user_root = shell_storage_user_root(state);
  if (parent == user_root || parent.rfind(user_root + "/", 0U) == 0U) {
    mkdir(user_root.c_str(), 0775);
    size_t pos = user_root.size() + 1U;
    while (pos <= parent.size()) {
      const size_t slash = parent.find('/', pos);
      const std::string dir = parent.substr(0U, slash == std::string::npos ? parent.size() : slash);
      if (!dir.empty()) {
        mkdir(dir.c_str(), 0775);
      }
      if (slash == std::string::npos) {
        break;
      }
      pos = slash + 1U;
    }
  } else {
    mkdir(parent.c_str(), 0775);
  }
  return shell_path_exists(state, parent, &is_dir, nullptr, nullptr) && is_dir;
}

std::string to_littlefs_path(ShellState& state, const std::string& normalized_path) {
  const std::string mount = shell_storage_mount_path(state);
  if (normalized_path == mount) {
    return "/";
  }
  if (normalized_path.rfind(mount + "/", 0U) == 0U) {
    return normalized_path.substr(mount.size());
  }
  return normalized_path;
}

int handle_downloader(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mros downloader URL [TARGET]");
    shell_write_line(ctx.state, "Download a file to /ESPUSER. Use TARGET=/user/path or auto.");
    shell_write_line(ctx.state, "If TARGET is omitted and filename looks like update/bin, /ESPUSER/updates is used.");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  if (!wifi_manager_is_connected()) {
    shell_write_line(ctx.state, "mros downloader: WiFi is not connected");
    return 1;
  }
  if (!shell_is_storage_mounted(ctx.state)) {
    shell_write_line(ctx.state, "mros downloader: LittleFS is not mounted");
    return 1;
  }

  const std::string url = ctx.args[2];
  const std::string target = resolve_downloader_target(ctx.state, url, ctx.args.size() >= 4U ? ctx.args[3] : "");
  if (!shell_is_user_writable_path(ctx.state, target)) {
    shell_write_line(ctx.state, "mros downloader: target must be inside /ESPUSER or /user");
    return 1;
  }
  if (!ensure_parent_dir(ctx.state, target)) {
    shell_write_line(ctx.state, "mros downloader: cannot create target directory");
    return 1;
  }

  mros::platform::HttpClientStream stream {};
  mros::platform::HttpClientConfig http_config {};
  http_config.allow_insecure_tls = false;
  http_config.max_redirects = 0;
  http_config.timeout_ms = 15000;
  http_config.buffer_size = 1024U;
  if (!mros::platform::mros_http_client_begin_get(url.c_str(), http_config, &stream)) {
    shell_write_line(ctx.state, "mros downloader: HTTPS URL, valid TLS and public host required");
    return 1;
  }
  const int code = stream.status_code;
  if (code < 200 || code >= 300) {
    shell_printf(ctx.state, "mros downloader: HTTP error %d\n", code);
    mros::platform::mros_http_client_close(&stream);
    return 1;
  }
  const int64_t total = stream.content_length;
  const std::string fs_target = to_littlefs_path(ctx.state, target);
  FILE* file = mros::platform::mros_fs_open(fs_target.c_str(), "wb");
  if (file == nullptr) {
    shell_printf(ctx.state, "mros downloader: cannot open %s\n", target.c_str());
    mros::platform::mros_http_client_close(&stream);
    return 1;
  }

  uint8_t buffer[1024] = {};
  int64_t written_total = 0;
  int last_percent = -1;
  shell_printf(ctx.state, "mros downloader: %s -> %s\n", url.c_str(), target.c_str());
  if (total < 0) {
    shell_write_line(ctx.state, "mros downloader: unknown size/chunked transfer, streaming...");
    while (true) {
      const int read_len = mros::platform::mros_http_client_read(&stream, buffer, sizeof(buffer));
      if (read_len < 0) {
        std::fclose(file);
        mros::platform::mros_http_client_close(&stream);
        shell_write_line(ctx.state, "mros downloader: stream read failed");
        return 1;
      }
      if (read_len == 0) {
        break;
      }
      if (std::fwrite(buffer, 1U, static_cast<size_t>(read_len), file) != static_cast<size_t>(read_len)) {
        std::fclose(file);
        mros::platform::mros_http_client_close(&stream);
        shell_write_line(ctx.state, "mros downloader: write failed");
        return 1;
      }
      written_total += read_len;
    }
    std::fclose(file);
    mros::platform::mros_http_client_close(&stream);
    shell_printf(ctx.state, "mros downloader: completed %lld bytes -> %s\n",
                 static_cast<long long>(written_total),
                 target.c_str());
    return 0;
  }

  while (written_total < total) {
    const int read_len = mros::platform::mros_http_client_read(&stream, buffer, sizeof(buffer));
    if (read_len < 0) {
      std::fclose(file);
      mros::platform::mros_http_client_close(&stream);
      shell_write_line(ctx.state, "mros downloader: stream read failed");
      return 1;
    }
    if (read_len == 0) break;
    if (std::fwrite(buffer, 1U, static_cast<size_t>(read_len), file) != static_cast<size_t>(read_len)) {
      shell_write_line(ctx.state, "mros downloader: write failed");
      std::fclose(file);
      mros::platform::mros_http_client_close(&stream);
      return 1;
    }
    written_total += read_len;
    if (total > 0) {
      const int percent = static_cast<int>((written_total * 100) / total);
      if (percent != last_percent && (percent == 100 || percent - last_percent >= 5)) {
        shell_printf(ctx.state, "mros downloader: %d%% (%lld/%lld bytes)\n",
                     percent,
                     static_cast<long long>(written_total),
                     static_cast<long long>(total));
        last_percent = percent;
      }
    }
  }
  std::fclose(file);
  mros::platform::mros_http_client_close(&stream);
  shell_printf(ctx.state, "mros downloader: completed %lld bytes -> %s\n",
               static_cast<long long>(written_total),
               target.c_str());
  return 0;
}

int dispatch_mros(ShellContext& ctx) {
  if (ctx.args.size() < 2U || is_help_arg(ctx.args[1])) {
    shell_help_mros(ctx.state);
    return ctx.args.size() < 2U ? 1 : 0;
  }
  const std::string top = lower_copy(ctx.args[1]);
  if (top == "overview") {
    print_overview(ctx.state);
    return 0;
  }
  if (top == "health") {
    return print_health(ctx.state);
  }
  if (top == "doctor") return handle_doctor(ctx);
  if (top == "report") return handle_report(ctx);
  if (top == "audit") return handle_audit(ctx);
  if (top == "security") return handle_security(ctx);
  if (top == "mem") return handle_mem(ctx);
  if (top == "perf" && (ctx.args.size() == 2U ||
                        (ctx.args.size() == 3U && lower_copy(ctx.args[2]) == "status"))) {
    print_perf_status(ctx.state);
    return 0;
  }
  if (top == "rtos" && (ctx.args.size() == 2U ||
                        (ctx.args.size() == 3U && lower_copy(ctx.args[2]) == "status"))) {
    print_rtos_status(ctx.state);
    return 0;
  }
  if (top == "rtos" && ctx.args.size() >= 3U && lower_copy(ctx.args[2]) == "policy") {
    std::vector<std::string> args = {"dpm", "policy"};
    for (size_t i = 3U; i < ctx.args.size(); ++i) args.push_back(ctx.args[i]);
    ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
    return shell_cmd_dpm(child);
  }
  if (top == "rtos" && ctx.args.size() >= 4U && lower_copy(ctx.args[2]) == "wake") {
    std::vector<std::string> args = {"dpm", "wake"};
    for (size_t i = 3U; i < ctx.args.size(); ++i) args.push_back(ctx.args[i]);
    ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
    return shell_cmd_dpm(child);
  }
  if (top == "wifi" && ctx.args.size() >= 3U && lower_copy(ctx.args[2]) == "diag") {
    print_wifi_diag_status(ctx.state);
    return 0;
  }
  if (top == "users") return handle_users(ctx);
  if (top == "ssh") return handle_ssh(ctx);
  if (top == "connections") return handle_connections(ctx);
  if (top == "bus") return handle_bus(ctx);
  if (top == "spi") return handle_spi(ctx);
  if (top == "uart") return handle_uart(ctx);
  if (top == "i2c") return handle_i2c(ctx);
  if (top == "pca9685") return handle_pca(ctx);
  if (top == "power" && ctx.args.size() >= 3U && ctx.args[2] == "status") {
    print_power_status(ctx.state);
    return 0;
  }
  if (top == "power" && ctx.args.size() >= 3U && ctx.args[2] == "mode") {
    if (ctx.args.size() == 3U) {
      mros::power::Status power {};
      mros::power::get_status(&power);
      shell_printf(ctx.state, "mode=%s\n", power.mode);
      return 0;
    }
    const std::string mode_text = lower_copy(ctx.args[3]);
    mros::power::Mode mode {};
    if (!mros::power::parse_mode(mode_text.c_str(), &mode)) {
      shell_write_line(ctx.state, "Usage: mros power mode cool|balanced|performance|motion-safe|update-safe");
      return 1;
    }
    (void)mros::power::set_mode(mode, true);
    shell_printf(ctx.state, "power mode=%s\n", mros::power::mode_name(mode));
    return 0;
  }
  if (top == "power" && ctx.args.size() >= 3U && ctx.args[2] == "locks") {
    print_power_locks(ctx.state);
    return 0;
  }
  if (top == "power" && ctx.args.size() >= 3U && ctx.args[2] == "temp") {
    mros::power::Status power {};
    mros::power::get_status(&power);
    if (power.temperature_valid) {
      shell_printf(ctx.state, "temperature_c=%.1f\n", power.temperature_c);
    } else {
      shell_write_line(ctx.state, "temperature_c=unavailable");
    }
    return power.temperature_valid ? 0 : 2;
  }
  if (top == "power" && ctx.args.size() >= 3U && ctx.args[2] == "trace") {
    print_power_trace(ctx.state);
    return 0;
  }
  if (top == "power" && ctx.args.size() >= 3U && ctx.args[2] == "report") {
    std::vector<std::string> args = {"dpm", "report"};
    ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
    return shell_cmd_dpm(child);
  }
  if (top == "sram" && ctx.args.size() >= 3U && ctx.args[2] == "status") {
    mros::memory::sample("mros-sram-status");
    print_sram_status(ctx.state);
    return 0;
  }
  if (top == "sram" && ctx.args.size() >= 3U && ctx.args[2] == "reclaim-plan") {
    print_sram_reclaim_plan(ctx.state);
    return 0;
  }
  if (top == "robot" && ctx.args.size() >= 3U) {
    const std::string robot_sub = lower_copy(ctx.args[2]);
    if (robot_sub == "state") {
      print_robot_state(ctx.state);
      return 0;
    }
    if (robot_sub == "errors") {
      print_robot_errors(ctx.state);
      return 0;
    }
  }
  if (top == "pid") {
    if (ctx.args.size() == 2U || (ctx.args.size() == 3U && lower_copy(ctx.args[2]) == "status")) {
      print_pid_status(ctx.state);
      return 0;
    }
    shell_write_line(ctx.state, "Usage: mros pid status");
    return 1;
  }
  if (top == "fk") {
    if (ctx.args.size() == 2U || (ctx.args.size() == 3U && lower_copy(ctx.args[2]) == "status")) {
      print_fk_status(ctx.state);
      return 0;
    }
    shell_write_line(ctx.state, "Usage: mros fk status");
    return 1;
  }
  if (top == "ik") {
    if (ctx.args.size() == 2U || (ctx.args.size() == 3U && lower_copy(ctx.args[2]) == "status")) {
      print_ik_status(ctx.state);
      return 0;
    }
    shell_write_line(ctx.state, "Usage: mros ik status");
    return 1;
  }
  if (top == "telemetry" && ctx.args.size() >= 3U && ctx.args[2] == "status") {
    print_telemetry_status(ctx.state);
    return 0;
  }
  if (top == "log") return handle_log(ctx);
  if (top == "watch") return run_watch(ctx, 2U);
  if (top == "record") return handle_record(ctx);
  if (top == "export") return handle_export(ctx);
  if (top == "diag") return handle_diag(ctx);
  if (top == "alerts") return handle_alerts(ctx);
  if (top == "config") return handle_config(ctx);
  if (top == "test") return handle_test(ctx);
  if (top == "downloader") return handle_downloader(ctx);
  shell_printf(ctx.state, "mros: unknown command '%s'\n", top.c_str());
  return 1;
}

}  // namespace

void shell_help_mros(ShellState& state) {
  shell_write_line(state, "Usage: mros <command> [args]");
  shell_write_line(state, "Diagnostics and live monitors for the MROS S3 bridge.");
  shell_write_line(state, "");
  shell_write_line(state, "System:");
  shell_write_line(state, "  mros overview");
  shell_write_line(state, "  mros health");
  shell_write_line(state, "  mros doctor quick|all|wifi|fs|robot|security|peer");
  shell_write_line(state, "  mros report create|list|show|delete");
  shell_write_line(state, "  mros audit list|export|clear");
  shell_write_line(state, "  mros security status|logout-all|audit");
  shell_write_line(state, "  mros perf status");
  shell_write_line(state, "  mros mem status|watch|snapshot|diff|leaks");
  shell_write_line(state, "  mros power status|locks|temp|trace");
  shell_write_line(state, "  mros power mode cool|balanced|performance|motion-safe|update-safe");
  shell_write_line(state, "  mros sram status --detail");
  shell_write_line(state, "  mros sram reclaim-plan");
  shell_write_line(state, "  mros rtos status");
  shell_write_line(state, "  mros rtos policy get|set observe|balanced|cool|performance|motion-safe|update-safe");
  shell_write_line(state, "  mros rtos wake <task> [reason]");
  shell_write_line(state, "  mros wifi diag");
  shell_write_line(state, "  mros users list|add|disable|passwd|roles");
  shell_write_line(state, "  mros ssh status|enable|set|list");
  shell_write_line(state, "  mros bus summary|errors");
  shell_write_line(state, "  mros export diagnostics");
  shell_write_line(state, "  mros diag bundle");
  shell_write_line(state, "  mros alerts");
  shell_write_line(state, "  mros config diff");
  shell_write_line(state, "  mros test spi|uart|i2c|all");
  shell_write_line(state, "");
  shell_write_line(state, "Connections:");
  shell_write_line(state, "  mros connections list|status|tree");
  shell_write_line(state, "  mros connections watch -n 5 [--count 12]");
  shell_write_line(state, "");
  shell_write_line(state, "SPI:");
  shell_write_line(state, "  mros spi list");
  shell_write_line(state, "  mros spi status [t41|c3|all]");
  shell_write_line(state, "  mros spi errors --last 25");
  shell_write_line(state, "  mros spi reset-stats");
  shell_write_line(state, "  mros spi monitor [t41|c3|all] -n 5 [--count 12]");
  shell_write_line(state, "");
  shell_write_line(state, "UART/I2C/PCA:");
  shell_write_line(state, "  mros uart list|status|log-mode");
  shell_write_line(state, "  mros uart shell status|reset");
  shell_write_line(state, "  mros uart tail encoder -n 20");
  shell_write_line(state, "  mros uart monitor encoder -n 5 [--count 12]");
  shell_write_line(state, "  mros i2c scan|status");
  shell_write_line(state, "  mros pca9685 status|channels");
  shell_write_line(state, "");
  shell_write_line(state, "Robot and telemetry:");
  shell_write_line(state, "  mros power status");
  shell_write_line(state, "  mros power report");
  shell_write_line(state, "  mros robot state|errors");
  shell_write_line(state, "  mros PID status");
  shell_write_line(state, "  mros FK status");
  shell_write_line(state, "  mros IK status");
  shell_write_line(state, "  mros telemetry status");
  shell_write_line(state, "  mros log tail -n 50");
  shell_write_line(state, "  mros log follow -n 50 --cycles 30 --interval 1");
  shell_write_line(state, "  mros watch -n 5 <mros-subcommand>");
  shell_write_line(state, "  mros record start spi --seconds 30");
  shell_write_line(state, "  mros downloader \"url\" [/user/path-or-auto]");
}

int shell_cmd_mros(ShellContext& ctx) {
  return dispatch_mros(ctx);
}

}  // namespace mros::shell
