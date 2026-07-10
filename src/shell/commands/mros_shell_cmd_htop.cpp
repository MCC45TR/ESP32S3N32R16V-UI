#include "src/shell/mros_shell_internal.h"

#include <esp_heap_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdlib>
#include <string>

namespace mros::shell {
namespace {

constexpr size_t kHtopOutputCapacity = 3072U;
char* g_htop_output_buffer = nullptr;

char* ensure_htop_output_buffer() {
  if (g_htop_output_buffer != nullptr) {
    return g_htop_output_buffer;
  }
  g_htop_output_buffer = static_cast<char*>(
      heap_caps_malloc(kHtopOutputCapacity,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (g_htop_output_buffer == nullptr) {
    g_htop_output_buffer = static_cast<char*>(
        heap_caps_malloc(kHtopOutputCapacity,
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  return g_htop_output_buffer;
}

bool parse_u32(const std::string& text, uint32_t* value) {
  if (value == nullptr) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || (end != nullptr && *end != '\0') || parsed == 0UL) {
    return false;
  }
  *value = static_cast<uint32_t>(parsed);
  return true;
}

bool emit_snapshot(ShellContext& ctx) {
  if (ctx.state.config.diagnostics_callback == nullptr) {
    shell_write_line(ctx.state, "mtop: diagnostics callback is not configured");
    return false;
  }

  char* output = ensure_htop_output_buffer();
  if (output == nullptr) {
    shell_write_line(ctx.state, "mtop: output buffer allocation failed");
    return false;
  }
  output[0] = '\0';
  const bool ok =
      ctx.state.config.diagnostics_callback(output, kHtopOutputCapacity,
                                            ctx.state.config.user_data);
  shell_write_line(
      ctx.state,
      output[0] != '\0' ? output
                        : (ok ? "mtop: no diagnostics available"
                              : "mtop: diagnostics failed"));
  return ok;
}

}  // namespace

void shell_help_htop(ShellState& state) {
  shell_write_line(state, "Usage: mtop [OPTION]...");
  shell_write_line(state, "Show RTOS task diagnostics or repeat them like watch.");
  shell_write_line(state, "  --once                   print a single snapshot (default)");
  shell_write_line(state, "  -c, --continuous SEC     rerun every SEC seconds");
  shell_write_line(state, "      --count NUM          stop after NUM refresh cycles");
  shell_write_line(state, "      --clear              clear the terminal between cycles");
}

int shell_cmd_htop(ShellContext& ctx) {
  uint32_t interval_sec = 0U;
  uint32_t count = 0U;
  bool clear = false;

  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help" || arg == "-h") {
      shell_help_htop(ctx.state);
      return 0;
    }
    if (arg == "--once") {
      interval_sec = 0U;
      continue;
    }
    if (arg == "-c" || arg == "--continuous") {
      if ((i + 1U) >= ctx.args.size() || !parse_u32(ctx.args[i + 1U], &interval_sec)) {
        shell_write_line(ctx.state, "mtop: invalid continuous interval");
        return 1;
      }
      ++i;
      continue;
    }
    if (arg == "--count") {
      if ((i + 1U) >= ctx.args.size() || !parse_u32(ctx.args[i + 1U], &count)) {
        shell_write_line(ctx.state, "mtop: invalid count");
        return 1;
      }
      ++i;
      continue;
    }
    if (arg == "--clear") {
      clear = true;
      continue;
    }
    shell_printf(ctx.state, "mtop: unknown option '%s'\n", arg.c_str());
    return 1;
  }

  if (interval_sec == 0U) {
    return emit_snapshot(ctx) ? 0 : 1;
  }

  if (count == 0U) {
    count = ctx.state.command_timeout_ms > 0U
                ? (ctx.state.command_timeout_ms / (interval_sec * 1000U))
                : 60U;
    if (count == 0U) {
      count = 1U;
    }
  }

  int result = 0;
  for (uint32_t cycle = 1U; cycle <= count; ++cycle) {
    if (clear || cycle > 1U) {
      shell_write(ctx.state, "\033[2J\033[H");
    }
    shell_printf(ctx.state,
                 "[mtop] cycle=%lu/%lu interval=%lus\n",
                 static_cast<unsigned long>(cycle),
                 static_cast<unsigned long>(count),
                 static_cast<unsigned long>(interval_sec));
    if (!emit_snapshot(ctx)) {
      result = 1;
    }
    if (cycle < count) {
      vTaskDelay(pdMS_TO_TICKS(interval_sec * 1000U));
    }
  }
  return result;
}

}  // namespace mros::shell
