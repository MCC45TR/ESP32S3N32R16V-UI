#ifndef PROTOCOL_DEF_H
#define PROTOCOL_DEF_H

#include <stdint.h>
#include <stddef.h>

/* ========= PROTOCOL DEFINITIONS ========= */

/* ========= QSPI (ESP32-S3 <-> Teensy4.1) MQ FRAME ========= */
#define T41_QSPI_FRAME_MAGIC 0x4D51U
#define T41_QSPI_PROTOCOL_VERSION 0x01U
#define T41_QSPI_HEADER_BYTES 48U
#define T41_QSPI_MAX_PAYLOAD_BYTES 208U
#define T41_QSPI_MAX_FRAME_BYTES (T41_QSPI_HEADER_BYTES + T41_QSPI_MAX_PAYLOAD_BYTES)

typedef enum {
  T41_QSPI_LANE_SAFETY = 0,
  T41_QSPI_LANE_COMMAND = 1,
  T41_QSPI_LANE_BULK_CURVE = 2,
  T41_QSPI_LANE_RT_SNAPSHOT = 3,
  T41_QSPI_LANE_DIAG_LOG = 4,
} T41_QSPI_Lane;

typedef enum {
  T41_QSPI_FRAME_HEARTBEAT = 0,
  T41_QSPI_FRAME_SNAPSHOT = 1,
  T41_QSPI_FRAME_SAFETY_EVENT = 2,
  T41_QSPI_FRAME_COMMAND = 3,
  T41_QSPI_FRAME_ACK = 4,
  T41_QSPI_FRAME_NACK = 5,
  T41_QSPI_FRAME_BULK_FRAGMENT = 6,
  T41_QSPI_FRAME_DIAG_LOG = 7,
} T41_QSPI_FrameType;

#define T41_QSPI_FLAG_ACK_REQUIRED 0x01U
#define T41_QSPI_FLAG_FRAGMENTED   0x02U
#define T41_QSPI_FLAG_SAFETY       0x04U
#define T41_QSPI_FLAG_QUAD_DATA    0x08U

#pragma pack(push, 1)
typedef struct {
  uint16_t magic;
  uint8_t version;
  uint8_t header_len;
  uint8_t type;
  uint8_t lane;
  uint8_t priority;
  uint8_t flags;
  uint32_t seq;
  uint32_t ack;
  uint32_t msg_id;
  uint16_t frag_idx;
  uint16_t frag_count;
  uint64_t timestamp_us;
  uint32_t deadline_us;
  uint32_t payload_len;
  uint32_t payload_crc32c;
  uint32_t header_crc32c;
} T41_QSPI_FrameHeader;
#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(T41_QSPI_FrameHeader) == T41_QSPI_HEADER_BYTES,
              "T41_QSPI_FrameHeader must remain 48 bytes");
#endif

/* ========= SPI5 (ESP32-S3) PROTOCOL ========= */
/* Legacy fixed 64-byte layout (retained for compatibility paths that are
 * outside the new T41-QSPI transport). */
#define SPI5_FRAME_SIZE 64
#define SPI5_START_MARKER_STM_TO_ESP 0xAA
#define SPI5_START_MARKER_ESP_TO_STM 0xBB

// Legacy status packet (historical S3 peer -> S3 bridge)
#pragma pack(push, 1)
typedef struct {
  uint8_t start_marker;      // 0: 0xAA
  uint8_t sequence_id;       // 1: Sequence/Heartbeat
  uint16_t loop_duration_ms; // 2-3: peer main loop duration
  uint16_t turret_angle;     // 4-5: Encoded Turret Angle
  uint16_t joint_angles[6];  // 6-17: Servo Angles
  uint8_t gripper_angle;     // 18: Gripper Angle
  uint8_t motor_state;       // 19: Motor Power State
  uint16_t error_code;       // 20-21: System Error Code
  uint16_t coord_x;          // 22-23: End Effector X
  uint16_t coord_y;          // 24-25: End Effector Y
  uint16_t coord_z;          // 26-27: End Effector Z
  uint16_t alpha_angle;      // 28-29: Tool Alpha Angle
  uint16_t last_ack_id;      // 30-31: ACK for received commands
  uint8_t protocol_ver;      // 32: Protocol Version
  int16_t coord_roll;        // 33-34: End Effector Roll * 10
  int16_t coord_pitch;       // 35-36: End Effector Pitch * 10
  int16_t coord_yaw;         // 37-38: End Effector Yaw * 10
  uint8_t reserved[21];      // 39-59: Padding for future use
  uint32_t crc32;            // 60-63: CRC32 of bytes 0-59
} SPI_Packet_STM_to_ESP;
#pragma pack(pop)

