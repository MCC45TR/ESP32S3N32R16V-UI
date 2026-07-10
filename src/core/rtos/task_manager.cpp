#include "task_manager.h"

#include "src/comm_interfaces/spi/spi_c3_master.h"
#include "src/comm_interfaces/spi/spi_t41_link.h"
#include "src/comm_interfaces/uart/uart_cobs.h"
#include "src/config/pin_config.h"
#include "src/core/health/bearing_health.h"
#include "src/core/rtos/device_process_manager.h"
#include "src/core/power/power_manager.h"
#include "src/core/state/event_bus.h"
#include "src/drivers/i2c/pca9685_driver.h"
#include "src/drivers/storage/logger_driver.h"
#include "src/drivers/utils/mros_console.h"
#include "src/experimental/experimental_worker.h"
#include "src/net/mcp_service.h"
#include "src/net/ssh_service.h"
#include "src/platform/mros_gpio.h"
#include "src/platform/mros_time.h"
#include "src/shell/mros_shell.h"
#include "src/shell/shell_service.h"
#include "src/update/update_runtime.h"
#include "src/web/web_server.h"
#include "src/web/server/wifi_manager.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

namespace {

constexpr BaseType_t kNetCore = 0;
constexpr BaseType_t kRtCore = 1;

constexpr UBaseType_t kTurretPidPriority = 8;
constexpr UBaseType_t kServoPriority = 7;
constexpr UBaseType_t kSpiT41Priority = 6;
constexpr UBaseType_t kSpiC3Priority = 6;
constexpr UBaseType_t kJointPriority = 5;
constexpr UBaseType_t kWebPriority = 4;
constexpr UBaseType_t kWifiPriority = 3;
constexpr UBaseType_t kUartPriority = 3;
constexpr UBaseType_t kStoragePriority = 2;
constexpr UBaseType_t kFkPriority = 2;
constexpr UBaseType_t kExperimentalPriority = 2;
constexpr UBaseType_t kShellPriority = 1;
constexpr UBaseType_t kSshPriority = 1;
constexpr UBaseType_t kMcpPriority = 1;
constexpr UBaseType_t kDpmPriority = 1;
constexpr UBaseType_t kBearingHealthPriority = tskIDLE_PRIORITY + 1;

constexpr uint32_t kWebStackWords = 5120;
constexpr uint32_t kWifiStackWords = 3584;
constexpr uint32_t kUartStackWords = 3584;
constexpr uint32_t kStorageStackWords = 5120;
constexpr uint32_t kFkStackWords = 2560;
constexpr uint32_t kShellStackWords = 8192;
constexpr uint32_t kSshStackWords = 3072;
constexpr uint32_t kMcpStackWords = 2560;
constexpr uint32_t kSpiT41StackWords = 4096;
constexpr uint32_t kSpiC3StackWords = 3072;
constexpr uint32_t kJointStackWords = 2560;
constexpr uint32_t kExperimentalStackWords = 3072;
constexpr uint32_t kBearingHealthStackWords = 4096;
constexpr uint32_t kTurretPidStackWords = 3072;
constexpr uint32_t kServoStackWords = 3072;
constexpr uint32_t kDpmStackWords = 2048;

constexpr uint32_t kWebIdlePeriodMs = 250;
constexpr uint32_t kWebActivePeriodMs = 50;
constexpr uint32_t kWebShellPeriodMs = 20;
constexpr uint32_t kWebDebugPeriodMs = 1000;
constexpr uint32_t kWifiPeriodMs = 50;
constexpr uint32_t kUartIdleMs = 20;
constexpr uint32_t kStorageIdleMs = 200;
constexpr uint32_t kSpiT41IdleMs = 50;
constexpr uint32_t kJointTickMs = 20;
constexpr uint32_t kFastControlTickUs = 2000;

TaskHandle_t g_web_runtime_task = nullptr;
TaskHandle_t g_wifi_runtime_task = nullptr;
TaskHandle_t g_comm_uart_t41_task = nullptr;
TaskHandle_t g_storage_task = nullptr;
TaskHandle_t g_fk_preview_task = nullptr;
TaskHandle_t g_shell_task = nullptr;
TaskHandle_t g_ssh_task = nullptr;
TaskHandle_t g_mcp_task = nullptr;
TaskHandle_t g_comm_spi_t41_task = nullptr;
TaskHandle_t g_comm_spi_c3_task = nullptr;
TaskHandle_t g_joint_traj_task = nullptr;
TaskHandle_t g_experimental_worker_task = nullptr;
TaskHandle_t g_bearing_health_task = nullptr;
TaskHandle_t g_turret_pid_task = nullptr;
TaskHandle_t g_servo_drive_task = nullptr;
TaskHandle_t g_device_process_manager_task = nullptr;
TaskHandle_t g_boot_rtos_snapshot_task = nullptr;
esp_timer_handle_t g_fast_control_timer = nullptr;
portMUX_TYPE g_task_diag_mux = portMUX_INITIALIZER_UNLOCKED;

struct TaskDiagSlot {
  const char* name = nullptr;
  uint32_t expected_period_ms = 0;
  uint32_t last_period_ms = 0;
  uint32_t last_exec_ms = 0;
  uint32_t last_slip_ms = 0;
  uint32_t max_slip_ms = 0;
  uint32_t wake_count = 0;
  uint32_t slip_count = 0;
};

TaskDiagSlot g_task_diag[static_cast<size_t>(MrosRtosTaskDiagId::Count)] = {
    {"web_runtime_task"},
    {"wifi_runtime_task"},
    {"comm_uart_t41_task"},
    {"storage_task"},
    {"fk_preview_task"},
    {"mros_shell_task"},
    {"mros_ssh_task"},
    {"mros_mcp_task"},
    {"comm_spi_t41_task"},
    {"comm_spi_c3_task"},
    {"joint_traj_task"},
    {"experimental_worker_task"},
    {"bearing_health_task"},
    {"turret_pid_task"},
    {"servo_drive_pca"},
    {"device_process_manager_task"},
};

uint32_t elapsed_ms_since(const unsigned long start_ms,
                          const unsigned long end_ms) {
  return static_cast<uint32_t>(end_ms - start_ms);
}

uint32_t web_runtime_wait_timeout_ms() {
  const uint32_t clients = web_server_total_ws_client_count();
  if (clients == 0U) {
    return kWebIdlePeriodMs;
  }
  const WebServerDiagSnapshot diag = [] {
    WebServerDiagSnapshot snapshot {};
    web_server_get_diag_snapshot(&snapshot);
    return snapshot;
  }();
  if ((diag.ws_clients_shell + diag.ws_clients_legacy +
       diag.ws_clients_telemetry) > 0U) {
    return kWebShellPeriodMs;
  }
  if (diag.ws_clients_debug > 0U) {
    return kWebDebugPeriodMs;
  }
  return kWebActivePeriodMs;
}

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

void fast_control_tick_callback(void* /*arg*/) {
  static uint32_t c3_notify_accum_ms = 0;
  static uint32_t turret_notify_accum_ms = 0;
  if (g_comm_spi_c3_task != nullptr) {
    uint32_t c3_period_ms = spi_c3_get_effective_period_ms();
    c3_period_ms = mros::rtos::dpm::adjust_wait_ms(
        MrosRtosTaskDiagId::CommSpiC3, c3_period_ms, spi_c3_is_connected());
    if (c3_period_ms < kFastControlTickUs / 1000U) {
      c3_period_ms = kFastControlTickUs / 1000U;
    }
    c3_notify_accum_ms += kFastControlTickUs / 1000U;
    if (c3_notify_accum_ms >= c3_period_ms) {
      c3_notify_accum_ms = 0;
      xTaskNotifyGive(g_comm_spi_c3_task);
    }
  }
  if (g_turret_pid_task != nullptr) {
    const bool turret_active = pca9685_is_ready() &&
                               spi_s3_get_motor_state() != 0U &&
                               !spi_s3_get_turret_output_lock();
    const uint32_t turret_period_ms = turret_active ? 2U : 20U;
    turret_notify_accum_ms += kFastControlTickUs / 1000U;
    if (turret_notify_accum_ms >= turret_period_ms) {
      turret_notify_accum_ms = 0;
      xTaskNotifyGive(g_turret_pid_task);
    }
  }
}

TickType_t ticks_from_wait_ms(const uint32_t wait_ms) {
  if (wait_ms == 0U || wait_ms == 0xFFFFFFFFUL) {
    return portMAX_DELAY;
  }
  TickType_t ticks = pdMS_TO_TICKS(wait_ms);
  return ticks == 0 ? 1 : ticks;
}

uint32_t dpm_notify_wait(const MrosRtosTaskDiagId id,
                         const uint32_t proposed_wait_ms,
                         const bool active,
                         const char* reason) {
  const uint32_t wait_ms =
      mros::rtos::dpm::adjust_wait_ms(id, proposed_wait_ms, active);
  mros::rtos::dpm::record_sleep(id, wait_ms, reason);
  const TickType_t ticks = ticks_from_wait_ms(wait_ms);
  const uint32_t notified = ulTaskNotifyTake(pdTRUE, ticks);
  mros::rtos::dpm::record_wake(id, notified > 0U ? "notify" : "timeout", "rtos");
  return wait_ms;
}

void web_runtime_task(void* /*arg*/) {
  mros_console.printf("[RTOS] Web runtime task running on Core %d\n",
                      xPortGetCoreID());
  web_server_set_runtime_task_handle(xTaskGetCurrentTaskHandle());
  web_server_init();
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();
  unsigned long last_stack_report_ms = 0;

  while (true) {
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    web_server_loop();
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    uint32_t wait_ms = web_runtime_wait_timeout_ms();
    wait_ms = mros::rtos::dpm::adjust_wait_ms(
        MrosRtosTaskDiagId::WebRuntime, wait_ms,
        web_server_total_ws_client_count() > 0U);
    const UBaseType_t stack_hw = uxTaskGetStackHighWaterMark(nullptr);
    if (stack_hw < 768U &&
        (cycle_start_ms - last_stack_report_ms) > 5000UL) {
      last_stack_report_ms = cycle_start_ms;
      mros_console.printf("[SEC] web_runtime_task stack high-water low: %lu words\n",
                          static_cast<unsigned long>(stack_hw));
    }
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::WebRuntime, wait_ms,
                               period_ms, exec_ms);
    dpm_notify_wait(MrosRtosTaskDiagId::WebRuntime, wait_ms,
                    web_server_total_ws_client_count() > 0U, "web runtime");
  }
}

