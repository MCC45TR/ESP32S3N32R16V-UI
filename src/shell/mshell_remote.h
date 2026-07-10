#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mros::shell {
struct ShellState;
}

namespace mros::shell::remote {

enum class Target : uint8_t {
  None = 0,
  S3,
  T41,
  C3,
  All,
};

enum class BridgeMode : uint8_t {
  Off = 0,
  Listen,
  On,
};

enum class FsMount : uint8_t {
  T41 = 0,
  T41Sdcard,
};

struct FsMountSnapshot {
  FsMount mount;
  const char* name;
  const char* root;
  const char* provider;
  bool mounted;
  bool remote;
  bool writable;
  bool bridge_on;
  bool peer_seen;
  bool protocol_ready;
  const char* peer_status;
  const char* error_code;
  uint32_t mounted_ms;
};

struct RemoteTunnelMetrics {
  uint32_t remote_bin_frames = 0;
  uint32_t remote_bin_bytes = 0;
  uint32_t remote_text_frames = 0;
  uint32_t remote_text_bytes = 0;
  uint32_t remote_fallbacks = 0;
  uint32_t remote_cobs_decode_errors = 0;
  uint32_t remote_rx_drops = 0;
  bool remote_cap_msh1 = false;
};

struct RemoteDiagSnapshot {
  uint32_t ping_sent = 0;
  uint32_t ping_recv = 0;
  uint32_t ping_timeout = 0;
  uint32_t rtt_min_ms = 0;
  uint32_t rtt_avg_ms = 0;
  uint32_t rtt_max_ms = 0;
  uint32_t last_clock_ack_hz = 0;
  uint32_t last_clock_ack_nonce = 0;
  bool last_clock_ack_ok = false;
  bool last_clock_ack_seen = false;
};

const char* target_name(Target target);
bool parse_target(const std::string& text, Target* out_target);
const char* bridge_mode_name(BridgeMode mode);
bool parse_bridge_mode(const std::string& text, BridgeMode* out_mode);
BridgeMode bridge_mode();
bool set_bridge_mode(BridgeMode mode);

Target active_target(const ShellState& state);
void set_active_target(ShellState& state, Target target);
void clear_active_target(ShellState& state);
std::string prompt_suffix(const ShellState& state);

bool connect_session(ShellState& state, Target target, std::string* message);
bool disconnect_session(ShellState& state, Target target, std::string* message);
std::string status_report(Target target, const ShellState* active_state);
std::string devices_report(const ShellState* active_state);
void reset_bridge_state();
bool execute_active_line(ShellState& state, const std::string& line, std::string* output, std::string* error);

void note_plain_uart_activity();
bool handle_uart_line(const char* line);
bool handle_uart_binary_frame(const uint8_t* data, size_t len);
void note_uart_binary_decode_error();
void get_tunnel_metrics(RemoteTunnelMetrics* out_metrics);
void reset_diag_metrics();
void get_diag_snapshot(RemoteDiagSnapshot* out_snapshot);
bool send_diag_ping(uint32_t id, uint32_t t_ms);
bool send_diag_clock_prep(uint32_t hz, uint32_t nonce);
bool send_diag_clock_commit(uint32_t hz, uint32_t nonce);
bool send_diag_clock_rollback(uint32_t hz, uint32_t nonce);
bool consume_last_clock_ack(uint32_t* out_hz, uint32_t* out_nonce, bool* out_ok);

bool fs_parse_mount(const std::string& text, FsMount* out_mount);
bool fs_mount_for_path(const std::string& path, FsMount* out_mount);
bool fs_is_remote_path(const std::string& path);
const char* fs_mount_name(FsMount mount);
const char* fs_mount_root(FsMount mount);
const char* fs_provider_name(FsMount mount);
void fs_snapshot(FsMount mount, FsMountSnapshot* out_snapshot);
std::string fs_mounts_json(bool include_local, bool local_mounted);
bool fs_mount(FsMount mount, std::string* message);
bool fs_umount(FsMount mount, std::string* message);
void fs_umount_all();
std::string fs_list_json(const std::string& path, size_t offset, size_t limit);
std::string fs_error_json(const std::string& path, const char* op, const char* code, const char* message);

}  // namespace mros::shell::remote
