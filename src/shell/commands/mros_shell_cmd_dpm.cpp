#include "src/shell/mros_shell_internal.h"

#include <cstring>

#include <esp_heap_caps.h>

#include "src/core/power/power_manager.h"
#include "src/core/rtos/device_process_manager.h"

namespace mros::shell {
namespace {

constexpr size_t kDpmJsonBufferBytes = 12288;

bool arg_is_json(const ShellContext& ctx) {
  if (ctx.json_output) return true;
  for (const std::string& arg : ctx.args) {
    if (arg == "--json") return true;
  }
  return false;
}

int write_json(ShellState& state, bool (*writer_fn)(char*, size_t)) {
  char* buffer = static_cast<char*>(
      heap_caps_malloc(kDpmJsonBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    buffer = static_cast<char*>(
        heap_caps_malloc(kDpmJsonBufferBytes, MALLOC_CAP_8BIT));
  }
  if (buffer == nullptr) {
    shell_write_line(state, "{\"ok\":false,\"error\":\"no_buffer\"}");
    return 1;
  }
  const bool ok = writer_fn(buffer, kDpmJsonBufferBytes);
  shell_write_line(state, buffer);
  heap_caps_free(buffer);
  return ok ? 0 : 1;
}

void print_status(ShellState& state) {
  mros::rtos::dpm::Summary summary {};
  mros::rtos::dpm::get_summary(&summary);
  shell_write_line(state, "Device Process Manager");
  shell_printf(state, "  policy            : %s\n", summary.mode);
  shell_printf(state, "  runtime state     : %s\n", summary.runtime_state);
  shell_printf(state, "  managed/critical  : %lu / %lu\n",
               static_cast<unsigned long>(summary.managed_tasks),
               static_cast<unsigned long>(summary.critical));
  shell_printf(state, "  sleeping/active   : %lu / %lu\n",
               static_cast<unsigned long>(summary.sleeping),
               static_cast<unsigned long>(summary.active));
  shell_printf(state, "  degraded/fault    : %lu / %lu\n",
               static_cast<unsigned long>(summary.degraded),
               static_cast<unsigned long>(summary.fault));
  shell_printf(state, "  trace seq         : %lu\n",
               static_cast<unsigned long>(summary.seq));
  shell_printf(state, "  telemetry periods : fast=%lu ms medium=%lu ms slow=%lu ms\n",
               static_cast<unsigned long>(summary.telemetry_fast_period_ms),
               static_cast<unsigned long>(summary.telemetry_medium_period_ms),
               static_cast<unsigned long>(summary.telemetry_slow_period_ms));
  shell_printf(state, "  wifi/light sleep  : %s / %s\n",
               summary.wifi_power_save_allowed ? "allowed" : "locked",
               summary.light_sleep_allowed ? "allowed" : "locked");
}

void print_freq_status(ShellState& state) {
  mros::power::Status power {};
  mros::power::get_status(&power);
  shell_write_line(state, "DPM frequency governor");
  shell_printf(state, "  policy/power-mode       : %s / %s\n",
               mros::rtos::dpm::policy_name(mros::rtos::dpm::policy()),
               power.mode);
  shell_printf(state, "  actual/target CPU       : %lu / %lu MHz\n",
               static_cast<unsigned long>(power.actual_cpu_mhz),
               static_cast<unsigned long>(power.target_mhz));
  shell_printf(state, "  demand net/rt           : %lu / %lu MHz\n",
               static_cast<unsigned long>(power.net_demand_mhz),
               static_cast<unsigned long>(power.rt_demand_mhz));
  shell_printf(state, "  min/max/light-sleep     : %lu / %lu MHz / %s\n",
               static_cast<unsigned long>(power.min_mhz),
               static_cast<unsigned long>(power.max_mhz),
               power.light_sleep_enabled ? "yes" : "no");
  shell_printf(state, "  pm locks                : %s\n",
               power.active_locks && power.active_locks[0] ? power.active_locks : "-");
  shell_printf(state, "  lock ttl/expired        : %lu expiring / %lu expired\n",
               static_cast<unsigned long>(power.expiring_lock_count),
               static_cast<unsigned long>(power.expired_lock_count));
  shell_printf(state, "  policy telemetry        : fast=%lu ms medium=%lu ms slow=%lu ms\n",
               static_cast<unsigned long>(power.telemetry_fast_period_ms),
               static_cast<unsigned long>(power.telemetry_medium_period_ms),
               static_cast<unsigned long>(power.telemetry_slow_period_ms));
  shell_printf(state, "  wait floors             : web=%lu wifi=%lu storage=%lu ms\n",
               static_cast<unsigned long>(power.dpm_web_wait_floor_ms),
               static_cast<unsigned long>(power.dpm_wifi_wait_floor_ms),
               static_cast<unsigned long>(power.dpm_storage_wait_floor_ms));
  shell_write_line(state, "  per-core frequency      : not hardware-independent on ESP32-S3");
  shell_write_line(state, "  per-core budget         : core0=network/web/storage, core1=robot/control");
}

void print_decision(ShellState& state) {
  mros::rtos::dpm::PolicyDecision decision {};
  mros::rtos::dpm::get_policy_decision(&decision);
  shell_write_line(state, "DPM policy decision");
  shell_printf(state, "  policy/runtime          : %s / %s\n",
               decision.policy, decision.runtime_state);
  shell_printf(state, "  wait floors             : web=%lu wifi=%lu storage=%lu ms\n",
               static_cast<unsigned long>(decision.web_wait_floor_ms),
               static_cast<unsigned long>(decision.wifi_wait_floor_ms),
               static_cast<unsigned long>(decision.storage_wait_floor_ms));
  shell_printf(state, "  telemetry periods       : fast=%lu medium=%lu slow=%lu ms\n",
               static_cast<unsigned long>(decision.telemetry_fast_period_ms),
               static_cast<unsigned long>(decision.telemetry_medium_period_ms),
               static_cast<unsigned long>(decision.telemetry_slow_period_ms));
  shell_printf(state, "  wifi/light sleep/fast   : %s / %s / %s\n",
               decision.wifi_power_save_allowed ? "allowed" : "locked",
               decision.light_sleep_allowed ? "allowed" : "locked",
               decision.telemetry_fast_allowed ? "allowed" : "deferred");
}

void print_tasks(ShellState& state) {
  mros::rtos::dpm::TaskSnapshot tasks[static_cast<size_t>(MrosRtosTaskDiagId::Count)] {};
  const size_t count = mros::rtos::dpm::get_task_snapshots(tasks, sizeof(tasks) / sizeof(tasks[0]));
  shell_write_line(state, "task                         state       pol            wake   sleep  wait  exec  miss  reason");
  shell_write_line(state, "---------------------------  ----------  -------------  -----  -----  ----  ----  ----  ----------------");
  for (size_t i = 0; i < count; ++i) {
    shell_printf(state, "%-27s  %-10s  %-13s  %5lu  %5lu  %4lu  %4lu  %4lu  %s%s%s\n",
                 tasks[i].name != nullptr ? tasks[i].name : "-",
                 tasks[i].state != nullptr ? tasks[i].state : "-",
                 tasks[i].policy != nullptr ? tasks[i].policy : "-",
                 static_cast<unsigned long>(tasks[i].wake_count),
                 static_cast<unsigned long>(tasks[i].sleep_count),
                 static_cast<unsigned long>(tasks[i].wait_ms),
                 static_cast<unsigned long>(tasks[i].last_exec_ms),
                 static_cast<unsigned long>(tasks[i].deadline_miss),
                 tasks[i].last_wake_reason != nullptr ? tasks[i].last_wake_reason : "-",
                 tasks[i].critical ? " / " : "",
                 tasks[i].critical ? (tasks[i].critical_reason != nullptr ? tasks[i].critical_reason : "critical") : "");
  }
}

void print_trace(ShellState& state) {
  mros::rtos::dpm::TraceEntry trace[16] {};
  const size_t count = mros::rtos::dpm::get_trace(trace, sizeof(trace) / sizeof(trace[0]));
  shell_write_line(state, "seq   ms         task                         event       reason              source");
  shell_write_line(state, "----  ---------  ---------------------------  ----------  ------------------  --------");
  for (size_t i = 0; i < count; ++i) {
    shell_printf(state, "%-4lu  %-9lu  %-27s  %-10s  %-18s  %s\n",
                 static_cast<unsigned long>(trace[i].seq),
                 static_cast<unsigned long>(trace[i].ms),
                 trace[i].task != nullptr ? trace[i].task : "-",
                 trace[i].event != nullptr ? trace[i].event : "-",
                 trace[i].reason != nullptr ? trace[i].reason : "-",
                 trace[i].source != nullptr ? trace[i].source : "-");
  }
}

int set_policy(ShellContext& ctx) {
  if (ctx.args.size() < 4U) {
    shell_write_line(ctx.state, "usage: dpm policy set observe|conservative|adaptive|balanced|cool|performance|motion-safe|update-safe|power-save");
    return 1;
  }
  mros::rtos::dpm::Policy policy {};
  if (!mros::rtos::dpm::parse_policy(ctx.args[3].c_str(), &policy)) {
    shell_write_line(ctx.state, "dpm: invalid policy");
    return 1;
  }
  (void)mros::rtos::dpm::set_policy(policy, true);
  shell_printf(ctx.state, "policy=%s\n", mros::rtos::dpm::policy_name(policy));
  return 0;
}

int wake_target(ShellContext& ctx) {
  if (ctx.args.size() < 3U) {
    shell_write_line(ctx.state, "usage: dpm wake <task|peers|web|storage> [reason]");
    return 1;
  }
  const std::string& target = ctx.args[2];
  const char* reason = ctx.args.size() >= 4U ? ctx.args[3].c_str() : "manual";
  bool ok = false;
  if (target == "peers") {
    ok |= mros::rtos::dpm::wake_task_by_name("comm_spi_t41", reason, "shell");
    ok |= mros::rtos::dpm::wake_task_by_name("comm_spi_c3", reason, "shell");
    ok |= mros::rtos::dpm::wake_task_by_name("comm_uart_t41", reason, "shell");
  } else if (target == "web") {
    ok = mros::rtos::dpm::wake_task_by_name("web_runtime", reason, "shell");
  } else if (target == "storage") {
    ok = mros::rtos::dpm::wake_task_by_name("storage", reason, "shell");
  } else {
    ok = mros::rtos::dpm::wake_task_by_name(target.c_str(), reason, "shell");
  }
  shell_printf(ctx.state, "wake %s: %s\n", target.c_str(), ok ? "ok" : "not-found");
  return ok ? 0 : 1;
}

void print_why_awake(ShellState& state) {
  mros::rtos::dpm::TaskSnapshot tasks[static_cast<size_t>(MrosRtosTaskDiagId::Count)] {};
  const size_t count = mros::rtos::dpm::get_task_snapshots(tasks, sizeof(tasks) / sizeof(tasks[0]));
  shell_write_line(state, "awake or critical tasks");
  for (size_t i = 0; i < count; ++i) {
    const bool awake = tasks[i].state != nullptr &&
                       std::strcmp(tasks[i].state, "SLEEPING") != 0 &&
                       std::strcmp(tasks[i].state, "IDLE") != 0;
    if (!awake && !tasks[i].critical) continue;
    shell_printf(state, "  %-27s %-10s reason=%s%s%s\n",
                 tasks[i].name != nullptr ? tasks[i].name : "-",
                 tasks[i].state != nullptr ? tasks[i].state : "-",
                 tasks[i].last_wake_reason != nullptr ? tasks[i].last_wake_reason : "-",
                 tasks[i].critical ? " critical=" : "",
                 tasks[i].critical ? (tasks[i].critical_reason != nullptr ? tasks[i].critical_reason : "yes") : "");
  }
}

}  // namespace

int shell_cmd_dpm(ShellContext& ctx) {
  if (ctx.args.size() <= 1U || ctx.args[1] == "status") {
    if (arg_is_json(ctx)) return write_json(ctx.state, mros::rtos::dpm::status_json);
    print_status(ctx.state);
    return 0;
  }
  const std::string& sub = ctx.args[1];
  if (sub == "report") {
    if (arg_is_json(ctx)) return write_json(ctx.state, mros::rtos::dpm::tasks_json);
    print_status(ctx.state);
    shell_write_line(ctx.state, "");
    print_tasks(ctx.state);
    shell_write_line(ctx.state, "");
    print_trace(ctx.state);
    return 0;
  }
  if (sub == "tasks") {
    if (arg_is_json(ctx)) return write_json(ctx.state, mros::rtos::dpm::tasks_json);
    print_tasks(ctx.state);
    return 0;
  }
  if (sub == "decision" || sub == "why") {
    if (arg_is_json(ctx)) {
      return write_json(ctx.state, mros::rtos::dpm::policy_decision_json);
    }
    print_decision(ctx.state);
    return 0;
  }
  if (sub == "trace") {
    if (arg_is_json(ctx)) return write_json(ctx.state, mros::rtos::dpm::trace_json);
    print_trace(ctx.state);
    return 0;
  }
  if (sub == "policy") {
    if (ctx.args.size() >= 3U && ctx.args[2] == "get") {
      shell_printf(ctx.state, "policy=%s\n",
                   mros::rtos::dpm::policy_name(mros::rtos::dpm::policy()));
      return 0;
    }
    if (ctx.args.size() >= 3U && ctx.args[2] == "set") return set_policy(ctx);
    shell_write_line(ctx.state, "usage: dpm policy get|set observe|conservative|adaptive|balanced|cool|performance|motion-safe|update-safe|power-save");
    return 1;
  }
  if (sub == "freq") {
    if (ctx.args.size() >= 3U && ctx.args[2] == "trace") {
      char* buffer = static_cast<char*>(
          heap_caps_malloc(4096U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (buffer == nullptr) {
        buffer = static_cast<char*>(heap_caps_malloc(4096U, MALLOC_CAP_8BIT));
      }
      if (buffer == nullptr) {
        shell_write_line(ctx.state, "{\"ok\":false,\"error\":\"no_buffer\"}");
        return 1;
      }
      const bool ok = mros::power::trace_json(buffer, 4096U);
      shell_write_line(ctx.state, buffer);
      heap_caps_free(buffer);
      return ok ? 0 : 1;
    }
    if (arg_is_json(ctx)) return write_json(ctx.state, mros::power::status_json);
    print_freq_status(ctx.state);
    return 0;
  }
  if (sub == "wake") return wake_target(ctx);
  if (sub == "why-awake") {
    print_why_awake(ctx.state);
    return 0;
  }
  if (sub == "reset-stats") {
    mros::rtos::dpm::reset_stats();
    shell_write_line(ctx.state, "dpm stats reset");
    return 0;
  }
  shell_help_dpm(ctx.state);
  return 1;
}

void shell_help_dpm(ShellState& state) {
  shell_write_line(state, "Kullanım:");
  shell_write_line(state, "  dpm status [--json]");
  shell_write_line(state, "  dpm decision [--json]");
  shell_write_line(state, "  dpm report [--json]");
  shell_write_line(state, "  dpm tasks [--json]");
  shell_write_line(state, "  dpm policy get|set observe|conservative|adaptive|balanced|cool|performance|motion-safe|update-safe|power-save");
  shell_write_line(state, "  dpm freq status|trace [--json]");
  shell_write_line(state, "  dpm wake <task|peers|web|storage> [reason]");
  shell_write_line(state, "  dpm why-awake");
  shell_write_line(state, "  dpm trace [--json]");
  shell_write_line(state, "  dpm reset-stats");
}

}  // namespace mros::shell