void wifi_runtime_task(void* /*arg*/) {
  mros_console.printf("[RTOS] WiFi runtime task running on Core %d\n",
                      xPortGetCoreID());
  wifi_manager_set_task_handle(xTaskGetCurrentTaskHandle());
  wifi_manager_init();
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    const unsigned long now_ms = mros::platform::mros_millis();
    const unsigned long cycle_start_ms = now_ms;
    wifi_manager_loop(now_ms);
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    uint32_t wait_ms = wifi_manager_wait_timeout_ms(now_ms);
    if (wait_ms == 0) {
      wait_ms = kWifiPeriodMs;
    }
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::WifiRuntime, wait_ms,
                               period_ms, exec_ms);
    dpm_notify_wait(MrosRtosTaskDiagId::WifiRuntime, wait_ms, false,
                    "wifi runtime");
  }
}

void comm_uart_t41_task(void* /*arg*/) {
  mros_console.printf("[RTOS] UART t41 task running on Core %d\n",
                      xPortGetCoreID());
  uart1_cobs_init();
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    uart1_cobs_periodic(cycle_start_ms);
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    const uint32_t wait_ms = mros::rtos::dpm::adjust_wait_ms(
        MrosRtosTaskDiagId::CommUartT41, kUartIdleMs, false);
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::CommUartT41, wait_ms,
                               period_ms, exec_ms);
    dpm_notify_wait(MrosRtosTaskDiagId::CommUartT41, wait_ms, false,
                    "uart idle");
  }
}

