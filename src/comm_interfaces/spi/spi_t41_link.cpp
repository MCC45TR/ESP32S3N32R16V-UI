#include "spi_t41_link.h"
#include "src/communication/protocol_def.h"
#include "src/app/planetary_pid_advanced.h"
#include "src/config/pin_config.h"
#include "src/drivers/i2c/pca9685_driver.h"
#include "src/drivers/spi/spi_c3_master.h"
#include "src/platform/mros_gpio.h"
#include "src/platform/mros_time.h"
#include "WString.h"
#include <driver/spi_slave.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstdio>
#include <math.h>
#include <string.h>
#include "src/core/state/event_bus.h"
#include "src/drivers/utils/mros_console.h"

namespace {

constexpr uint32_t kFlagAckRequired = T41_QSPI_FLAG_ACK_REQUIRED;
constexpr uint32_t kFlagFragmented = T41_QSPI_FLAG_FRAGMENTED;
constexpr uint32_t kFlagSafety = T41_QSPI_FLAG_SAFETY;
constexpr uint32_t kAckTimeoutMs = 40U;
constexpr uint8_t kMaxCmdRetries = 3U;
constexpr uint16_t kMaxFragmentCount = 16U;
constexpr size_t kMaxFragmentBytes =
    static_cast<size_t>(T41_QSPI_MAX_PAYLOAD_BYTES) * kMaxFragmentCount;
constexpr size_t kWireFrameBytes = T41_QSPI_MAX_FRAME_BYTES;
constexpr size_t kDecodedPayloadBytes = T41_QSPI_MAX_PAYLOAD_BYTES;

static spi_slave_transaction_t trans;
static volatile bool spi_transaction_done = true;
static volatile bool spi_new_data = false;
static TaskHandle_t g_spi_notify_task = nullptr;
static TaskHandle_t g_joint_task_handle = nullptr;
static SpiS3EndpointMode g_endpoint_mode = SpiS3EndpointMode::ClassicSpi1Bit;
static bool t41Connected = false;
static bool g_c3_failsafe_auto_forced = false;
static bool g_t41_failsafe_link_state = false;
static bool g_t41_failsafe_link_candidate = false;
static unsigned long g_t41_failsafe_link_candidate_ms = 0;
static unsigned long last_packet_time = 0;
static volatile int16_t g_device_status_code = 1111;
static unsigned long g_last_spi_queue_fail_log_ms = 0;
static uint32_t g_next_tx_seq = 0U;

__attribute__((aligned(4))) static uint8_t spi_tx_buf[kWireFrameBytes] = {};
__attribute__((aligned(4))) static uint8_t spi_rx_buf[kWireFrameBytes] = {};
__attribute__((aligned(4))) static uint8_t spi_rx_buf_safe[kWireFrameBytes] = {};

struct T41TelemetryCache {
  uint16_t error_code = 0U;
  uint16_t loop_ms = 0U;
  float q[3] = {0.0F, 0.0F, 0.0F};
  float fk[3] = {0.0F, 0.0F, 0.0F};
  float coord_roll = 0.0F;
  float coord_pitch = 0.0F;
  float coord_yaw = 0.0F;
  float alpha = 0.0F;
  uint8_t online_count = 0U;
  uint8_t degraded[3] = {0U, 0U, 0U};
  bool motion_active = false;
  uint32_t remote_ms = 0U;
  uint32_t last_ack_seq = 0U;
  bool last_ack_ok = false;
  char last_ack_reason[64] = {};
  uint8_t safety_code = 0U;
  char safety_state[24] = "unknown";
};

static T41TelemetryCache g_t41_telemetry = {};

struct PendingControlFrame {
  bool used = false;
  uint32_t seq = 0U;
  uint8_t retries = 0U;
  unsigned long last_send_ms = 0UL;
  uint8_t frame[kWireFrameBytes] = {};
  size_t frame_len = 0U;
};

static PendingControlFrame g_pending_control = {};

static bool g_pending_ack_response = false;
static bool g_pending_ack_is_nack = false;
static uint32_t g_pending_ack_target = 0U;
static char g_pending_ack_reason[64] = "accepted";

struct FragmentReassembly {
  bool used = false;
  uint32_t msg_id = 0U;
  uint16_t frag_count = 0U;
  uint32_t received_mask = 0U;
  uint16_t frag_lengths[kMaxFragmentCount] = {};
  uint8_t payload[kMaxFragmentBytes] = {};
  unsigned long last_update_ms = 0UL;
};

static FragmentReassembly g_fragments = {};

// SPI Link Diagnostic Counters
static uint32_t spi_diag_total = 0;
static uint32_t spi_diag_crc_err = 0;
static uint32_t spi_diag_marker_err = 0;
static uint8_t spi_diag_last_marker = 0;
static uint8_t spi_diag_last_seq = 0;
static uint32_t spi_diag_ack_frames = 0;
static uint32_t spi_diag_nack_frames = 0;
static uint32_t spi_diag_retry_frames = 0;
static uint32_t spi_diag_ack_timeouts = 0;
static uint32_t spi_diag_fragment_errors = 0;
static uint32_t spi_diag_reassembled = 0;
static uint32_t spi_diag_duplicate_cmd = 0;
static uint32_t spi_diag_last_command_seq = 0U;

bool quad_lane_pins_valid() {
  return PIN_SPI_WP >= 0 && PIN_SPI_HD >= 0;
}

bool spi_clock_prep_safe_locked() {
  const bool queue_idle = spi_transaction_done && !spi_new_data;
  const bool pending_idle = !g_pending_ack_response && !g_pending_control.used;
  const bool cs_high = (PIN_SPI_CS >= 0) && mros::platform::mros_gpio_read(PIN_SPI_CS);
  const bool ready_low = (PIN_DATA_READY < 0) || !mros::platform::mros_gpio_read(PIN_DATA_READY);
  const bool irq_low = (PIN_TEENSY_IRQ < 0) || !mros::platform::mros_gpio_read(PIN_TEENSY_IRQ);
  return queue_idle && pending_idle && cs_high && ready_low && irq_low;
}

// Error Log Ring Buffer (last 25 errors)
#define SPI_ERR_LOG_SIZE 25
typedef struct {
  uint32_t timestamp;
  uint8_t err_type; // 1=header, 2=crc
  uint8_t rx_marker;
  uint8_t rx_seq;
  uint8_t rx_bytes[4]; // first 4 bytes of packet
  uint32_t expected_crc;
  uint32_t actual_crc;
} SPI_ErrLogEntry_t;

static SPI_ErrLogEntry_t spi_err_log[SPI_ERR_LOG_SIZE];
static int spi_err_log_idx = 0;
static int spi_err_log_count = 0;
static unsigned long spi_last_diag_print_ms = 0;
static uint32_t spi_suppressed_diag = 0;

static void spi_log_error(uint8_t type, uint32_t exp_crc, uint32_t act_crc) {
  SPI_ErrLogEntry_t *e = &spi_err_log[spi_err_log_idx];
  e->timestamp = mros::platform::mros_millis();
  e->err_type = type;
  e->rx_marker = spi_rx_buf_safe[0];
  e->rx_seq = spi_rx_buf_safe[1];
  e->rx_bytes[0] = spi_rx_buf_safe[0];
  e->rx_bytes[1] = spi_rx_buf_safe[1];
  e->rx_bytes[2] = spi_rx_buf_safe[2];
  e->rx_bytes[3] = spi_rx_buf_safe[3];
  e->expected_crc = exp_crc;
  e->actual_crc = act_crc;
  spi_err_log_idx = (spi_err_log_idx + 1) % SPI_ERR_LOG_SIZE;
  if (spi_err_log_count < SPI_ERR_LOG_SIZE)
    spi_err_log_count++;
}

static void spi_diag_print_error(const char *type, uint32_t exp_crc,
                                 uint32_t act_crc) {
  unsigned long now = mros::platform::mros_millis();
  if ((now - spi_last_diag_print_ms) < 250) {
    spi_suppressed_diag++;
    return;
  }
  uint8_t *b = spi_rx_buf_safe;
  mros_console.printf(
      "[QSPI-S3][%s] marker=0x%02X seq=%u raw=%02X %02X %02X %02X %02X %02X %02X "
      "%02X crc(exp/act)=0x%08X/0x%08X suppressed=%lu\n",
      type, (unsigned)b[0], (unsigned)b[1], (unsigned)b[0], (unsigned)b[1],
      (unsigned)b[2], (unsigned)b[3], (unsigned)b[4], (unsigned)b[5],
      (unsigned)b[6], (unsigned)b[7], (unsigned)exp_crc, (unsigned)act_crc,
      (unsigned long)spi_suppressed_diag);
  spi_suppressed_diag = 0;
  spi_last_diag_print_ms = now;
}

enum class DecodeResult : uint8_t {
  Ok = 0,
  TooShort,
  BadMagic,
  BadVersion,
  BadHeaderLen,
  BadLength,
  BadHeaderCrc,
  BadPayloadCrc
};

static uint32_t crc32c(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0U; i < length; ++i) {
    crc ^= data != nullptr ? data[i] : 0U;
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^ (0x82F63B78UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

static size_t align4(const size_t value) {
  return (value + 3U) & ~static_cast<size_t>(3U);
}

static size_t payload_to_cstr(const uint8_t *payload,
                              const size_t payload_len,
                              char *out_text,
                              const size_t out_size) {
  if (out_text == nullptr || out_size == 0U) {
    return 0U;
  }
  if (payload == nullptr || payload_len == 0U) {
    out_text[0] = '\0';
    return 0U;
  }
  const size_t to_copy = payload_len < (out_size - 1U) ? payload_len : (out_size - 1U);
  memcpy(out_text, payload, to_copy);
  out_text[to_copy] = '\0';
  return to_copy;
}

static DecodeResult decode_frame(const uint8_t *bytes,
                                 const size_t length,
                                 T41_QSPI_FrameHeader *out_header,
                                 const uint8_t **out_payload,
                                 size_t *out_payload_len) {
  if (bytes == nullptr || length < T41_QSPI_HEADER_BYTES) {
    return DecodeResult::TooShort;
  }
  T41_QSPI_FrameHeader header = {};
  memcpy(&header, bytes, sizeof(header));
  if (header.magic != T41_QSPI_FRAME_MAGIC) {
    return DecodeResult::BadMagic;
  }
  if (header.version != T41_QSPI_PROTOCOL_VERSION) {
    return DecodeResult::BadVersion;
  }
  if (header.header_len != T41_QSPI_HEADER_BYTES) {
    return DecodeResult::BadHeaderLen;
  }
  if (header.payload_len > T41_QSPI_MAX_PAYLOAD_BYTES) {
    return DecodeResult::BadLength;
  }
  if ((T41_QSPI_HEADER_BYTES + header.payload_len) > length) {
    return DecodeResult::BadLength;
  }
  uint8_t header_copy[T41_QSPI_HEADER_BYTES] = {};
  memcpy(header_copy, bytes, T41_QSPI_HEADER_BYTES);
  reinterpret_cast<T41_QSPI_FrameHeader *>(header_copy)->header_crc32c = 0U;
  const uint32_t calc_header_crc = crc32c(header_copy, T41_QSPI_HEADER_BYTES);
  if (calc_header_crc != header.header_crc32c) {
    return DecodeResult::BadHeaderCrc;
  }
  const uint8_t *payload = bytes + T41_QSPI_HEADER_BYTES;
  const uint32_t calc_payload_crc = crc32c(payload, header.payload_len);
  if (calc_payload_crc != header.payload_crc32c) {
    return DecodeResult::BadPayloadCrc;
  }
  if (out_header != nullptr) {
    *out_header = header;
  }
  if (out_payload != nullptr) {
    *out_payload = payload;
  }
  if (out_payload_len != nullptr) {
    *out_payload_len = header.payload_len;
  }
  return DecodeResult::Ok;
}

}  // namespace

// Target caches before queueing
static float g_cache_turret = 0.0f;
static float g_cache_joints[6] = {0.0f};
static float g_cache_gripper = 0.0f;
static float g_cache_target_x = 0.0f;
static float g_cache_target_y = 0.0f;
static float g_cache_target_z = 0.0f;
static float g_cache_target_roll = 0.0f;
static float g_cache_target_pitch = 0.0f;
static float g_cache_target_yaw = 0.0f;
static bool g_cache_target_use_orientation = false;
static bool g_cache_cartesian_active = false;
static uint8_t g_cache_motor = 1;
static uint8_t g_cache_reset = 0; // one shot

static PlanetaryPID_t g_turret_pid;
static bool g_turret_pid_initialized = false;
static unsigned long g_turret_pid_prev_ms = 0;

static float g_turret_actual_deg = 0.0f;
static float g_turret_pid_error = 0.0f;
static int g_last_turret_speed_cmd = 999;
static bool g_last_turret_speed_valid = false;
static unsigned long g_last_turret_speed_apply_ms = 0;
static unsigned long g_last_pca_reinit_ms = 0;
static unsigned long g_last_c3_feedback_ms = 0;
static unsigned long g_last_turret_feedback_log_ms = 0;
static unsigned long g_last_turret_active_log_ms = 0;
static unsigned long g_last_turret_blocked_log_ms = 0;
static unsigned long g_last_turret_apply_fail_log_ms = 0;
static unsigned long g_last_turret_pca_missing_log_ms = 0;
static unsigned long g_last_arm_batch_fail_log_ms = 0;
static unsigned long g_last_arm_batch_recover_ms = 0;
static unsigned long g_last_turret_feedback_offline_summary_ms = 0;
static uint32_t g_turret_feedback_offline_suppressed_count = 0;
static bool g_turret_feedback_offline_state = false;
static float g_last_turret_active_log_out = NAN;
static float g_last_turret_active_log_err = NAN;
static uint32_t g_last_c3_total_rx_seen = 0;
static uint32_t g_last_c3_crc_err_seen = 0;
static uint32_t g_last_c3_marker_err_seen = 0;
static bool g_turret_output_lock = false;

static bool teensy_ready_line_high() {
  return mros::platform::mros_gpio_read(PIN_T41_READY);
}

static void notify_joint_task() {
  event_bus_publish(BIT_TRAJ_ACTIVE);
  if (g_joint_task_handle != nullptr) {
    xTaskNotifyGive(g_joint_task_handle);
  }
}

static constexpr float kTurretSunTeeth = 12.0f;
static constexpr float kTurretRingTeeth = 48.0f;
static constexpr float kTurretSign = 1.0f;
static constexpr float kTurretCarrierRatio =
    kTurretSunTeeth / (kTurretSunTeeth + kTurretRingTeeth);
static constexpr float kTurretErrorDeadbandDeg = 0.30f;
static constexpr float kTurretSpeedDeadbandDegS = 0.40f;
static float g_turret_dspc = 6.0f; // Dead-Spot Compensation (minimum control effort)
// MG996R-360 hiz surusu (bu projede genis pencere): 500us / 1500us / 2500us @ 50Hz
static constexpr float kTurretPulseMinUs = 500.0f;
static constexpr float kTurretPulseCenterUs = 1500.0f;
static constexpr float kTurretPulseMaxUs = 2500.0f;
static constexpr int8_t kTurretDirCh1 = 1;  // Ch 1: Normal
static constexpr int8_t kTurretDirCh2 = -1; // Ch 2: Reverse
static constexpr int8_t kTurretDirCh3 = 1;  // Ch 3: Normal
static constexpr int kArmJointCount = 6;
static constexpr float kArmJointMinDeg = -90.0f;
static constexpr float kArmJointMaxDeg = 90.0f;
static constexpr float kGripperMinDeg = 0.0f;
static constexpr float kGripperMaxDeg = 180.0f;

// IK sonrasi yumusatma: hedefler 50Hz (20ms) tick ile S-curve olarak uygulanir.
static constexpr unsigned long kJointTrajTickMs = 20UL;
static constexpr unsigned long kJointTrajDebounceMs = 8UL;
static constexpr unsigned long kJointSyncHoldMs = 350UL;
// MG996R / yuksek torklu servo tabanli muhafazakar hiz limitleri (deg/s).
// Omuz eksenleri (J1-J3) daha yuklu oldugu icin daha dusuk tutuldu.
static constexpr float kJointServoMaxDegPerSec[kArmJointCount] = {
    220.0f, // J1
    220.0f, // J2
    220.0f, // J3
    260.0f, // J4
    260.0f, // J5
    280.0f  // J6
};
static constexpr float kJointTrajMinDeltaDeg = 0.05f;
static constexpr float kJointTrajScaleMin = 1.0f;
static constexpr float kJointTrajScaleMax = 3.0f;

static uint8_t make_device_digit(bool connected, bool fault) {
  if (!connected) return 1U;
  if (fault) return 3U;
  return 2U;
}

static int16_t build_device_status_code() {
  const bool pca_connected = pca9685_is_ready();
  const bool c3_connected = spi_c3_is_connected();
  const bool s3_connected = true;
  const bool t41_connected_now = t41Connected;
  const bool c3_fault =
      c3_connected &&
      ((spi_c3_get_crc_errors() > 0U) || (spi_c3_get_marker_errors() > 0U));

  const uint8_t pca = make_device_digit(pca_connected, false);
  const uint8_t c3 = make_device_digit(c3_connected, c3_fault);
  const uint8_t s3 = make_device_digit(s3_connected, false);
  const uint8_t t41 = make_device_digit(t41_connected_now, false);
  return static_cast<int16_t>((int)pca * 1000 + (int)c3 * 100 + (int)s3 * 10 +
                              (int)t41);
}

typedef struct {
  bool active;
  bool pending_replan;
  unsigned long pending_since_ms;
  unsigned long start_ms;
  unsigned long last_update_ms;
  unsigned long total_time_ms;
  uint16_t total_steps;
  uint16_t last_tick_idx;
  float start_deg[kArmJointCount];
  float target_deg[kArmJointCount];
  float delta_deg[kArmJointCount];
} JointTrajPlanner_t;

static JointTrajPlanner_t g_joint_traj = {};
static float g_joint_target_request[kArmJointCount] = {0.0f};
static unsigned long g_last_local_joint_cmd_ms = 0;
static float g_joint_traj_time_scale = 1.0f;

// Forward declarations
static void apply_arm_joint_batch();

static float clampf(float v, float lo, float hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static float clamp_joint_deg(float deg) {
  return clampf(deg, kArmJointMinDeg, kArmJointMaxDeg);
}

static float clamp_gripper_deg(float deg) {
  return clampf(deg, kGripperMinDeg, kGripperMaxDeg);
}

static float joint_traj_blend_scurve(float u) {
  if (u <= 0.0f)
    return 0.0f;
  if (u >= 1.0f)
    return 1.0f;
  const float u2 = u * u;
  const float u3 = u2 * u;
  const float u4 = u3 * u;
  const float u5 = u4 * u;
  return (6.0f * u5) - (15.0f * u4) + (10.0f * u3);
}

static bool joint_traj_local_override_active(unsigned long now_ms) {
  if (g_joint_traj.active || g_joint_traj.pending_replan)
    return true;
  if (g_last_local_joint_cmd_ms == 0)
    return false;
  return (now_ms - g_last_local_joint_cmd_ms) <= kJointSyncHoldMs;
}

static void joint_traj_cancel(bool sync_target_to_cache) {
  g_joint_traj.active = false;
  g_joint_traj.pending_replan = false;
  g_joint_traj.pending_since_ms = 0;
  g_joint_traj.start_ms = 0;
  g_joint_traj.last_update_ms = 0;
  g_joint_traj.total_time_ms = 0;
  g_joint_traj.total_steps = 0;
  g_joint_traj.last_tick_idx = 0;
  if (sync_target_to_cache) {
    for (int i = 0; i < kArmJointCount; i++) {
      g_joint_target_request[i] = clamp_joint_deg(g_cache_joints[i]);
    }
  }
}

static void joint_traj_apply_tick(uint16_t tick_idx) {
  if (!g_joint_traj.active || g_joint_traj.total_steps == 0)
    return;
  if (tick_idx > g_joint_traj.total_steps)
    tick_idx = g_joint_traj.total_steps;

  const float u = (float)tick_idx / (float)g_joint_traj.total_steps;
  const float s = joint_traj_blend_scurve(u);
  for (int i = 0; i < kArmJointCount; i++) {
    g_cache_joints[i] =
        clamp_joint_deg(g_joint_traj.start_deg[i] + g_joint_traj.delta_deg[i] * s);
  }
  apply_arm_joint_batch();
  g_joint_traj.last_tick_idx = tick_idx;
  if (tick_idx >= g_joint_traj.total_steps) {
    g_joint_traj.active = false;
    for (int i = 0; i < kArmJointCount; i++) {
      g_joint_target_request[i] = g_cache_joints[i];
    }
  }
}

static void joint_traj_start(unsigned long now_ms) {
  float min_time_ms = 0.0f;
  bool any_change = false;
  for (int i = 0; i < kArmJointCount; i++) {
    const float start = clamp_joint_deg(g_cache_joints[i]);
    const float target = clamp_joint_deg(g_joint_target_request[i]);
    const float delta = target - start;
    g_joint_traj.start_deg[i] = start;
    g_joint_traj.target_deg[i] = target;
    g_joint_traj.delta_deg[i] = delta;
    const float abs_delta = fabsf(delta);
    if (abs_delta >= kJointTrajMinDeltaDeg) {
      any_change = true;
      float axis_speed_deg_s = kJointServoMaxDegPerSec[i];
      if (!std::isfinite(axis_speed_deg_s) || axis_speed_deg_s < 1.0f) {
        axis_speed_deg_s = 220.0f;
      }
      const float axis_time_ms = (abs_delta * 1000.0f) / axis_speed_deg_s;
      if (axis_time_ms > min_time_ms) {
        min_time_ms = axis_time_ms;
      }
    }
  }

  g_joint_traj.pending_replan = false;
  if (!any_change) {
    for (int i = 0; i < kArmJointCount; i++) {
      g_cache_joints[i] = g_joint_traj.target_deg[i];
    }
    apply_arm_joint_batch();
    joint_traj_cancel(true);
    return;
  }

  float scale = g_joint_traj_time_scale;
  if (!std::isfinite(scale))
    scale = 1.0f;
  scale = clampf(scale, kJointTrajScaleMin, kJointTrajScaleMax);
  const float scaled_min_time_ms = min_time_ms * scale;
  unsigned long total_time_ms = (unsigned long)ceilf(scaled_min_time_ms / (float)kJointTrajTickMs) *
                                kJointTrajTickMs;
  if (total_time_ms < kJointTrajTickMs)
    total_time_ms = kJointTrajTickMs;

  g_joint_traj.active = true;
  g_joint_traj.start_ms = now_ms;
  g_joint_traj.last_update_ms = now_ms;
  g_joint_traj.total_time_ms = total_time_ms;
  g_joint_traj.total_steps = (uint16_t)(total_time_ms / kJointTrajTickMs);
  g_joint_traj.last_tick_idx = 0;
}

static void joint_traj_mark_target_updated(unsigned long now_ms) {
  g_joint_traj.pending_replan = true;
  g_joint_traj.pending_since_ms = now_ms;
  notify_joint_task();
}

static void joint_traj_update(unsigned long now_ms) {
  if (g_joint_traj.pending_replan &&
      (now_ms - g_joint_traj.pending_since_ms) >= kJointTrajDebounceMs) {
    joint_traj_start(now_ms);
  }
  if (!g_joint_traj.active || g_joint_traj.total_steps == 0)
    return;

  unsigned long dt_since_last = now_ms - g_joint_traj.last_update_ms;
  g_joint_traj.last_update_ms = now_ms;

  const bool motors_enabled =
      (g_cache_motor != 0) || g_t41_telemetry.motion_active;
  if (!motors_enabled) {
    // Motor kapaliyken trajectory zamanini dondur.
    g_joint_traj.start_ms += dt_since_last;
    return;
  }

  const unsigned long elapsed_ms = now_ms - g_joint_traj.start_ms;
  uint16_t tick_idx = (uint16_t)(elapsed_ms / kJointTrajTickMs);
  if (tick_idx > g_joint_traj.total_steps)
    tick_idx = g_joint_traj.total_steps;
  if (tick_idx <= g_joint_traj.last_tick_idx)
    return;

  joint_traj_apply_tick(tick_idx);
}

static float c3_raw_to_turret_carrier(float raw_deg) {
  return raw_deg * kTurretCarrierRatio * kTurretSign;
}

static uint16_t turret_us_to_tick(float us) {
  float freq = pca9685_get_frequency();
  if (!(freq > 1.0f))
    freq = 50.0f;
  float tick_f = (us * 4096.0f * freq) / 1000000.0f;
  if (tick_f < 0.0f)
    tick_f = 0.0f;
  if (tick_f > 4095.0f)
    tick_f = 4095.0f;
  return (uint16_t)(tick_f + 0.5f);
}

static float turret_speed_to_us(int speed_cmd) {
  if (speed_cmd > 100)
    speed_cmd = 100;
  if (speed_cmd < -100)
    speed_cmd = -100;

  if (speed_cmd == 0)
    return kTurretPulseCenterUs;
  if (speed_cmd > 0) {
    return kTurretPulseCenterUs +
           ((float)speed_cmd / 100.0f) * (kTurretPulseMaxUs - kTurretPulseCenterUs);
  }
  return kTurretPulseCenterUs +
         ((float)speed_cmd / 100.0f) * (kTurretPulseCenterUs - kTurretPulseMinUs);
}

static uint16_t turret_tick_for_speed(int speed_cmd, int8_t dir_mul) {
  int cmd = speed_cmd * (int)dir_mul;
  if (cmd > 100)
    cmd = 100;
  if (cmd < -100)
    cmd = -100;
  return turret_us_to_tick(turret_speed_to_us(cmd));
}

static bool turret_write_channel(uint8_t ch, int speed_cmd, int8_t dir_mul) {
  int cmd = speed_cmd * (int)dir_mul;
  if (cmd > 100)
    cmd = 100;
  if (cmd < -100)
    cmd = -100;

  float us = turret_speed_to_us(cmd);
  uint16_t tick = turret_us_to_tick(us);
  bool ok = pca9685_set_pwm(ch, 0, tick);
  if (!ok) {
    // Fallback for transient block-write issues
    ok = pca9685_set_servo_speed(ch, cmd);
  }
  return ok;
}

static void apply_arm_joint_batch() {
  bool motors_enabled = (g_cache_motor != 0) || g_t41_telemetry.motion_active;
  if (!motors_enabled || !pca9685_is_ready()) return;

  // Channels 4-13 Batch (J2 Normal, J2 Reverse, J3 Normal, J3 Reverse, J4, J5, J6, J7, Grip Normal, Grip Reverse)
  float angles[10];

  // J2 (Ch 4, 5)
  float j2_norm = clamp_joint_deg(g_cache_joints[0]) + 90.0f;
  angles[0] = j2_norm;
  angles[1] = 180.0f - j2_norm;

  // J3 (Ch 6, 7)
  float j3_norm = clamp_joint_deg(g_cache_joints[1]) + 90.0f;
  angles[2] = j3_norm;
  angles[3] = 180.0f - j3_norm;

  // J4-J7 (Ch 8, 9, 10, 11)
  for (int i = 0; i < 4; i++) {
    angles[4 + i] = clamp_joint_deg(g_cache_joints[2 + i]) + 90.0f;
  }

  // Gripper (Ch 12, 13)
  float gripper = clamp_gripper_deg(g_cache_gripper);
  angles[8] = gripper;
  angles[9] = 180.0f - gripper;

  auto apply_single_fallback = [&](void) -> bool {
    bool fallback_ok = true;
    for (uint8_t i = 0; i < 10; i++) {
      uint8_t ch = (uint8_t)(4 + i);
      if (!pca9685_set_servo_angle(ch, angles[i])) {
        fallback_ok = false;
        break;
      }
    }
    return fallback_ok;
  };

  bool ok = pca9685_set_servo_angle_group(4, angles, 10);
  if (!ok) {
    // Clone boards may intermittently reject long block writes; fall back to
    // per-channel writes before declaring failure.
    ok = apply_single_fallback();
  }
  if (!ok) {
    unsigned long now_ms = mros::platform::mros_millis();
    if ((now_ms - g_last_arm_batch_recover_ms) > 1000UL) {
      g_last_arm_batch_recover_ms = now_ms;
      if (pca9685_init()) {
        ok = pca9685_set_servo_angle_group(4, angles, 10);
        if (!ok) {
          ok = apply_single_fallback();
        }
      }
    }
  }
  if (ok) {
    // mros_console.println("[ARM-BATCH] J2-J7 + Gripper applied in single I2C packet (Ch 4-13)");
  } else {
    unsigned long now_ms = mros::platform::mros_millis();
    if ((now_ms - g_last_arm_batch_fail_log_ms) > 250UL) {
      g_last_arm_batch_fail_log_ms = now_ms;
      mros_console.println("[ARM-BATCH] FAILED to apply joint batch");
    }
  }
}

static void turret_apply_speed_command(int speed_cmd) {
  if (speed_cmd > 100)
    speed_cmd = 100;
  if (speed_cmd < -100)
    speed_cmd = -100;

  unsigned long now_ms = mros::platform::mros_millis();
  bool same_cmd = g_last_turret_speed_valid && (speed_cmd == g_last_turret_speed_cmd);
  if (same_cmd && (now_ms - g_last_turret_speed_apply_ms) < 40)
    return;
  if (!pca9685_is_ready()) {
    g_last_turret_speed_valid = false;
    if ((now_ms - g_last_turret_pca_missing_log_ms) > 30000) {
      g_last_turret_pca_missing_log_ms = now_ms;
      mros_console.println("[TURRET] PCA not ready, speed command skipped");
    }
    return;
  }

  uint16_t ticks[3] = {turret_tick_for_speed(speed_cmd, kTurretDirCh1),
                       turret_tick_for_speed(speed_cmd, kTurretDirCh2),
                       turret_tick_for_speed(speed_cmd, kTurretDirCh3)};

  // Single I2C block write keeps CH1-CH3 phase aligned and reduces bus jitter.
  bool ok = pca9685_set_pwm_group(1, ticks, 3);
  if (!ok) {
    ok = true;
    ok = ok && turret_write_channel(1, speed_cmd, kTurretDirCh1);
    ok = ok && turret_write_channel(2, speed_cmd, kTurretDirCh2);
    ok = ok && turret_write_channel(3, speed_cmd, kTurretDirCh3);
  }
  if (ok) {
    g_last_turret_speed_cmd = speed_cmd;
    g_last_turret_speed_valid = true;
    g_last_turret_speed_apply_ms = now_ms;
    // mros_console.printf("[TURRET] speed_cmd=%d ticks=[%u,%u,%u] CH1-CH3\n", speed_cmd,
    //              (unsigned)ticks[0], (unsigned)ticks[1], (unsigned)ticks[2]);
  } else {
    // Keep retrying on next cycle if I2C/PCA had a transient write failure.
    g_last_turret_speed_valid = false;
    if ((now_ms - g_last_turret_apply_fail_log_ms) > 5000) {
      g_last_turret_apply_fail_log_ms = now_ms;
      mros_console.printf("[TURRET] speed_cmd=%d apply FAILED (PCA write)\n", speed_cmd);
    }
  }
}

static void turret_pid_reset_state(bool clear_init) {
  g_turret_pid.last_error = 0.0f;
  g_turret_pid.prev_error = 0.0f;
  g_turret_pid.integrator = 0.0f;
  g_turret_pid.last_output = 0.0f;
  if (clear_init) {
    g_turret_pid_initialized = false;
    g_turret_pid_prev_ms = 0;
  }
  g_turret_pid_error = 0.0f;
}

static void turret_feedback_offline_log_summary(unsigned long now_ms, bool force) {
  (void)force;
  if (g_turret_feedback_offline_suppressed_count == 0) return;
  // PCA offline durumunda timeout logu tamamen susturulur.
  g_turret_feedback_offline_suppressed_count = 0;
  g_last_turret_feedback_offline_summary_ms = now_ms;
}

static void sync_c3_failsafe_with_t41_link() {
  const unsigned long now_ms = mros::platform::mros_millis();
  if (t41Connected != g_t41_failsafe_link_candidate) {
    g_t41_failsafe_link_candidate = t41Connected;
    g_t41_failsafe_link_candidate_ms = now_ms;
  } else if (g_t41_failsafe_link_state != g_t41_failsafe_link_candidate &&
             (now_ms - g_t41_failsafe_link_candidate_ms) >= 1000UL) {
    g_t41_failsafe_link_state = g_t41_failsafe_link_candidate;
  }

  const bool stable_t41_connected = g_t41_failsafe_link_state;
  const int8_t current = spi_c3_get_failsafe_option();
  if (!stable_t41_connected) {
    // t41 linki yoksa C3'e ESP-NOW fallback modunu zorla.
    if (!g_c3_failsafe_auto_forced && current == C3_FAILSAFE_T41_QSPI) {
      spi_c3_set_failsafe_option(C3_FAILSAFE_C3SPI_T41_ESPNOW);
      g_c3_failsafe_auto_forced = true;
      mros_console.println("[C3-SPI] t41 offline -> C3 failsafe: C3SPI+T41_ESPNOW");
    }
    return;
  }

  // t41 linki geri geldiginde sadece otomatik zorlanan modu geri al.
  if (g_c3_failsafe_auto_forced) {
    if (current == C3_FAILSAFE_C3SPI_T41_ESPNOW) {
      spi_c3_set_failsafe_option(C3_FAILSAFE_T41_QSPI);
      mros_console.println("[C3-SPI] t41 online -> C3 failsafe: T41_QSPI");
    }
    g_c3_failsafe_auto_forced = false;
  }
}

static void turret_pid_update(unsigned long now_ms) {
  float raw_pos_deg = spi_c3_get_position_deg();
  bool pca_ready = pca9685_is_ready();
  
  // Update internal actual degree cache
  g_turret_actual_deg = c3_raw_to_turret_carrier(raw_pos_deg);
  
  bool c3_connected = spi_c3_is_connected();
  const uint32_t c3_last_good_rx_ms = spi_c3_get_last_good_rx_ms();
  const uint32_t c3_total_rx = spi_c3_get_total_rx();
  const uint32_t c3_crc_err = spi_c3_get_crc_errors();
  const uint32_t c3_marker_err = spi_c3_get_marker_errors();
  const bool c3_clean_rx_progress =
      (c3_total_rx != g_last_c3_total_rx_seen) &&
      (c3_crc_err == g_last_c3_crc_err_seen) &&
      (c3_marker_err == g_last_c3_marker_err_seen);
  g_last_c3_total_rx_seen = c3_total_rx;
  g_last_c3_crc_err_seen = c3_crc_err;
  g_last_c3_marker_err_seen = c3_marker_err;

  if (c3_connected || c3_clean_rx_progress) {
    g_last_c3_feedback_ms = now_ms;
  }

  float target = g_cache_turret;
  g_turret_pid_error = target - g_turret_actual_deg;

  // Allow brief C3/SPI glitches without killing turret drive immediately.
  bool feedback_recent =
      ((c3_last_good_rx_ms != 0) && ((now_ms - c3_last_good_rx_ms) <= 2000)) ||
      ((g_last_c3_feedback_ms != 0) && ((now_ms - g_last_c3_feedback_ms) <= 2000));
  if (feedback_recent && g_turret_feedback_offline_state) {
    turret_feedback_offline_log_summary(now_ms, true);
    g_turret_feedback_offline_state = false;
  }
  if (!feedback_recent) {
    if (!pca_ready) {
      g_turret_feedback_offline_state = true;
      g_turret_feedback_offline_suppressed_count++;
      turret_feedback_offline_log_summary(now_ms, false);
    } else {
      if (g_turret_feedback_offline_state) {
        turret_feedback_offline_log_summary(now_ms, true);
        g_turret_feedback_offline_state = false;
      }
      if ((now_ms - g_last_turret_feedback_log_ms) > 5000UL) {
        g_last_turret_feedback_log_ms = now_ms;
        mros_console.printf(
            "[TURRET] feedback timeout >2s, forcing stop (c3=%d last_good=%lu rx=%lu crc=%lu marker=%lu)\n",
            (int)c3_connected, (unsigned long)c3_last_good_rx_ms,
            (unsigned long)c3_total_rx, (unsigned long)c3_crc_err,
            (unsigned long)c3_marker_err);
      }
    }
    turret_pid_reset_state(true);
    if (!g_turret_output_lock && pca_ready) {
      turret_apply_speed_command(0);
    }
    return;
  }

  // Drive gate is separate from compute gate.
  if (!pca_ready) {
    if ((now_ms - g_last_pca_reinit_ms) > 1000) {
      g_last_pca_reinit_ms = now_ms;
      if (pca9685_init()) {
        g_last_turret_speed_valid = false;
        pca_ready = pca9685_is_ready();
      }
    }
  }
  
  bool motors_enabled = (g_cache_motor != 0) || g_t41_telemetry.motion_active;
  bool drive_enabled = motors_enabled && pca_ready;

  if (!g_turret_pid_initialized) {
    g_turret_pid_prev_ms = now_ms;
    g_turret_pid_initialized = true;
    turret_pid_reset_state(false);
    if (!g_turret_output_lock) {
      turret_apply_speed_command(0);
    }
    return;
  }

  float dt = (float)(now_ms - g_turret_pid_prev_ms) * 0.001f;
  if (dt <= 0.0f || dt > 0.25f)
    dt = 0.02f;

  g_turret_pid.Ts = dt;

  // Compute PID using specialized Planetary Kinematics module
  float out = planetary_pid_compute(&g_turret_pid, target, raw_pos_deg);

  // Deadband and Dead-Spot Compensation (minimum effort)
  if (fabsf(g_turret_pid_error) < kTurretErrorDeadbandDeg) {
    out = 0.0f;
    g_turret_pid.last_output = 0.0f; // Reset incremental state
  } else {
    float dspc = clampf(g_turret_dspc, 0.0f, 100.0f);
    if (dspc > 0.0f && fabsf(out) < dspc) {
      out = (out >= 0.0f) ? dspc : -dspc;
    }
  }

  if (drive_enabled) {
    if (!g_turret_output_lock) {
      turret_apply_speed_command((int)lroundf(clampf(out, -100.0f, 100.0f)));
    } else {
      if ((now_ms - g_last_turret_blocked_log_ms) > 3000UL) {
        g_last_turret_blocked_log_ms = now_ms;
        mros_console.printf(
            "[TURRET] LOCKED: output_lock=1 suppressing PID drive (out=%.1f err=%.2f actual=%.2f target=%.2f)\n",
            out, g_turret_pid_error, g_turret_actual_deg, target);
      }
    }
    bool out_changed = !std::isfinite(g_last_turret_active_log_out) ||
                       (fabsf(out - g_last_turret_active_log_out) >= 30.0f) ||
                       ((out >= 0.0f) != (g_last_turret_active_log_out >= 0.0f));
    bool err_changed = !std::isfinite(g_last_turret_active_log_err) ||
                       (fabsf(g_turret_pid_error - g_last_turret_active_log_err) >= 2.0f);
    if ((out_changed || err_changed) &&
        (now_ms - g_last_turret_active_log_ms) > 8000) {
      g_last_turret_active_log_ms = now_ms;
      g_last_turret_active_log_out = out;
      g_last_turret_active_log_err = g_turret_pid_error;
      mros_console.printf("[TURRET] ACTIVE: out=%.1f err=%.2f actual=%.2f target=%.2f\n",
                   out, g_turret_pid_error, g_turret_actual_deg, target);
    }
  } else {
    if ((now_ms - g_last_turret_blocked_log_ms) > 5000) {
      g_last_turret_blocked_log_ms = now_ms;
      mros_console.printf("[TURRET] BLOCKED: motors=%d pca=%d c3=%d feedback=%d loop_dt=%.3f target=%.2f actual=%.2f err=%.2f\n",
                    (int)motors_enabled, (int)pca_ready, (int)c3_connected, (int)feedback_recent, dt, target, g_turret_actual_deg, g_turret_pid_error);
    }
    if (!g_turret_output_lock && pca_ready) {
      turret_apply_speed_command(0);
    }
  }
  
  g_turret_pid_prev_ms = now_ms;
}

// Forward declaration.
static bool queue_transaction();

static void request_ack_response(const bool nack,
                                 const uint32_t ack_target,
                                 const char *reason) {
  g_pending_ack_response = true;
  g_pending_ack_is_nack = nack;
  g_pending_ack_target = ack_target;
  std::snprintf(g_pending_ack_reason,
                sizeof(g_pending_ack_reason),
                "%s",
                reason != nullptr ? reason : (nack ? "nack" : "ack"));
}

static bool parse_snapshot_payload(const char *payload, const unsigned long now_ms) {
  if (payload == nullptr) {
    return false;
  }
  float q0 = 0.0F, q1 = 0.0F, q2 = 0.0F;
  float fk0 = 0.0F, fk1 = 0.0F, fk2 = 0.0F;
  long pu0 = 0L, pu1 = 0L, pu2 = 0L;
  unsigned sw0 = 0U, sw1 = 0U, sw2 = 0U;
  unsigned online = 0U, d0 = 0U, d1 = 0U, d2 = 0U, motion = 0U;
  char safety[24] = {};
  const int matched =
      std::sscanf(payload,
                  "schema=mros-qspi-snapshot-v1 q=[%f,%f,%f] fk=[%f,%f,%f] "
                  "pu=[%ld,%ld,%ld] sw=[0x%x,0x%x,0x%x] online=%u "
                  "degraded=[%u,%u,%u] safety=%23s motion=%u",
                  &q0, &q1, &q2, &fk0, &fk1, &fk2, &pu0, &pu1, &pu2, &sw0, &sw1,
                  &sw2, &online, &d0, &d1, &d2, &safety[0], &motion);
  if (matched < 18) {
    return false;
  }
  g_t41_telemetry.q[0] = q0;
  g_t41_telemetry.q[1] = q1;
  g_t41_telemetry.q[2] = q2;
  g_t41_telemetry.fk[0] = fk0;
  g_t41_telemetry.fk[1] = fk1;
  g_t41_telemetry.fk[2] = fk2;
  g_t41_telemetry.alpha = q2;
  g_t41_telemetry.coord_pitch = g_t41_telemetry.alpha;
  g_t41_telemetry.online_count = static_cast<uint8_t>(online);
  g_t41_telemetry.degraded[0] = static_cast<uint8_t>(d0);
  g_t41_telemetry.degraded[1] = static_cast<uint8_t>(d1);
  g_t41_telemetry.degraded[2] = static_cast<uint8_t>(d2);
  g_t41_telemetry.motion_active = motion != 0U;
  std::snprintf(g_t41_telemetry.safety_state, sizeof(g_t41_telemetry.safety_state), "%s", safety);
  (void)now_ms;

  const bool allow_joint_sync = !joint_traj_local_override_active(now_ms);
  if (allow_joint_sync) {
    bool arm_changed = false;
    for (int i = 0; i < 3; ++i) {
      const float next = clamp_joint_deg(g_t41_telemetry.q[i]);
      if (fabsf(g_cache_joints[i] - next) > 0.5F) {
        g_cache_joints[i] = next;
        g_joint_target_request[i] = next;
        arm_changed = true;
      }
    }
    if (arm_changed) {
      notify_joint_task();
    }
  }
  return true;
}

static void parse_safety_payload(const char *payload) {
  if (payload == nullptr) {
    return;
  }
  unsigned code = 0U;
  unsigned ack_required = 0U;
  char state[24] = {};
  const int matched = std::sscanf(payload,
                                  "schema=mros-qspi-safety-v1 code=%u state=%23s ack_required=%u",
                                  &code,
                                  &state[0],
                                  &ack_required);
  if (matched >= 2) {
    g_t41_telemetry.safety_code = static_cast<uint8_t>(code);
    g_t41_telemetry.error_code = static_cast<uint16_t>(code);
    std::snprintf(g_t41_telemetry.safety_state, sizeof(g_t41_telemetry.safety_state), "%s", state);
  }
}

static void parse_command_ack_payload(const char *payload) {
  if (payload == nullptr) {
    return;
  }
  unsigned ok = 0U;
  char reason[64] = {};
  const int matched = std::sscanf(payload,
                                  "schema=mros-qspi-command-ack-v1 ok=%u reason=\"%63[^\"]\"",
                                  &ok,
                                  &reason[0]);
  if (matched >= 1) {
    g_t41_telemetry.last_ack_ok = (ok != 0U);
    if (matched >= 2) {
      std::snprintf(g_t41_telemetry.last_ack_reason,
                    sizeof(g_t41_telemetry.last_ack_reason),
                    "%s",
                    reason);
    }
  }
}

static void reset_fragment_state() { g_fragments = {}; }

static void ingest_bulk_fragment(const T41_QSPI_FrameHeader &header,
                                 const uint8_t *payload,
                                 const size_t payload_len,
                                 const unsigned long now_ms) {
  if (header.frag_count == 0U || header.frag_count > kMaxFragmentCount ||
      header.frag_idx >= header.frag_count ||
      payload_len > T41_QSPI_MAX_PAYLOAD_BYTES) {
    ++spi_diag_fragment_errors;
    request_ack_response(true, header.seq, "fragment metadata invalid");
    return;
  }
  if (!g_fragments.used || g_fragments.msg_id != header.msg_id ||
      g_fragments.frag_count != header.frag_count) {
    reset_fragment_state();
    g_fragments.used = true;
    g_fragments.msg_id = header.msg_id;
    g_fragments.frag_count = header.frag_count;
    g_fragments.last_update_ms = now_ms;
  }
  const size_t offset =
      static_cast<size_t>(header.frag_idx) * T41_QSPI_MAX_PAYLOAD_BYTES;
  if ((offset + payload_len) > sizeof(g_fragments.payload)) {
    ++spi_diag_fragment_errors;
    request_ack_response(true, header.seq, "fragment overflow");
    return;
  }
  memcpy(&g_fragments.payload[offset], payload, payload_len);
  g_fragments.frag_lengths[header.frag_idx] = static_cast<uint16_t>(payload_len);
  g_fragments.received_mask |= (1UL << header.frag_idx);
  g_fragments.last_update_ms = now_ms;

  const uint32_t complete_mask =
      header.frag_count >= 32U ? 0xFFFFFFFFUL : ((1UL << header.frag_count) - 1UL);
  if ((g_fragments.received_mask & complete_mask) == complete_mask) {
    ++spi_diag_reassembled;
    reset_fragment_state();
    request_ack_response(false, header.seq, "fragments complete");
    return;
  }
  request_ack_response(false, header.seq, "fragment accepted");
}

static void handle_rx_frame(const T41_QSPI_FrameHeader &header,
                            const uint8_t *payload,
                            const size_t payload_len,
                            const unsigned long now_ms) {
  const unsigned long prev_packet_time = last_packet_time;
  const uint16_t frame_loop_ms =
      (prev_packet_time == 0UL || now_ms < prev_packet_time)
          ? 0U
          : static_cast<uint16_t>(now_ms - prev_packet_time);
  last_packet_time = now_ms;
  t41Connected = true;
  if (frame_loop_ms != 0U) {
    g_t41_telemetry.loop_ms = frame_loop_ms;
  }
  spi_diag_last_marker = static_cast<uint8_t>(header.type);
  spi_diag_last_seq = static_cast<uint8_t>(header.seq & 0xFFU);
  char payload_text[kDecodedPayloadBytes + 1U] = {};
  payload_to_cstr(payload, payload_len, payload_text, sizeof(payload_text));

  switch (static_cast<T41_QSPI_FrameType>(header.type)) {
    case T41_QSPI_FRAME_ACK:
      ++spi_diag_ack_frames;
      g_t41_telemetry.last_ack_seq = header.ack;
      parse_command_ack_payload(payload_text);
      if (g_pending_control.used && header.ack == g_pending_control.seq) {
        g_pending_control.used = false;
      }
      return;
    case T41_QSPI_FRAME_NACK:
      ++spi_diag_nack_frames;
      g_t41_telemetry.last_ack_seq = header.ack;
      parse_command_ack_payload(payload_text);
      if (g_pending_control.used && header.ack == g_pending_control.seq) {
        if (g_pending_control.retries >= kMaxCmdRetries) {
          g_pending_control.used = false;
          ++spi_diag_ack_timeouts;
        } else {
          g_pending_control.last_send_ms = 0UL;
        }
      }
      return;
    case T41_QSPI_FRAME_SNAPSHOT:
      (void)parse_snapshot_payload(payload_text, now_ms);
      return;
    case T41_QSPI_FRAME_HEARTBEAT: {
      unsigned long remote_ms = 0UL;
      char mode[20] = {};
      if (std::sscanf(payload_text, "schema=mros-qspi-heartbeat-v1 ms=%lu mode=%19s",
                      &remote_ms, &mode[0]) >= 1) {
        if (g_t41_telemetry.remote_ms != 0U && remote_ms >= g_t41_telemetry.remote_ms) {
          g_t41_telemetry.loop_ms =
              static_cast<uint16_t>(remote_ms - g_t41_telemetry.remote_ms);
        }
        g_t41_telemetry.remote_ms = static_cast<uint32_t>(remote_ms);
      }
      return;
    }
    case T41_QSPI_FRAME_SAFETY_EVENT:
      parse_safety_payload(payload_text);
      break;
    case T41_QSPI_FRAME_BULK_FRAGMENT:
      ingest_bulk_fragment(header, payload, payload_len, now_ms);
      return;
    case T41_QSPI_FRAME_COMMAND:
      if (spi_diag_last_command_seq == header.seq) {
        ++spi_diag_duplicate_cmd;
      }
      spi_diag_last_command_seq = header.seq;
      request_ack_response(false, header.seq, "command accepted");
      return;
    case T41_QSPI_FRAME_DIAG_LOG:
    default:
      break;
  }

  if ((header.flags & kFlagAckRequired) != 0U) {
    request_ack_response(false, header.seq, "ack");
  }
}

static bool encode_frame(T41_QSPI_FrameType type,
                         T41_QSPI_Lane lane,
                         const uint8_t flags,
                         const uint32_t ack,
                         const uint8_t *payload,
                         const size_t payload_len,
                         uint8_t *out_frame,
                         size_t *out_len,
                         uint32_t *out_seq) {
  if (payload_len > T41_QSPI_MAX_PAYLOAD_BYTES || out_frame == nullptr ||
      out_len == nullptr) {
    return false;
  }
  memset(out_frame, 0, kWireFrameBytes);
  T41_QSPI_FrameHeader header = {};
  header.magic = T41_QSPI_FRAME_MAGIC;
  header.version = T41_QSPI_PROTOCOL_VERSION;
  header.header_len = T41_QSPI_HEADER_BYTES;
  header.type = static_cast<uint8_t>(type);
  header.lane = static_cast<uint8_t>(lane);
  header.priority = static_cast<uint8_t>(lane);
  header.flags = flags;
  header.seq = ++g_next_tx_seq;
  header.ack = ack;
  header.msg_id = header.seq;
  header.frag_idx = 0U;
  header.frag_count = 1U;
  header.timestamp_us = static_cast<uint64_t>(mros::platform::mros_millis()) * 1000ULL;
  header.deadline_us = (lane == T41_QSPI_LANE_SAFETY) ? 1000U : 50000U;
  header.payload_len = static_cast<uint32_t>(payload_len);
  header.payload_crc32c = crc32c(payload, payload_len);
  header.header_crc32c = 0U;
  memcpy(out_frame, &header, sizeof(header));
  if (payload_len > 0U && payload != nullptr) {
    memcpy(out_frame + T41_QSPI_HEADER_BYTES, payload, payload_len);
  }
  header.header_crc32c = crc32c(out_frame, T41_QSPI_HEADER_BYTES);
  memcpy(out_frame, &header, sizeof(header));
  *out_len = align4(T41_QSPI_HEADER_BYTES + payload_len);
  if (*out_len > kWireFrameBytes) {
    return false;
  }
  if (out_seq != nullptr) {
    *out_seq = header.seq;
  }
  return true;
}

static bool queue_transaction() {
  const unsigned long now_ms = mros::platform::mros_millis();
  uint8_t local_frame[kWireFrameBytes] = {};
  size_t local_len = 0U;
  uint32_t local_seq = 0U;
  bool queued = false;
  bool waiting_for_ack = false;

  if (g_pending_ack_response) {
    char ack_payload[128] = {};
    std::snprintf(ack_payload,
                  sizeof(ack_payload),
                  "schema=mros-qspi-command-ack-v1 ok=%u reason=\"%s\"",
                  g_pending_ack_is_nack ? 0U : 1U,
                  g_pending_ack_reason);
    queued = encode_frame(g_pending_ack_is_nack ? T41_QSPI_FRAME_NACK : T41_QSPI_FRAME_ACK,
                          T41_QSPI_LANE_COMMAND,
                          0U,
                          g_pending_ack_target,
                          reinterpret_cast<const uint8_t *>(ack_payload),
                          strnlen(ack_payload, sizeof(ack_payload)),
                          local_frame,
                          &local_len,
                          &local_seq);
    g_pending_ack_response = false;
  } else if (g_pending_control.used) {
    if ((now_ms - g_pending_control.last_send_ms) >= kAckTimeoutMs) {
      if (g_pending_control.retries < kMaxCmdRetries) {
        ++g_pending_control.retries;
        ++spi_diag_retry_frames;
        memcpy(local_frame, g_pending_control.frame, g_pending_control.frame_len);
        local_len = g_pending_control.frame_len;
        local_seq = g_pending_control.seq;
        g_pending_control.last_send_ms = now_ms;
        queued = true;
      } else {
        g_pending_control.used = false;
        ++spi_diag_ack_timeouts;
      }
    } else {
      waiting_for_ack = true;
    }
  }

  if (!queued && !waiting_for_ack) {
    char payload[208] = {};
    std::snprintf(
        payload,
        sizeof(payload),
        "schema=mros-qspi-command-v1 turret=%.3f joints=[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f] "
        "gripper=%.3f motor=%u reset=%u cart=[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u]",
        g_cache_turret,
        g_cache_joints[0], g_cache_joints[1], g_cache_joints[2], g_cache_joints[3],
        g_cache_joints[4], g_cache_joints[5], g_cache_gripper,
        static_cast<unsigned>(g_cache_motor), static_cast<unsigned>(g_cache_reset),
        g_cache_target_x, g_cache_target_y, g_cache_target_z, g_cache_target_roll,
        g_cache_target_pitch, g_cache_target_yaw, g_cache_target_use_orientation ? 1U : 0U);
    queued = encode_frame(T41_QSPI_FRAME_COMMAND,
                          T41_QSPI_LANE_COMMAND,
                          static_cast<uint8_t>(kFlagAckRequired),
                          0U,
                          reinterpret_cast<const uint8_t *>(payload),
                          strnlen(payload, sizeof(payload)),
                          local_frame,
                          &local_len,
                          &local_seq);
    if (queued) {
      g_pending_control.used = true;
      g_pending_control.seq = local_seq;
      g_pending_control.retries = 0U;
      g_pending_control.last_send_ms = now_ms;
      g_pending_control.frame_len = local_len;
      memcpy(g_pending_control.frame, local_frame, local_len);
    }
    if (g_cache_reset == 1U) {
      g_cache_reset = 0U;
    }
  }

  if (!queued) {
    char heartbeat[96] = {};
    std::snprintf(heartbeat, sizeof(heartbeat),
                  "schema=mros-qspi-heartbeat-v1 ms=%lu mode=safe-1bit-dma",
                  static_cast<unsigned long>(now_ms));
    queued = encode_frame(T41_QSPI_FRAME_HEARTBEAT,
                          T41_QSPI_LANE_DIAG_LOG,
                          0U,
                          0U,
                          reinterpret_cast<const uint8_t *>(heartbeat),
                          strnlen(heartbeat, sizeof(heartbeat)),
                          local_frame,
                          &local_len,
                          &local_seq);
  }

  if (!queued) {
    return false;
  }

  memset(spi_tx_buf, 0, sizeof(spi_tx_buf));
  memcpy(spi_tx_buf, local_frame, local_len);

  memset(&trans, 0, sizeof(trans));
  trans.length = static_cast<uint32_t>(kWireFrameBytes * 8U);
  trans.tx_buffer = spi_tx_buf;
  trans.rx_buffer = spi_rx_buf;

  const esp_err_t err = spi_slave_queue_trans(SPI2_HOST, &trans, 0);
  if (err != ESP_OK) {
    if ((now_ms - g_last_spi_queue_fail_log_ms) > 250UL) {
      g_last_spi_queue_fail_log_ms = now_ms;
      mros_console.printf("[QSPI-S3] Queue failed: %s\n", esp_err_to_name(err));
    }
    return false;
  }
  spi_transaction_done = false;
  return true;
}

void IRAM_ATTR spi_s3_post_setup_cb(spi_slave_transaction_t * /*trans*/) {
  (void)mros::platform::mros_gpio_write(PIN_DATA_READY, true);
  if (PIN_TEENSY_IRQ >= 0) {
    (void)mros::platform::mros_gpio_write(PIN_TEENSY_IRQ, true);
  }
}

void IRAM_ATTR spi_s3_post_trans_cb(spi_slave_transaction_t * /*trans*/) {
  memcpy((void *)spi_rx_buf_safe, (void *)spi_rx_buf, sizeof(spi_rx_buf_safe));
  spi_new_data = true;
  spi_transaction_done = true;
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (g_spi_notify_task != nullptr) {
    vTaskNotifyGiveFromISR(g_spi_notify_task, &xHigherPriorityTaskWoken);
  }
  if (xHigherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
  if (PIN_TEENSY_IRQ >= 0) {
    (void)mros::platform::mros_gpio_write(PIN_TEENSY_IRQ, false);
  }
  (void)mros::platform::mros_gpio_write(PIN_DATA_READY, false);
}

void spi_slave_s3_init() {
  g_endpoint_mode = quad_lane_pins_valid() ? SpiS3EndpointMode::QuadReady4Bit
                                           : SpiS3EndpointMode::ClassicSpi1Bit;
  mros_console.printf(
      "[QSPI-S3] Init mode=%s SCK=%d MISO=%d MOSI=%d CS=%d WP=%d HD=%d ESP_READY=%d T41_READY=%d IRQ=%d RESET=%d\n",
      g_endpoint_mode == SpiS3EndpointMode::QuadReady4Bit ? "quad-ready-4bit" : "classic-1bit",
      PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS, PIN_SPI_WP,
      PIN_SPI_HD, PIN_DATA_READY, PIN_T41_READY, PIN_TEENSY_IRQ, PIN_TEENSY_RESET);
  planetary_pid_init(&g_turret_pid, 0.60f, 0.60f, 0.05f, 0.02f);
  g_turret_pid.out_min = -100.0f;
  g_turret_pid.out_max = 100.0f;
  g_turret_pid.i_limit = 0.0f;

  if (PIN_SPI_SCK < 0 || PIN_SPI_MISO < 0 || PIN_SPI_MOSI < 0 ||
      PIN_SPI_CS < 0) {
    mros_console.println("[QSPI-S3] Disabled: PIN_SPI_* not assigned.");
    return;
  }

  (void)mros::platform::mros_gpio_config(PIN_SPI_CS, mros::platform::GpioMode::InputPullup);
  (void)mros::platform::mros_gpio_config(PIN_SPI_SCK, mros::platform::GpioMode::InputPullup);
  (void)mros::platform::mros_gpio_config(PIN_SPI_MOSI, mros::platform::GpioMode::InputPullup);
  (void)mros::platform::mros_gpio_config(PIN_SPI_MISO, mros::platform::GpioMode::InputPullup);
  if (g_endpoint_mode == SpiS3EndpointMode::QuadReady4Bit) {
    (void)mros::platform::mros_gpio_config(PIN_SPI_WP, mros::platform::GpioMode::InputPullup);
    (void)mros::platform::mros_gpio_config(PIN_SPI_HD, mros::platform::GpioMode::InputPullup);
  }
  (void)mros::platform::mros_gpio_config(PIN_DATA_READY, mros::platform::GpioMode::Output);
  (void)mros::platform::mros_gpio_write(PIN_DATA_READY, false);
  (void)mros::platform::mros_gpio_config(PIN_T41_READY, mros::platform::GpioMode::InputPullup);
  if (PIN_TEENSY_IRQ >= 0) {
    (void)mros::platform::mros_gpio_config(PIN_TEENSY_IRQ, mros::platform::GpioMode::Output);
    (void)mros::platform::mros_gpio_write(PIN_TEENSY_IRQ, false);
  }
  if (PIN_TEENSY_RESET >= 0) {
    (void)mros::platform::mros_gpio_config(PIN_TEENSY_RESET, mros::platform::GpioMode::InputPullup);
  }

  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = PIN_SPI_MOSI;
  buscfg.miso_io_num = PIN_SPI_MISO;
  buscfg.sclk_io_num = PIN_SPI_SCK;
  buscfg.quadwp_io_num = (g_endpoint_mode == SpiS3EndpointMode::QuadReady4Bit) ? PIN_SPI_WP : -1;
  buscfg.quadhd_io_num = (g_endpoint_mode == SpiS3EndpointMode::QuadReady4Bit) ? PIN_SPI_HD : -1;
  buscfg.max_transfer_sz = static_cast<int>(kWireFrameBytes);

  spi_slave_interface_config_t slvcfg = {};
  slvcfg.mode = 0;
  slvcfg.spics_io_num = PIN_SPI_CS;
  slvcfg.queue_size = 1;
  slvcfg.flags = 0;
  slvcfg.post_setup_cb = spi_s3_post_setup_cb;
  slvcfg.post_trans_cb = spi_s3_post_trans_cb;

  const esp_err_t err = spi_slave_initialize(SPI2_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    mros_console.printf("[QSPI-S3] Init failed: %s\n", esp_err_to_name(err));
    return;
  }
  joint_traj_cancel(true);
  (void)queue_transaction();
}

static void process_rx() {
  const unsigned long now_ms = mros::platform::mros_millis();
  ++spi_diag_total;

  bool all_zero = true;
  bool all_ff = true;
  for (size_t i = 0; i < sizeof(spi_rx_buf_safe); ++i) {
    all_zero = all_zero && (spi_rx_buf_safe[i] == 0x00U);
    all_ff = all_ff && (spi_rx_buf_safe[i] == 0xFFU);
  }
  if (all_zero || all_ff) {
    return;
  }

  T41_QSPI_FrameHeader header = {};
  const uint8_t *payload = nullptr;
  size_t payload_len = 0U;
  const DecodeResult result = decode_frame(spi_rx_buf_safe,
                                           sizeof(spi_rx_buf_safe),
                                           &header,
                                           &payload,
                                           &payload_len);
  if (result == DecodeResult::Ok) {
    handle_rx_frame(header, payload, payload_len, now_ms);
    return;
  }

  if (result == DecodeResult::BadHeaderCrc || result == DecodeResult::BadPayloadCrc) {
    ++spi_diag_crc_err;
    spi_log_error(2, 0U, 0U);
    spi_diag_print_error("CRC32C", 0U, 0U);
  } else {
    ++spi_diag_marker_err;
    spi_log_error(1, 0U, 0U);
    spi_diag_print_error("HEADER", 0U, 0U);
  }
}

void spi_slave_s3_loop(unsigned long now) {
  spi_s3_service_comm(now);
  spi_s3_service_joint_traj(now);
  spi_s3_service_turret_pid(now);
}

void spi_s3_service_comm(unsigned long now) {
  if (spi_new_data) {
    spi_new_data = false;
    process_rx();
  }
  if (spi_transaction_done) {
    (void)queue_transaction();
  }
  if (PIN_TEENSY_RESET >= 0 && !mros::platform::mros_gpio_read(PIN_TEENSY_RESET)) {
    g_cache_reset = 1U;
    g_cache_motor = 0U;
  }
  if (!teensy_ready_line_high() || (now - last_packet_time > 5000UL)) {
    t41Connected = false;
  }
  g_device_status_code = build_device_status_code();
  sync_c3_failsafe_with_t41_link();
}

void spi_s3_service_joint_traj(unsigned long now) { joint_traj_update(now); }

void spi_s3_service_turret_pid(unsigned long now) {
  if (!t41Connected && g_cache_motor == 0) {
    if (!g_turret_output_lock) {
      turret_apply_speed_command(0);
    }
    turret_pid_reset_state(true);
    return;
  }
  turret_pid_update(now);
}

bool spi_s3_joint_traj_is_active() {
  return g_joint_traj.active || g_joint_traj.pending_replan;
}

// Setters (Web -> Logic)
void spi_s3_set_target_turret(float deg) {
  if (!std::isfinite(deg))
    return;
  g_cache_cartesian_active = false;
  float clamped = clampf(deg, -270.0f, 270.0f);
  if (fabsf(g_cache_turret - clamped) > 0.01f) {
    g_cache_turret = clamped;
    turret_pid_reset_state(true);
  }
}
void spi_s3_set_target_joint(int index, float deg) {
  if (index < 0 || index >= kArmJointCount)
    return;
  if (!std::isfinite(deg))
    return;
  g_cache_cartesian_active = false;

  const unsigned long now_ms = mros::platform::mros_millis();
  const float clamped = clamp_joint_deg(deg);
  g_joint_target_request[index] = clamped;
  g_last_local_joint_cmd_ms = now_ms;
  joint_traj_mark_target_updated(now_ms);
}

void spi_s3_set_target_cartesian(float x_mm, float y_mm, float z_mm,
                                 float roll_deg, float pitch_deg,
                                 float yaw_deg, bool use_orientation) {
  if (!std::isfinite(x_mm) || !std::isfinite(y_mm) || !std::isfinite(z_mm)) return;
  g_cache_target_x = x_mm;
  g_cache_target_y = y_mm;
  g_cache_target_z = z_mm;
  g_cache_target_roll = std::isfinite(roll_deg) ? roll_deg : 0.0f;
  g_cache_target_pitch = std::isfinite(pitch_deg) ? pitch_deg : 0.0f;
  g_cache_target_yaw = std::isfinite(yaw_deg) ? yaw_deg : 0.0f;
  g_cache_target_use_orientation = use_orientation;
  g_cache_cartesian_active = true;
}

void spi_s3_set_notify_task(TaskHandle_t task_handle) {
  g_spi_notify_task = task_handle;
}
void spi_s3_set_joint_task_handle(TaskHandle_t task_handle) {
  g_joint_task_handle = task_handle;
}
void spi_s3_set_target_gripper(float deg) {
  if (!std::isfinite(deg))
    return;
  g_cache_gripper = clamp_gripper_deg(deg);
  notify_joint_task();
}
void spi_s3_set_joint_traj_time_scale(float scale) {
  if (!std::isfinite(scale))
    return;
  g_joint_traj_time_scale = clampf(scale, kJointTrajScaleMin, kJointTrajScaleMax);

  // Yeni katsayi, mevcut hedefe kalan yolu ayni anda yeniden zamanlasin.
  if (g_joint_traj.active || g_joint_traj.pending_replan) {
    joint_traj_mark_target_updated(mros::platform::mros_millis());
  }
}
float spi_s3_get_joint_traj_time_scale() { return g_joint_traj_time_scale; }
void spi_s3_set_motor_power(uint8_t power) {
  g_cache_motor = (power != 0) ? 1 : 0;
  if (g_cache_motor != 0 && g_turret_output_lock) {
    g_turret_output_lock = false;
    mros_console.println(
        "[TURRET] output lock auto-cleared on motor power ON");
  }
  // Software lock only: PWM freeze. Do NOT toggle Hardware OE here.
  g_last_turret_speed_valid = false;
  if (g_cache_motor == 0) {
    notify_joint_task();
    if (!g_turret_output_lock) {
      turret_apply_speed_command(0);
    }
    turret_pid_reset_state(true);
  } else {
    notify_joint_task();
    turret_pid_reset_state(true);
  }
}
void spi_s3_set_reset_encoder() { g_cache_reset = 1; }
void spi_s3_set_turret_pid(float kp, float ki, float kd, float i_limit) {
  if (!std::isfinite(kp) || !std::isfinite(ki) || !std::isfinite(kd) || !std::isfinite(i_limit))
    return;
  g_turret_pid.Kp = clampf(kp, 0.0f, 100.0f);
  g_turret_pid.Ki = clampf(ki, 0.0f, 100.0f);
  g_turret_pid.Kd = clampf(kd, 0.0f, 100.0f);
  g_turret_pid.i_limit = clampf(i_limit, 0.0f, 500.0f);
  turret_pid_reset_state(true);
}
void spi_s3_set_turret_dspc(float dspc) {
  if (!std::isfinite(dspc))
    return;
  g_turret_dspc = clampf(dspc, 0.0f, 100.0f);
}
void spi_s3_set_turret_output_lock(bool lock) {
  g_turret_output_lock = lock;
  g_last_turret_speed_valid = false;
  if (lock && pca9685_is_ready()) {
    turret_apply_speed_command(0);
  }
  turret_pid_reset_state(true);
}
bool spi_s3_get_turret_output_lock() { return g_turret_output_lock; }

// Getters (Logic -> Web)
float spi_s3_get_turret_deg() { return g_cache_turret; }
float spi_s3_get_joint_deg(int index) {
  if (index < 0 || index >= 6)
    return 0.0f;
  return g_cache_joints[index];
}
uint8_t spi_s3_get_gripper() { return (uint8_t)g_cache_gripper; }
uint8_t spi_s3_get_motor_state() { return g_cache_motor; }
uint16_t spi_s3_get_error_code() { return g_t41_telemetry.error_code; }
uint16_t spi_s3_get_loop_ms() { return g_t41_telemetry.loop_ms; }
int16_t spi_s3_get_device_status_code() { return g_device_status_code; }
bool spi_s3_is_connected() { return t41Connected; }

// FK End-Effector coordinates (from Teensy QSPI snapshot)
float spi_s3_get_coord_x() { return g_t41_telemetry.fk[0]; }
float spi_s3_get_coord_y() { return g_t41_telemetry.fk[1]; }
float spi_s3_get_coord_z() { return g_t41_telemetry.fk[2]; }
float spi_s3_get_alpha() { return g_t41_telemetry.alpha; }
float spi_s3_get_coord_roll() { return g_t41_telemetry.coord_roll; }
float spi_s3_get_coord_pitch() {
  if (g_t41_telemetry.coord_pitch != 0.0f) return g_t41_telemetry.coord_pitch;
  return spi_s3_get_alpha();
}
float spi_s3_get_coord_yaw() { return g_t41_telemetry.coord_yaw; }
float spi_s3_get_turret_actual_deg() { return g_turret_actual_deg; }
float spi_s3_get_turret_pid_output() { return g_turret_pid.last_output; }
float spi_s3_get_turret_pid_error() { return g_turret_pid_error; }
void spi_s3_get_turret_pid(float *kp, float *ki, float *kd, float *i_limit) {
  if (kp)
    *kp = g_turret_pid.Kp;
  if (ki)
    *ki = g_turret_pid.Ki;
  if (kd)
    *kd = g_turret_pid.Kd;
  if (i_limit)
    *i_limit = g_turret_pid.i_limit;
}
float spi_s3_get_turret_dspc() { return g_turret_dspc; }

uint32_t spi_s3_get_total_transactions() { return spi_diag_total; }
uint32_t spi_s3_get_crc_errors() { return spi_diag_crc_err; }
uint32_t spi_s3_get_marker_errors() { return spi_diag_marker_err; }
uint32_t spi_s3_get_ack_frames() { return spi_diag_ack_frames; }
uint32_t spi_s3_get_nack_frames() { return spi_diag_nack_frames; }
uint32_t spi_s3_get_retry_frames() { return spi_diag_retry_frames; }
uint32_t spi_s3_get_ack_timeouts() { return spi_diag_ack_timeouts; }
uint8_t spi_s3_get_last_rx_marker() { return spi_diag_last_marker; }
uint8_t spi_s3_get_last_rx_seq() { return spi_diag_last_seq; }
SpiS3EndpointMode spi_s3_endpoint_mode() { return g_endpoint_mode; }
const char* spi_s3_endpoint_mode_name() {
  return g_endpoint_mode == SpiS3EndpointMode::QuadReady4Bit
             ? "quad-ready-4bit"
             : "classic-1bit";
}
bool spi_s3_is_clock_prep_safe() { return spi_clock_prep_safe_locked(); }
void spi_s3_reset_error_counters() {
  spi_diag_crc_err = 0;
  spi_diag_marker_err = 0;
  spi_err_log_idx = 0;
  spi_err_log_count = 0;
  spi_suppressed_diag = 0;
}

String spi_s3_get_error_log_json() {
  String json = "[";
  bool first = true;

  // Walk the ring buffer from oldest to newest
  int start = (spi_err_log_count < SPI_ERR_LOG_SIZE)
                  ? 0
                  : spi_err_log_idx; // oldest entry index
  int count = (spi_err_log_count < SPI_ERR_LOG_SIZE) ? spi_err_log_count
                                                      : SPI_ERR_LOG_SIZE;

  for (int i = 0; i < count; i++) {
    int idx = (start + i) % SPI_ERR_LOG_SIZE;
    SPI_ErrLogEntry_t *e = &spi_err_log[idx];

    if (!first)
      json += ",";
    first = false;

    json += "{";
    json += "\"ts\":" + String(e->timestamp) + ",";
    json += "\"type\":\"" + String(e->err_type == 1 ? "MARKER" : "CRC") + "\",";

    // Format marker as 2-digit hex
    char hex_marker[5];
    snprintf(hex_marker, sizeof(hex_marker), "0x%02X", e->rx_marker);
    json += "\"marker\":\"" + String(hex_marker) + "\",";
    json += "\"seq\":" + String(e->rx_seq) + ",";

    // Raw first 4 bytes as hex string
    char raw[16];
    snprintf(raw, sizeof(raw), "%02X %02X %02X %02X", e->rx_bytes[0],
             e->rx_bytes[1], e->rx_bytes[2], e->rx_bytes[3]);
    json += "\"raw\":\"" + String(raw) + "\",";

    char crc_buf[32];
    snprintf(crc_buf, sizeof(crc_buf), "0x%08X", (unsigned int)e->expected_crc);
    json += "\"exp_crc\":\"" + String(crc_buf) + "\",";
    snprintf(crc_buf, sizeof(crc_buf), "0x%08X", (unsigned int)e->actual_crc);
    json += "\"act_crc\":\"" + String(crc_buf) + "\"";
    json += "}";
  }

  json += "]";
  return json;
}


