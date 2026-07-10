#pragma once

#include "WString.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace mros::ssh {

void service_init();
void service_set_task_handle(TaskHandle_t handle);
void service_notify();
void service_process();
uint32_t service_wait_timeout_ms();

bool service_enable();
bool service_disable();
bool service_set_port(uint16_t port);
String service_status_text();
String service_sessions_text();
String service_backend_text();

}  // namespace mros::ssh