void storage_task(void* /*arg*/) {
  mros_console.printf("[RTOS] Storage task running on Core %d\n",
                      xPortGetCoreID());
  logger_set_task_handle(xTaskGetCurrentTaskHandle());
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    logger_process_pending();
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    const uint32_t wait_ms = mros::rtos::dpm::adjust_wait_ms(
        MrosRtosTaskDiagId::Storage, kStorageIdleMs, false);
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::Storage, wait_ms,
                               period_ms, exec_ms);
    dpm_notify_wait(MrosRtosTaskDiagId::Storage, wait_ms, false,
                    "storage idle");
  }
}

void fk_preview_task(void* /*arg*/) {
  mros_console.printf("[RTOS] FK preview task running on Core %d\n",
                      xPortGetCoreID());
  web_server_set_fk_task_handle(xTaskGetCurrentTaskHandle());
  web_server_notify_fk_task();
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    dpm_notify_wait(MrosRtosTaskDiagId::FkPreview, 0U, false, "fk preview");
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    web_server_fk_loop();
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::FkPreview, 0U, period_ms,
                               exec_ms);
    event_bus_clear(BIT_FK_ACTIVE);
  }
}

void shell_task(void* /*arg*/) {
  mros_console.printf("[RTOS] Shell task running on Core %d\n", xPortGetCoreID());
  mros::shell::service::set_task_handle(xTaskGetCurrentTaskHandle());
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    mros::shell::service::process_pending_requests();
    const TickType_t raw_wait_ticks =
        (mros::shell::serial_has_pending_input() ||
         mros::shell::serial_recently_active(3000U))
            ? pdMS_TO_TICKS(25)
            : pdMS_TO_TICKS(250);
    const uint32_t wait_ms =
        static_cast<uint32_t>(raw_wait_ticks) * portTICK_PERIOD_MS;
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::Shell,
                               wait_ms,
                               period_ms, exec_ms);
    dpm_notify_wait(MrosRtosTaskDiagId::Shell, wait_ms,
                    mros::shell::serial_has_pending_input(), "shell idle");
  }
}

void ssh_task(void* /*arg*/) {
  mros_console.printf("[RTOS] SSH task running on Core %d\n", xPortGetCoreID());
  mros::ssh::service_set_task_handle(xTaskGetCurrentTaskHandle());
  mros::ssh::service_init();
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    mros::ssh::service_process();
    const uint32_t wait_ms = mros::ssh::service_wait_timeout_ms();
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::Ssh, wait_ms, period_ms,
                               exec_ms);
    dpm_notify_wait(MrosRtosTaskDiagId::Ssh, wait_ms, wait_ms > 0U,
                    "ssh service");
  }
}

