#include "src/shell/mros_shell_internal.h"

#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "src/core/rtos/task_manager.h"

extern "C" esp_err_t esp_psram_impl_get_physical_size(uint32_t* out_size_bytes);

#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
extern "C" uint8_t _ext_ram_bss_start;
extern "C" uint8_t _ext_ram_bss_end;
#endif

#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS
extern "C" char _instruction_reserved_start;
extern "C" char _instruction_reserved_end;
#endif

#if CONFIG_SPIRAM_RODATA
extern "C" char _rodata_reserved_start;
extern "C" char _rodata_reserved_end;
#endif

namespace mros::shell {
namespace {

namespace {

uint64_t clamp_subtract(const uint64_t lhs, const uint64_t rhs) {
  return lhs >= rhs ? (lhs - rhs) : 0U;
}

uint64_t query_psram_physical_bytes() {
  uint32_t physical_size = 0U;
  if (esp_psram_impl_get_physical_size(&physical_size) == ESP_OK) {
    return physical_size;
  }
  return esp_psram_get_size();
}

uint64_t query_psram_ext_bss_bytes() {
#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
  const intptr_t start = reinterpret_cast<intptr_t>(&_ext_ram_bss_start);
  const intptr_t end = reinterpret_cast<intptr_t>(&_ext_ram_bss_end);
  return end > start ? static_cast<uint64_t>(end - start) : 0U;
#else
  return 0U;
#endif
}

uint64_t query_psram_xip_text_bytes() {
#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS
  const uintptr_t start = reinterpret_cast<uintptr_t>(&_instruction_reserved_start);
  const uintptr_t end = reinterpret_cast<uintptr_t>(&_instruction_reserved_end);
  if (end <= start) {
    return 0U;
  }
  const uintptr_t aligned_start = start & ~(static_cast<uintptr_t>(CONFIG_MMU_PAGE_SIZE) - 1U);
  const uintptr_t aligned_end =
      (end + static_cast<uintptr_t>(CONFIG_MMU_PAGE_SIZE) - 1U) &
      ~(static_cast<uintptr_t>(CONFIG_MMU_PAGE_SIZE) - 1U);
  return aligned_end > aligned_start ? static_cast<uint64_t>(aligned_end - aligned_start) : 0U;
#else
  return 0U;
#endif
}

uint64_t query_psram_xip_rodata_bytes() {
#if CONFIG_SPIRAM_RODATA
  const uintptr_t start = reinterpret_cast<uintptr_t>(&_rodata_reserved_start);
  const uintptr_t end = reinterpret_cast<uintptr_t>(&_rodata_reserved_end);
  if (end <= start) {
    return 0U;
  }
  const uintptr_t aligned_start = start & ~(static_cast<uintptr_t>(CONFIG_MMU_PAGE_SIZE) - 1U);
  const uintptr_t aligned_end =
      (end + static_cast<uintptr_t>(CONFIG_MMU_PAGE_SIZE) - 1U) &
      ~(static_cast<uintptr_t>(CONFIG_MMU_PAGE_SIZE) - 1U);
  return aligned_end > aligned_start ? static_cast<uint64_t>(aligned_end - aligned_start) : 0U;
#else
  return 0U;
#endif
}

struct PsramBreakdown {
  uint64_t physical_total = 0U;
  uint64_t heap_total = 0U;
  uint64_t heap_free = 0U;
  uint64_t heap_used = 0U;
  uint64_t non_heap_used = 0U;
  uint64_t ext_bss_used = 0U;
  uint64_t xip_text_used = 0U;
  uint64_t xip_rodata_used = 0U;
  uint64_t other_non_heap_used = 0U;
};

PsramBreakdown collect_psram_breakdown() {
  PsramBreakdown out {};
  out.physical_total = query_psram_physical_bytes();
  out.heap_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  out.heap_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  out.heap_used = clamp_subtract(out.heap_total, out.heap_free);
  out.non_heap_used = clamp_subtract(out.physical_total, out.heap_total);
  out.ext_bss_used = query_psram_ext_bss_bytes();
  out.xip_text_used = query_psram_xip_text_bytes();
  out.xip_rodata_used = query_psram_xip_rodata_bytes();

  const uint64_t known_non_heap =
      out.ext_bss_used + out.xip_text_used + out.xip_rodata_used;
  out.other_non_heap_used = clamp_subtract(out.non_heap_used, known_non_heap);
  return out;
}

}  // namespace

enum class FreeUnit : uint8_t {
  Human,
  Bytes,
  Kib,
  Mib,
};

struct FreeFlags {
  FreeUnit unit = FreeUnit::Kib;
  bool json = false;
};

struct TaskStackBudget {
  const char* name;
  uint32_t configured_bytes;
};

struct TaskStackRow {
  const char* name = nullptr;
  uint32_t configured_bytes = 0U;
  uint32_t high_watermark_bytes = 0U;
  uint32_t used_bytes = 0U;
  uint32_t hint_bytes = 0U;
  uint32_t reclaimable_bytes = 0U;
  uint32_t used_percent = 0U;
  uint32_t cpu_x100 = 0U;
  uint32_t last_slip_ms = 0U;
  uint32_t max_slip_ms = 0U;
  uint32_t last_exec_ms = 0U;
};

constexpr TaskStackBudget kKnownTaskStacks[] = {
    {"web_runtime_task", 5120U},
    {"wifi_runtime_task", 3584U},
    {"comm_uart_t41_task", 3584U},
    {"storage_task", 5120U},
    {"fk_preview_task", 2560U},
    {"mros_shell_task", 8192U},
    {"mros_ssh_task", 3072U},
    {"mros_mcp_task", 2560U},
    {"comm_spi_t41_task", 4096U},
    {"comm_spi_c3_task", 3072U},
    {"joint_traj_task", 2560U},
    {"experimental_worker_task", 3072U},
    {"turret_pid_task", 3072U},
    {"servo_drive_pca", 3072U},
    {"device_process_manager_task", 2048U},
};

bool task_name_matches(const char* actual, const char* configured) {
  if (actual == nullptr || configured == nullptr) {
    return false;
  }
  const size_t actual_len = std::strlen(actual);
  const size_t configured_len = std::strlen(configured);
  if (actual_len == 0U || configured_len == 0U) {
    return false;
  }
  if (actual_len == configured_len) {
    return std::strncmp(actual, configured, configured_len) == 0;
  }
  if (actual_len < 8U || actual_len > configured_len) {
    return false;
  }
  return std::strncmp(actual, configured, actual_len) == 0;
}

uint32_t configured_stack_for_task(const char* name) {
  for (const TaskStackBudget& stack : kKnownTaskStacks) {
    if (task_name_matches(name, stack.name)) {
      return stack.configured_bytes;
    }
  }
  return 0U;
}

uint32_t stack_tuning_hint(const uint32_t configured_bytes,
                           const uint32_t high_watermark_bytes) {
  if (configured_bytes == 0U || high_watermark_bytes <= 2048U) {
    return configured_bytes;
  }
  const uint32_t reclaimable = high_watermark_bytes - 1024U;
  if (reclaimable < 1024U || reclaimable >= configured_bytes) {
    return configured_bytes;
  }
  const uint32_t hinted = configured_bytes - reclaimable;
  return hinted < 3072U ? 3072U : hinted;
}

TaskStatus_t* alloc_task_snapshot_buffer(const size_t count) {
  if (count == 0U) {
    return nullptr;
  }
  return static_cast<TaskStatus_t*>(
      heap_caps_malloc(sizeof(TaskStatus_t) * count,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

std::string format_free_value(const uint64_t bytes, const FreeFlags& flags) {
  char buffer[32] = {};
  switch (flags.unit) {
    case FreeUnit::Bytes:
      std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(bytes));
      return buffer;
    case FreeUnit::Mib:
      std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(bytes / (1024U * 1024U)));
      return buffer;
    case FreeUnit::Human: {
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
    case FreeUnit::Kib:
    default:
      std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(bytes / 1024U));
      return buffer;
  }
}

bool parse_free_args(ShellContext& ctx, FreeFlags* flags, bool* help_requested) {
  if (flags == nullptr || help_requested == nullptr) {
    return false;
  }

  *help_requested = false;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      *help_requested = true;
      return true;
    }
    if (arg == "--json") {
      flags->json = true;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'b':
            flags->unit = FreeUnit::Bytes;
            break;
          case 'h':
            flags->unit = FreeUnit::Human;
            break;
          case 'k':
            flags->unit = FreeUnit::Kib;
            break;
          case 'm':
            flags->unit = FreeUnit::Mib;
            break;
          default:
            shell_printf(ctx.state, "free: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }
    shell_printf(ctx.state, "free: extra operand '%s'\n", arg.c_str());
    return false;
  }
  return true;
}

void print_task_stack_budget(ShellContext& ctx, const FreeFlags& flags) {
  const UBaseType_t task_count = uxTaskGetNumberOfTasks();
  if (task_count == 0U) {
    return;
  }

  const size_t snapshot_capacity = static_cast<size_t>(task_count + 8U);
  TaskStatus_t* tasks = alloc_task_snapshot_buffer(snapshot_capacity);
  if (tasks == nullptr) {
    shell_write_line(ctx.state, "task.stack: snapshot allocation failed");
    return;
  }

  uint32_t total_runtime = 0U;
  const UBaseType_t actual_count =
      uxTaskGetSystemState(tasks, snapshot_capacity, &total_runtime);
  if (actual_count == 0U) {
    heap_caps_free(tasks);
    return;
  }

  TaskStackRow rows[32] = {};
  size_t row_count = 0U;
  uint64_t configured_total = 0U;
  uint64_t used_total = 0U;
  uint64_t free_total = 0U;
  uint64_t hint_total = 0U;
  uint64_t reclaimable_total = 0U;
  uint32_t unknown_cfg_count = 0U;

  for (UBaseType_t i = 0; i < actual_count; ++i) {
    const char* task_name =
        (tasks[i].pcTaskName != nullptr && tasks[i].pcTaskName[0] != '\0')
            ? tasks[i].pcTaskName
            : "(task)";
    const uint32_t configured = configured_stack_for_task(task_name);
    if (configured == 0U) {
      ++unknown_cfg_count;
      continue;
    }
    const uint32_t high_watermark =
        static_cast<uint32_t>(tasks[i].usStackHighWaterMark);
    const uint32_t used = configured > high_watermark
                              ? (configured - high_watermark)
                              : 0U;
    const uint32_t hint = stack_tuning_hint(configured, high_watermark);
    const uint32_t reclaimable = configured > hint ? (configured - hint) : 0U;
    const uint32_t used_percent =
        configured > 0U ? static_cast<uint32_t>((static_cast<uint64_t>(used) * 100U) / configured) : 0U;
    const uint32_t cpu_x100 =
        total_runtime > 0U
            ? static_cast<uint32_t>((static_cast<uint64_t>(tasks[i].ulRunTimeCounter) *
                                     10000ULL) /
                                    total_runtime)
            : 0U;
    MrosRtosTaskDiagSnapshot diag {};
    (void)app_rtos_get_task_diag_by_name(task_name, &diag);

    configured_total += configured;
    used_total += used;
    free_total += high_watermark;
    hint_total += hint;
    reclaimable_total += reclaimable;

    if (row_count < (sizeof(rows) / sizeof(rows[0]))) {
      rows[row_count++] = TaskStackRow{
          task_name, configured, high_watermark, used, hint, reclaimable,
          used_percent, cpu_x100, diag.last_slip_ms, diag.max_slip_ms,
          diag.last_exec_ms};
    }
  }

  heap_caps_free(tasks);

  if (configured_total == 0U) {
    return;
  }

  std::sort(rows, rows + row_count, [](const TaskStackRow& lhs,
                                       const TaskStackRow& rhs) {
    if (lhs.reclaimable_bytes != rhs.reclaimable_bytes) {
      return lhs.reclaimable_bytes > rhs.reclaimable_bytes;
    }
    return lhs.configured_bytes > rhs.configured_bytes;
  });

  shell_write_line(ctx.state, "");
  shell_write_line(ctx.state, "Task stacks: known MROS task stack reservations from internal SRAM");
  shell_printf(ctx.state,
               "task.stack total=%s used(max)=%s free(hw)=%s hint=%s reclaimable=%s unknown_cfg_tasks=%lu\n",
               format_free_value(configured_total, flags).c_str(),
               format_free_value(used_total, flags).c_str(),
               format_free_value(free_total, flags).c_str(),
               format_free_value(hint_total, flags).c_str(),
               format_free_value(reclaimable_total, flags).c_str(),
               static_cast<unsigned long>(unknown_cfg_count));
  shell_printf(ctx.state, "%-16s %8s %8s %8s %5s %7s %6s %6s %6s %8s %8s\n",
               "NAME", "CFG", "USED", "FREE", "USE%", "CPU%",
               "SLIP", "MAX", "EXEC", "HINT", "SAVE");
  for (size_t i = 0U; i < row_count; ++i) {
    const TaskStackRow& row = rows[i];
    shell_printf(ctx.state,
                 "%-16s %8s %8s %8s %4lu%% %3lu.%02lu%% %6lu %6lu %6lu %8s %8s\n",
                 row.name,
                 format_free_value(row.configured_bytes, flags).c_str(),
                 format_free_value(row.used_bytes, flags).c_str(),
                 format_free_value(row.high_watermark_bytes, flags).c_str(),
                 static_cast<unsigned long>(row.used_percent),
                 static_cast<unsigned long>(row.cpu_x100 / 100U),
                 static_cast<unsigned long>(row.cpu_x100 % 100U),
                 static_cast<unsigned long>(row.last_slip_ms),
                 static_cast<unsigned long>(row.max_slip_ms),
                 static_cast<unsigned long>(row.last_exec_ms),
                 format_free_value(row.hint_bytes, flags).c_str(),
                 format_free_value(row.reclaimable_bytes, flags).c_str());
  }
}

}  // namespace

void shell_help_free(ShellState& state) {
  shell_write_line(state, "Usage: free [OPTION]...");
  shell_write_line(state, "Show heap memory plus PSRAM physical/mapped usage.");
  shell_write_line(state, "  -b                        show values in bytes");
  shell_write_line(state, "  -h                        show values in a human-readable form");
  shell_write_line(state, "  -k                        show values in KiB");
  shell_write_line(state, "  -m                        show values in MiB");
  shell_write_line(state, "      --json                print raw byte counters as JSON");
}

int shell_cmd_free(ShellContext& ctx) {
  FreeFlags flags {};
  bool help_requested = false;
  if (!parse_free_args(ctx, &flags, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_free(ctx.state);
    return 0;
  }

  const uint64_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint64_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const PsramBreakdown psram = collect_psram_breakdown();
  const uint64_t total = internal_total + psram.heap_total;
  const uint64_t available = internal_free + psram.heap_free;
  const uint64_t used = total >= available ? (total - available) : 0U;
  const uint64_t min_free = esp_get_minimum_free_heap_size();
  const uint64_t internal_used = clamp_subtract(internal_total, internal_free);
  const uint64_t psram_physical_used = clamp_subtract(psram.physical_total, psram.heap_free);
  if (flags.json) {
    shell_printf(
        ctx.state,
        "{\"total\":%llu,\"used\":%llu,\"available\":%llu,"
        "\"internal\":{\"total\":%llu,\"used\":%llu,\"free\":%llu},"
        "\"psram\":{\"physical_total\":%llu,\"physical_used\":%llu,\"heap_total\":%llu,\"heap_used\":%llu,\"heap_free\":%llu,"
        "\"non_heap_used\":%llu,\"ext_bss\":%llu,\"text\":%llu,\"rodata\":%llu,\"other\":%llu},"
        "\"min_heap\":%llu}\n",
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(used),
        static_cast<unsigned long long>(available),
        static_cast<unsigned long long>(internal_total),
        static_cast<unsigned long long>(internal_used),
        static_cast<unsigned long long>(internal_free),
        static_cast<unsigned long long>(psram.physical_total),
        static_cast<unsigned long long>(psram_physical_used),
        static_cast<unsigned long long>(psram.heap_total),
        static_cast<unsigned long long>(psram.heap_used),
        static_cast<unsigned long long>(psram.heap_free),
        static_cast<unsigned long long>(psram.non_heap_used),
        static_cast<unsigned long long>(psram.ext_bss_used),
        static_cast<unsigned long long>(psram.xip_text_used),
        static_cast<unsigned long long>(psram.xip_rodata_used),
        static_cast<unsigned long long>(psram.other_non_heap_used),
        static_cast<unsigned long long>(min_free));
    return 0;
  }
  const auto print_triplet = [&](const char* label, const uint64_t total_value, const uint64_t used_value, const uint64_t free_value) {
    const std::string total_text = format_free_value(total_value, flags);
    const std::string used_text = format_free_value(used_value, flags);
    const std::string free_text = format_free_value(free_value, flags);
    shell_printf(
        ctx.state,
        "%-10s %12s %12s %12s\n",
        label,
        total_text.c_str(),
        used_text.c_str(),
        free_text.c_str());
  };

  shell_printf(
      ctx.state,
      "%-10s %12s %12s %12s %12s %12s %12s\n",
      "",
      "total",
      "used",
      "free",
      "shared",
      "buff/cache",
      "available");
  shell_printf(
      ctx.state,
      "%-10s %12s %12s %12s %12s %12s %12s\n",
      "Mem:",
      format_free_value(total, flags).c_str(),
      format_free_value(used, flags).c_str(),
      format_free_value(available, flags).c_str(),
      "0",
      "0",
      format_free_value(available, flags).c_str());
  print_triplet("Internal:", internal_total, internal_used, internal_free);
  print_triplet("PSRAM:", psram.heap_total, psram.heap_used, psram.heap_free);
  shell_printf(ctx.state, "%-10s %12s\n", "MinHeap:", format_free_value(min_free, flags).c_str());
  shell_printf(
      ctx.state,
      "psram.phys total=%s used=%s free=%s\n",
      format_free_value(psram.physical_total, flags).c_str(),
      format_free_value(psram_physical_used, flags).c_str(),
      format_free_value(psram.heap_free, flags).c_str());
  shell_printf(
      ctx.state,
      "psram.map  total=%s ext-bss=%s text=%s rodata=%s other=%s\n",
      format_free_value(psram.non_heap_used, flags).c_str(),
      format_free_value(psram.ext_bss_used, flags).c_str(),
      format_free_value(psram.xip_text_used, flags).c_str(),
      format_free_value(psram.xip_rodata_used, flags).c_str(),
      format_free_value(psram.other_non_heap_used, flags).c_str());
  print_task_stack_budget(ctx, flags);
  return 0;
}

}  // namespace mros::shell
