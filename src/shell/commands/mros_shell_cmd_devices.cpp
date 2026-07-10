#include "src/shell/mros_shell_internal.h"

#include "src/comm_interfaces/spi/spi_c3_master.h"
#include "src/comm_interfaces/spi/spi_t41_link.h"
#include "src/drivers/i2c_pca9685.h"
#include "src/experimental/experimental_worker.h"
#include "src/web/server/wifi_manager.h"
#include "src/web/web_server.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace mros::shell {
namespace {

constexpr const char* kRawJsonPrefix = "@@RAW_JSON@@";

uint8_t target_mask(const std::string& target) {
  if (target == "t41" || target == "teensy" ||
      target == "teensy41") {
    return mros::experimental::kDeviceTestT41;
  }
  if (target == "wifi" || target == "net") return mros::experimental::kDeviceTestWifi;
  if (target == "pca" || target == "pca9685") return mros::experimental::kDeviceTestPca;
  if (target == "web" || target == "ws") return mros::experimental::kDeviceTestWeb;
  if (target == "all") {
    return static_cast<uint8_t>(mros::experimental::kDeviceTestT41 |
                                mros::experimental::kDeviceTestWifi |
                                mros::experimental::kDeviceTestPca |
                                mros::experimental::kDeviceTestWeb);
  }
  return 0U;
}

std::string passive_status_json() {
  WifiManagerState wifi {};
  wifi_manager_get_state(&wifi);
  std::string json = "{";
  json += "\"t41_connected\":" + std::string(spi_s3_is_connected() ? "true" : "false") + ",";
  json += "\"t41_qspi_txn\":" + std::to_string(spi_s3_get_total_transactions()) + ",";
  json += "\"t41_qspi_crc_errors\":" + std::to_string(spi_s3_get_crc_errors()) + ",";
    json += "\"c3_disabled\":true,";
  json += "\"c3_total_rx\":" + std::to_string(spi_c3_get_total_rx()) + ",";
  json += "\"c3_crc_errors\":" + std::to_string(spi_c3_get_crc_errors()) + ",";
  json += "\"wifi_connected\":" + std::string(wifi.sta_connected ? "true" : "false") + ",";
  json += "\"wifi_ap_active\":" + std::string(wifi.ap_active ? "true" : "false") + ",";
  json += "\"wifi_rssi\":" + std::to_string(wifi.rssi) + ",";
  json += "\"pca_ready\":" + std::string(pca9685_is_ready() ? "true" : "false") + ",";
  json += "\"espnow_active\":" + std::string(spi_c3_is_espnow_active() ? "true" : "false") + ",";
  json += "\"espnow_connected\":" + std::string(spi_c3_is_espnow_connected() ? "true" : "false") + ",";
  json += "\"ws_clients\":" + std::to_string(web_server_total_ws_client_count()) + ",";
  json += "\"worker\":" + std::string(mros::experimental::worker_status_json().c_str());
  json += "}";
  return json;
}

void emit_json(ShellContext& ctx, bool ok, const std::string& result_json) {
  if (!ctx.json_output) return;
  std::string json = kRawJsonPrefix;
  json += "{\"ok\":";
  json += ok ? "true" : "false";
  json += ",\"resource\":\"devices\",\"result\":";
  json += result_json.empty() ? "{}" : result_json;
  json += "}";
  shell_write(ctx.state, json.c_str());
}

}  // namespace

void shell_help_devices(ShellState& state) {
  shell_write_line(state, "Usage:");
  shell_write_line(state, "  devices status                 passive connection snapshot");
  shell_write_line(state, "  devices test t41|uart|s3-uart|wifi|pca|web|all");
  shell_write_line(state, "  devices test status            show experimental worker state");
  shell_write_line(state, "  devices test cancel            request cancel for active diagnostic");
}