void mcp_task(void* /*arg*/) {
  mros_console.printf("[RTOS] MCP task running on Core %d\n", xPortGetCoreID());
  mros::mcp::service_set_task_handle(xTaskGetCurrentTaskHandle());
  mros::mcp::service_init();
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    mros::mcp::service_process();
    const uint32_t wait_ms = mros::mcp::service_wait_timeout_ms();
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::Mcp, wait_ms, period_ms,
                               exec_ms);
    dpm_notify_wait(MrosRtosTaskDiagId::Mcp, wait_ms, wait_ms > 0U,
                    "mcp service");
  }
}

void comm_spi_t41_task(void* /*arg*/) {
  mros_console.printf("[RTOS] SPI t41 task running on Core %d\n",
                      xPortGetCoreID());
  spi_s3_set_notify_task(xTaskGetCurrentTaskHandle());
  spi_s3_service_comm(mros::platform::mros_millis());
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    const uint32_t wait_ms = dpm_notify_wait(
        MrosRtosTaskDiagId::CommSpiT41, kSpiT41IdleMs, spi_s3_is_connected(),
        "t41 spi");
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    spi_s3_service_comm(cycle_start_ms);
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::CommSpiT41, wait_ms,
                               period_ms, exec_ms);
  }
}

void comm_spi_c3_task(void* /*arg*/) {
  mros_console.printf("[RTOS] SPI C3 task running on Core %d\n",
                      xPortGetCoreID());
  spi_c3_set_task_handle(xTaskGetCurrentTaskHandle());
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    const uint32_t base_wait_ms = spi_c3_get_effective_period_ms();
    const uint32_t wait_ms = dpm_notify_wait(
        MrosRtosTaskDiagId::CommSpiC3, base_wait_ms > 0U ? base_wait_ms : 20U,
        spi_c3_is_connected(), "c3 spi");
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    spi_c3_service_cycle(cycle_start_ms);
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::CommSpiC3,
                               wait_ms, period_ms, exec_ms);
  }
}

void joint_traj_task(void* /*arg*/) {
  mros_console.printf("[RTOS] Joint trajectory task running on Core %d\n",
                      xPortGetCoreID());
  spi_s3_set_joint_task_handle(xTaskGetCurrentTaskHandle());
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    const bool active = spi_s3_joint_traj_is_active();
    const uint32_t wait_ms = active ? kJointTickMs : 0U;
    dpm_notify_wait(MrosRtosTaskDiagId::JointTraj, wait_ms, active,
                    "joint trajectory");
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    spi_s3_service_joint_traj(cycle_start_ms);
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::JointTraj,
                               wait_ms, period_ms, exec_ms);
    if (!spi_s3_joint_traj_is_active()) {
      event_bus_clear(BIT_TRAJ_ACTIVE);
    }
  }
}

void experimental_worker_task(void* /*arg*/) {
  mros_console.printf("[RTOS] Experimental worker task sleeping on Core %d\n",
                      xPortGetCoreID());
  mros::experimental::worker_set_task_handle(xTaskGetCurrentTaskHandle());
  mros::experimental::worker_task_loop();
}

void device_process_manager_task(void* /*arg*/) {
  mros_console.printf("[RTOS] Device Process Manager task sleeping on Core %d\n",
                      xPortGetCoreID());
  mros::rtos::dpm::task_loop();
}

void turret_pid_task(void* /*arg*/) {
  mros_console.printf("[RTOS] Turret PID task running on Core %d\n",
                      xPortGetCoreID());
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    const bool turret_active = pca9685_is_ready() &&
                               spi_s3_get_motor_state() != 0U &&
                               !spi_s3_get_turret_output_lock();
    const uint32_t pid_wait_ms = turret_active ? 2U : 20U;
    if (turret_active) {
      (void)mros::power::acquire_lock(mros::power::LockOwner::RobotMotion,
                                      "turret-pid");
    } else {
      (void)mros::power::release_lock(mros::power::LockOwner::RobotMotion);
    }
    dpm_notify_wait(MrosRtosTaskDiagId::TurretPid, pid_wait_ms, turret_active,
                    "turret pid");
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    const uint32_t cycle_period_ms =
        static_cast<uint32_t>(cycle_start_ms - last_cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    spi_s3_service_turret_pid(cycle_start_ms);
    const uint32_t cycle_exec_ms =
        static_cast<uint32_t>(mros::platform::mros_millis() - cycle_start_ms);
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::TurretPid, pid_wait_ms,
                               cycle_period_ms, cycle_exec_ms);
    web_server_report_pid_cycle_ms(cycle_period_ms, cycle_exec_ms);
  }
}

