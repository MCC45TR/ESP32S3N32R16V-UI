#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mros::memory {

enum class FloorState : uint8_t {
  Ok = 0,
  Warn,
  Degraded,
  Critical,
};

struct Snapshot {
  char name[24] = {};
  uint32_t seq = 0;
  uint32_t ms = 0;
  uint32_t internal_total = 0;
  uint32_t internal_free = 0;
  uint32_t internal_min_free = 0;
  uint32_t internal_largest_block = 0;
  uint32_t internal_allocated_blocks = 0;
  uint32_t internal_free_blocks = 0;
  uint32_t psram_total = 0;
  uint32_t psram_free = 0;
  uint32_t psram_largest_block = 0;
  uint32_t psram_allocated_blocks = 0;
  uint32_t psram_free_blocks = 0;
  uint32_t fragmentation_pct = 0;
  FloorState floor_state = FloorState::Ok;
};

struct DropEvent {
  uint32_t seq = 0;
  uint32_t ms = 0;
  char scope[24] = {};
  uint32_t before_free = 0;
  uint32_t after_free = 0;
  uint32_t drop_bytes = 0;
  uint32_t largest_block = 0;
  FloorState floor_state = FloorState::Ok;
};

const char* floor_state_name(FloorState state);

void init();
void sample(const char* scope);
Snapshot capture(const char* name);
bool store_snapshot(const char* name, Snapshot* out = nullptr);
bool find_snapshot(const char* name, Snapshot* out);
size_t get_recent_snapshots(Snapshot* out, size_t cap);
size_t get_drop_events(DropEvent* out, size_t cap);
void reset();

bool watch_start(uint32_t interval_ms);
void watch_stop();
bool watch_enabled();
uint32_t watch_interval_ms();
void service(uint32_t now_ms);
bool heavy_diagnostic_allowed();

bool status_json(char* buffer, size_t capacity);
bool leaks_json(char* buffer, size_t capacity);
bool diff_json(const char* left, const char* right, char* buffer, size_t capacity);

}  // namespace mros::memory
