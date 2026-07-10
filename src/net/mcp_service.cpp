#include "mcp_service.h"

#include <esp_log.h>

#include "src/platform/mros_nvs.h"
#include "src/platform/mros_time.h"

namespace mros::mcp {
namespace {

using mros::platform::NvsNamespace;
using mros::platform::NvsPartitionMode;

constexpr const char* kTag = "MCP";
constexpr const char* kNamespace = "mcp_cfg";
constexpr uint32_t kActivePollMs = 5000U;

TaskHandle_t g_task_handle = nullptr;
ServiceConfig g_config {};
bool g_initialized = false;
uint32_t g_last_activity_ms = 0U;
uint32_t g_process_count = 0U;

bool open_nvs(NvsNamespace* pref, const bool read_only) {
  return pref != nullptr &&
         pref->open(kNamespace, read_only, NvsPartitionMode::UserPartitionsThenDefault);
}

void save_config() {
  NvsNamespace pref;
  if (!open_nvs(&pref, false)) {
    return;
  }
  pref.set_bool("enabled", g_config.enabled);
  pref.set_bool("allow_shell", g_config.allow_shell);
}

void load_config() {
  NvsNamespace pref;
  bool enabled = false;
  bool allow_shell = true;
  if (open_nvs(&pref, true)) {
    (void)pref.get_bool("enabled", &enabled);
    (void)pref.get_bool("allow_shell", &allow_shell);
  }
  g_config.enabled = enabled;
  g_config.allow_shell = allow_shell;
}

void ensure_init() {
  if (!g_initialized) {
    service_init();
  }
}

}  // namespace

void service_init() {
  if (g_initialized) {
    return;
  }
  load_config();
  g_initialized = true;
  save_config();
  ESP_LOGI(kTag, "MCP service initialized: %s", g_config.enabled ? "enabled" : "disabled");
}

void service_set_task_handle(const TaskHandle_t handle) { g_task_handle = handle; }

void service_notify() {
  if (g_task_handle != nullptr) {
    xTaskNotifyGive(g_task_handle);
  }
}

void service_process() {
  ensure_init();
  if (!g_config.enabled) {
    return;
  }
  ++g_process_count;
}

uint32_t service_wait_timeout_ms() {
  ensure_init();
  if (!g_config.enabled) {
    return 0U;
  }
  return kActivePollMs;
}

bool service_enable() { return service_set_enabled(true); }

bool service_disable() { return service_set_enabled(false); }

bool service_set_enabled(const bool enabled) {
  ensure_init();
  g_config.enabled = enabled;
  save_config();
  service_notify();
  return true;
}

bool service_set_allow_shell(const bool allow_shell) {
  ensure_init();
  g_config.allow_shell = allow_shell;
  save_config();
  service_notify();
  return true;
}

bool service_is_enabled() {
  ensure_init();
  return g_config.enabled;
}

bool service_allow_shell() {
  ensure_init();
  return g_config.allow_shell;
}

void service_mark_activity() {
  ensure_init();
  g_last_activity_ms = mros::platform::mros_millis();
  service_notify();
}

String service_status_text() {
  ensure_init();
  String out = g_config.enabled ? "enabled" : "disabled";
  out += " allow_shell=";
  out += g_config.allow_shell ? "yes" : "no";
  if (g_last_activity_ms > 0U) {
    out += " last_activity_ms=";
    out += String(static_cast<unsigned long>(g_last_activity_ms));
  }
  return out;
}

String service_status_json() {
  ensure_init();
  String json = "{";
  json += "\"enabled\":";
  json += g_config.enabled ? "true" : "false";
  json += ",\"allow_shell\":";
  json += g_config.allow_shell ? "true" : "false";
  json += ",\"wait_ms\":";
  json += String(static_cast<unsigned long>(service_wait_timeout_ms()));
  json += ",\"last_activity_ms\":";
  json += String(static_cast<unsigned long>(g_last_activity_ms));
  json += ",\"process_count\":";
  json += String(static_cast<unsigned long>(g_process_count));
  json += ",\"http\":\"/mcp\",\"websocket\":\"/ws/mcp\"";
  json += "}";
  return json;
}

}  // namespace mros::mcp
