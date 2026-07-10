#include "src/shell/mros_shell_internal.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/ip_addr.h>
#include <lwip/netdb.h>
#include <ping/ping_sock.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace mros::shell {
namespace {

constexpr size_t kPingOutputCapacity = 4096U;

struct PingOptions {
  uint32_t count = 4U;
  uint32_t timeout_ms = 1000U;
  uint32_t interval_ms = 1000U;
  uint32_t data_size = 56U;
  std::string target;
};

struct PingRun {
  TaskHandle_t waiter = nullptr;
  SemaphoreHandle_t mutex = nullptr;
  char* output = nullptr;
  size_t output_capacity = 0U;
  size_t used = 0U;
  uint32_t transmitted = 0U;
  uint32_t received = 0U;
  uint32_t min_ms = UINT32_MAX;
  uint32_t max_ms = 0U;
  uint64_t total_ms = 0U;
  char host[128] = {};
  char resolved_ip[64] = {};
};

void ping_append(PingRun* run, const char* format, ...) {
  if (run == nullptr || format == nullptr || run->mutex == nullptr) {
    return;
  }
  if (xSemaphoreTake(run->mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  va_list args;
  va_start(args, format);
  const int written = std::vsnprintf(
      run->output + run->used, run->output_capacity - run->used, format, args);
  va_end(args);
  if (written > 0) {
    run->used = std::min(run->output_capacity - 1U,
                         run->used + static_cast<size_t>(written));
    run->output[run->used] = '\0';
  }

  xSemaphoreGive(run->mutex);
}

void ping_success_callback(esp_ping_handle_t handle, void* args) {
  PingRun* run = static_cast<PingRun*>(args);
  if (run == nullptr) {
    return;
  }

  uint32_t seqno = 0U;
  uint32_t elapsed_ms = 0U;
  uint32_t recv_len = 0U;
  uint32_t ttl = 0U;
  ip_addr_t target_addr {};
  esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
  esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
  esp_ping_get_profile(handle, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
  esp_ping_get_profile(handle, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
  esp_ping_get_profile(handle, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));

  char ip_text[64] = {};
  ipaddr_ntoa_r(&target_addr, ip_text, sizeof(ip_text));
  if (ip_text[0] == '\0') {
    std::snprintf(ip_text, sizeof(ip_text), "%s", run->resolved_ip);
  }

  ++run->transmitted;
  ++run->received;
  run->total_ms += elapsed_ms;
  run->min_ms = std::min(run->min_ms, elapsed_ms);
  run->max_ms = std::max(run->max_ms, elapsed_ms);
  ping_append(
      run,
      "%lu bytes from %s: icmp_seq=%lu ttl=%lu time=%lums\n",
      static_cast<unsigned long>(recv_len),
      ip_text,
      static_cast<unsigned long>(seqno),
      static_cast<unsigned long>(ttl),
      static_cast<unsigned long>(elapsed_ms));
}

void ping_timeout_callback(esp_ping_handle_t handle, void* args) {
  PingRun* run = static_cast<PingRun*>(args);
  if (run == nullptr) {
    return;
  }

  uint32_t seqno = 0U;
  esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
  ++run->transmitted;
  ping_append(run, "Request timeout for icmp_seq %lu\n", static_cast<unsigned long>(seqno));
}

void ping_end_callback(esp_ping_handle_t handle, void* args) {
  PingRun* run = static_cast<PingRun*>(args);
  if (run == nullptr) {
    return;
  }

  uint32_t requests = 0U;
  uint32_t replies = 0U;
  uint32_t duration_ms = 0U;
  esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST, &requests, sizeof(requests));
  esp_ping_get_profile(handle, ESP_PING_PROF_REPLY, &replies, sizeof(replies));
  esp_ping_get_profile(handle, ESP_PING_PROF_DURATION, &duration_ms, sizeof(duration_ms));

  const uint32_t packet_loss = requests == 0U ? 0U : ((requests - replies) * 100U) / requests;
  const uint32_t avg_ms = replies == 0U ? 0U : static_cast<uint32_t>(run->total_ms / replies);
  ping_append(run, "--- %s ping statistics ---\n", run->host);
  ping_append(
      run,
      "%lu packets transmitted, %lu received, %lu%% packet loss, time %lums\n",
      static_cast<unsigned long>(requests),
      static_cast<unsigned long>(replies),
      static_cast<unsigned long>(packet_loss),
      static_cast<unsigned long>(duration_ms));
  if (replies > 0U) {
    ping_append(
        run,
        "rtt min/avg/max = %lu/%lu/%lums\n",
        static_cast<unsigned long>(run->min_ms),
        static_cast<unsigned long>(avg_ms),
        static_cast<unsigned long>(run->max_ms));
  }

  if (run->waiter != nullptr) {
    xTaskNotifyGive(run->waiter);
  }
}

bool parse_ping_args(ShellContext& ctx, PingOptions* options, bool* help_requested) {
  if (options == nullptr || help_requested == nullptr) {
    return false;
  }

  *help_requested = false;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      *help_requested = true;
      return true;
    }
    if ((arg == "-c" || arg == "-W" || arg == "-i" || arg == "-s") && (i + 1U) >= ctx.args.size()) {
      shell_printf(ctx.state, "ping: option requires an argument -- '%s'\n", arg.c_str());
      return false;
    }
    if (arg == "-c") {
      options->count = static_cast<uint32_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10));
      continue;
    }
    if (arg == "-W") {
      options->timeout_ms = static_cast<uint32_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10)) * 1000U;
      continue;
    }
    if (arg == "-i") {
      options->interval_ms = static_cast<uint32_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10)) * 1000U;
      continue;
    }
    if (arg == "-s") {
      options->data_size = static_cast<uint32_t>(std::strtoul(ctx.args[++i].c_str(), nullptr, 10));
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      shell_printf(ctx.state, "ping: invalid option '%s'\n", arg.c_str());
      return false;
    }
    if (!options->target.empty()) {
      shell_printf(ctx.state, "ping: extra operand '%s'\n", arg.c_str());
      return false;
    }
    options->target = arg;
  }

  if (options->count == 0U) {
    shell_write_line(ctx.state, "ping: count must be greater than zero");
    return false;
  }
  if (options->target.empty()) {
    shell_write_line(ctx.state, "ping: missing host operand");
    return false;
  }
  return true;
}