void servo_drive_pca9685_task(void* /*arg*/) {
  mros_console.printf("[RTOS] Servo drive task running on Core %d\n",
                      xPortGetCoreID());
  pca9685_set_task_handle(xTaskGetCurrentTaskHandle());
  unsigned long last_cycle_start_ms = mros::platform::mros_millis();

  while (true) {
    mros::rtos::dpm::record_sleep(MrosRtosTaskDiagId::ServoDrive, 0U,
                                  "pca queue");
    const unsigned long cycle_start_ms = mros::platform::mros_millis();
    pca9685_process_queue();
    mros::rtos::dpm::record_wake(MrosRtosTaskDiagId::ServoDrive, "queue",
                                 "pca9685");
    const uint32_t exec_ms =
        elapsed_ms_since(cycle_start_ms, mros::platform::mros_millis());
    const uint32_t period_ms =
        elapsed_ms_since(last_cycle_start_ms, cycle_start_ms);
    last_cycle_start_ms = cycle_start_ms;
    app_rtos_record_task_cycle(MrosRtosTaskDiagId::ServoDrive, 0U, period_ms,
                               exec_ms);
  }
}

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
  snprintf(out, out_size, "cpu");
}

void format_task_core(const TaskStatus_t& task, char* out, const size_t out_size) {
  if (out == nullptr || out_size == 0U) {
    return;
  }
  snprintf(out, out_size, "-");
#if defined(configTASKLIST_INCLUDE_COREID) && (configTASKLIST_INCLUDE_COREID == 1)
  if (task.xCoreID == tskNO_AFFINITY) {
    snprintf(out, out_size, "any");
  } else {
    format_small_core_id(task.xCoreID, out, out_size);
  }
#elif defined(INCLUDE_xTaskGetCoreID) && (INCLUDE_xTaskGetCoreID == 1)
  const BaseType_t core_id = xTaskGetCoreID(task.xHandle);
  if (core_id == tskNO_AFFINITY) {
    snprintf(out, out_size, "any");
  } else {
    format_small_core_id(core_id, out, out_size);
  }
#endif
}

void print_rtos_task_snapshot() {
  constexpr UBaseType_t kMaxTasks = 48;
  TaskStatus_t* tasks = static_cast<TaskStatus_t*>(
      heap_caps_malloc(sizeof(TaskStatus_t) * kMaxTasks,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (tasks == nullptr) {
    mros_console.println("[RTOS] boot snapshot skipped: PSRAM task buffer unavailable");
    return;
  }
  uint32_t total_runtime = 0;
  const UBaseType_t task_count =
      uxTaskGetSystemState(tasks, kMaxTasks, &total_runtime);

  mros_console.println();
  mros_console.println("[RTOS] ps -l boot snapshot");
  mros_console.printf("%-5s %-4s %-5s %-4s %-7s %-6s %-5s %-5s %-10s %s\n",
                      "PID", "PRI", "CORE", "ST", "STK_HW", "CPU%",
                      "SLIP", "EXEC", "RUNTIME", "NAME");
  for (UBaseType_t i = 0; i < task_count; ++i) {
    char core_text[8] = "-";
    format_task_core(tasks[i], core_text, sizeof(core_text));
    const char* task_name = tasks[i].pcTaskName != nullptr ? tasks[i].pcTaskName
                                                           : "(task)";
    const uint32_t cpu_x100 =
        total_runtime > 0U
            ? static_cast<uint32_t>((static_cast<uint64_t>(tasks[i].ulRunTimeCounter) *
                                     10000ULL) /
                                    total_runtime)
            : 0U;
    MrosRtosTaskDiagSnapshot diag {};
    (void)app_rtos_get_task_diag_by_name(task_name, &diag);
    mros_console.printf("%-5lu %-4lu %-5s %-4s %-7lu %3lu.%02lu %-5lu %-5lu %-10lu %s\n",
                        static_cast<unsigned long>(tasks[i].xTaskNumber),
                        static_cast<unsigned long>(tasks[i].uxCurrentPriority),
                        core_text,
                        task_state_text(tasks[i].eCurrentState),
                        static_cast<unsigned long>(tasks[i].usStackHighWaterMark),
                        static_cast<unsigned long>(cpu_x100 / 100U),
                        static_cast<unsigned long>(cpu_x100 % 100U),
                        static_cast<unsigned long>(diag.last_slip_ms),
                        static_cast<unsigned long>(diag.last_exec_ms),
                        static_cast<unsigned long>(tasks[i].ulRunTimeCounter),
                        task_name);
  }

  mros_console.println("[RTOS] htop boot snapshot");
  mros_console.printf(
      "tasks=%lu internal_free=%lu internal_min=%lu psram_free=%lu psram_min=%lu\n",
      static_cast<unsigned long>(task_count),
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)));
  mros_console.println("[RTOS] end boot snapshot");
  heap_caps_free(tasks);
}

