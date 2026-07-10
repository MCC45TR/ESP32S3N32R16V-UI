#include "trajectory_handler.h"
#include "src/platform/mros_system.h"
#include <cmath>
#include <ctype.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

TrajectoryPoint *g_points = nullptr;
size_t g_capacity = 0;
size_t g_count = 0;
bool g_init_done = false;

char *g_stream_buf = nullptr;
size_t g_stream_capacity = 0;
size_t g_stream_size = 0;
bool g_stream_active = false;

char g_last_error[96] = {0};

constexpr size_t kDefaultCapacity = 8192;
constexpr size_t kInternalFallbackCapacity = 1024;
constexpr size_t kMaxPayloadBytes = 1024 * 1024;
constexpr size_t kMaxPointCapacity = 65536;
constexpr size_t kLargeTolerantPsramOnlyThreshold = 4096;
#ifndef MROS_USE_STATIC_PSRAM_TRAJECTORY_HANDLER
#define MROS_USE_STATIC_PSRAM_TRAJECTORY_HANDLER 0
#endif
#if MROS_USE_STATIC_PSRAM_TRAJECTORY_HANDLER && \
    defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
EXT_RAM_BSS_ATTR TrajectoryPoint g_points_storage[kDefaultCapacity];
#endif

static bool points_use_static_storage() {
#if MROS_USE_STATIC_PSRAM_TRAJECTORY_HANDLER && \
    defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
  return g_points == g_points_storage;
#else
  return false;
#endif
}

static void set_last_error(const char *msg) {
  if (msg == nullptr || msg[0] == '\0') {
    g_last_error[0] = '\0';
    return;
  }
  strncpy(g_last_error, msg, sizeof(g_last_error) - 1);
  g_last_error[sizeof(g_last_error) - 1] = '\0';
}

