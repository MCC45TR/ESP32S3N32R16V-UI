#include "src/core/memory/memory_monitor.h"

#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "src/platform/mros_time.h"
#include "src/utils/mros_json_writer.h"

namespace mros::memory {
namespace {

constexpr size_t kSnapshotCap = 8;
constexpr size_t kDropCap = 16;
constexpr uint32_t kWarnBytes = 100U * 1024U;
constexpr uint32_t kDegradedBytes = 80U * 1024U;
constexpr uint32_t kCriticalBytes = 64U * 1024U;
constexpr uint32_t kDropRecordThresholdBytes = 4U * 1024U;
constexpr uint32_t kDefaultWatchIntervalMs = 5000U;
constexpr uint32_t kMinWatchIntervalMs = 1000U;
constexpr uint32_t kMaxWatchIntervalMs = 60000U;

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
Snapshot g_snapshots[kSnapshotCap] = {};
DropEvent g_drops[kDropCap] = {};
Snapshot g_last_sample = {};
uint32_t g_snapshot_write = 0;
uint32_t g_snapshot_count = 0;
uint32_t g_drop_write = 0;
uint32_t g_drop_count = 0;
uint32_t g_seq = 0;
uint32_t g_drop_seq = 0;
bool g_has_last_sample = false;
bool g_watch_enabled = false;
uint32_t g_watch_interval_ms = kDefaultWatchIntervalMs;
uint32_t g_last_watch_ms = 0;

void copy_small(char* dst, const size_t cap, const char* src) {
  if (dst == nullptr || cap == 0U) return;
  const char* text = src != nullptr ? src : "";
  std::strncpy(dst, text, cap - 1U);
  dst[cap - 1U] = '\0';
}

uint32_t saturating_delta(const uint32_t before, const uint32_t after) {
  return before > after ? before - after : 0U;
}

FloorState classify_floor(const uint32_t internal_free) {
  if (internal_free < kCriticalBytes) return FloorState::Critical;
  if (internal_free < kDegradedBytes) return FloorState::Degraded;
  if (internal_free < kWarnBytes) return FloorState::Warn;
  return FloorState::Ok;
}

uint32_t fragmentation_pct_from(const uint32_t free_bytes,
                                const uint32_t largest_block) {
  if (free_bytes == 0U || largest_block >= free_bytes) return 0U;
  const uint32_t fragmented = free_bytes - largest_block;
  const uint64_t pct = (static_cast<uint64_t>(fragmented) * 100ULL) /
                       static_cast<uint64_t>(free_bytes);
  return pct > 100ULL ? 100U : static_cast<uint32_t>(pct);
}

uint32_t next_seq() {
  portENTER_CRITICAL(&g_mux);
  const uint32_t seq = ++g_seq;
  portEXIT_CRITICAL(&g_mux);
  return seq;
}

Snapshot capture_raw(const char* name) {
  Snapshot out {};
  copy_small(out.name, sizeof(out.name), name != nullptr ? name : "sample");
  out.seq = next_seq();
  out.ms = platform::mros_millis();

  multi_heap_info_t internal {};
  multi_heap_info_t psram {};
  heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM);

