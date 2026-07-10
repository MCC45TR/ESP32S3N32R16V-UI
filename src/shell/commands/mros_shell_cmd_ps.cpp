#include "src/shell/mros_shell_internal.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "src/comm_interfaces/spi/spi_c3_master.h"
#include "src/comm_interfaces/uart/uart_cobs.h"
#include "src/core/rtos/task_manager.h"
#include "src/drivers/i2c_pca9685.h"
#include "src/drivers/storage/logger_driver.h"
#include "src/drivers/utils/mros_console.h"
namespace mros::shell {
namespace {

struct PsFlags {
  bool json = false;
  bool long_format = false;
};

const char* task_state_text(const eTaskState state) {
  switch (state) {
    case eRunning:
      return "RUN";
    case eReady:
      return "RDY";
    case eBlocked:
      return "BLK";
    case eSuspended:
      return "SUS";
    case eDeleted:
      return "DEL";
    default:
      return "UNK";
  }
}

void format_small_core_id(const BaseType_t core_id, char* out, const size_t out_size) {
  if (out == nullptr || out_size == 0U) {
    return;
  }
  if (core_id < 0) {
    return;
  }
  if (core_id < 10) {
    out[0] = static_cast<char>('0' + core_id);
    if (out_size > 1U) {
      out[1] = '\0';
    }
    return;
  }
  if (core_id < 100 && out_size > 2U) {
    out[0] = static_cast<char>('0' + (core_id / 10));
    out[1] = static_cast<char>('0' + (core_id % 10));
    out[2] = '\0';
    return;
  }
  std::snprintf(out, out_size, "cpu");
}

void format_task_core(const TaskStatus_t& task, char* out, const size_t out_size) {
  if (out == nullptr || out_size == 0U) {
    return;
  }
  std::snprintf(out, out_size, "-");
#if defined(configTASKLIST_INCLUDE_COREID) && (configTASKLIST_INCLUDE_COREID == 1)
  if (task.xCoreID == tskNO_AFFINITY) {
    std::snprintf(out, out_size, "any");
  } else {
    format_small_core_id(task.xCoreID, out, out_size);
  }
#elif defined(INCLUDE_xTaskGetCoreID) && (INCLUDE_xTaskGetCoreID == 1)
  const BaseType_t core_id = xTaskGetCoreID(task.xHandle);
  if (core_id == tskNO_AFFINITY) {
    std::snprintf(out, out_size, "any");
  } else {
    format_small_core_id(core_id, out, out_size);
  }
#endif
}

bool parse_ps_args(ShellContext& ctx, PsFlags* flags, bool* help_requested) {
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
    if (arg == "--long") {
      flags->long_format = true;
      continue;
    }
    if (arg == "--wide") {
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'l':
            flags->long_format = true;
            break;
          case 'w':
            break;
          default:
            shell_printf(ctx.state, "ps: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }
    shell_printf(ctx.state, "ps: extra operand '%s'\n", arg.c_str());
    return false;
  }
  return true;
}

TaskStatus_t* alloc_task_snapshot_buffer(const size_t count) {
  if (count == 0U) {
    return nullptr;
  }
  return static_cast<TaskStatus_t*>(
      heap_caps_malloc(sizeof(TaskStatus_t) * count,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void format_cpu_percent(const uint32_t cpu_x100, char* out, const size_t out_size) {
  if (out == nullptr || out_size == 0U) {
    return;
  }
  std::snprintf(out, out_size, "%lu.%02lu%%",
                static_cast<unsigned long>(cpu_x100 / 100U),
                static_cast<unsigned long>(cpu_x100 % 100U));
}

}  // namespace

void shell_help_ps(ShellState& state) {
  shell_write_line(state, "Usage: ps [OPTION]...");
  shell_write_line(state, "Show the current RTOS task list.");
  shell_write_line(state, "      --json                print task snapshot as JSON");
  shell_write_line(state, "  -l, --long                show runtime and handle details");
  shell_write_line(state, "  -w, --wide                accept wide output (compatibility)");
}

int shell_cmd_ps(ShellContext& ctx) {
  PsFlags flags {};
  bool help_requested = false;
  if (!parse_ps_args(ctx, &flags, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_ps(ctx.state);
    return 0;
  }

  const UBaseType_t task_count = uxTaskGetNumberOfTasks();
  if (task_count == 0U) {
    shell_write_line(ctx.state, "ps: no tasks reported");
    return 1;
  }

  const size_t snapshot_capacity = static_cast<size_t>(task_count + 8U);
  TaskStatus_t* tasks = alloc_task_snapshot_buffer(snapshot_capacity);
  if (tasks == nullptr) {
    shell_write_line(ctx.state, "ps: task buffer allocation failed");
    return 1;
  }
  uint32_t total_runtime = 0U;
  const UBaseType_t actual_count =
      uxTaskGetSystemState(tasks, snapshot_capacity, &total_runtime);
  if (actual_count == 0U) {
    heap_caps_free(tasks);
    shell_write_line(ctx.state, "ps: task snapshot is unavailable");
    return 1;
  }

  std::sort(tasks, tasks + actual_count, [](const TaskStatus_t& lhs, const TaskStatus_t& rhs) {
    return lhs.xTaskNumber < rhs.xTaskNumber;
  });

  if (flags.json) {
    shell_printf(ctx.state, "{\"total_runtime\":%lu,\"tasks\":[", static_cast<unsigned long>(total_runtime));
    for (UBaseType_t i = 0; i < actual_count; ++i) {
      const TaskStatus_t& task = tasks[i];
      char core_text[8] = "-";
      format_task_core(task, core_text, sizeof(core_text));
      const uint32_t cpu_x100 =
          total_runtime > 0U
              ? static_cast<uint32_t>((static_cast<uint64_t>(task.ulRunTimeCounter) * 10000ULL) / total_runtime)
              : 0U;
      const char* task_name = (task.pcTaskName != nullptr && task.pcTaskName[0] != '\0') ? task.pcTaskName : "(task)";
      MrosRtosTaskDiagSnapshot diag {};
      (void)app_rtos_get_task_diag_by_name(task_name, &diag);
      shell_printf(
          ctx.state,
          "%s{\"pid\":%lu,\"priority\":%lu,\"core\":\"%s\",\"state\":\"%s\",\"stack_hw\":%lu,"
          "\"cpu_x100\":%lu,\"slip_ms\":%lu,\"max_slip_ms\":%lu,\"exec_ms\":%lu,\"runtime\":%lu,\"name\":\"%s\"}",
          i == 0U ? "" : ",",
          static_cast<unsigned long>(task.xTaskNumber),
          static_cast<unsigned long>(task.uxCurrentPriority),
          core_text,
          task_state_text(task.eCurrentState),
          static_cast<unsigned long>(task.usStackHighWaterMark),
          static_cast<unsigned long>(cpu_x100),
          static_cast<unsigned long>(diag.last_slip_ms),
          static_cast<unsigned long>(diag.max_slip_ms),
          static_cast<unsigned long>(diag.last_exec_ms),
          static_cast<unsigned long>(task.ulRunTimeCounter),
          task_name);
    }
    shell_write_line(ctx.state, "]}");
    heap_caps_free(tasks);
    return 0;
  }

  if (flags.long_format) {
    shell_printf(ctx.state, "%-5s %-4s %-5s %-4s %-7s %-7s %-6s %-6s %-7s %-10s %s\n",
                 "PID", "PRI", "CORE", "ST", "STK_HW", "CPU%", "SLIP",
                 "MAX", "EXEC", "RUNTIME", "NAME");
  } else {
    shell_printf(ctx.state, "%-5s %-4s %-5s %-4s %-7s %-7s %-6s %s\n",
                 "PID", "PRI", "CORE", "ST", "STK_HW", "CPU%", "SLIP",
                 "NAME");
  }

  for (UBaseType_t i = 0; i < actual_count; ++i) {
    const TaskStatus_t& task = tasks[i];
    char core_text[8] = "-";
    format_task_core(task, core_text, sizeof(core_text));
    const uint32_t cpu_x100 =
        total_runtime > 0U
            ? static_cast<uint32_t>((static_cast<uint64_t>(task.ulRunTimeCounter) *
                                     10000ULL) /
                                    total_runtime)
            : 0U;
    char cpu_text[16] = "0.00%";
    format_cpu_percent(cpu_x100, cpu_text, sizeof(cpu_text));

    const char* task_name = (task.pcTaskName != nullptr && task.pcTaskName[0] != '\0') ? task.pcTaskName : "(task)";
    MrosRtosTaskDiagSnapshot diag {};
    (void)app_rtos_get_task_diag_by_name(task_name, &diag);
    if (flags.long_format) {
      shell_printf(
          ctx.state,
          "%-5lu %-4lu %-5s %-4s %-7lu %-7s %-6lu %-6lu %-7lu %-10lu %s\n",
          static_cast<unsigned long>(task.xTaskNumber),
          static_cast<unsigned long>(task.uxCurrentPriority),
          core_text,
          task_state_text(task.eCurrentState),
          static_cast<unsigned long>(task.usStackHighWaterMark),
          cpu_text,
          static_cast<unsigned long>(diag.last_slip_ms),
          static_cast<unsigned long>(diag.max_slip_ms),
          static_cast<unsigned long>(diag.last_exec_ms),
          static_cast<unsigned long>(task.ulRunTimeCounter),
          task_name);
    } else {
      shell_printf(
          ctx.state,
          "%-5lu %-4lu %-5s %-4s %-7lu %-7s %-6lu %s\n",
          static_cast<unsigned long>(task.xTaskNumber),
          static_cast<unsigned long>(task.uxCurrentPriority),
          core_text,
          task_state_text(task.eCurrentState),
          static_cast<unsigned long>(task.usStackHighWaterMark),
          cpu_text,
          static_cast<unsigned long>(diag.last_slip_ms),
          task_name);
    }
  }

  heap_caps_free(tasks);

  if (flags.long_format) {
    PCA9685_DiagSnapshot_t pca {};
    LoggerDiagSnapshot storage {};
    MrosConsoleDiagSnapshot console {};
    UartLogDiagSnapshot uart {};
    C3SpiDiagSnapshot c3 {};
    pca9685_get_diag_snapshot(&pca);
    logger_get_diag_snapshot(&storage);
    mros_console_get_diag_snapshot(&console);
    uart1_cobs_get_diag_snapshot(&uart);
    spi_c3_get_diag_snapshot(&c3);
    shell_write_line(ctx.state, "");
    shell_write_line(ctx.state, "queue/runtime diagnostics");
    shell_printf(ctx.state,
                 "  pca      q=%lu/%lu hi=%lu enq=%lu proc=%lu dup=%lu coal=%lu drop_old=%lu drop=%lu\n",
                 static_cast<unsigned long>(pca.queue_depth),
                 static_cast<unsigned long>(pca.queue_capacity),
                 static_cast<unsigned long>(pca.queue_high_watermark),
                 static_cast<unsigned long>(pca.enqueue_count),
                 static_cast<unsigned long>(pca.process_count),
                 static_cast<unsigned long>(pca.duplicate_skip_count),
                 static_cast<unsigned long>(pca.coalesced_count),
                 static_cast<unsigned long>(pca.drop_oldest_count),
                 static_cast<unsigned long>(pca.drop_count));
    shell_printf(ctx.state,
                 "  storage  q=%lu/%lu hi=%lu enq=%lu proc=%lu drop=%lu batch=%lu flush=%lu direct=%lu csv=%lu\n",
                 static_cast<unsigned long>(storage.queue_depth),
                 static_cast<unsigned long>(storage.queue_capacity),
                 static_cast<unsigned long>(storage.queue_high_watermark),
                 static_cast<unsigned long>(storage.enqueue_count),
                 static_cast<unsigned long>(storage.processed_count),
                 static_cast<unsigned long>(storage.drop_count),
                 static_cast<unsigned long>(storage.batched_write_count),
                 static_cast<unsigned long>(storage.batch_flush_count),
                 static_cast<unsigned long>(storage.direct_write_fallback_count),
                 static_cast<unsigned long>(storage.csv_flush_count));
    shell_printf(ctx.state,
                 "  console  q=%lu/%lu hi=%lu enq=%lu proc=%lu drop=%lu lines=%lu trunc=%lu\n",
                 static_cast<unsigned long>(console.queue_depth),
                 static_cast<unsigned long>(console.queue_capacity),
                 static_cast<unsigned long>(console.queue_high_watermark),
                 static_cast<unsigned long>(console.enqueued_bytes),
                 static_cast<unsigned long>(console.processed_bytes),
                 static_cast<unsigned long>(console.dropped_bytes),
                 static_cast<unsigned long>(console.emitted_lines),
                 static_cast<unsigned long>(console.truncated_lines));
    shell_printf(ctx.state,
                 "  uartlog  size=%lu/%lu full=%lu since=%lu string=%lu lock_fail=%lu\n",
                 static_cast<unsigned long>(uart.log_size),
                 static_cast<unsigned long>(uart.log_capacity),
                 static_cast<unsigned long>(uart.full_copy_count),
                 static_cast<unsigned long>(uart.since_copy_count),
                 static_cast<unsigned long>(uart.snapshot_string_count),
                 static_cast<unsigned long>(uart.lock_fail_count));
    shell_printf(ctx.state,
                 "  c3spi    period=%lums queued=%lu done=%lu timeout=%lu fail=%lu rx=%lu crc=%lu marker=%lu alive=%s link=%s\n",
                 static_cast<unsigned long>(c3.effective_period_ms),
                 static_cast<unsigned long>(c3.transactions_queued),
                 static_cast<unsigned long>(c3.transactions_completed),
                 static_cast<unsigned long>(c3.transaction_timeout_count),
                 static_cast<unsigned long>(c3.transaction_fail_count),
                 static_cast<unsigned long>(c3.total_rx_count),
                 static_cast<unsigned long>(c3.crc_error_count),
                 static_cast<unsigned long>(c3.marker_error_count),
                 c3.alive_ok ? "yes" : "no",
                 c3.link_synced ? "synced" : "search");
  }

  return 0;
}

}  // namespace mros::shell