void boot_rtos_snapshot_task(void* /*arg*/) {
  vTaskDelay(pdMS_TO_TICKS(15000));
  print_rtos_task_snapshot();
  vTaskDelete(nullptr);
}

bool create_task(TaskFunction_t fn,
                 const char* name,
                 uint32_t stack_bytes,
                 UBaseType_t priority,
                 TaskHandle_t* handle,
                 BaseType_t core) {
  return xTaskCreatePinnedToCore(fn, name, stack_bytes, nullptr, priority, handle,
                                 core) == pdPASS;
}

void register_dpm_tasks() {
  using mros::rtos::dpm::register_task;
  register_task(MrosRtosTaskDiagId::WebRuntime, "web_runtime_task", true, false, "");
  register_task(MrosRtosTaskDiagId::WifiRuntime, "wifi_runtime_task", true, false, "");
  register_task(MrosRtosTaskDiagId::CommUartT41, "comm_uart_t41_task", true, false, "");
  register_task(MrosRtosTaskDiagId::Storage, "storage_task", true, false, "");
  register_task(MrosRtosTaskDiagId::FkPreview, "fk_preview_task", true, false, "");
  register_task(MrosRtosTaskDiagId::Shell, "mros_shell_task", true, false, "");
  register_task(MrosRtosTaskDiagId::Ssh, "mros_ssh_task", true, false, "");
  register_task(MrosRtosTaskDiagId::Mcp, "mros_mcp_task", true, false, "");
  register_task(MrosRtosTaskDiagId::CommSpiT41, "comm_spi_t41_task", true, false, "");
  register_task(MrosRtosTaskDiagId::CommSpiC3, "comm_spi_c3_task", true, false, "");
  register_task(MrosRtosTaskDiagId::JointTraj, "joint_traj_task", true, false, "");
  register_task(MrosRtosTaskDiagId::ExperimentalWorker, "experimental_worker_task", true, false, "");
  register_task(MrosRtosTaskDiagId::BearingHealth, "bearing_health_task", true, false, "SKF bearing life estimator");
  register_task(MrosRtosTaskDiagId::TurretPid, "turret_pid_task", false, true, "control safety loop");
  register_task(MrosRtosTaskDiagId::ServoDrive, "servo_drive_pca", false, true, "physical output queue");
}

void publish_dpm_task_handles() {
  using mros::rtos::dpm::set_task_handle;
  set_task_handle(MrosRtosTaskDiagId::WebRuntime, g_web_runtime_task);
  set_task_handle(MrosRtosTaskDiagId::WifiRuntime, g_wifi_runtime_task);
  set_task_handle(MrosRtosTaskDiagId::CommUartT41, g_comm_uart_t41_task);
  set_task_handle(MrosRtosTaskDiagId::Storage, g_storage_task);
  set_task_handle(MrosRtosTaskDiagId::FkPreview, g_fk_preview_task);
  set_task_handle(MrosRtosTaskDiagId::Shell, g_shell_task);
  set_task_handle(MrosRtosTaskDiagId::Ssh, g_ssh_task);
  set_task_handle(MrosRtosTaskDiagId::Mcp, g_mcp_task);
  set_task_handle(MrosRtosTaskDiagId::CommSpiT41, g_comm_spi_t41_task);
  set_task_handle(MrosRtosTaskDiagId::CommSpiC3, g_comm_spi_c3_task);
  set_task_handle(MrosRtosTaskDiagId::JointTraj, g_joint_traj_task);
  set_task_handle(MrosRtosTaskDiagId::ExperimentalWorker, g_experimental_worker_task);
  set_task_handle(MrosRtosTaskDiagId::BearingHealth, g_bearing_health_task);
  set_task_handle(MrosRtosTaskDiagId::TurretPid, g_turret_pid_task);
  set_task_handle(MrosRtosTaskDiagId::ServoDrive, g_servo_drive_task);
  set_task_handle(MrosRtosTaskDiagId::DeviceProcessManager, g_device_process_manager_task);
}

void start_fast_control_timer() {
  if (g_fast_control_timer != nullptr) {
    return;
  }

  const esp_timer_create_args_t timer_args = {
      .callback = &fast_control_tick_callback,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "mros_fast_tick",
      .skip_unhandled_events = true,
  };

  if (esp_timer_create(&timer_args, &g_fast_control_timer) == ESP_OK) {
    (void)esp_timer_start_periodic(g_fast_control_timer, kFastControlTickUs);
  } else {
    mros_console.println("[RTOS] Failed to create fast control timer");
  }
}

bool task_diag_counts_as_deadline(const MrosRtosTaskDiagId id) {
  switch (id) {
    case MrosRtosTaskDiagId::CommUartT41:
    case MrosRtosTaskDiagId::CommSpiT41:
    case MrosRtosTaskDiagId::CommSpiC3:
    case MrosRtosTaskDiagId::JointTraj:
    case MrosRtosTaskDiagId::TurretPid:
    case MrosRtosTaskDiagId::ServoDrive:
      return true;
    default:
      return false;
  }
}

}  // namespace