// Legacy control packet (historical S3 bridge -> peer)
#pragma pack(push, 1)
typedef struct {
  uint8_t start_marker;      // 0: 0xBB
  uint8_t sequence_id;       // 1: Sequence/Heartbeat
  uint16_t command_id;       // 2-3: Unique Command ID
  uint8_t control_mode;      // 4: Bit0=Coords, Bit1=Estop
  uint8_t reset_encoder;     // 5: 1=Reset Turret Encoder
  uint16_t target_turret;    // 6-7: Target Turret Angle
  uint16_t target_joints[6]; // 8-19: Target Servo Angles
  uint8_t target_gripper;    // 20: Target Gripper Angle
  uint8_t motor_cmd;         // 21: 1=ON, 0=OFF
  uint8_t padding_byte;      // 22: Alignment byte
  int32_t target_x;          // 23-26: IK Target X * 10
  int32_t target_y;          // 27-30: IK Target Y * 10
  int32_t target_z;          // 31-34: IK Target Z * 10
  uint16_t target_alpha;     // 35-36: IK Target Alpha
  uint8_t protocol_ver;      // 37: Protocol Version
  int32_t reserved_old_c3;   // 38-41: Was encoder_raw_c3, now unused/reserved
  int16_t target_roll;       // 42-43: IK Target Roll * 10
  int16_t target_pitch;      // 44-45: IK Target Pitch * 10
  int16_t target_yaw;        // 46-47: IK Target Yaw * 10
  uint8_t reserved[12];      // 48-59: Padding
  uint32_t crc32;            // 60-63: CRC32 of bytes 0-59
} SPI_Packet_ESP_to_STM;
#pragma pack(pop)

/* ========= SPI4 (ESP32-C3) PROTOCOL ========= */
#define SPI4_FRAME_SIZE 16

// Legacy peer -> ESP32-C3 (RxPacket - Commands)
#pragma pack(push, 1)
typedef struct {
  uint8_t reset_turret;      // 0: 0xA5 = Reset Encoder
  int8_t failsafe_option;    // 1: 0..3 failsafe selection for C3
  uint8_t cmd_flags;         // 2: Command Flags
  int32_t pid_setpoint_x100; // 3-6: PID Target Angle * 100
  uint8_t reserved[7];       // 7-13: Reserved
  uint8_t padding[2];        // 14-15: Alignment Padding
} SPI_Packet_STM_to_C3;
#pragma pack(pop)

// Legacy ESP32-C3 -> peer (TxPacket - Encoder Data)
#pragma pack(push, 1)
typedef struct {
  int32_t position_x100; // 0-3: Position * 100 (e.g., 9000 = 90.00 deg)
  int16_t speed_x10;     // 4-5: Speed * 10
  int16_t accel_x10;     // 6-7: Acceleration * 10
  uint8_t seq_id;        // 8: Sequence ID (0-255)
  uint8_t stat;          // 9: Status (1 = OK)
  uint16_t timestamp_ms; // 10-11: C3 Millis % 65536
  uint8_t direction;     // 12: Direction encoding
  uint16_t crc16;        // 13-14: CRC16-CCITT of bytes 0-12
  uint8_t padding;       // 15: Alignment Padding
} SPI_Packet_C3_to_STM;
#pragma pack(pop)

/* ========= UART1 (ESP32-S3 <-> Teensy4.1) PROTOCOL ========= */
#define UART_HEADER_WIFI_STAT 0xAA
#define UART_HEADER_WIFI_STAT_2 0x41
#define UART_TYPE_WIFI_STAT 0x02
#define UART_LINK_PROTOCOL_VERSION 0x01
#define UART_PEER_TEENSY41 0x41

#pragma pack(push, 1)
typedef struct {
  uint8_t header0;       // 0: 0xAA
  uint8_t header1;       // 1: 0x41
  uint8_t protocol_ver;  // 2: UART_LINK_PROTOCOL_VERSION
  uint8_t type;          // 3: UART_TYPE_WIFI_STAT
  uint8_t payload_len;   // 4: payload bytes after link header
  uint8_t peer_id;       // 5: UART_PEER_TEENSY41
  uint8_t wifi_status;   // 6: 0=Disconnected, 1=Connected, 2=Connecting
  uint8_t reserved0;     // 7
  uint8_t ip[4];         // 8-11: IP Address
  char ssid[20];         // 12-31: Network Name
  char user[20];         // 32-51: Active Web User
  int8_t rssi;           // 52: Signal Strength
  uint8_t crc8;          // 53: CRC8 of bytes 0-52
} UART_Wifi_Status_t;    // Total: 54 bytes
#pragma pack(pop)

#endif // PROTOCOL_DEF_H
