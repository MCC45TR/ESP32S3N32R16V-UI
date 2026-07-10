#include "src/shell/mros_shell_internal.h"

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "src/communication/protocol_def.h"
#include "src/comm_interfaces/spi/spi_c3_master.h"
#include "src/comm_interfaces/spi/spi_t41_link.h"
#include "src/comm_interfaces/uart/uart_cobs.h"
#include "src/config/hw_config.h"
#include "src/drivers/i2c_pca9685.h"
#include "src/platform/mros_gpio.h"
#include "src/platform/mros_i2c.h"
#include "src/platform/mros_time.h"
#include "src/platform/mros_uart.h"
#include "src/security/uart_secure.h"
#include "src/shell/mshell_remote.h"

namespace mros::shell {
namespace {

struct NamedPin {
  const char* name;
  int pin;
  bool immutable;
  const char* owner;
};

const NamedPin kPins[] = {
    {"onboard_rgb", PIN_ONBOARD_RGB_LED, false, "status-led"},
    {"spi_sck", PIN_SPI_SCK, true, "teensy41-qspi"},
    {"spi_miso", PIN_SPI_MISO, true, "teensy41-qspi"},
    {"spi_mosi", PIN_SPI_MOSI, true, "teensy41-qspi"},
    {"spi_cs", PIN_SPI_CS, true, "teensy41-qspi"},
    {"spi_wp", PIN_SPI_WP, true, "teensy41-qspi"},
    {"spi_hd", PIN_SPI_HD, true, "teensy41-qspi"},
    {"data_ready", PIN_DATA_READY, true, "teensy41-esp-ready"},
    {"t41_ready", PIN_T41_READY, true, "teensy41-ready-in"},
    {"teensy_irq", PIN_TEENSY_IRQ, true, "teensy41-irq"},
    {"teensy_reset", PIN_TEENSY_RESET, true, "teensy41-reset"},
    {"alive_led", PIN_ALIVE_LED, true, "teensy41-ready-in"},
    {"uart1_cts", PIN_UART1_CTS, true, "teensy41-uart-flow"},
    {"uart1_rts", PIN_UART1_RTS, true, "teensy41-uart-flow"},
    {"uart1_tx", PIN_UART1_TX, true, "teensy41-uart"},
    {"uart1_rx", PIN_UART1_RX, true, "teensy41-uart"},
    {"c3_spi_sck", PIN_C3_SPI_SCK, true, "c3-spi"},
    {"c3_spi_mosi", PIN_C3_SPI_MOSI, true, "c3-spi"},
    {"c3_spi_miso", PIN_C3_SPI_MISO, true, "c3-spi"},
    {"c3_spi_cs", PIN_C3_SPI_CS, true, "c3-spi"},
    {"c3_alive", PIN_C3_ALIVE, true, "c3-alive"},
    {"i2c_sda", PIN_I2C_SDA, true, "pca-i2c"},
    {"i2c_scl", PIN_I2C_SCL, true, "pca-i2c"},
    {"pca_oe", PIN_PCA_OE, true, "pca-oe"},
};

bool parse_int(const std::string& text, int* out) {
  if (out == nullptr || text.empty()) return false;
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 0);
  if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
  *out = static_cast<int>(value);
  return true;
}

bool parse_u8(const std::string& text, uint8_t* out) {
  int value = 0;
  if (!parse_int(text, &value) || value < 0 || value > 255) return false;
  *out = static_cast<uint8_t>(value);
  return true;
}

bool parse_u16(const std::string& text, uint16_t* out) {
  int value = 0;
  if (!parse_int(text, &value) || value < 0 || value > 65535) return false;
  *out = static_cast<uint16_t>(value);
  return true;
}

bool parse_float_arg(const std::string& text, float* out) {
  if (out == nullptr || text.empty()) return false;
  char* end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
  *out = value;
  return true;
}

const NamedPin* named_pin_by_gpio(int pin) {
  for (const NamedPin& p : kPins) {
    if (p.pin == pin) return &p;
  }
  return nullptr;
}

bool gpio_valid(int pin) {
  return pin >= 0 && pin <= 48;
}

bool gpio_can_write(ShellState& state, int pin) {
  if (!gpio_valid(pin)) {
    shell_write_line(state, "gpio: invalid GPIO number");
    return false;
  }
  const NamedPin* known = named_pin_by_gpio(pin);
  if (known != nullptr && known->immutable) {
    shell_printf(state, "gpio: GPIO%d is owned by %s and is immutable\n", pin, known->owner);
    return false;
  }
  return true;
}

std::vector<std::string> log_args_from_journal(const std::vector<std::string>& args) {
  std::vector<std::string> out = {"log"};
  for (size_t i = 1U; i < args.size(); ++i) {
    if (args[i] == "--help") {
      out.push_back("--help");
      continue;
    }
    if (args[i] == "-n" && (i + 1U) < args.size()) {
      out.push_back("-n");
      out.push_back(args[++i]);
      continue;
    }
    if (args[i] == "-f" || args[i] == "--follow") {
      out.push_back("--follow");
      continue;
    }
    if ((args[i] == "-g" || args[i] == "--grep") && (i + 1U) < args.size()) {
      out.push_back("--filter");
      out.push_back(args[++i]);
      continue;
    }
    if ((args[i] == "-u" || args[i] == "--unit") && (i + 1U) < args.size()) {
      out.push_back("--source");
      out.push_back(args[++i]);
      continue;
    }
  }
  return out;
}

int run_line(ShellContext& ctx, const std::string& line) {
  return execute_line_on_state(ctx.state, line.c_str(), false, ctx.transport) ? 0 : 1;
}

void print_spi_status(ShellState& state) {
  C3SpiDiagSnapshot c3 {};
  spi_c3_get_diag_snapshot(&c3);
  shell_printf(state, "t41 connected=%s total=%lu crc=%lu marker=%lu loop_ms=%u last_marker=0x%02x seq=%u\n",
               spi_s3_is_connected() ? "yes" : "no",
               static_cast<unsigned long>(spi_s3_get_total_transactions()),
               static_cast<unsigned long>(spi_s3_get_crc_errors()),
               static_cast<unsigned long>(spi_s3_get_marker_errors()),
               static_cast<unsigned>(spi_s3_get_loop_ms()),
               static_cast<unsigned>(spi_s3_get_last_rx_marker()),
               static_cast<unsigned>(spi_s3_get_last_rx_seq()));
  shell_printf(state, "t41 endpoint=%s clock_prep_safe=%s\n",
               spi_s3_endpoint_mode_name(),
               spi_s3_is_clock_prep_safe() ? "yes" : "no");
  shell_printf(state, "c3 disabled=true legacy_connected=%s alive=%s synced=%s rx=%lu crc=%lu marker=%lu period_ms=%lu last_good_ms=%lu\n",
               c3.connected ? "yes" : "no",
               c3.alive_ok ? "yes" : "no",
               c3.link_synced ? "yes" : "no",
               static_cast<unsigned long>(c3.total_rx_count),
               static_cast<unsigned long>(c3.crc_error_count),
               static_cast<unsigned long>(c3.marker_error_count),
               static_cast<unsigned long>(c3.effective_period_ms),
               static_cast<unsigned long>(c3.last_good_rx_ms));
}

struct LinkDiagReport {
  uint32_t duration_s = 10U;
  uint32_t sent = 0U;
  uint32_t received = 0U;
  float loss_pct = 0.0f;
  uint32_t rtt_min_ms = 0U;
  uint32_t rtt_avg_ms = 0U;
  uint32_t rtt_max_ms = 0U;
  uint32_t throughput_bps = 0U;
  std::string frame_label = "Link Frame";
  float health_pct = 0.0f;
  bool comm_ok = false;
  bool response_ok = false;
  bool uart_diag_response_ok = false;
  bool link_frame_response_ok = false;
};

uint32_t u32_delta(const uint32_t now_value, const uint32_t old_value) {
  return now_value >= old_value ? (now_value - old_value) : 0U;
}

LinkDiagReport run_spi_link_test(const uint32_t duration_s) {
  LinkDiagReport report {};
  report.duration_s = duration_s;
  report.frame_label = "QSPI Frame";
  const uint32_t duration_ms = duration_s * 1000U;
  const uint32_t tx0 = spi_s3_get_total_transactions();
  const uint32_t crc0 = spi_s3_get_crc_errors();
  const uint32_t marker0 = spi_s3_get_marker_errors();
  const uint32_t ack0 = spi_s3_get_ack_frames();
  const uint32_t nack0 = spi_s3_get_nack_frames();
  const uint32_t retry0 = spi_s3_get_retry_frames();
  const uint32_t ack_to0 = spi_s3_get_ack_timeouts();

  mros::shell::remote::reset_diag_metrics();
  uint32_t ping_id = 1U;
  const uint32_t start_ms = mros::platform::mros_millis();
  uint32_t next_ping_ms = start_ms;
  while ((mros::platform::mros_millis() - start_ms) < duration_ms) {
    const uint32_t now_ms = mros::platform::mros_millis();
    if (now_ms >= next_ping_ms) {
      (void)mros::shell::remote::send_diag_ping(ping_id++, now_ms);
      next_ping_ms = now_ms + 250U;
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  vTaskDelay(pdMS_TO_TICKS(200));

  mros::shell::remote::RemoteDiagSnapshot diag {};
  mros::shell::remote::get_diag_snapshot(&diag);
  const uint32_t tx1 = spi_s3_get_total_transactions();
  const uint32_t crc1 = spi_s3_get_crc_errors();
  const uint32_t marker1 = spi_s3_get_marker_errors();
  const uint32_t ack1 = spi_s3_get_ack_frames();
  const uint32_t nack1 = spi_s3_get_nack_frames();
  const uint32_t retry1 = spi_s3_get_retry_frames();
  const uint32_t ack_to1 = spi_s3_get_ack_timeouts();

  report.sent = diag.ping_sent;
  report.received = diag.ping_recv;
  report.loss_pct = report.sent == 0U ? 100.0f :
      (100.0f * static_cast<float>(report.sent - report.received) / static_cast<float>(report.sent));
  report.rtt_min_ms = diag.rtt_min_ms;
  report.rtt_avg_ms = diag.rtt_avg_ms;
  report.rtt_max_ms = diag.rtt_max_ms;
  const uint32_t frame_delta = u32_delta(tx1, tx0);
  const uint32_t ack_delta = u32_delta(ack1, ack0);
  const uint32_t nack_delta = u32_delta(nack1, nack0);
  report.throughput_bps = duration_s == 0U ? 0U : static_cast<uint32_t>(
      (static_cast<uint64_t>(frame_delta) *
       static_cast<uint64_t>(T41_QSPI_MAX_FRAME_BYTES) * 8ULL) / duration_s);
  report.comm_ok = spi_s3_is_connected();
  report.uart_diag_response_ok = report.received > 0U;
  report.link_frame_response_ok = frame_delta > 0U || ack_delta > 0U || nack_delta > 0U;
  report.response_ok = report.uart_diag_response_ok && report.link_frame_response_ok;
  const uint32_t critical =
      u32_delta(crc1, crc0) + u32_delta(marker1, marker0) + u32_delta(ack_to1, ack_to0);
  const uint32_t minor =
      u32_delta(nack1, nack0) + u32_delta(retry1, retry0) + u32_delta(ack1, ack0) * 0U;
  float health = 100.0f;
  health -= report.loss_pct * 0.55f;
  health -= static_cast<float>(critical) * 4.0f;
  health -= static_cast<float>(minor) * 0.25f;
  if (!report.comm_ok) health -= 20.0f;
  if (!report.uart_diag_response_ok) health -= 6.0f;
  if (!report.link_frame_response_ok) health -= 12.0f;
  if (health < 0.0f) health = 0.0f;
  if (health > 100.0f) health = 100.0f;
  report.health_pct = health;
  return report;
}

LinkDiagReport run_uart_link_test(const uint32_t duration_s) {
  LinkDiagReport report {};
  report.duration_s = duration_s;
  report.frame_label = "UART Frame";
  const uint32_t duration_ms = duration_s * 1000U;
  mros::shell::remote::RemoteTunnelMetrics tunnel0 {};
  mros::shell::remote::get_tunnel_metrics(&tunnel0);
  mros::shell::remote::reset_diag_metrics();
  uint32_t ping_id = 1U;
  const uint32_t start_ms = mros::platform::mros_millis();
  uint32_t next_ping_ms = start_ms;
  while ((mros::platform::mros_millis() - start_ms) < duration_ms) {
    const uint32_t now_ms = mros::platform::mros_millis();
    if (now_ms >= next_ping_ms) {
      (void)mros::shell::remote::send_diag_ping(ping_id++, now_ms);
      next_ping_ms = now_ms + 250U;
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  vTaskDelay(pdMS_TO_TICKS(200));

  mros::shell::remote::RemoteDiagSnapshot diag {};
  mros::shell::remote::get_diag_snapshot(&diag);
  mros::shell::remote::RemoteTunnelMetrics tunnel1 {};
  mros::shell::remote::get_tunnel_metrics(&tunnel1);
  report.sent = diag.ping_sent;
  report.received = diag.ping_recv;
  report.loss_pct = report.sent == 0U ? 100.0f :
      (100.0f * static_cast<float>(report.sent - report.received) / static_cast<float>(report.sent));
  report.rtt_min_ms = diag.rtt_min_ms;
  report.rtt_avg_ms = diag.rtt_avg_ms;
  report.rtt_max_ms = diag.rtt_max_ms;
  const uint32_t text_bytes = u32_delta(tunnel1.remote_text_bytes, tunnel0.remote_text_bytes);
  const uint32_t bin_bytes = u32_delta(tunnel1.remote_bin_bytes, tunnel0.remote_bin_bytes);
  report.throughput_bps = duration_s == 0U ? 0U : static_cast<uint32_t>(
      (static_cast<uint64_t>(text_bytes + bin_bytes) * 8ULL) / duration_s);
  report.comm_ok = mros::platform::mros_uart_is_ready(UART_NUM_1);
  report.uart_diag_response_ok = report.received > 0U;
  report.link_frame_response_ok = u32_delta(tunnel1.remote_text_frames, tunnel0.remote_text_frames) > 0U ||
                                  u32_delta(tunnel1.remote_bin_frames, tunnel0.remote_bin_frames) > 0U;
  report.response_ok = report.uart_diag_response_ok && report.link_frame_response_ok;
  float health = 100.0f;
  health -= report.loss_pct * 0.60f;
  health -= static_cast<float>(diag.ping_timeout) * 0.75f;
  if (!report.comm_ok) health -= 20.0f;
  if (!report.uart_diag_response_ok) health -= 6.0f;
  if (!report.link_frame_response_ok) health -= 12.0f;
  if (health < 0.0f) health = 0.0f;
  if (health > 100.0f) health = 100.0f;
  report.health_pct = health;
  return report;
}

void print_link_diag(ShellState& state, const LinkDiagReport& report) {
  shell_printf(state, "Cihaz ile iletisim: %s\n", report.comm_ok ? "OK" : "FAIL");
  shell_printf(state, "Yanit alindi (UART DIAG): %s\n",
               report.uart_diag_response_ok ? "OK" : "FAIL");
  shell_printf(state, "Yanit alindi (%s): %s\n",
               report.frame_label.c_str(),
               report.link_frame_response_ok ? "OK" : "FAIL");
  shell_printf(state, "Saglik: %%%.1f\n", report.health_pct);
  shell_printf(state, "Veri hizi: %.3f Mbps\n", static_cast<double>(report.throughput_bps) / 1000000.0);
}

std::string link_diag_json(const LinkDiagReport& report) {
  char out[768] = {};
  std::snprintf(out,
                sizeof(out),
                "{\"duration_s\":%lu,\"sent\":%lu,\"received\":%lu,\"loss_pct\":%.3f,"
                "\"rtt_ms_min\":%lu,\"rtt_ms_avg\":%lu,\"rtt_ms_max\":%lu,"
                "\"throughput_bps\":%lu,\"health_pct\":%.3f,\"frame_label\":\"%s\","
                "\"comm_ok\":%s,\"response_ok\":%s,\"uart_diag_response_ok\":%s,"
                "\"link_frame_response_ok\":%s}",
                static_cast<unsigned long>(report.duration_s),
                static_cast<unsigned long>(report.sent),
                static_cast<unsigned long>(report.received),
                static_cast<double>(report.loss_pct),
                static_cast<unsigned long>(report.rtt_min_ms),
                static_cast<unsigned long>(report.rtt_avg_ms),
                static_cast<unsigned long>(report.rtt_max_ms),
                static_cast<unsigned long>(report.throughput_bps),
                static_cast<double>(report.health_pct),
                report.frame_label.c_str(),
                report.comm_ok ? "true" : "false",
                report.response_ok ? "true" : "false",
                report.uart_diag_response_ok ? "true" : "false",
                report.link_frame_response_ok ? "true" : "false");
  return out;
}

bool adc_channel_for_pin(int pin, adc_channel_t* channel) {
  if (channel == nullptr || pin < 1 || pin > 10) return false;
  *channel = static_cast<adc_channel_t>(pin - 1);
  return true;
}

}  // namespace

void shell_help_hostname(ShellState& state) {
  shell_write_line(state, "Usage: hostname [NAME]");
}

int shell_cmd_hostname(ShellContext& ctx) {
  if (ctx.args.size() > 1U && ctx.args[1] == "--help") {
    shell_help_hostname(ctx.state);
    return 0;
  }
  if (ctx.args.size() == 1U) {
    shell_write_line(ctx.state, ctx.state.config.hostname != nullptr ? ctx.state.config.hostname : "mros");
    return 0;
  }
  if (!ctx.state.root_session) {
    shell_write_line(ctx.state, "hostname: changing hostname requires root");
    return 1;
  }
  ctx.state.env_vars["HOSTNAME"] = ctx.args[1];
  shell_printf(ctx.state, "hostname staged as %s for this shell session\n", ctx.args[1].c_str());
  return 0;
}

void shell_help_who(ShellState& state) {
  shell_write_line(state, "Usage: who");
}

int shell_cmd_who(ShellContext& ctx) {
  if (ctx.args.size() > 1U && ctx.args[1] == "--help") {
    shell_help_who(ctx.state);
    return 0;
  }
  shell_printf(ctx.state, "%-12s %-8s root=%s admin=%s\n",
               ctx.state.session_username.empty() ? "user" : ctx.state.session_username.c_str(),
               ctx.state.active_transport == ShellTransport::Web ? "web" :
                   (ctx.state.active_transport == ShellTransport::Ssh ? "ssh" : "serial"),
               ctx.state.root_session ? "yes" : "no",
               ctx.state.session_admin ? "yes" : "no");
  return 0;
}

void shell_help_w(ShellState& state) {
  shell_write_line(state, "Usage: w");
}

int shell_cmd_w(ShellContext& ctx) {
  if (ctx.args.size() > 1U && ctx.args[1] == "--help") {
    shell_help_w(ctx.state);
    return 0;
  }
  shell_printf(ctx.state, "sessions=%lu roots=%lu capacity=%lu user=%s tty=%s\n",
               static_cast<unsigned long>(active_session_count()),
               static_cast<unsigned long>(active_root_session_count()),
               static_cast<unsigned long>(session_capacity()),
               ctx.state.session_username.empty() ? "user" : ctx.state.session_username.c_str(),
               ctx.state.active_transport == ShellTransport::Web ? "web" :
                   (ctx.state.active_transport == ShellTransport::Ssh ? "ssh" : "serial"));
  return 0;
}

void shell_help_last(ShellState& state) {
  shell_write_line(state, "Usage: last");
}

int shell_cmd_last(ShellContext& ctx) {
  if (ctx.args.size() > 1U && ctx.args[1] == "--help") {
    shell_help_last(ctx.state);
    return 0;
  }
  const std::string report = audit_report();
  shell_write(ctx.state, report.empty() ? "no audit events\n" : report.c_str());
  return 0;
}

void shell_help_dmesg(ShellState& state) {
  shell_write_line(state, "Usage: dmesg [-n NUM] [-g TEXT]");
}

int shell_cmd_dmesg(ShellContext& ctx) {
  std::vector<std::string> args = {"log"};
  for (size_t i = 1U; i < ctx.args.size(); ++i) args.push_back(ctx.args[i]);
  ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
  return shell_cmd_log(child);
}

void shell_help_logger(ShellState& state) {
  shell_write_line(state, "Usage: logger MESSAGE...");
}

int shell_cmd_logger(ShellContext& ctx) {
  if (ctx.args.size() <= 1U || ctx.args[1] == "--help") {
    shell_help_logger(ctx.state);
    return ctx.args.size() <= 1U ? 1 : 0;
  }
  std::string msg;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    if (!msg.empty()) msg.push_back(' ');
    msg += ctx.args[i];
  }
  audit_record("logger", msg.c_str());
  appendSystemLog("shell", String(msg.c_str()));
  return 0;
}

void shell_help_journalctl(ShellState& state) {
  shell_write_line(state, "Usage: journalctl [-n NUM] [-f] [-g TEXT] [-u UNIT]");
}

int shell_cmd_journalctl(ShellContext& ctx) {
  if (ctx.args.size() > 1U && ctx.args[1] == "--help") {
    shell_help_journalctl(ctx.state);
    return 0;
  }
  std::vector<std::string> args = log_args_from_journal(ctx.args);
  ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
  return shell_cmd_log(child);
}

void shell_help_systemctl(ShellState& state) {
  shell_write_line(state, "Usage: systemctl list-units|status [UNIT]|start UNIT|stop UNIT|restart UNIT");
}

int shell_cmd_systemctl(ShellContext& ctx) {
  if (ctx.args.size() <= 1U || ctx.args[1] == "--help") {
    shell_help_systemctl(ctx.state);
    return ctx.args.size() <= 1U ? 1 : 0;
  }
  const std::string sub = ctx.args[1];
  if (sub == "list-units") return run_line(ctx, "dpm tasks");
  if (sub == "status") {
    if (ctx.args.size() >= 3U) return run_line(ctx, std::string("dpm wake ") + ctx.args[2] + " status-probe");
    return run_line(ctx, "dpm status");
  }
  if (sub == "start" || sub == "restart") {
    if (ctx.args.size() < 3U) return 1;
    return run_line(ctx, std::string("dpm wake ") + ctx.args[2] + " systemctl");
  }
  if (sub == "stop") {
    shell_write_line(ctx.state, "systemctl: stop is staged only; DPM hard stop policy is not enabled in this firmware");
    return 0;
  }
  shell_help_systemctl(ctx.state);
  return 1;
}

void shell_help_service(ShellState& state) {
  shell_write_line(state, "Usage: service NAME status|start|stop|restart");
}

int shell_cmd_service(ShellContext& ctx) {
  if (ctx.args.size() < 3U || ctx.args[1] == "--help") {
    shell_help_service(ctx.state);
    return ctx.args.size() >= 2U && ctx.args[1] == "--help" ? 0 : 1;
  }
  std::vector<std::string> args = {"systemctl", ctx.args[2], ctx.args[1]};
  ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
  return shell_cmd_systemctl(child);
}

void shell_help_gpioinfo(ShellState& state) {
  shell_write_line(state, "Usage: gpioinfo");
}

int shell_cmd_gpioinfo(ShellContext& ctx) {
  shell_write_line(ctx.state, "gpio   name              owner       immutable  level");
  for (const NamedPin& p : kPins) {
    shell_printf(ctx.state, "%-6d %-17s %-11s %-9s %d\n",
                 p.pin, p.name, p.owner, p.immutable ? "yes" : "no", mros::platform::mros_gpio_read(p.pin));
  }
  return 0;
}

void shell_help_gpioget(ShellState& state) {
  shell_write_line(state, "Usage: gpioget GPIO");
}

int shell_cmd_gpioget(ShellContext& ctx) {
  if (ctx.args.size() != 2U || ctx.args[1] == "--help") {
    shell_help_gpioget(ctx.state);
    return ctx.args.size() == 2U ? 0 : 1;
  }
  int pin = -1;
  if (!parse_int(ctx.args[1], &pin) || !gpio_valid(pin)) return 1;
  (void)mros::platform::mros_gpio_config(pin, mros::platform::GpioMode::Input);
  shell_printf(ctx.state, "%d\n", mros::platform::mros_gpio_read(pin));
  return 0;
}

void shell_help_gpioset(ShellState& state) {
  shell_write_line(state, "Usage: gpioset GPIO=0|1");
}

int shell_cmd_gpioset(ShellContext& ctx) {
  if (ctx.args.size() != 2U || ctx.args[1] == "--help") {
    shell_help_gpioset(ctx.state);
    return ctx.args.size() == 2U ? 0 : 1;
  }
  const size_t eq = ctx.args[1].find('=');
  if (eq == std::string::npos) return 1;
  int pin = -1;
  int level = -1;
  if (!parse_int(ctx.args[1].substr(0, eq), &pin) || !parse_int(ctx.args[1].substr(eq + 1U), &level) ||
      !gpio_can_write(ctx.state, pin)) return 1;
  (void)mros::platform::mros_gpio_config(pin, mros::platform::GpioMode::Output);
  const bool ok = mros::platform::mros_gpio_write(pin, level != 0);
  shell_printf(ctx.state, "GPIO%d=%d %s\n", pin, level != 0 ? 1 : 0, ok ? "ok" : "failed");
  return ok ? 0 : 1;
}

void shell_help_gpio(ShellState& state) {
  shell_write_line(state, "Usage: gpio info|get GPIO|set GPIO 0|1");
}

int shell_cmd_gpio(ShellContext& ctx) {
  if (ctx.args.size() <= 1U || ctx.args[1] == "--help") {
    shell_help_gpio(ctx.state);
    return ctx.args.size() <= 1U ? 1 : 0;
  }
  if (ctx.args[1] == "info") return shell_cmd_gpioinfo(ctx);
  if (ctx.args[1] == "get" && ctx.args.size() >= 3U) {
    std::vector<std::string> args = {"gpioget", ctx.args[2]};
    ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
    return shell_cmd_gpioget(child);
  }
  if (ctx.args[1] == "set" && ctx.args.size() >= 4U) {
    std::vector<std::string> args = {"gpioset", ctx.args[2] + "=" + ctx.args[3]};
    ShellContext child {ctx.state, args, ctx.stdin_buffer, ctx.json_output, ctx.transport};
    return shell_cmd_gpioset(child);
  }
  shell_help_gpio(ctx.state);
  return 1;
}

void shell_help_i2cdetect(ShellState& state) {
  shell_write_line(state, "Usage: i2cdetect");
}

int shell_cmd_i2cdetect(ShellContext& ctx) {
  if (!mros::platform::mros_i2c_is_ready()) {
    shell_write_line(ctx.state, "i2cdetect: I2C bus is not ready");
    return 1;
  }
  shell_write_line(ctx.state, "     00 10 20 30 40 50 60 70");
  for (uint8_t base = 0; base < 0x80; base += 0x10) {
    shell_printf(ctx.state, "%02x: ", base);
    for (uint8_t off = 0; off < 0x10; ++off) {
      const uint8_t addr = base + off;
      if (addr < 0x03 || addr > 0x77) shell_write(ctx.state, "   ");
      else shell_printf(ctx.state, "%s%02x", mros::platform::mros_i2c_probe(addr) == ESP_OK ? " " : " --", addr);
    }
    shell_write(ctx.state, "\n");
  }
  return 0;
}

void shell_help_i2cget(ShellState& state) {
  shell_write_line(state, "Usage: i2cget ADDR REG");
}

int shell_cmd_i2cget(ShellContext& ctx) {
  if (ctx.args.size() != 3U || ctx.args[1] == "--help") {
    shell_help_i2cget(ctx.state);
    return ctx.args.size() == 2U ? 0 : 1;
  }
  uint8_t addr = 0, reg = 0, value = 0;
  if (!parse_u8(ctx.args[1], &addr) || !parse_u8(ctx.args[2], &reg)) return 1;
  const esp_err_t err = mros::platform::mros_i2c_read_reg_u8(addr, reg, &value);
  if (err != ESP_OK) {
    shell_printf(ctx.state, "i2cget: %s\n", esp_err_to_name(err));
    return 1;
  }
  shell_printf(ctx.state, "0x%02x\n", value);
  return 0;
}

void shell_help_i2cset(ShellState& state) {
  shell_write_line(state, "Usage: i2cset ADDR REG VALUE");
}

int shell_cmd_i2cset(ShellContext& ctx) {
  if (ctx.args.size() != 4U || ctx.args[1] == "--help") {
    shell_help_i2cset(ctx.state);
    return ctx.args.size() == 2U ? 0 : 1;
  }
  uint8_t addr = 0, reg = 0, value = 0;
  if (!parse_u8(ctx.args[1], &addr) || !parse_u8(ctx.args[2], &reg) || !parse_u8(ctx.args[3], &value)) return 1;
  const esp_err_t err = mros::platform::mros_i2c_write_reg_u8(addr, reg, value);
  if (err != ESP_OK) {
    shell_printf(ctx.state, "i2cset: %s\n", esp_err_to_name(err));
    return 1;
  }
  shell_write_line(ctx.state, "ok");
  return 0;
}

void shell_help_i2cdump(ShellState& state) {
  shell_write_line(state, "Usage: i2cdump ADDR [START] [COUNT]");
}

int shell_cmd_i2cdump(ShellContext& ctx) {
  if (ctx.args.size() < 2U || ctx.args[1] == "--help") {
    shell_help_i2cdump(ctx.state);
    return ctx.args.size() >= 2U ? 0 : 1;
  }
  uint8_t addr = 0;
  uint8_t start = 0;
  uint8_t count = 16;
  if (!parse_u8(ctx.args[1], &addr)) return 1;
  if (ctx.args.size() >= 3U && !parse_u8(ctx.args[2], &start)) return 1;
  if (ctx.args.size() >= 4U && !parse_u8(ctx.args[3], &count)) return 1;
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t value = 0;
    const uint8_t reg = static_cast<uint8_t>(start + i);
    if ((i % 16U) == 0U) shell_printf(ctx.state, "%02x:", reg);
    if (mros::platform::mros_i2c_read_reg_u8(addr, reg, &value) == ESP_OK) shell_printf(ctx.state, " %02x", value);
    else shell_write(ctx.state, " --");
    if ((i % 16U) == 15U) shell_write(ctx.state, "\n");
  }
  shell_write(ctx.state, "\n");
  return 0;
}

void shell_help_pwm(ShellState& state) {
  shell_write_line(state, "Usage: pwm status|oe on|off|freq [HZ]|set CH TICKS|angle CH DEG|speed CH VALUE");
}

int shell_cmd_pwm(ShellContext& ctx) {
  if (ctx.args.size() <= 1U || ctx.args[1] == "--help") {
    shell_help_pwm(ctx.state);
    return ctx.args.size() <= 1U ? 1 : 0;
  }
  const std::string sub = ctx.args[1];
  if (sub == "status") {
    PCA9685_DiagSnapshot_t diag {};
    pca9685_get_diag_snapshot(&diag);
    shell_printf(ctx.state, "ready=%s oe=%s freq=%.2fHz queue=%lu/%lu shadow=%s flush=%lu\n",
                 pca9685_is_ready() ? "yes" : "no",
                 pca9685_get_output_enable() ? "on" : "off",
                 pca9685_get_frequency(),
                 static_cast<unsigned long>(diag.queue_depth),
                 static_cast<unsigned long>(diag.queue_capacity),
                 diag.shadow_valid ? "valid" : "empty",
                 static_cast<unsigned long>(diag.shadow_flush_count));
    return 0;
  }
  if (sub == "oe" && ctx.args.size() >= 3U) {
    pca9685_set_output_enable(ctx.args[2] == "on" || ctx.args[2] == "1" || ctx.args[2] == "enable");
    return 0;
  }
  if (sub == "freq") {
    if (ctx.args.size() == 2U) {
      shell_printf(ctx.state, "%.2f\n", pca9685_get_frequency());
      return 0;
    }
    float freq = 0.0f;
    if (!parse_float_arg(ctx.args[2], &freq)) return 1;
    return pca9685_set_frequency(freq) ? 0 : 1;
  }
  uint8_t ch = 0;
  if (ctx.args.size() >= 4U && !parse_u8(ctx.args[2], &ch)) return 1;
  if (sub == "set" && ctx.args.size() >= 4U) {
    uint16_t ticks = 0;
    if (!parse_u16(ctx.args[3], &ticks)) return 1;
    return pca9685_set_pwm(ch, 0, ticks) ? 0 : 1;
  }
  if (sub == "angle" && ctx.args.size() >= 4U) {
    float deg = 0.0f;
    if (!parse_float_arg(ctx.args[3], &deg)) return 1;
    return pca9685_set_servo_angle(ch, deg) ? 0 : 1;
  }
  if (sub == "speed" && ctx.args.size() >= 4U) {
    int speed = 0;
    if (!parse_int(ctx.args[3], &speed)) return 1;
    return pca9685_set_servo_speed(ch, speed) ? 0 : 1;
  }
  shell_help_pwm(ctx.state);
  return 1;
}

void shell_help_adc(ShellState& state) {
  shell_write_line(state, "Usage: adc read GPIO");
}

int shell_cmd_adc(ShellContext& ctx) {
  if (ctx.args.size() != 3U || ctx.args[1] == "--help" || ctx.args[1] != "read") {
    shell_help_adc(ctx.state);
    return ctx.args.size() >= 2U && ctx.args[1] == "--help" ? 0 : 1;
  }
  int pin = -1;
  adc_channel_t channel {};
  if (!parse_int(ctx.args[2], &pin) || !adc_channel_for_pin(pin, &channel)) {
    shell_write_line(ctx.state, "adc: only ESP32-S3 ADC1 GPIO1..GPIO10 are supported");
    return 1;
  }
  adc_oneshot_unit_handle_t unit = nullptr;
  adc_oneshot_unit_init_cfg_t init = {};
  init.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&init, &unit) != ESP_OK) return 1;
  adc_oneshot_chan_cfg_t cfg = {};
  cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  cfg.atten = ADC_ATTEN_DB_12;
  (void)adc_oneshot_config_channel(unit, channel, &cfg);
  int raw = 0;
  const esp_err_t err = adc_oneshot_read(unit, channel, &raw);
  (void)adc_oneshot_del_unit(unit);
  if (err != ESP_OK) {
    shell_printf(ctx.state, "adc: %s\n", esp_err_to_name(err));
    return 1;
  }
  shell_printf(ctx.state, "gpio=%d raw=%d\n", pin, raw);
  return 0;
}