int shell_cmd_devices(ShellContext& ctx) {
  if (ctx.args.size() < 2U || ctx.args[1] == "--help" || ctx.args[1] == "-h") {
    shell_help_devices(ctx.state);
    return ctx.args.size() < 2U ? 1 : 0;
  }

  const std::string sub = ctx.args[1];
  if (sub == "status" || sub == "info") {
    const std::string json = passive_status_json();
    if (ctx.json_output) {
      emit_json(ctx, true, json);
      return 0;
    }
    WifiManagerState wifi {};
    wifi_manager_get_state(&wifi);
    shell_printf(ctx.state, "t41=%s tx=%lu crc=%lu\n",
                 spi_s3_is_connected() ? "connected" : "down",
                 static_cast<unsigned long>(spi_s3_get_total_transactions()),
                 static_cast<unsigned long>(spi_s3_get_crc_errors()));
    shell_printf(ctx.state, "c3=disabled legacy_rx=%lu legacy_crc=%lu espnow=%s/%s\n",
                 static_cast<unsigned long>(spi_c3_get_total_rx()),
                 static_cast<unsigned long>(spi_c3_get_crc_errors()),
                 spi_c3_is_espnow_active() ? "active" : "idle",
                 spi_c3_is_espnow_connected() ? "connected" : "down");
    shell_printf(ctx.state, "wifi sta=%s ap=%s rssi=%ld ws=%lu pca=%s\n",
                 wifi.sta_connected ? "connected" : "down",
                 wifi.ap_active ? "on" : "off",
                 static_cast<long>(wifi.rssi),
                 static_cast<unsigned long>(web_server_total_ws_client_count()),
                 pca9685_is_ready() ? "ready" : "down");
    return 0;
  }

  if (sub == "test") {
    const std::string target = ctx.args.size() >= 3U ? ctx.args[2] : "status";
    if (target == "status") {
      const std::string json = mros::experimental::worker_status_json().c_str();
      if (ctx.json_output) {
        emit_json(ctx, true, json);
      } else {
        mros::experimental::WorkerSnapshot snap {};
        mros::experimental::worker_get_snapshot(&snap);
        shell_printf(ctx.state, "experimental_worker=%s %s wake_count=%lu busy=%s\n",
                     snap.enabled ? "enabled" : "disabled",
                     snap.active ? "active" : "sleeping",
                     static_cast<unsigned long>(snap.wake_count),
                     snap.busy ? "yes" : "no");
      }
      return 0;
    }
    if (target == "cancel") {
      mros::experimental::worker_cancel();
      const std::string json = mros::experimental::worker_status_json().c_str();
      if (ctx.json_output) emit_json(ctx, true, json);
      else shell_write_line(ctx.state, "devices test: cancel requested");
      return 0;
    }
    if (target == "c3") {
      if (ctx.json_output) {
        emit_json(ctx, false, "{\"disabled\":true,\"message\":\"c3 topology disabled; use uart/s3-uart\"}");
      } else {
        shell_write_line(ctx.state, "devices test c3: c3 topolojisi devre disi. 'uart' veya 's3-uart' kullanin.");
      }
      return 1;
    }
    if (target == "uart" || target == "s3-uart") {
      std::vector<std::string> uart_args = {"uart", "test", "--duration", "10"};
      if (ctx.json_output) uart_args.push_back("--json");
      ShellContext uart_ctx {ctx.state, uart_args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
      return shell_cmd_uart(uart_ctx);
    }

    const uint8_t mask = target_mask(target);
    if (mask == 0U) {
      shell_write_line(ctx.state, "devices test: target must be t41, uart, s3-uart, wifi, pca, web or all");
      return 1;
    }
    mros::experimental::WorkerRequest request {};
    request.type = mros::experimental::WorkerJobType::DeviceTest;
    request.device_mask = mask;
    std::snprintf(request.source, sizeof(request.source), "shell");
    mros::experimental::WorkerResult result {};
    const bool ok = mros::experimental::worker_submit_sync(request, 5000U, &result);
    const std::string json = mros::experimental::worker_result_json(result).c_str();
    if (ctx.json_output) {
      emit_json(ctx, ok, json);
    } else {
      shell_printf(ctx.state, "devices test %s: %s (%lu us)\n",
                   target.c_str(),
                   result.message[0] != '\0' ? result.message : (ok ? "ok" : "failed"),
                   static_cast<unsigned long>(result.duration_us));
      shell_printf(ctx.state, "t41=%s c3=disabled wifi=%s ap=%s pca=%s ws=%lu\n",
                   result.t41_connected ? "ok" : "down",
                   result.wifi_connected ? "ok" : "down",
                   result.wifi_ap_active ? "on" : "off",
                   result.pca_ready ? "ready" : "down",
                   static_cast<unsigned long>(result.ws_clients));
    }
    return ok ? 0 : 1;
  }

  shell_write_line(ctx.state, "Usage: devices status | devices test t41|uart|s3-uart|wifi|pca|web|all|status|cancel");
  return 1;
}

}  // namespace mros::shell
