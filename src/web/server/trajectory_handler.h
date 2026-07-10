#pragma once

#include <stddef.h>
#include <stdint.h>

struct TrajectoryPoint {
  float x;
  float y;
  float z;
  float t_ms;
  float roll_deg;
  float ee_pitch_deg;
  float yaw_deg;
  bool ee_auto;
};

bool trajectory_handler_parse_csv(const char *csv);
bool trajectory_handler_parse_payload(const char *payload, size_t payload_len);
void trajectory_handler_reset();
size_t trajectory_handler_count();
bool trajectory_handler_get_point(size_t index, TrajectoryPoint *out);

bool trajectory_handler_stream_begin(size_t total_bytes);
bool trajectory_handler_stream_write(size_t index, const uint8_t *data,
                                     size_t len);
bool trajectory_handler_stream_parse();
void trajectory_handler_stream_reset();
const char *trajectory_handler_last_error();