void app_rtos_record_task_cycle(const MrosRtosTaskDiagId id,
                                const uint32_t expected_period_ms,
                                const uint32_t actual_period_ms,
                                const uint32_t exec_ms) {
  const size_t index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(MrosRtosTaskDiagId::Count)) {
    return;
  }
  const bool deadline_sensitive = task_diag_counts_as_deadline(id);
  const uint32_t slip_ms =
      (deadline_sensitive && expected_period_ms > 0U && actual_period_ms > expected_period_ms)
          ? (actual_period_ms - expected_period_ms)
          : 0U;
  portENTER_CRITICAL(&g_task_diag_mux);
  TaskDiagSlot& slot = g_task_diag[index];
  slot.expected_period_ms = expected_period_ms;
  slot.last_period_ms = actual_period_ms;
  slot.last_exec_ms = exec_ms;
  slot.last_slip_ms = slip_ms;
  if (slip_ms > slot.max_slip_ms) {
    slot.max_slip_ms = slip_ms;
  }
  ++slot.wake_count;
  if (slip_ms > 0U) {
    ++slot.slip_count;
  }
  portEXIT_CRITICAL(&g_task_diag_mux);
  mros::rtos::dpm::record_cycle(id, expected_period_ms, actual_period_ms,
                                exec_ms, slip_ms);
}

bool app_rtos_get_task_diag_by_name(const char* task_name,
                                    MrosRtosTaskDiagSnapshot* snapshot) {
  if (task_name == nullptr || snapshot == nullptr) {
    return false;
  }
  for (size_t i = 0; i < static_cast<size_t>(MrosRtosTaskDiagId::Count); ++i) {
    if (!task_name_matches(task_name, g_task_diag[i].name)) {
      continue;
    }
    portENTER_CRITICAL(&g_task_diag_mux);
    const TaskDiagSlot slot = g_task_diag[i];
    portEXIT_CRITICAL(&g_task_diag_mux);
    snapshot->name = slot.name;
    snapshot->expected_period_ms = slot.expected_period_ms;
    snapshot->last_period_ms = slot.last_period_ms;
    snapshot->last_exec_ms = slot.last_exec_ms;
    snapshot->last_slip_ms = slot.last_slip_ms;
    snapshot->max_slip_ms = slot.max_slip_ms;
    snapshot->wake_count = slot.wake_count;
    snapshot->slip_count = slot.slip_count;
    return true;
  }
  return false;
}

void app_rtos_get_aggregate_diag(MrosRtosAggregateSnapshot* snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  *snapshot = MrosRtosAggregateSnapshot {};
  portENTER_CRITICAL(&g_task_diag_mux);
  for (size_t i = 0; i < static_cast<size_t>(MrosRtosTaskDiagId::Count); ++i) {
    const TaskDiagSlot& slot = g_task_diag[i];
    if (slot.name == nullptr) {
      continue;
    }
    ++snapshot->task_count;
    const uint32_t wake_room = 0xFFFFFFFFUL - snapshot->total_wake_count;
    snapshot->total_wake_count +=
        slot.wake_count > wake_room ? wake_room : slot.wake_count;
    const uint32_t slip_room = 0xFFFFFFFFUL - snapshot->total_slip_count;
    snapshot->total_slip_count +=
        slot.slip_count > slip_room ? slip_room : slot.slip_count;
    if (slot.max_slip_ms > snapshot->max_slip_ms) {
      snapshot->max_slip_ms = slot.max_slip_ms;
      snapshot->max_slip_task = slot.name;
    }
    if (slot.last_exec_ms > snapshot->max_exec_ms) {
      snapshot->max_exec_ms = slot.last_exec_ms;
      snapshot->max_exec_task = slot.name;
    }
  }
  portEXIT_CRITICAL(&g_task_diag_mux);
}

