#include "src/shell/mros_shell_internal.h"

#include <esp_err.h>
#include <esp_heap_caps.h>

#include "src/core/debug/mros_debug_tools.h"

namespace mros::shell {
namespace {

constexpr size_t kDebugJsonBytes = 1536U;

bool wants_json(const ShellContext& ctx) {
  if (ctx.json_output) return true;
  for (const std::string& arg : ctx.args) {
    if (arg == "--json") return true;
  }
  return false;
}

int write_json(ShellState& state, bool (*fn)(char*, size_t)) {
  char* buffer = static_cast<char*>(heap_caps_malloc(kDebugJsonBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) buffer = static_cast<char*>(heap_caps_malloc(kDebugJsonBytes, MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    shell_write_line(state, "{\"ok\":false,\"error\":\"NO_BUFFER\"}");
    return 1;
  }
  const bool ok = fn(buffer, kDebugJsonBytes);
  shell_write_line(state, buffer);
  heap_caps_free(buffer);
  return ok ? 0 : 1;
}

void print_coredump_status(ShellState& state) {
  char buffer[kDebugJsonBytes] = {};
  (void)mros::debug::coredump_json(buffer, sizeof(buffer));
  shell_write_line(state, "Coredump");
  shell_printf(state, "  present : %s\n", mros::debug::coredump_present() ? "yes" : "no");
  shell_printf(state, "  size    : %lu bytes\n",
               static_cast<unsigned long>(mros::debug::coredump_partition_size()));
  shell_write_line(state, "  json    :");
  shell_write_line(state, buffer);
}

void print_heap_trace_status(ShellState& state) {
  char buffer[kDebugJsonBytes] = {};
  (void)mros::debug::heap_trace_json(buffer, sizeof(buffer));
  shell_write_line(state, "Heap trace");
  shell_write_line(state, "  state   : release-disabled");
  shell_write_line(state, "  enable  : build with heap tracing config for live allocation traces");
  shell_write_line(state, "  json    :");
  shell_write_line(state, buffer);
}

}  // namespace

int shell_cmd_coredump(ShellContext& ctx) {
  const std::string sub = ctx.args.size() >= 2U ? ctx.args[1] : "status";
  if (sub == "status" || sub == "summary") {
    if (wants_json(ctx)) return write_json(ctx.state, mros::debug::coredump_json);
    print_coredump_status(ctx.state);
    return 0;
  }
  if (sub == "download") {
    shell_write_line(ctx.state, "coredump download: use /api/debug/coredump/download");
    return 0;
  }
  if (sub == "clear") {
    const esp_err_t err = mros::debug::coredump_clear();
    if (wants_json(ctx)) {
      shell_printf(ctx.state, "{\"ok\":%s,\"error\":%lu}\n",
                   err == ESP_OK ? "true" : "false",
                   static_cast<unsigned long>(err));
    } else {
      shell_printf(ctx.state, "coredump clear: %s (%lu)\n",
                   err == ESP_OK ? "ok" : "failed",
                   static_cast<unsigned long>(err));
    }
    return err == ESP_OK ? 0 : 1;
  }
  shell_help_coredump(ctx.state);
  return 1;
}

void shell_help_coredump(ShellState& state) {
  shell_write_line(state, "Kullanım:");
  shell_write_line(state, "  coredump status|summary [--json]");
  shell_write_line(state, "  coredump download");
  shell_write_line(state, "  coredump clear [--json]");
}

int shell_cmd_heap_trace(ShellContext& ctx) {
  const size_t sub_index = (ctx.args.size() >= 2U && ctx.args[1] == "trace") ? 2U : 1U;
  const std::string sub = ctx.args.size() > sub_index ? ctx.args[sub_index] : "summary";
  if (sub == "summary" || sub == "status" || sub == "dump") {
    if (wants_json(ctx)) return write_json(ctx.state, mros::debug::heap_trace_json);
    print_heap_trace_status(ctx.state);
    return sub == "dump" ? 1 : 0;
  }
  if (sub == "start") {
    const esp_err_t err = mros::debug::heap_trace_start();
    if (wants_json(ctx)) {
      shell_printf(ctx.state, "{\"ok\":false,\"error\":\"FEATURE_DISABLED\",\"code\":%lu}\n",
                   static_cast<unsigned long>(err));
    } else {
      shell_write_line(ctx.state, "heap trace: FEATURE_DISABLED in this build");
    }
    return 1;
  }
  if (sub == "stop") {
    const esp_err_t err = mros::debug::heap_trace_stop();
    if (wants_json(ctx)) {
      shell_printf(ctx.state, "{\"ok\":false,\"error\":\"FEATURE_DISABLED\",\"code\":%lu}\n",
                   static_cast<unsigned long>(err));
    } else {
      shell_write_line(ctx.state, "heap trace: not active");
    }
    return err == ESP_OK ? 0 : 1;
  }
  if (sub == "clear") {
    mros::debug::heap_trace_clear();
    shell_write_line(ctx.state, wants_json(ctx) ? "{\"ok\":true}" : "heap trace counters cleared");
    return 0;
  }
  shell_help_heap_trace(ctx.state);
  return 1;
}

void shell_help_heap_trace(ShellState& state) {
  shell_write_line(state, "Kullanım:");
  shell_write_line(state, "  heap trace start|stop|summary|dump|clear [--json]");
  shell_write_line(state, "Not: release build varsayılanında FEATURE_DISABLED döner.");
}

}  // namespace mros::shell
