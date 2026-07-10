#include "src/shell/mros_shell_internal.h"

#include "src/comm_interfaces/spi/spi_c3_master.h"
#include "src/platform/mros_time.h"
#include "src/web/server/wifi_manager.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "WString.h"

namespace mros::shell {
namespace {

String to_string_arg(const std::string& value) {
  return String(value.c_str());
}

void print_state(ShellState& state) {
  WifiManagerSnapshot snap = {};
  wifi_manager_get_snapshot(&snap);
  shell_printf(state, "phase=%s enabled=%s\n", snap.phase.c_str(),
               wifi_manager_is_enabled() ? "yes" : "no");
  shell_printf(state, "sta=%s connecting=%s ssid=%s ip=%s rssi=%ld ch=%u\n",
               snap.state.sta_connected ? "connected" : "down",
               snap.state.sta_connecting ? "yes" : "no",
               snap.ssid.length() ? snap.ssid.c_str() : "-",
               snap.ip.length() ? snap.ip.c_str() : "-",
               static_cast<long>(snap.state.rssi),
               static_cast<unsigned>(snap.current_channel));
  shell_printf(state, "ap=%s ap_ip=%s clients=%u grace_ms=%lu\n",
               snap.state.ap_active ? "on" : "off",
               snap.ap_ip.length() ? snap.ap_ip.c_str() : "-",
               static_cast<unsigned>(snap.state.ap_station_count),
               static_cast<unsigned long>(snap.ap_grace_remaining_ms));
  shell_printf(state, "scan=%s scan_age_ms=%lu backoff_ms=%lu reason=%lu attempts=%lu\n",
               snap.state.scan_in_progress ? "running" : "idle",
               static_cast<unsigned long>(snap.scan_age_ms),
               static_cast<unsigned long>(snap.reconnect_backoff_ms),
               static_cast<unsigned long>(snap.state.last_disconnect_reason),
               static_cast<unsigned long>(snap.state.reconnect_attempts));
  shell_printf(state, "connect_ms=%lu fast_path=%lu/%lu\n",
               static_cast<unsigned long>(snap.last_connect_duration_ms),
               static_cast<unsigned long>(snap.fast_path_successes),
               static_cast<unsigned long>(snap.fast_path_attempts));
  shell_printf(state, "last_good_ssid=%s last_good_bssid=%s last_good_ch=%u\n",
               snap.last_good_ssid.length() ? snap.last_good_ssid.c_str() : "-",
               snap.last_good_bssid.length() ? snap.last_good_bssid.c_str() : "-",
               static_cast<unsigned>(snap.last_good_channel));
  shell_printf(state, "espnow failsafe=%d active=%s connected=%s recent=%s\n",
               static_cast<int>(spi_c3_get_failsafe_option()),
               spi_c3_is_espnow_active() ? "yes" : "no",
               spi_c3_is_espnow_connected() ? "yes" : "no",
               spi_c3_has_recent_espnow_cmd() ? "yes" : "no");
}

int list_now(ShellState& state) {
  wifi_manager_request_scan();
  const unsigned long start_ms = mros::platform::mros_millis();
  bool saw_scan = false;
  while ((mros::platform::mros_millis() - start_ms) < 12000UL) {
    if (wifi_manager_is_scan_in_progress()) {
      saw_scan = true;
    } else if (saw_scan) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  String text;
  wifi_manager_get_scan_results_text(&text);
  shell_write(state, text.c_str());
  if (!saw_scan) {
    shell_write_line(state, "wifi: scan was queued or skipped; showing cached results.");
  }
  return 0;
}

}  // namespace

void shell_help_wifi(ShellState& state) {
  shell_write_line(state, "Usage:");
  shell_write_line(state, "  wifi connect \"SSID\" \"PASSWORD\"   save credentials and connect now");
  shell_write_line(state, "  wifi save \"SSID\" \"PASSWORD\"      save credentials without reconnect");
  shell_write_line(state, "  wifi state on                    enable STA manager and reconnect");
  shell_write_line(state, "  wifi state off                   turn WiFi off");
  shell_write_line(state, "  wifi state hotspot               force AP/captive portal mode");
  shell_write_line(state, "  wifi state espnow                set C3 failsafe to C3SPI+t41 ESP-NOW");
  shell_write_line(state, "  wifi reconnect                   restart STA auto-connect flow");
  shell_write_line(state, "  wifi scan                        alias for 'wifi list now'");
  shell_write_line(state, "  wifi info                        show WiFi diagnostics");
  shell_write_line(state, "  wifi list now                    scan/list visible networks");
  shell_write_line(state, "  wifi list saved                  list stored and firmware-known networks");
}

int shell_cmd_wifi(ShellContext& ctx) {
  if (ctx.args.size() < 2U || ctx.args[1] == "--help" || ctx.args[1] == "-h") {
    shell_help_wifi(ctx.state);
    return ctx.args.size() < 2U ? 1 : 0;
  }

  const std::string& sub = ctx.args[1];
  if (sub == "connect" || sub == "save") {
    if (ctx.args.size() < 4U) {
      shell_printf(ctx.state, "wifi: %s requires SSID and PASSWORD\n", sub.c_str());
      return 1;
    }
    const String ssid = to_string_arg(ctx.args[2]);
    const String pass = to_string_arg(ctx.args[3]);
    const bool connect_now = (sub == "connect");
    if (!wifi_manager_save_credentials(ssid, pass, connect_now)) {
      shell_write_line(ctx.state, "wifi: failed to save credentials");
      return 1;
    }
    shell_printf(ctx.state, "wifi: credentials saved for '%s'%s\n", ssid.c_str(),
                 connect_now ? ", reconnecting" : "");
    return 0;
  }

  if (sub == "state") {
    if (ctx.args.size() < 3U) {
      print_state(ctx.state);
      return 0;
    }
    const std::string& mode = ctx.args[2];
    if (mode == "on") {
      wifi_manager_set_enabled(true);
      wifi_manager_request_reconnect();
      shell_write_line(ctx.state, "wifi: STA manager enabled, reconnect requested");
      return 0;
    }
    if (mode == "off") {
      wifi_manager_set_enabled(false);
      shell_write_line(ctx.state, "wifi: off");
      return 0;
    }
    if (mode == "hotspot") {
      wifi_manager_force_hotspot();
      shell_write_line(ctx.state, "wifi: hotspot mode requested");
      return 0;
    }
    if (mode == "espnow") {
      spi_c3_set_failsafe_option(C3_FAILSAFE_C3SPI_T41_ESPNOW);
      shell_write_line(ctx.state, "wifi: C3 failsafe set to C3SPI+t41 ESP-NOW");
      return 0;
    }
    shell_printf(ctx.state, "wifi: unknown state '%s'\n", mode.c_str());
    return 1;
  }

  if (sub == "info") {
    print_state(ctx.state);
    return 0;
  }

  if (sub == "reconnect") {
    wifi_manager_request_reconnect();
    shell_write_line(ctx.state, "wifi: reconnect requested");
    return 0;
  }

  if (sub == "scan") {
    return list_now(ctx.state);
  }

  if (sub == "list") {
    if (ctx.args.size() < 3U) {
      shell_write_line(ctx.state, "wifi: list requires 'now' or 'saved'");
      return 1;
    }
    if (ctx.args[2] == "now") {
      return list_now(ctx.state);
    }
    if (ctx.args[2] == "saved") {
      String text;
      wifi_manager_get_saved_networks_text(&text);
      shell_write(ctx.state, text.c_str());
      return 0;
    }
    shell_printf(ctx.state, "wifi: unknown list target '%s'\n", ctx.args[2].c_str());
    return 1;
  }

  shell_printf(ctx.state, "wifi: unknown subcommand '%s'\n", sub.c_str());
  return 1;
}

}  // namespace mros::shell