void shell_help_spi(ShellState& state) {
  shell_write_line(state, "Usage: spi status|reset-errors|test [--duration 10] [--json]");
}

int shell_cmd_spi(ShellContext& ctx) {
  if (ctx.args.size() <= 1U || ctx.args[1] == "status") {
    print_spi_status(ctx.state);
    return 0;
  }
  if (ctx.args[1] == "--help") {
    shell_help_spi(ctx.state);
    return 0;
  }
  if (ctx.args[1] == "reset-errors") {
    spi_s3_reset_error_counters();
    spi_c3_reset_error_counters();
    shell_write_line(ctx.state, "spi: error counters reset");
    return 0;
  }
  if (ctx.args[1] == "test") {
    uint32_t duration_s = 10U;
    for (size_t i = 2U; i + 1U < ctx.args.size(); ++i) {
      if (ctx.args[i] == "--duration") {
        duration_s = static_cast<uint32_t>(std::strtoul(ctx.args[i + 1U].c_str(), nullptr, 10));
      }
    }
    duration_s = std::max<uint32_t>(1U, std::min<uint32_t>(duration_s, 120U));
    const LinkDiagReport report = run_spi_link_test(duration_s);
    if (std::find(ctx.args.begin(), ctx.args.end(), "--json") != ctx.args.end()) {
      shell_write_line(ctx.state, link_diag_json(report).c_str());
      return 0;
    }
    print_link_diag(ctx.state, report);
    return 0;
  }
  shell_help_spi(ctx.state);
  return 1;
}

