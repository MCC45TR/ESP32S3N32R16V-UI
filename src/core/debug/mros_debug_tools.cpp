#include "src/core/debug/mros_debug_tools.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>

#include "src/utils/mros_json_writer.h"

namespace mros::debug {
namespace {

constexpr uint32_t kProbeBytes = 256U;
constexpr const char* kCoredumpPartitionLabel = "coredump";

bool is_coredump_partition(const esp_partition_t* part) {
  return part != nullptr &&
         part->type == ESP_PARTITION_TYPE_DATA &&
         part->subtype == ESP_PARTITION_SUBTYPE_DATA_COREDUMP &&
         std::strcmp(part->label, kCoredumpPartitionLabel) == 0;
}

bool probe_coredump(uint32_t* out_used_hint, uint32_t* out_first_word) {
  if (out_used_hint != nullptr) *out_used_hint = 0;
  if (out_first_word != nullptr) *out_first_word = 0xFFFFFFFFUL;
  const esp_partition_t* part = coredump_partition_handle();
  if (part == nullptr || part->size == 0U) return false;
  uint8_t probe[kProbeBytes] = {};
  const size_t n = part->size < sizeof(probe) ? part->size : sizeof(probe);
  if (esp_partition_read(part, 0, probe, n) != ESP_OK) return false;
  uint32_t used = 0;
  for (size_t i = 0; i < n; ++i) {
    if (probe[i] != 0xFFU && probe[i] != 0x00U) ++used;
  }
  uint32_t first = 0xFFFFFFFFUL;
  if (n >= sizeof(first)) {
    std::memcpy(&first, probe, sizeof(first));
  }
  if (out_used_hint != nullptr) *out_used_hint = used;
  if (out_first_word != nullptr) *out_first_word = first;
  return used > 0U && first != 0xFFFFFFFFUL && first != 0x00000000UL;
}

struct HeapTraceState {
  bool active = false;
  uint32_t start_count = 0;
  uint32_t stop_count = 0;
  uint32_t clear_count = 0;
  uint32_t last_error = ESP_ERR_NOT_SUPPORTED;
};

portMUX_TYPE g_heap_mux = portMUX_INITIALIZER_UNLOCKED;
HeapTraceState g_heap_trace {};

}  // namespace

const esp_partition_t* coredump_partition_handle() {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
      kCoredumpPartitionLabel);
  return is_coredump_partition(part) ? part : nullptr;
}

bool coredump_present() {
  return probe_coredump(nullptr, nullptr);
}

size_t coredump_partition_size() {
  const esp_partition_t* part = coredump_partition_handle();
  return part != nullptr ? part->size : 0U;
}

bool coredump_json(char* buffer, const size_t capacity) {
  const esp_partition_t* part = coredump_partition_handle();
  uint32_t used_hint = 0;
  uint32_t first_word = 0xFFFFFFFFUL;
  const bool present = probe_coredump(&used_hint, &first_word);
  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", part != nullptr);
  writer.bool_field("present", present);
  writer.string_field("label", part != nullptr ? part->label : "");
  writer.u32_field("address", part != nullptr ? part->address : 0U);
  writer.u32_field("size", part != nullptr ? part->size : 0U);
  writer.u32_field("probe_used_bytes", used_hint);
  writer.u32_field("first_word", first_word);
  writer.i32_field("reset_reason", static_cast<int32_t>(esp_reset_reason()));
  writer.string_field("download", "/api/debug/coredump/download");
  writer.end();
  return !writer.overflow();
}

esp_err_t coredump_clear() {
  const esp_partition_t* part = coredump_partition_handle();
  if (part == nullptr) return ESP_ERR_NOT_FOUND;
  return esp_partition_erase_range(part, 0, part->size);
}

bool heap_trace_json(char* buffer, const size_t capacity) {
  HeapTraceState snap {};
  portENTER_CRITICAL(&g_heap_mux);
  snap = g_heap_trace;
  portEXIT_CRITICAL(&g_heap_mux);

  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", true);
  writer.bool_field("enabled", false);
  writer.bool_field("active", snap.active);
  writer.string_field("state", snap.active ? "active" : "disabled");
  writer.string_field("mode", "release-disabled");
  writer.string_field("error", "FEATURE_DISABLED");
  writer.u32_field("start_count", snap.start_count);
  writer.u32_field("stop_count", snap.stop_count);
  writer.u32_field("clear_count", snap.clear_count);
  writer.u32_field("last_error", snap.last_error);
  writer.u32_field("internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  writer.u32_field("internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  writer.u32_field("psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  writer.u32_field("psram_largest", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  writer.end();
  return !writer.overflow();
}

esp_err_t heap_trace_start() {
  portENTER_CRITICAL(&g_heap_mux);
  g_heap_trace.active = false;
  g_heap_trace.start_count++;
  g_heap_trace.last_error = ESP_ERR_NOT_SUPPORTED;
  portEXIT_CRITICAL(&g_heap_mux);
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t heap_trace_stop() {
  portENTER_CRITICAL(&g_heap_mux);
  g_heap_trace.active = false;
  g_heap_trace.stop_count++;
  g_heap_trace.last_error = ESP_ERR_NOT_SUPPORTED;
  portEXIT_CRITICAL(&g_heap_mux);
  return ESP_ERR_NOT_SUPPORTED;
}

void heap_trace_clear() {
  portENTER_CRITICAL(&g_heap_mux);
  g_heap_trace.active = false;
  g_heap_trace.clear_count++;
  g_heap_trace.last_error = ESP_OK;
  portEXIT_CRITICAL(&g_heap_mux);
}

}  // namespace mros::debug