void app_init() {
  mros_console.begin();
  mros::platform::mros_delay_ms(50);

  event_bus_init();
  mros::shell::service::init();
  mros::ssh::service_init();
  mros::mcp::service_init();
  logger_init();

  (void)mros::platform::mros_gpio_config(PIN_DATA_READY,
                                         mros::platform::GpioMode::Output);
  (void)mros::platform::mros_gpio_write(PIN_DATA_READY, false);
  (void)mros::platform::mros_gpio_config(PIN_T41_READY,
                                         mros::platform::GpioMode::InputPullup);

  spi_c3_master_init();
  spi_slave_s3_init();
  pca9685_init();
  mros::experimental::worker_init();
  mros::health::bearing::init();
  mros::power::init();
  mros::rtos::dpm::init();
  register_dpm_tasks();

  // External PSRAM stacks are not safe in this firmware because multiple code
  // paths still disable flash cache (LittleFS/NVS/preferences/update flows).
  // When that happens ESP-IDF asserts if another core is running with an
  // external task stack, so we keep stacks internal and reclaim SRAM by
  // right-sizing them instead.
  if (!create_task(web_runtime_task, "web_runtime_task", kWebStackWords,
                   kWebPriority, &g_web_runtime_task, kNetCore)) {
    mros_console.println("[RTOS] Failed to create web_runtime_task");
  }
  if (!create_task(wifi_runtime_task, "wifi_runtime_task", kWifiStackWords,
                   kWifiPriority, &g_wifi_runtime_task, kNetCore)) {
    mros_console.println("[RTOS] Failed to create wifi_runtime_task");
  }
  if (!create_task(comm_uart_t41_task, "comm_uart_t41_task", kUartStackWords,
                   kUartPriority, &g_comm_uart_t41_task, kNetCore)) {
    mros_console.println("[RTOS] Failed to create comm_uart_t41_task");
  }
  if (!create_task(storage_task, "storage_task", kStorageStackWords,
                   kStoragePriority, &g_storage_task, kNetCore)) {
    mros_console.println("[RTOS] Failed to create storage_task");
  }
  if (!create_task(fk_preview_task, "fk_preview_task", kFkStackWords,
                   kFkPriority, &g_fk_preview_task, kNetCore)) {
    mros_console.println("[RTOS] Failed to create fk_preview_task");
  }
  // Keep the shell stack internal. Shell commands touch flash/LittleFS paths,
  // and ESP-IDF asserts if a task with an external PSRAM stack is active while
  // caches are disabled for flash operations.
  if (!create_task(shell_task, "mros_shell_task", kShellStackWords,
                   kShellPriority, &g_shell_task, kNetCore)) {
    mros_console.println("[RTOS] Failed to create mros_shell_task");
  }
  if (!create_task(ssh_task, "mros_ssh_task", kSshStackWords,
                   kSshPriority, &g_ssh_task, kNetCore)) {
    mros_console.println("[RTOS] Failed to create mros_ssh_task");
  }
  if (!create_task(mcp_task, "mros_mcp_task", kMcpStackWords,
                   kMcpPriority, &g_mcp_task, kNetCore)) {
    mros_console.println("[RTOS] Failed to create mros_mcp_task");
  }
  if (!create_task(comm_spi_t41_task, "comm_spi_t41_task", kSpiT41StackWords,
                   kSpiT41Priority, &g_comm_spi_t41_task, kRtCore)) {
    mros_console.println("[RTOS] Failed to create comm_spi_t41_task");
  }
  if (!create_task(comm_spi_c3_task, "comm_spi_c3_task", kSpiC3StackWords,
                   kSpiC3Priority, &g_comm_spi_c3_task, kRtCore)) {
    mros_console.println("[RTOS] Failed to create comm_spi_c3_task");
  }
  if (!create_task(joint_traj_task, "joint_traj_task", kJointStackWords,
                   kJointPriority, &g_joint_traj_task, kRtCore)) {
    mros_console.println("[RTOS] Failed to create joint_traj_task");
  }
  if (!create_task(experimental_worker_task, "experimental_worker_task",
                   kExperimentalStackWords, kExperimentalPriority,
                   &g_experimental_worker_task, kRtCore)) {
    mros_console.println("[RTOS] Failed to create experimental_worker_task");
  }
  if (!create_task(mros::health::bearing::task, "bearing_health_task",
                   kBearingHealthStackWords, kBearingHealthPriority,
                   &g_bearing_health_task, kNetCore)) {
    mros_console.println("[RTOS] Failed to create bearing_health_task");
  }
  if (!create_task(turret_pid_task, "turret_pid_task", kTurretPidStackWords,
                   kTurretPidPriority, &g_turret_pid_task, kRtCore)) {
    mros_console.println("[RTOS] Failed to create turret_pid_task");
  }
  if (!create_task(servo_drive_pca9685_task, "servo_drive_pca9685_task",
                   kServoStackWords, kServoPriority, &g_servo_drive_task,
                   kRtCore)) {
    mros_console.println("[RTOS] Failed to create servo_drive_pca9685_task");
  }
  if (!create_task(device_process_manager_task, "device_process_manager_task",
                   kDpmStackWords, kDpmPriority,
                   &g_device_process_manager_task, kNetCore)) {
    mros_console.println("[RTOS] Failed to create device_process_manager_task");
  }
  publish_dpm_task_handles();
  if (!create_task(boot_rtos_snapshot_task, "rtos_snapshot_task", 4096,
                   kJointPriority, &g_boot_rtos_snapshot_task, kRtCore)) {
    mros_console.println("[RTOS] Failed to create rtos_snapshot_task");
  }

  start_fast_control_timer();
  mros::update::update_runtime_mark_app_ready();
}

void app_loop() {
  mros::update::update_runtime_process();
  vTaskDelay(pdMS_TO_TICKS(1000));
}