void shell_help_uart(ShellState& state) {
  shell_write_line(state, "Usage: uart status|test [--duration 10] [--json]|noise quiet|normal|verbose|send TEXT|read [N]|crypto status|self-test|enable --psk HEX|disable");
}

int shell_cmd_uart(ShellContext& ctx) {
  if (ctx.args.size() <= 1U || ctx.args[1] == "status") {
    UartLogDiagSnapshot diag {};
    uart1_cobs_get_diag_snapshot(&diag);
    shell_printf(ctx.state, "uart1 ready=%s available=%d writable=%d log=%lu/%lu noise=%s rev=%lu\n",
                 mros::platform::mros_uart_is_ready(UART_NUM_1) ? "yes" : "no",
                 mros::platform::mros_uart_available(UART_NUM_1),
                 mros::platform::mros_uart_writable(UART_NUM_1),
                 static_cast<unsigned long>(diag.log_size),
                 static_cast<unsigned long>(diag.log_capacity),
                 uart1_cobs_log_noise_mode_name(uart1_cobs_get_log_noise_mode()),
                 static_cast<unsigned long>(diag.revision));
    shell_write_line(ctx.state, mros::security::uart_secure::status_text(false).c_str());
    return 0;
  }
  if (ctx.args[1] == "--help") {
    shell_help_uart(ctx.state);
    return 0;
  }
  if (ctx.args[1] == "noise" && ctx.args.size() >= 3U) {
    if (ctx.args[2] == "quiet") uart1_cobs_set_log_noise_mode(UartLogNoiseMode::Quiet);
    else if (ctx.args[2] == "verbose") uart1_cobs_set_log_noise_mode(UartLogNoiseMode::Verbose);
    else uart1_cobs_set_log_noise_mode(UartLogNoiseMode::Normal);
    return 0;
  }
  if (ctx.args[1] == "crypto") {
    const std::string action = ctx.args.size() >= 3U ? ctx.args[2] : "status";
    if (action == "status") {
      shell_write_line(ctx.state, mros::security::uart_secure::status_text(false).c_str());
      return 0;
    }
    std::string error;
    if (action == "self-test") {
      const bool ok = mros::security::uart_secure::self_test(&error);
      shell_printf(ctx.state, "uart crypto self-test: %s%s%s\n", ok ? "PASS" : "FAIL",
                   error.empty() ? "" : " - ", error.c_str());
      return ok ? 0 : 1;
    }
    if (action == "disable") {
      const bool ok = mros::security::uart_secure::disable(&error);
      if (!ok) shell_printf(ctx.state, "uart crypto disable failed: %s\n", error.c_str());
      return ok ? 0 : 1;
    }
    if (action == "enable") {
      const auto psk_it = std::find(ctx.args.begin() + 3U, ctx.args.end(), "--psk");
      if (psk_it == ctx.args.end() || (psk_it + 1U) == ctx.args.end()) {
        shell_write_line(ctx.state, "Usage: uart crypto enable --psk <64-hex>; use only on a trusted local console");
        return 1;
      }
      const bool ok = mros::security::uart_secure::enable_with_psk_hex((psk_it + 1U)->c_str(), &error);
      if (!ok) shell_printf(ctx.state, "uart crypto enable failed: %s\n", error.c_str());
      return ok ? 0 : 1;
    }
    shell_help_uart(ctx.state);
    return 1;
  }
  if (ctx.args[1] == "send" && ctx.args.size() >= 3U) {
    std::string text;
    for (size_t i = 2U; i < ctx.args.size(); ++i) {
      if (!text.empty()) text.push_back(' ');
      text += ctx.args[i];
    }
    return uart1_cobs_send_text_line(text.c_str()) ? 0 : 1;
  }
  if (ctx.args[1] == "read") {
    int count = 64;
    if (ctx.args.size() >= 3U) (void)parse_int(ctx.args[2], &count);
    count = std::max(1, std::min(count, 256));
    std::vector<uint8_t> buf(static_cast<size_t>(count));
    const int n = mros::platform::mros_uart_read(UART_NUM_1, buf.data(), buf.size(), 0);
    for (int i = 0; i < n; ++i) shell_printf(ctx.state, "%02x ", buf[static_cast<size_t>(i)]);
    shell_write(ctx.state, "\n");
    return 0;
  }
  if (ctx.args[1] == "test") {
    uint32_t duration_s = 10U;
    for (size_t i = 2U; i + 1U < ctx.args.size(); ++i) {
      if (ctx.args[i] == "--duration") {
        duration_s = static_cast<uint32_t>(std::strtoul(ctx.args[i + 1U].c_str(), nullptr, 10));
      }
    }
    duration_s = std::max<uint32_t>(1U, std::min<uint32_t>(duration_s, 120U));
    const LinkDiagReport report = run_uart_link_test(duration_s);
    if (std::find(ctx.args.begin(), ctx.args.end(), "--json") != ctx.args.end()) {
      shell_write_line(ctx.state, link_diag_json(report).c_str());
      return 0;
    }
    print_link_diag(ctx.state, report);
    return 0;
  }
  shell_help_uart(ctx.state);
  return 1;
}

}  // namespace mros::shell