static void *alloc_tolerant_buffer(size_t bytes, bool *is_psram = nullptr) {
  if (is_psram) *is_psram = false;
  if (bytes == 0) return nullptr;

  const bool has_psram = mros::platform::mros_system_psram_total() > 0U;
  if (has_psram) {
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) {
      if (is_psram) *is_psram = true;
      return ptr;
    }
    // If PSRAM exists, avoid moving large tolerant buffers into internal SRAM.
    if (bytes > kLargeTolerantPsramOnlyThreshold) {
      return nullptr;
    }
  }

  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static bool ensure_points_buffer() {
  if (g_init_done) return g_points != nullptr;
  g_init_done = true;

#if MROS_USE_STATIC_PSRAM_TRAJECTORY_HANDLER && \
    defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
  g_points = g_points_storage;
  g_capacity = kDefaultCapacity;
  g_count = 0;
  return true;
#else
  size_t selected_capacity = kDefaultCapacity;
  g_points = static_cast<TrajectoryPoint *>(
      alloc_tolerant_buffer(sizeof(TrajectoryPoint) * selected_capacity,
                            nullptr));
  if (!g_points) {
    selected_capacity = kInternalFallbackCapacity;
    g_points = static_cast<TrajectoryPoint *>(
        heap_caps_malloc(sizeof(TrajectoryPoint) * selected_capacity,
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (!g_points) {
    set_last_error("point_buffer_alloc_failed");
    return false;
  }
  g_capacity = selected_capacity;
  g_count = 0;
  return true;
#endif
}

static bool ensure_points_capacity(size_t needed) {
  if (!ensure_points_buffer()) return false;
  if (needed <= g_capacity) return true;
  if (needed > kMaxPointCapacity) {
    set_last_error("point_capacity_limit");
    return false;
  }

  size_t target = g_capacity;
  while (target < needed && target < kMaxPointCapacity) {
    const size_t doubled = target * 2;
    if (doubled <= target) break;
    target = (doubled > kMaxPointCapacity) ? kMaxPointCapacity : doubled;
  }
  if (target < needed) target = needed;
  if (target > kMaxPointCapacity) target = kMaxPointCapacity;
  if (target < needed) {
    set_last_error("point_capacity_limit");
    return false;
  }

  TrajectoryPoint *next = static_cast<TrajectoryPoint *>(
      alloc_tolerant_buffer(sizeof(TrajectoryPoint) * target));
  if (!next) {
    set_last_error("point_buffer_grow_failed");
    return false;
  }
  if (g_count > 0 && g_points) {
    memcpy(next, g_points, sizeof(TrajectoryPoint) * g_count);
  }
  if (g_points && !points_use_static_storage()) {
    heap_caps_free(g_points);
  }
  g_points = next;
  g_capacity = target;
  return true;
}

static inline float parse_float_or(const char *tok, float fallback) {
  if (!tok) return fallback;
  char *end = nullptr;
  float v = strtof(tok, &end);
  if (end == tok || !std::isfinite(v)) return fallback;
  return v;
}

static inline void skip_ws(const char *&p, const char *end) {
  while (p < end && isspace(static_cast<unsigned char>(*p))) p++;
}

static bool parse_json_string_token(const char *&p, const char *end,
                                    const char **out_start,
                                    size_t *out_len) {
  skip_ws(p, end);
  if (p >= end || *p != '"') return false;
  p++;  // skip opening quote
  const char *start = p;
  while (p < end) {
    if (*p == '\\') {
      p++;
      if (p < end) p++;
      continue;
    }
    if (*p == '"') {
      if (out_start) *out_start = start;
      if (out_len) *out_len = static_cast<size_t>(p - start);
      p++;  // skip closing quote
      return true;
    }
    p++;
  }
  return false;
}

static bool key_equals(const char *start, size_t len, const char *lit) {
  const size_t lit_len = strlen(lit);
  if (len != lit_len) return false;
  return strncmp(start, lit, len) == 0;
}

static bool parse_json_number(const char *&p, const char *end, float *out) {
  skip_ws(p, end);
  if (p >= end || out == nullptr) return false;

  const char *start = p;
  if (*p == '-' || *p == '+') p++;
  bool has_digits = false;
  while (p < end && isdigit(static_cast<unsigned char>(*p))) {
    has_digits = true;
    p++;
  }
  if (p < end && *p == '.') {
    p++;
    while (p < end && isdigit(static_cast<unsigned char>(*p))) {
      has_digits = true;
      p++;
    }
  }
  if (!has_digits) {
    p = start;
    return false;
  }
  if (p < end && (*p == 'e' || *p == 'E')) {
    const char *exp_start = p;
    p++;
    if (p < end && (*p == '+' || *p == '-')) p++;
    bool exp_digits = false;
    while (p < end && isdigit(static_cast<unsigned char>(*p))) {
      exp_digits = true;
      p++;
    }
    if (!exp_digits) {
      p = exp_start;
    }
  }

  const size_t token_len = static_cast<size_t>(p - start);
  if (token_len == 0 || token_len >= 64) return false;
  char tmp[64];
  memcpy(tmp, start, token_len);
  tmp[token_len] = '\0';
  char *num_end = nullptr;
  float v = strtof(tmp, &num_end);
  if (num_end == tmp || !std::isfinite(v)) return false;
  *out = v;
  return true;
}

static bool consume_json_literal(const char *&p, const char *end,
                                 const char *lit) {
  skip_ws(p, end);
  const size_t lit_len = strlen(lit);
  if (static_cast<size_t>(end - p) < lit_len) return false;
  if (strncmp(p, lit, lit_len) != 0) return false;
  p += lit_len;
  return true;
}

static bool skip_json_composite(const char *&p, const char *end, char open_ch,
                                char close_ch) {
  if (p >= end || *p != open_ch) return false;
  int depth = 0;
  while (p < end) {
    char c = *p++;
    if (c == '"') {
      // Skip string contents inside objects/arrays.
      while (p < end) {
        char sc = *p++;
        if (sc == '\\') {
          if (p < end) p++;
          continue;
        }
        if (sc == '"') break;
      }
      continue;
    }
    if (c == open_ch) depth++;
    if (c == close_ch) {
      depth--;
      if (depth == 0) return true;
    }
  }
  return false;
}

static bool skip_json_value(const char *&p, const char *end) {
  skip_ws(p, end);
  if (p >= end) return false;

  if (*p == '"') {
    return parse_json_string_token(p, end, nullptr, nullptr);
  }
  if (*p == '{') {
    return skip_json_composite(p, end, '{', '}');
  }
  if (*p == '[') {
    return skip_json_composite(p, end, '[', ']');
  }
  if (*p == 't') return consume_json_literal(p, end, "true");
  if (*p == 'f') return consume_json_literal(p, end, "false");
  if (*p == 'n') return consume_json_literal(p, end, "null");

  float dummy = 0.0f;
  return parse_json_number(p, end, &dummy);
}

static bool parse_json_bool_or_number(const char *&p, const char *end,
                                      bool *out_bool) {
  if (out_bool == nullptr) return false;
  skip_ws(p, end);
  if (p >= end) return false;

  if (*p == 't') {
    if (!consume_json_literal(p, end, "true")) return false;
    *out_bool = true;
    return true;
  }
  if (*p == 'f') {
    if (!consume_json_literal(p, end, "false")) return false;
    *out_bool = false;
    return true;
  }
  if (*p == '"') {
    const char *s = nullptr;
    size_t slen = 0;
    if (!parse_json_string_token(p, end, &s, &slen)) return false;
    if (slen == 0) return false;
    char c0 = static_cast<char>(tolower(static_cast<unsigned char>(s[0])));
    *out_bool = (c0 == 'a' || c0 == 't' || c0 == '1' || c0 == 'y');
    return true;
  }

  float num = 0.0f;
  if (!parse_json_number(p, end, &num)) return false;
  *out_bool = num >= 0.5f;
  return true;
}

static bool append_point(float x, float y, float z, float t_ms, bool ee_auto,
                         float roll_deg, float ee_pitch_deg, float yaw_deg) {
  if (!ensure_points_capacity(g_count + 1) || g_points == nullptr) {
    return false;
  }
  TrajectoryPoint &pt = g_points[g_count++];
  pt.x = x;
  pt.y = y;
  pt.z = z;
  pt.t_ms = (std::isfinite(t_ms) && t_ms > 0.0f) ? t_ms : 80.0f;
  pt.ee_auto = ee_auto;
  pt.roll_deg = std::isfinite(roll_deg) ? roll_deg : 0.0f;
  pt.ee_pitch_deg = std::isfinite(ee_pitch_deg) ? ee_pitch_deg : 0.0f;
  pt.yaw_deg = std::isfinite(yaw_deg) ? yaw_deg : 0.0f;
  return true;
}

static bool parse_json_point_array(const char *&p, const char *end) {
  skip_ws(p, end);
  if (p >= end || *p != '[') return false;
  p++;

  float vals[8] = {0, 0, 0, 80, 1, 0, 0, 0};
  bool auto_bool = true;
  int idx = 0;
  while (p < end) {
    skip_ws(p, end);
    if (p < end && *p == ']') {
      p++;
      break;
    }

    if (idx == 4) {
      bool b = true;
      if (parse_json_bool_or_number(p, end, &b)) {
        auto_bool = b;
      } else if (!skip_json_value(p, end)) {
        return false;
      }
    } else {
      float v = vals[(idx < 8) ? idx : 7];
      if (parse_json_number(p, end, &v)) {
        if (idx < 8) vals[idx] = v;
      } else if (!skip_json_value(p, end)) {
        return false;
      }
    }

    skip_ws(p, end);
    if (p < end && *p == ',') {
      p++;
      idx++;
      continue;
    }
    if (p < end && *p == ']') {
      p++;
      break;
    }
    return false;
  }

  if (idx < 2) return false;
  if (!append_point(vals[0], vals[1], vals[2], vals[3], auto_bool, vals[5], vals[6], vals[7])) {
    return false;
  }
  return true;
}

static bool parse_json_point_object(const char *&p, const char *end) {
  skip_ws(p, end);
  if (p >= end || *p != '{') return false;
  p++;

  float x = 0.0f, y = 0.0f, z = 0.0f, t_ms = 80.0f, roll = 0.0f, pitch = 0.0f, yaw = 0.0f;
  bool ee_auto = true;
  bool has_x = false, has_y = false, has_z = false;

  while (p < end) {
    skip_ws(p, end);
    if (p < end && *p == '}') {
      p++;
      break;
    }

    const char *k = nullptr;
    size_t klen = 0;
    if (!parse_json_string_token(p, end, &k, &klen)) return false;
    skip_ws(p, end);
    if (p >= end || *p != ':') return false;
    p++;

    if (key_equals(k, klen, "x")) {
      if (!parse_json_number(p, end, &x)) return false;
      has_x = true;
    } else if (key_equals(k, klen, "y")) {
      if (!parse_json_number(p, end, &y)) return false;
      has_y = true;
    } else if (key_equals(k, klen, "z")) {
      if (!parse_json_number(p, end, &z)) return false;
      has_z = true;
    } else if (key_equals(k, klen, "t") || key_equals(k, klen, "t_ms") ||
               key_equals(k, klen, "dt")) {
      if (!parse_json_number(p, end, &t_ms)) return false;
    } else if (key_equals(k, klen, "roll") ||
               key_equals(k, klen, "roll_deg")) {
      if (!parse_json_number(p, end, &roll)) return false;
    } else if (key_equals(k, klen, "ee_pitch") ||
               key_equals(k, klen, "ee_pitch_deg") ||
               key_equals(k, klen, "pitch")) {
      if (!parse_json_number(p, end, &pitch)) return false;
    } else if (key_equals(k, klen, "yaw") ||
               key_equals(k, klen, "yaw_deg")) {
      if (!parse_json_number(p, end, &yaw)) return false;
    } else if (key_equals(k, klen, "ee_auto") || key_equals(k, klen, "auto")) {
      if (!parse_json_bool_or_number(p, end, &ee_auto)) return false;
    } else {
      if (!skip_json_value(p, end)) return false;
    }

    skip_ws(p, end);
    if (p < end && *p == ',') {
      p++;
      continue;
    }
    if (p < end && *p == '}') {
      p++;
      break;
    }
    return false;
  }

  if (!has_x || !has_y || !has_z) return false;
  if (!append_point(x, y, z, t_ms, ee_auto, roll, pitch, yaw)) {
    return false;
  }
  return true;
}

static bool parse_json_points_array(const char *&p, const char *end) {
  skip_ws(p, end);
  if (p >= end || *p != '[') return false;
  p++;

  while (p < end) {
    skip_ws(p, end);
    if (p < end && *p == ']') {
      p++;
      return g_count > 0;
    }

    if (*p == '{') {
      if (!parse_json_point_object(p, end)) return false;
    } else if (*p == '[') {
      if (!parse_json_point_array(p, end)) return false;
    } else {
      return false;
    }

    skip_ws(p, end);
    if (p < end && *p == ',') {
      p++;
      continue;
    }
    if (p < end && *p == ']') {
      p++;
      return g_count > 0;
    }
    return false;
  }
  return false;
}

static bool parse_json_payload(const char *payload, size_t payload_len) {
  if (payload == nullptr || payload_len == 0) return false;
  const char *start = payload;
  const char *end = payload + payload_len;
  skip_ws(start, end);
  if (start >= end) return false;

  g_count = 0;

  if (*start == '[') {
    return parse_json_points_array(start, end);
  }

  if (*start == '{') {
    // Accept wrapper objects like {"points":[...]} by finding first array token.
    const char *arr = start;
    while (arr < end && *arr != '[') arr++;
    if (arr >= end) return false;
    return parse_json_points_array(arr, end);
  }

  return false;
}

static bool parse_csv_mutable(char *buf) {
  if (buf == nullptr) return false;

  g_count = 0;
  char *save_outer = nullptr;
  char *row = strtok_r(buf, ";", &save_outer);

  while (row != nullptr) {
    while (*row == ' ' || *row == '\n' || *row == '\r' || *row == '\t') row++;
    if (*row == '\0') {
      row = strtok_r(nullptr, ";", &save_outer);
      continue;
    }

    float vals[8] = {0, 0, 0, 80, 1, 0, 0, 0};
    int vi = 0;
    char *save_inner = nullptr;
    char *tok = strtok_r(row, ",", &save_inner);
    while (tok != nullptr && vi < 8) {
      vals[vi] = parse_float_or(tok, vals[vi]);
      vi++;
      tok = strtok_r(nullptr, ",", &save_inner);
    }
    if (vi < 4) {
      g_count = 0;
      return false;
    }

    if (!append_point(vals[0], vals[1], vals[2], vals[3], vals[4] >= 0.5f,
                      vals[5], vals[6], vals[7])) {
      return false;
    }
    row = strtok_r(nullptr, ";", &save_outer);
  }

  return g_count > 0;
}

static bool parse_payload_mutable(char *buf, size_t payload_len) {
  if (buf == nullptr || payload_len == 0) return false;
  const char *p = buf;
  const char *end = buf + payload_len;
  skip_ws(p, end);
  if (p >= end) return false;

  if (*p == '[' || *p == '{') {
    return parse_json_payload(p, static_cast<size_t>(end - p));
  }
  return parse_csv_mutable(buf);
}

}  // namespace

void trajectory_handler_reset() { g_count = 0; }

size_t trajectory_handler_count() { return g_count; }

bool trajectory_handler_get_point(size_t index, TrajectoryPoint *out) {
  if (!out || index >= g_count || !g_points) return false;
  *out = g_points[index];
  return true;
}

bool trajectory_handler_parse_csv(const char *csv) {
  if (!csv) {
    set_last_error("null_csv");
    return false;
  }
  return trajectory_handler_parse_payload(csv, strlen(csv));
}

bool trajectory_handler_parse_payload(const char *payload, size_t payload_len) {
  set_last_error(nullptr);
  if (!payload) {
    set_last_error("null_payload");
    return false;
  }
  if (!ensure_points_buffer()) {
    if (g_last_error[0] == '\0') set_last_error("point_buffer_not_ready");
    return false;
  }
  if (payload_len == 0) {
    g_count = 0;
    return true;
  }
  if (payload_len > kMaxPayloadBytes) {
    set_last_error("payload_too_large");
    return false;
  }

  char *buf = static_cast<char *>(alloc_tolerant_buffer(payload_len + 1));
  if (!buf) {
    set_last_error("payload_buffer_alloc_failed");
    return false;
  }
  memcpy(buf, payload, payload_len);
  buf[payload_len] = '\0';

  const bool ok = parse_payload_mutable(buf, payload_len);
  heap_caps_free(buf);
  if (!ok && g_last_error[0] == '\0') set_last_error("payload_parse_failed");
  return ok;
}

bool trajectory_handler_stream_begin(size_t total_bytes) {
  set_last_error(nullptr);
  if (total_bytes == 0) {
    set_last_error("stream_empty");
    g_stream_active = false;
    g_stream_size = 0;
    return false;
  }
  if (total_bytes > kMaxPayloadBytes) {
    set_last_error("stream_too_large");
    g_stream_active = false;
    g_stream_size = 0;
    return false;
  }

  const size_t needed = total_bytes + 1;
  if (g_stream_buf == nullptr || g_stream_capacity < needed) {
    if (g_stream_buf != nullptr) {
      heap_caps_free(g_stream_buf);
      g_stream_buf = nullptr;
      g_stream_capacity = 0;
    }
    g_stream_buf = static_cast<char *>(alloc_tolerant_buffer(needed));
    if (!g_stream_buf) {
      set_last_error("stream_alloc_failed");
      g_stream_active = false;
      g_stream_size = 0;
      return false;
    }
    g_stream_capacity = needed;
  }

  g_stream_active = true;
  g_stream_size = 0;
  return true;
}

bool trajectory_handler_stream_write(size_t index, const uint8_t *data,
                                     size_t len) {
  if (!g_stream_active || g_stream_buf == nullptr) {
    set_last_error("stream_not_active");
    return false;
  }
  if (len == 0) return true;
  if (data == nullptr) {
    set_last_error("stream_null_chunk");
    return false;
  }
  if (index >= g_stream_capacity) {
    set_last_error("stream_overflow");
    return false;
  }
  const size_t usable_capacity = g_stream_capacity - 1;  // reserve NUL byte
  if (index > usable_capacity || len > (usable_capacity - index)) {
    set_last_error("stream_overflow");
    return false;
  }

  memcpy(g_stream_buf + index, data, len);
  const size_t end = index + len;
  if (end > g_stream_size) g_stream_size = end;
  return true;
}

bool trajectory_handler_stream_parse() {
  if (!g_stream_active || g_stream_buf == nullptr) {
    set_last_error("stream_not_active");
    return false;
  }
  if (g_stream_size == 0) {
    set_last_error("stream_empty");
    g_stream_active = false;
    return false;
  }

  g_stream_buf[g_stream_size] = '\0';
  const bool ok = trajectory_handler_parse_payload(g_stream_buf, g_stream_size);
  g_stream_active = false;
  return ok;
}

void trajectory_handler_stream_reset() {
  g_stream_active = false;
  g_stream_size = 0;
}

const char *trajectory_handler_last_error() {
  if (g_last_error[0] == '\0') return "none";
  return g_last_error;
}
