#pragma once

#include "WString.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace mros::mcp {

struct ServiceConfig {
  bool enabled = false;
  bool allow_shell = true;
};

void service_init();
void service_set_task_handle(TaskHandle_t handle);
void service_notify();
void service_process();
uint32_t service_wait_timeout_ms();

bool service_enable();
bool service_disable();
bool service_set_enabled(bool enabled);
bool service_set_allow_shell(bool allow_shell);
bool service_is_enabled();
bool service_allow_shell();
void service_mark_activity();
String service_status_text();
String service_status_json();

}  // namespace mros::mcp