bool resolve_ping_target(const std::string& target, ip_addr_t* out_addr, char* out_text, const size_t out_size) {
  if (out_addr == nullptr || out_text == nullptr || out_size == 0U) {
    return false;
  }

  out_text[0] = '\0';
  if (ipaddr_aton(target.c_str(), out_addr) != 0) {
    std::snprintf(out_text, out_size, "%s", target.c_str());
    return true;
  }

  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  struct addrinfo* result = nullptr;
  if (getaddrinfo(target.c_str(), nullptr, &hints, &result) != 0 || result == nullptr || result->ai_addr == nullptr) {
    if (result != nullptr) {
      freeaddrinfo(result);
    }
    return false;
  }

  const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
  if (inet_ntop(AF_INET, &address->sin_addr, out_text, out_size) == nullptr) {
    freeaddrinfo(result);
    return false;
  }

  const bool ok = ipaddr_aton(out_text, out_addr) != 0;
  freeaddrinfo(result);
  return ok;
}

}  // namespace

void shell_help_ping(ShellState& state) {
  shell_write_line(state, "Usage: ping [-c COUNT] [-W TIMEOUT] [-i INTERVAL] [-s SIZE] HOST");
  shell_write_line(state, "Send ICMP ECHO_REQUEST packets to a host.");
  shell_write_line(state, "  -c COUNT                  stop after COUNT replies");
  shell_write_line(state, "  -W TIMEOUT                response timeout in seconds");
  shell_write_line(state, "  -i INTERVAL               interval between probes in seconds");
  shell_write_line(state, "  -s SIZE                   payload size in bytes");
}

int shell_cmd_ping(ShellContext& ctx) {
  PingOptions options {};
  bool help_requested = false;
  if (!parse_ping_args(ctx, &options, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_ping(ctx.state);
    return 0;
  }

  ip_addr_t target_addr {};
  char resolved_ip[64] = {};
  if (!resolve_ping_target(options.target, &target_addr, resolved_ip, sizeof(resolved_ip))) {
    shell_printf(ctx.state, "ping: %s: Name or service not known\n", options.target.c_str());
    return 1;
  }

  PingRun run {};
  run.waiter = xTaskGetCurrentTaskHandle();
  run.output = static_cast<char*>(
      heap_caps_malloc(kPingOutputCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (run.output == nullptr) {
    run.output = static_cast<char*>(heap_caps_malloc(
        kPingOutputCapacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (run.output == nullptr) {
    shell_write_line(ctx.state, "ping: failed to allocate output buffer");
    return 1;
  }
  run.output_capacity = kPingOutputCapacity;
  run.output[0] = '\0';
  run.mutex = xSemaphoreCreateMutex();
  if (run.mutex == nullptr) {
    heap_caps_free(run.output);
    shell_write_line(ctx.state, "ping: failed to allocate synchronization object");
    return 1;
  }
  std::snprintf(run.host, sizeof(run.host), "%s", options.target.c_str());
  std::snprintf(run.resolved_ip, sizeof(run.resolved_ip), "%s", resolved_ip);
  ping_append(&run, "PING %s (%s) %lu bytes of data.\n", run.host, run.resolved_ip, static_cast<unsigned long>(options.data_size));

  esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
  config.count = options.count;
  config.timeout_ms = options.timeout_ms;
  config.interval_ms = options.interval_ms;
  config.data_size = options.data_size;
  config.target_addr = target_addr;

  esp_ping_callbacks_t callbacks {};
  callbacks.cb_args = &run;
  callbacks.on_ping_success = ping_success_callback;
  callbacks.on_ping_timeout = ping_timeout_callback;
  callbacks.on_ping_end = ping_end_callback;

  esp_ping_handle_t session = nullptr;
  const esp_err_t session_result = esp_ping_new_session(&config, &callbacks, &session);
  if (session_result != ESP_OK || session == nullptr) {
    vSemaphoreDelete(run.mutex);
    heap_caps_free(run.output);
    shell_printf(ctx.state, "ping: failed to create ping session (%ld)\n", static_cast<long>(session_result));
    return 1;
  }

  const esp_err_t start_result = esp_ping_start(session);
  if (start_result != ESP_OK) {
    esp_ping_delete_session(session);
    vSemaphoreDelete(run.mutex);
    heap_caps_free(run.output);
    shell_printf(ctx.state, "ping: failed to start ping session (%ld)\n", static_cast<long>(start_result));
    return 1;
  }

  const uint32_t wait_ms = (options.count * (options.interval_ms + options.timeout_ms)) + 2000U;
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
  esp_ping_delete_session(session);

  shell_write(ctx.state, run.output);
  vSemaphoreDelete(run.mutex);
  heap_caps_free(run.output);
  return run.received > 0U ? 0 : 1;
}

}  // namespace mros::shell