  out.internal_total = static_cast<uint32_t>(heap_caps_get_total_size(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  out.internal_free = static_cast<uint32_t>(internal.total_free_bytes);
  out.internal_min_free = static_cast<uint32_t>(internal.minimum_free_bytes);
  out.internal_largest_block = static_cast<uint32_t>(internal.largest_free_block);
  out.internal_allocated_blocks =
      static_cast<uint32_t>(internal.allocated_blocks);
  out.internal_free_blocks = static_cast<uint32_t>(internal.free_blocks);
  out.psram_total = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
  out.psram_free = static_cast<uint32_t>(psram.total_free_bytes);
  out.psram_largest_block = static_cast<uint32_t>(psram.largest_free_block);
  out.psram_allocated_blocks = static_cast<uint32_t>(psram.allocated_blocks);
  out.psram_free_blocks = static_cast<uint32_t>(psram.free_blocks);
  out.fragmentation_pct =
      fragmentation_pct_from(out.internal_free, out.internal_largest_block);
  out.floor_state = classify_floor(out.internal_free);
  return out;
}

void record_drop_unlocked(const char* scope,
                          const Snapshot& before,
                          const Snapshot& after) {
  const uint32_t drop = saturating_delta(before.internal_free, after.internal_free);
  if (drop < kDropRecordThresholdBytes && after.floor_state == FloorState::Ok) {
    return;
  }
  DropEvent& ev = g_drops[g_drop_write % kDropCap];
  ev.seq = ++g_drop_seq;
  ev.ms = after.ms;
  copy_small(ev.scope, sizeof(ev.scope), scope != nullptr ? scope : after.name);
  ev.before_free = before.internal_free;
  ev.after_free = after.internal_free;
  ev.drop_bytes = drop;
  ev.largest_block = after.internal_largest_block;
  ev.floor_state = after.floor_state;
  ++g_drop_write;
  if (g_drop_count < kDropCap) ++g_drop_count;
}

void append_snapshot_json(utils::FixedJsonWriter& writer, const Snapshot& s) {
  writer.append_raw("{\"name\":\"");
  writer.append_escaped(s.name);
  writer.append_raw("\",\"seq\":");
  writer.u32(s.seq);
  writer.append_raw(",\"ms\":");
  writer.u32(s.ms);
  writer.append_raw(",\"internal_free\":");
  writer.u32(s.internal_free);
  writer.append_raw(",\"internal_min_free\":");
  writer.u32(s.internal_min_free);
  writer.append_raw(",\"internal_largest_block\":");
  writer.u32(s.internal_largest_block);
  writer.append_raw(",\"internal_allocated_blocks\":");
  writer.u32(s.internal_allocated_blocks);
  writer.append_raw(",\"internal_free_blocks\":");
  writer.u32(s.internal_free_blocks);
  writer.append_raw(",\"psram_free\":");
  writer.u32(s.psram_free);
  writer.append_raw(",\"psram_largest_block\":");
  writer.u32(s.psram_largest_block);
  writer.append_raw(",\"fragmentation_pct\":");
  writer.u32(s.fragmentation_pct);
  writer.append_raw(",\"sram_floor_state\":\"");
  writer.append_escaped(floor_state_name(s.floor_state));
  writer.append_raw("\"}");
}

}  // namespace

const char* floor_state_name(const FloorState state) {
  switch (state) {
    case FloorState::Ok:
      return "OK";
    case FloorState::Warn:
      return "WARN";
    case FloorState::Degraded:
      return "DEGRADED";
    case FloorState::Critical:
      return "CRITICAL";
    default:
      return "UNKNOWN";
  }
}

void init() {
  const Snapshot boot = capture_raw("boot");
  portENTER_CRITICAL(&g_mux);
  if (!g_has_last_sample) {
    g_last_sample = boot;
    g_has_last_sample = true;
    g_snapshots[g_snapshot_write % kSnapshotCap] = g_last_sample;
    ++g_snapshot_write;
    g_snapshot_count = 1;
  }
  portEXIT_CRITICAL(&g_mux);
}

void sample(const char* scope) {
  const Snapshot now = capture_raw(scope);
  portENTER_CRITICAL(&g_mux);
  if (g_has_last_sample) {
    record_drop_unlocked(scope, g_last_sample, now);
  }
  g_last_sample = now;
  g_has_last_sample = true;
  portEXIT_CRITICAL(&g_mux);
}

Snapshot capture(const char* name) {
  return capture_raw(name);
}

bool store_snapshot(const char* name, Snapshot* out) {
  Snapshot snap = capture_raw(name != nullptr ? name : "snapshot");
  portENTER_CRITICAL(&g_mux);
  if (g_has_last_sample) {
    record_drop_unlocked(name, g_last_sample, snap);
  }
  g_last_sample = snap;
  g_has_last_sample = true;
  g_snapshots[g_snapshot_write % kSnapshotCap] = snap;
  ++g_snapshot_write;
  if (g_snapshot_count < kSnapshotCap) ++g_snapshot_count;
  portEXIT_CRITICAL(&g_mux);
  if (out != nullptr) *out = snap;
  return true;
}

bool find_snapshot(const char* name, Snapshot* out) {
  if (name == nullptr || name[0] == '\0' || out == nullptr) return false;
  bool found = false;
  portENTER_CRITICAL(&g_mux);
  const uint32_t count = std::min<uint32_t>(g_snapshot_count, kSnapshotCap);
  const uint32_t start = g_snapshot_write > count ? g_snapshot_write - count : 0U;
  for (uint32_t i = 0; i < count; ++i) {
    const Snapshot& snap = g_snapshots[(start + i) % kSnapshotCap];
    if (std::strcmp(snap.name, name) == 0) {
      *out = snap;
      found = true;
    }
  }
  portEXIT_CRITICAL(&g_mux);
  return found;
}

size_t get_recent_snapshots(Snapshot* out, const size_t cap) {
  if (out == nullptr || cap == 0U) return 0;
  size_t copied = 0;
  portENTER_CRITICAL(&g_mux);
  const uint32_t count = std::min<uint32_t>(g_snapshot_count, kSnapshotCap);
  const uint32_t start = g_snapshot_write > count ? g_snapshot_write - count : 0U;
  for (uint32_t i = 0; i < count && copied < cap; ++i) {
    out[copied++] = g_snapshots[(start + i) % kSnapshotCap];
  }
  portEXIT_CRITICAL(&g_mux);
  return copied;
}

size_t get_drop_events(DropEvent* out, const size_t cap) {
  if (out == nullptr || cap == 0U) return 0;
  size_t copied = 0;
  portENTER_CRITICAL(&g_mux);
  const uint32_t count = std::min<uint32_t>(g_drop_count, kDropCap);
  const uint32_t start = g_drop_write > count ? g_drop_write - count : 0U;
  for (uint32_t i = 0; i < count && copied < cap; ++i) {
    out[copied++] = g_drops[(start + i) % kDropCap];
  }
  portEXIT_CRITICAL(&g_mux);
  return copied;
}

void reset() {
  portENTER_CRITICAL(&g_mux);
  g_snapshot_write = 0;
  g_snapshot_count = 0;
  g_drop_write = 0;
  g_drop_count = 0;
  g_drop_seq = 0;
  g_has_last_sample = false;
  portEXIT_CRITICAL(&g_mux);
  init();
}

bool watch_start(const uint32_t interval_ms) {
  const uint32_t clamped =
      std::min<uint32_t>(std::max<uint32_t>(interval_ms, kMinWatchIntervalMs),
                         kMaxWatchIntervalMs);
  portENTER_CRITICAL(&g_mux);
  g_watch_interval_ms = clamped;
  g_watch_enabled = true;
  g_last_watch_ms = 0;
  portEXIT_CRITICAL(&g_mux);
  sample("watch-start");
  return true;
}

void watch_stop() {
  portENTER_CRITICAL(&g_mux);
  g_watch_enabled = false;
  portEXIT_CRITICAL(&g_mux);
}

bool watch_enabled() {
  portENTER_CRITICAL(&g_mux);
  const bool out = g_watch_enabled;
  portEXIT_CRITICAL(&g_mux);
  return out;
}

uint32_t watch_interval_ms() {
  portENTER_CRITICAL(&g_mux);
  const uint32_t out = g_watch_interval_ms;
  portEXIT_CRITICAL(&g_mux);
  return out;
}

void service(const uint32_t now_ms) {
  bool due = false;
  uint32_t interval = kDefaultWatchIntervalMs;
  portENTER_CRITICAL(&g_mux);
  interval = g_watch_interval_ms;
  if (g_watch_enabled &&
      (g_last_watch_ms == 0U || now_ms - g_last_watch_ms >= interval)) {
    g_last_watch_ms = now_ms;
    due = true;
  }
  portEXIT_CRITICAL(&g_mux);
  if (due) {
    sample("watch");
  }
}

bool heavy_diagnostic_allowed() {
  const Snapshot snap = capture("guard");
  return snap.floor_state != FloorState::Critical;
}

bool status_json(char* buffer, const size_t capacity) {
  Snapshot snap = capture("status");
  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", true);
  writer.raw_field("snapshot", "");
  writer.append_raw("{\"name\":\"");
  writer.append_escaped(snap.name);
  writer.append_raw("\",\"seq\":");
  writer.u32(snap.seq);
  writer.append_raw(",\"ms\":");
  writer.u32(snap.ms);
  writer.append_raw(",\"internal_total\":");
  writer.u32(snap.internal_total);
  writer.append_raw(",\"internal_free\":");
  writer.u32(snap.internal_free);
  writer.append_raw(",\"internal_min_free\":");
  writer.u32(snap.internal_min_free);
  writer.append_raw(",\"internal_largest_block\":");
  writer.u32(snap.internal_largest_block);
  writer.append_raw(",\"internal_allocated_blocks\":");
  writer.u32(snap.internal_allocated_blocks);
  writer.append_raw(",\"internal_free_blocks\":");
  writer.u32(snap.internal_free_blocks);
  writer.append_raw(",\"psram_total\":");
  writer.u32(snap.psram_total);
  writer.append_raw(",\"psram_free\":");
  writer.u32(snap.psram_free);
  writer.append_raw(",\"psram_largest_block\":");
  writer.u32(snap.psram_largest_block);
  writer.append_raw(",\"psram_allocated_blocks\":");
  writer.u32(snap.psram_allocated_blocks);
  writer.append_raw(",\"psram_free_blocks\":");
  writer.u32(snap.psram_free_blocks);
  writer.append_raw(",\"fragmentation_pct\":");
  writer.u32(snap.fragmentation_pct);
  writer.append_raw(",\"sram_floor_state\":\"");
  writer.append_escaped(floor_state_name(snap.floor_state));
  writer.append_raw("\"}");
  writer.bool_field("watch_enabled", watch_enabled());
  writer.u32_field("watch_interval_ms", watch_interval_ms());
  writer.u32_field("warn_threshold", kWarnBytes);
  writer.u32_field("degraded_threshold", kDegradedBytes);
  writer.u32_field("critical_threshold", kCriticalBytes);
  writer.end();
  return !writer.overflow();
}

bool leaks_json(char* buffer, const size_t capacity) {
  DropEvent drops[kDropCap] = {};
  const size_t count = get_drop_events(drops, kDropCap);
  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", true);
  writer.u32_field("count", static_cast<uint32_t>(count));
  writer.raw_field("drops", "[");
  for (size_t i = 0; i < count; ++i) {
    if (i > 0U) writer.append_raw(",");
    writer.append_raw("{\"seq\":");
    writer.u32(drops[i].seq);
    writer.append_raw(",\"ms\":");
    writer.u32(drops[i].ms);
    writer.append_raw(",\"scope\":\"");
    writer.append_escaped(drops[i].scope);
    writer.append_raw("\",\"before_free\":");
    writer.u32(drops[i].before_free);
    writer.append_raw(",\"after_free\":");
    writer.u32(drops[i].after_free);
    writer.append_raw(",\"drop_bytes\":");
    writer.u32(drops[i].drop_bytes);
    writer.append_raw(",\"largest_block\":");
    writer.u32(drops[i].largest_block);
    writer.append_raw(",\"sram_floor_state\":\"");
    writer.append_escaped(floor_state_name(drops[i].floor_state));
    writer.append_raw("\"}");
  }
  writer.append_raw("]");
  writer.end();
  return !writer.overflow();
}

bool diff_json(const char* left,
               const char* right,
               char* buffer,
               const size_t capacity) {
  Snapshot a {};
  Snapshot b {};
  bool found_a = false;
  bool found_b = false;
  if (left != nullptr && left[0] != '\0') found_a = find_snapshot(left, &a);
  if (right != nullptr && right[0] != '\0') found_b = find_snapshot(right, &b);

  if (!found_a || !found_b) {
    Snapshot snaps[kSnapshotCap] = {};
    const size_t count = get_recent_snapshots(snaps, kSnapshotCap);
    if (!found_a && count >= 2U) {
      a = snaps[count - 2U];
      found_a = true;
    }
    if (!found_b && count >= 1U) {
      b = snaps[count - 1U];
      found_b = true;
    }
  }

  utils::FixedJsonWriter writer(buffer, capacity);
  writer.begin();
  writer.bool_field("ok", found_a && found_b);
  if (!found_a || !found_b) {
    writer.string_field("error", "snapshot_not_found");
    writer.end();
    return false;
  }
  writer.raw_field("left", "");
  append_snapshot_json(writer, a);
  writer.raw_field("right", "");
  append_snapshot_json(writer, b);
  writer.i32_field("internal_free_delta",
                   static_cast<int32_t>(b.internal_free) -
                       static_cast<int32_t>(a.internal_free));
  writer.i32_field("largest_block_delta",
                   static_cast<int32_t>(b.internal_largest_block) -
                       static_cast<int32_t>(a.internal_largest_block));
  writer.i32_field("psram_free_delta",
                   static_cast<int32_t>(b.psram_free) -
                       static_cast<int32_t>(a.psram_free));
  writer.end();
  return !writer.overflow();
}

}  // namespace mros::memory
