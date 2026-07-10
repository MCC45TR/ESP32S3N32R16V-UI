#include "src/shell/mshell_remote.h"

#include "src/platform/mros_time.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "src/comm_interfaces/uart/uart_cobs.h"
#include "src/comm_interfaces/spi/spi_t41_link.h"
#include "src/platform/mros_nvs.h"
#include "src/shell/mros_shell.h"
#include "src/shell/mros_shell_internal.h"

namespace mros::shell::remote {
namespace {

constexpr const char* kProtoPrefix = "MSHELL2:";
constexpr const char* kLegacyProtoPrefix = "MSHELL:";
constexpr const char* kBridgeNvsNamespace = "security";
constexpr const char* kBridgeNvsKey = "uart_shell";
constexpr uint32_t kRemoteExecTimeoutMs = 1500U;
constexpr size_t kChunkRawBytes = 96U;
constexpr size_t kRemoteOutputMaxBytes = 8192U;
constexpr size_t kMsh1HeaderLen = 32U;
constexpr uint8_t kMsh1Version = 1U;
constexpr uint8_t kMsh1TypeOpen = 1U;
constexpr uint8_t kMsh1TypeStdout = 2U;
constexpr uint8_t kMsh1TypeExec = 3U;
constexpr uint8_t kMsh1TypeFinal = 4U;
constexpr uint8_t kMsh1TypeError = 5U;
constexpr uint8_t kMsh1TypeClose = 6U;
constexpr uint8_t kMsh1TypeHello = 10U;
constexpr uint8_t kMsh1TypeFsCaps = 11U;

struct PeerState {
  Target target = Target::None;
  const char* route = "n/a";
  bool peer_seen = false;
  bool peer_protocol_seen = false;
  bool session_open = false;
  bool session_pending = false;
  bool exec_pending = false;
  bool exec_ready = false;
  bool msh1_ready = false;
  uint32_t session_id = 0U;
  int last_exit_code = 0;
  unsigned long last_tx_ms = 0UL;
  unsigned long last_rx_ms = 0UL;
  std::string rx_output;
  std::string detail;
  ShellSession* local_session = nullptr;
};

SemaphoreHandle_t g_remote_mutex = nullptr;
uint32_t g_next_session_id = 1U;
bool g_bridge_loaded = false;
BridgeMode g_bridge_mode = BridgeMode::Off;
PeerState g_s3_peer {Target::S3, "local-loopback", false, false, false, false, false, false, false,
                     0U, 0, 0UL, 0UL, {}, {}, nullptr};
PeerState g_t41_peer {Target::T41, "uart1-direct", false, false, false, false, false, false, false,
                     0U, 0, 0UL, 0UL, {}, {}, nullptr};
PeerState g_c3_peer {Target::C3, "disabled", false, false, false, false, false, false, false,
                     0U, 0, 0UL, 0UL, {}, {}, nullptr};

struct InboundSession {
  Target source = Target::None;
  uint32_t session_id = 0U;
  ShellSession* shell = nullptr;
};

InboundSession g_inbound_t41 {};
InboundSession g_inbound_c3 {};

struct RemoteFsState {
  FsMount mount = FsMount::T41;
  bool mounted = false;
  bool writable = false;
  bool protocol_ready = false;
  uint32_t mounted_ms = 0U;
  std::string last_error = "REMOTE_NOT_MOUNTED";
};

RemoteFsState g_fs_t41 {FsMount::T41, false, false, false, 0U, "REMOTE_NOT_MOUNTED"};
RemoteFsState g_fs_t41_sd {FsMount::T41Sdcard, false, false, false, 0U, "REMOTE_NOT_MOUNTED"};
RemoteTunnelMetrics g_tunnel_metrics {};
constexpr uint32_t kDiagPingTimeoutMs = 1500U;
constexpr size_t kDiagPendingSlots = 24U;

struct PendingPing {
  bool used = false;
  uint32_t id = 0U;
  uint32_t sent_ms = 0U;
};

PendingPing g_diag_pending[kDiagPendingSlots] {};
uint32_t g_diag_ping_sent = 0U;
uint32_t g_diag_ping_recv = 0U;
uint32_t g_diag_ping_timeout = 0U;
uint32_t g_diag_rtt_min_ms = 0xFFFFFFFFUL;
uint32_t g_diag_rtt_max_ms = 0U;
uint64_t g_diag_rtt_sum_ms = 0ULL;
uint32_t g_diag_last_clock_ack_hz = 0U;
uint32_t g_diag_last_clock_ack_nonce = 0U;
bool g_diag_last_clock_ack_ok = false;
bool g_diag_last_clock_ack_seen = false;

bool ensure_mutex() {
  if (g_remote_mutex != nullptr) {
    return true;
  }
  g_remote_mutex = xSemaphoreCreateMutex();
  return g_remote_mutex != nullptr;
}

bool lock_remote(const TickType_t timeout = pdMS_TO_TICKS(100)) {
  return ensure_mutex() && xSemaphoreTake(g_remote_mutex, timeout) == pdTRUE;
}

void unlock_remote() {
  if (g_remote_mutex != nullptr) {
    xSemaphoreGive(g_remote_mutex);
  }
}

PeerState* peer_state_for(const Target target) {
  switch (target) {
    case Target::S3:
      return &g_s3_peer;
    case Target::T41:
      return &g_t41_peer;
    case Target::C3:
      return &g_c3_peer;
    case Target::All:
    case Target::None:
    default:
      return nullptr;
  }
}

InboundSession* inbound_session_for(const Target source) {
  switch (source) {
    case Target::T41:
      return &g_inbound_t41;
    case Target::C3:
      return &g_inbound_c3;
    case Target::S3:
    case Target::All:
    case Target::None:
    default:
      return nullptr;
  }
}

uint8_t target_id(const Target target) {
  switch (target) {
    case Target::S3:
      return 1U;
    case Target::T41:
      return 2U;
    case Target::C3:
      return 3U;
    case Target::All:
      return 4U;
    case Target::None:
    default:
      return 0U;
  }
}

Target target_from_id(const uint8_t id) {
  switch (id) {
    case 1U:
      return Target::S3;
    case 2U:
      return Target::T41;
    case 3U:
      return Target::C3;
    case 4U:
      return Target::All;
    default:
      return Target::None;
  }
}

uint32_t msh1_get_u32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) |
         (static_cast<uint32_t>(data[offset + 1U]) << 8U) |
         (static_cast<uint32_t>(data[offset + 2U]) << 16U) |
         (static_cast<uint32_t>(data[offset + 3U]) << 24U);
}

void msh1_put_u16(uint8_t* out, const size_t offset, const uint16_t value) {
  out[offset] = static_cast<uint8_t>(value & 0xFFU);
  out[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

void msh1_put_u32(uint8_t* out, const size_t offset, const uint32_t value) {
  out[offset] = static_cast<uint8_t>(value & 0xFFU);
  out[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  out[offset + 2U] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  out[offset + 3U] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

std::string lower_copy(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

bool parse_u32(const std::string& text, uint32_t* out_value) {
  if (out_value == nullptr || text.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out_value = static_cast<uint32_t>(parsed);
  return true;
}

bool parse_bool01(const std::string& text, bool* out_value) {
  if (out_value == nullptr) {
    return false;
  }
  if (text == "0") {
    *out_value = false;
    return true;
  }
  if (text == "1") {
    *out_value = true;
    return true;
  }
  return false;
}

void load_bridge_mode_locked() {
  if (g_bridge_loaded) {
    return;
  }
  g_bridge_loaded = true;
  uint8_t raw = 0U;
  mros::platform::NvsNamespace ns;
  if (ns.open(kBridgeNvsNamespace, true,
              mros::platform::NvsPartitionMode::UserPartitionsThenDefault) &&
      ns.get_u8(kBridgeNvsKey, &raw) && raw <= static_cast<uint8_t>(BridgeMode::On)) {
    g_bridge_mode = static_cast<BridgeMode>(raw);
  } else {
    g_bridge_mode = BridgeMode::Off;
  }
}

bool save_bridge_mode(const BridgeMode mode) {
  mros::platform::NvsNamespace ns;
  if (!ns.open(kBridgeNvsNamespace, false,
               mros::platform::NvsPartitionMode::UserPartitionsThenDefault)) {
    return false;
  }
  return ns.set_u8(kBridgeNvsKey, static_cast<uint8_t>(mode));
}

std::vector<std::string> split_fields(const std::string& text, const size_t max_parts) {
  std::vector<std::string> parts;
  size_t start = 0U;
  while (start <= text.size()) {
    if (parts.size() + 1U >= max_parts) {
      parts.push_back(text.substr(start));
      return parts;
    }
    const size_t pos = text.find(':', start);
    if (pos == std::string::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, pos - start));
    start = pos + 1U;
  }
  return parts;
}

char hex_digit(const uint8_t value) {
  return static_cast<char>(value < 10U ? ('0' + value) : ('a' + (value - 10U)));
}

std::string hex_encode(const std::string& text) {
  std::string out;
  out.resize(text.size() * 2U);
  for (size_t i = 0U; i < text.size(); ++i) {
    const uint8_t value = static_cast<uint8_t>(text[i]);
    out[(i * 2U) + 0U] = hex_digit((value >> 4U) & 0x0FU);
    out[(i * 2U) + 1U] = hex_digit(value & 0x0FU);
  }
  return out;
}

int hex_value(const char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

bool hex_decode(const std::string& text, std::string* out_text) {
  if (out_text == nullptr || (text.size() % 2U) != 0U) {
    return false;
  }
  out_text->clear();
  out_text->reserve(text.size() / 2U);
  for (size_t i = 0U; i < text.size(); i += 2U) {
    const int hi = hex_value(text[i]);
    const int lo = hex_value(text[i + 1U]);
    if (hi < 0 || lo < 0) {
      out_text->clear();
      return false;
    }
    out_text->push_back(static_cast<char>((hi << 4U) | lo));
  }
  return true;
}

bool send_proto_line(const std::string& line) {
  const bool ok = uart1_cobs_send_text_line(line.c_str());
  if (ok) {
    g_tunnel_metrics.remote_text_frames++;
    g_tunnel_metrics.remote_text_bytes += static_cast<uint32_t>(line.size() + 1U);
  }
  return ok;
}

uint32_t diag_now_ms() {
  return mros::platform::mros_millis();
}

void diag_sweep_timeouts_locked() {
  const uint32_t now_ms = diag_now_ms();
  for (PendingPing& slot : g_diag_pending) {
    if (!slot.used) continue;
    if ((now_ms - slot.sent_ms) > kDiagPingTimeoutMs) {
      slot.used = false;
      ++g_diag_ping_timeout;
    }
  }
}

int diag_find_pending_locked(const uint32_t id) {
  for (size_t i = 0U; i < kDiagPendingSlots; ++i) {
    if (g_diag_pending[i].used && g_diag_pending[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

int diag_find_free_pending_locked() {
  for (size_t i = 0U; i < kDiagPendingSlots; ++i) {
    if (!g_diag_pending[i].used) return static_cast<int>(i);
  }
  return -1;
}

void diag_mark_ping_sent_locked(const uint32_t id, const uint32_t sent_ms) {
  diag_sweep_timeouts_locked();
  int slot = diag_find_pending_locked(id);
  if (slot < 0) slot = diag_find_free_pending_locked();
  if (slot < 0) slot = 0;
  g_diag_pending[static_cast<size_t>(slot)].used = true;
  g_diag_pending[static_cast<size_t>(slot)].id = id;
  g_diag_pending[static_cast<size_t>(slot)].sent_ms = sent_ms;
  ++g_diag_ping_sent;
}

void diag_mark_ping_recv_locked(const uint32_t id, const uint32_t echoed_ms) {
  diag_sweep_timeouts_locked();
  int slot = diag_find_pending_locked(id);
  uint32_t rtt_ms = 0U;
  if (slot >= 0) {
    const uint32_t sent_ms = g_diag_pending[static_cast<size_t>(slot)].sent_ms;
    g_diag_pending[static_cast<size_t>(slot)].used = false;
    rtt_ms = diag_now_ms() - sent_ms;
  } else {
    rtt_ms = diag_now_ms() - echoed_ms;
  }
  ++g_diag_ping_recv;
  g_diag_rtt_sum_ms += rtt_ms;
  if (rtt_ms < g_diag_rtt_min_ms) g_diag_rtt_min_ms = rtt_ms;
  if (rtt_ms > g_diag_rtt_max_ms) g_diag_rtt_max_ms = rtt_ms;
}

void handle_diag_line_locked(const char* line) {
  if (line == nullptr || std::strncmp(line, "MSHELL:DIAG:", 12) != 0) return;
  const char* payload = line + 12;
  if (std::strncmp(payload, "PING:", 5) == 0) {
    uint32_t id = 0U;
    uint32_t t_ms = 0U;
    if (std::sscanf(payload, "PING:%lu:%lu", &id, &t_ms) >= 2) {
      char out[96] = {};
      std::snprintf(out, sizeof(out), "MSHELL:DIAG:PONG:%lu:%lu:OK",
                    static_cast<unsigned long>(id),
                    static_cast<unsigned long>(t_ms));
      (void)send_proto_line(out);
    }
    return;
  }
  if (std::strncmp(payload, "PONG:", 5) == 0) {
    uint32_t id = 0U;
    uint32_t t_ms = 0U;
    if (std::sscanf(payload, "PONG:%lu:%lu", &id, &t_ms) >= 2) {
      diag_mark_ping_recv_locked(id, t_ms);
    }
    return;
  }
  if (std::strncmp(payload, "CLOCK_PREP:", 11) == 0) {
    uint32_t hz = 0U;
    uint32_t nonce = 0U;
    if (std::sscanf(payload, "CLOCK_PREP:%lu:%lu", &hz, &nonce) == 2) {
      const char* prep_result = "OK";
      if (hz == 0U) {
        prep_result = "ERR";
      } else if (!spi_s3_is_clock_prep_safe()) {
        prep_result = "BUSY";
      }
      char out[128] = {};
      std::snprintf(out, sizeof(out), "MSHELL:DIAG:CLOCK_ACK:%lu:%lu:%s",
                    static_cast<unsigned long>(hz),
                    static_cast<unsigned long>(nonce),
                    prep_result);
      (void)send_proto_line(out);
    }
    return;
  }
  if (std::strncmp(payload, "CLOCK_ACK:", 10) == 0) {
    char result[12] = {};
    uint32_t hz = 0U;
    uint32_t nonce = 0U;
    if (std::sscanf(payload, "CLOCK_ACK:%lu:%lu:%11s", &hz, &nonce, result) == 3) {
      g_diag_last_clock_ack_hz = hz;
      g_diag_last_clock_ack_nonce = nonce;
      g_diag_last_clock_ack_ok = std::strcmp(result, "OK") == 0;
      g_diag_last_clock_ack_seen = true;
    }
    return;
  }
}

bool send_msh1_frame(const Target source,
                     const Target dest,
                     const uint32_t session_id,
                     const uint8_t type,
                     const uint32_t seq,
                     const uint8_t* payload,
                     const size_t payload_len) {
  if (payload_len > 1024U) {
    return false;
  }
  uint8_t frame[kMsh1HeaderLen + 4U + 1024U] = {};
  std::memcpy(frame, "MSH1", 4U);
  frame[4] = kMsh1Version;
  frame[5] = static_cast<uint8_t>(kMsh1HeaderLen);
  frame[6] = type;
  frame[7] = 0x03U;
  msh1_put_u32(frame, 8U, seq);
  msh1_put_u32(frame, 12U, 0U);
  msh1_put_u32(frame, 16U, 65536U);
  msh1_put_u32(frame, 20U, session_id);
  msh1_put_u16(frame, 24U, target_id(source));
  msh1_put_u16(frame, 26U, target_id(dest));
  msh1_put_u32(frame, 28U, static_cast<uint32_t>(payload_len + 4U));
  frame[kMsh1HeaderLen + 0U] = target_id(source);
  frame[kMsh1HeaderLen + 1U] = target_id(dest);
  frame[kMsh1HeaderLen + 2U] = type;
  frame[kMsh1HeaderLen + 3U] = 0U;
  if (payload != nullptr && payload_len > 0U) {
    std::memcpy(frame + kMsh1HeaderLen + 4U, payload, payload_len);
  }
  const size_t total_len = kMsh1HeaderLen + 4U + payload_len;
  const bool ok = uart1_cobs_send_binary_frame(frame, total_len);
  if (ok) {
    g_tunnel_metrics.remote_bin_frames++;
    g_tunnel_metrics.remote_bin_bytes += static_cast<uint32_t>(total_len);
  }
  return ok;
}

bool peer_prefers_msh1(const PeerState& peer) {
  return peer.target != Target::S3 && peer.msh1_ready;
}

void append_peer_output_locked(PeerState& peer, const uint8_t* data, const size_t len) {
  if (data == nullptr || len == 0U) {
    return;
  }
  const size_t room = peer.rx_output.size() < kRemoteOutputMaxBytes
                          ? (kRemoteOutputMaxBytes - peer.rx_output.size())
                          : 0U;
  if (room == 0U) {
    g_tunnel_metrics.remote_rx_drops++;
    return;
  }
  const size_t take = std::min(room, len);
  peer.rx_output.append(reinterpret_cast<const char*>(data), take);
  if (take < len) {
    g_tunnel_metrics.remote_rx_drops++;
  }
}

std::string build_frame(
    const char* verb,
    const Target source,
    const Target dest,
    const uint32_t session_id,
    const std::string& extra = {}) {
  std::string line = kProtoPrefix;
  line += verb;
  line += ":";
  line += target_name(source);
  line += ":";
  line += target_name(dest);
  line += ":";
  line += std::to_string(session_id);
  if (!extra.empty()) {
    line += ":";
    line += extra;
  }
  return line;
}

void destroy_peer_session(PeerState& peer) {
  if (peer.local_session != nullptr) {
    destroy_session(peer.local_session);
    peer.local_session = nullptr;
  }
  peer.session_open = false;
  peer.session_pending = false;
  peer.exec_pending = false;
  peer.exec_ready = false;
  peer.msh1_ready = false;
  peer.session_id = 0U;
  peer.last_exit_code = 0;
  peer.rx_output.clear();
}

void destroy_inbound(InboundSession& inbound) {
  if (inbound.shell != nullptr) {
    destroy_session(inbound.shell);
    inbound.shell = nullptr;
  }
  inbound.session_id = 0U;
}

uint32_t next_session_id_locked() {
  if (g_next_session_id == 0U) {
    g_next_session_id = 1U;
  }
  return g_next_session_id++;
}

void ensure_s3_local_session_locked(PeerState& peer) {
  if (peer.local_session == nullptr) {
    peer.local_session = create_session(ShellTransport::System);
  }
  peer.session_open = peer.local_session != nullptr;
  peer.session_pending = false;
  if (peer.session_id == 0U) {
    peer.session_id = next_session_id_locked();
  }
  peer.peer_seen = true;
  peer.peer_protocol_seen = true;
  peer.detail = "local loopback bridge ready";
}

bool begin_remote_session_locked(PeerState& peer) {
  if (peer.target == Target::S3) {
    ensure_s3_local_session_locked(peer);
    return peer.local_session != nullptr;
  }
  load_bridge_mode_locked();
  if (g_bridge_mode == BridgeMode::Off) {
    peer.detail = "uart shell bridge is off";
    return false;
  }
  if (g_bridge_mode == BridgeMode::Listen) {
    peer.detail = "uart shell bridge is listen-only; set uart-shell-bridge on for remote exec";
    return false;
  }
  if (peer.session_id == 0U) {
    peer.session_id = next_session_id_locked();
  }
  peer.session_pending = true;
  peer.exec_pending = false;
  peer.exec_ready = false;
  peer.last_tx_ms = mros::platform::mros_millis();
  peer.detail = std::string("open requested via ") + peer.route + ", waiting for peer ack";
  (void)send_proto_line(build_frame("HELLO", Target::S3, peer.target, peer.session_id, "caps=msh1"));
  if (peer_prefers_msh1(peer)) {
    return send_msh1_frame(Target::S3, peer.target, peer.session_id,
                           kMsh1TypeOpen, 0U, nullptr, 0U);
  }
  g_tunnel_metrics.remote_fallbacks++;
  return send_proto_line(build_frame("OPEN", Target::S3, peer.target, peer.session_id));
}

bool send_exec_locked(PeerState& peer, const std::string& line) {
  load_bridge_mode_locked();
  if (peer.target != Target::S3 && g_bridge_mode != BridgeMode::On) {
    peer.detail = "uart shell bridge is not enabled for exec";
    return false;
  }
  peer.exec_pending = true;
  peer.exec_ready = false;
  peer.rx_output.clear();
  peer.last_exit_code = 0;
  peer.last_tx_ms = mros::platform::mros_millis();
  peer.detail = std::string("command pending via ") + peer.route;
  if (peer_prefers_msh1(peer)) {
    return send_msh1_frame(Target::S3, peer.target, peer.session_id,
                           kMsh1TypeExec, 0U,
                           reinterpret_cast<const uint8_t*>(line.data()),
                           line.size());
  }
  g_tunnel_metrics.remote_fallbacks++;
  return send_proto_line(
      build_frame("EXEC", Target::S3, peer.target, peer.session_id, hex_encode(line)));
}

std::string format_age(const unsigned long last_ms) {
  if (last_ms == 0UL) {
    return "-";
  }
  const unsigned long age = mros::platform::mros_millis() - last_ms;
  return std::to_string(age) + "ms";
}

std::string peer_status_text(const PeerState& peer) {
  if (peer.target == Target::S3) {
    return "ready";
  }
  if (peer.session_open) {
    return "open";
  }
  if (peer.session_pending) {
    return "wait-ack";
  }
  if (peer.msh1_ready) {
    return "msh1";
  }
  if (peer.peer_protocol_seen) {
    return "bridge";
  }
  if (peer.peer_seen) {
    return "uart-only";
  }
  return "idle";
}

RemoteFsState* fs_state_for(const FsMount mount) {
  switch (mount) {
    case FsMount::T41:
      return &g_fs_t41;
    case FsMount::T41Sdcard:
      return &g_fs_t41_sd;
    default:
      return nullptr;
  }
}

const char* fs_peer_status_locked(const FsMount mount, const RemoteFsState& fs) {
  (void)mount;
  if (g_bridge_mode != BridgeMode::On) {
    return "bridge_disabled";
  }
  if (!g_t41_peer.peer_seen) {
    return "down";
  }
  if (!g_t41_peer.peer_protocol_seen) {
    return "protocol_missing";
  }
  if (!fs.protocol_ready) {
    return "protocol_missing";
  }
  return fs.mounted ? "ready" : "not_mounted";
}

const char* fs_error_code_locked(const FsMount mount, const RemoteFsState& fs) {
  (void)mount;
  if (g_bridge_mode != BridgeMode::On) {
    return "BRIDGE_DISABLED";
  }
  if (!g_t41_peer.peer_seen) {
    return "REMOTE_UNAVAILABLE";
  }
  if (!g_t41_peer.peer_protocol_seen || !fs.protocol_ready) {
    return "PEER_PROTOCOL_MISSING";
  }
  if (!fs.mounted) {
    return "REMOTE_NOT_MOUNTED";
  }
  if (!fs.last_error.empty()) {
    return fs.last_error.c_str();
  }
  return "OK";
}

std::string json_escape_copy(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8U);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          char tmp[8] = {};
          std::snprintf(tmp, sizeof(tmp), "\\u%04x", static_cast<unsigned char>(ch));
          out += tmp;
        } else {
          out += ch;
        }
        break;
    }
  }
  return out;
}

void append_status_line(std::string* out, const PeerState& peer, const Target active) {
  if (out == nullptr) {
    return;
  }
  char line[256] = {};
  std::snprintf(
      line,
      sizeof(line),
      "%-6s %-14s %-10s %-8s %-8s %s%s\n",
      target_name(peer.target),
      peer.route,
      peer_status_text(peer).c_str(),
      active == peer.target || active == Target::All ? "active" : "-",
      format_age(peer.last_rx_ms).c_str(),
      peer.detail.empty() ? "-" : peer.detail.c_str(),
      "");
  out->append(line);
}

bool execute_on_target(
    ShellState& state,
    const Target target,
    const std::string& line,
    std::string* output,
    std::string* error) {
  if (output != nullptr) {
    output->clear();
  }
  if (error != nullptr) {
    error->clear();
  }

  if (target == Target::S3) {
    if (!lock_remote()) {
      if (error != nullptr) {
        *error = "remote s3: mutex unavailable";
      }
      return false;
    }
    PeerState* peer = peer_state_for(Target::S3);
    ensure_s3_local_session_locked(*peer);
    ShellSession* session = peer->local_session;
    unlock_remote();
    if (session == nullptr) {
      if (error != nullptr) {
        *error = "remote s3: loopback session unavailable";
      }
      return false;
    }
    std::string captured;
    const bool ok = execute_session_line(session, line.c_str(), &captured, false);
    if (output != nullptr) {
      *output = captured;
    }
    if (session_close_requested(session)) {
      if (lock_remote()) {
        PeerState* s3 = peer_state_for(Target::S3);
        destroy_peer_session(*s3);
        unlock_remote();
      }
    }
    return ok;
  }

  if (!lock_remote()) {
    if (error != nullptr) {
      *error = "remote shell: mutex unavailable";
    }
    return false;
  }
  PeerState* peer = peer_state_for(target);
  if (peer == nullptr) {
    unlock_remote();
    if (error != nullptr) {
      *error = "remote shell: invalid target";
    }
    return false;
  }
  if (!peer->session_open && !peer->session_pending && !begin_remote_session_locked(*peer)) {
    const std::string detail = peer->detail;
    unlock_remote();
    if (error != nullptr) {
      *error = detail.empty() ? "remote shell: failed to start session" : detail;
    }
    return false;
  }
  if (!send_exec_locked(*peer, line)) {
    peer->exec_pending = false;
    const std::string detail = peer->detail;
    unlock_remote();
    if (error != nullptr) {
      *error = detail.empty() ? "remote shell: failed to send command" : detail;
    }
    return false;
  }
  unlock_remote();

  const unsigned long started_ms = mros::platform::mros_millis();
  for (;;) {
    mros::platform::mros_delay_ms(20);
    if (!lock_remote(pdMS_TO_TICKS(20))) {
      continue;
    }
    const PeerState snapshot = *peer_state_for(target);
    unlock_remote();
    if (snapshot.exec_ready) {
      if (output != nullptr) {
        *output = snapshot.rx_output;
      }
      if (snapshot.last_exit_code != 0 && error != nullptr && output != nullptr && error->empty()) {
        *error = *output;
      }
      if (lock_remote()) {
        PeerState* live = peer_state_for(target);
        live->exec_ready = false;
        live->exec_pending = false;
        live->rx_output.clear();
        unlock_remote();
      }
      return snapshot.last_exit_code == 0;
    }
    if ((mros::platform::mros_millis() - started_ms) > kRemoteExecTimeoutMs) {
      if (lock_remote()) {
        PeerState* live = peer_state_for(target);
        live->exec_pending = false;
        live->detail =
            std::string("no response on ") + live->route + "; peer firmware bridge not ready yet";
        unlock_remote();
      }
      if (error != nullptr) {
        *error = std::string("remote ") + target_name(target) +
                 ": no response yet; peer firmware needs mshell UART bridge support";
      }
      return false;
    }
  }
}

void send_output_chunks(const Target source, const Target dest, const uint32_t session_id, const std::string& text) {
  size_t offset = 0U;
  uint32_t seq = 0U;
  do {
    const size_t remaining = text.size() - offset;
    const size_t raw_len = std::min(kChunkRawBytes, remaining);
    const bool more = (offset + raw_len) < text.size();
    const std::string chunk = text.substr(offset, raw_len);
    PeerState* dest_peer = peer_state_for(dest);
    if (dest_peer != nullptr && peer_prefers_msh1(*dest_peer)) {
      (void)send_msh1_frame(source, dest, session_id, kMsh1TypeStdout, seq,
                            reinterpret_cast<const uint8_t*>(chunk.data()),
                            chunk.size());
      offset += raw_len;
      ++seq;
      continue;
    }
    std::string extra = std::to_string(seq);
    extra += ":";
    extra += more ? "1" : "0";
    extra += ":";
    extra += hex_encode(chunk);
    (void)send_proto_line(build_frame("OUT", source, dest, session_id, extra));
    offset += raw_len;
    ++seq;
  } while (offset < text.size());

  if (text.empty()) {
    PeerState* dest_peer = peer_state_for(dest);
    if (dest_peer != nullptr && peer_prefers_msh1(*dest_peer)) {
      (void)send_msh1_frame(source, dest, session_id, kMsh1TypeStdout, 0U,
                            nullptr, 0U);
    } else {
      (void)send_proto_line(build_frame("OUT", source, dest, session_id, "0:0:"));
    }
  }
}

void handle_open_request(const Target source, const uint32_t session_id) {
  InboundSession* inbound = inbound_session_for(source);
  if (inbound == nullptr) {
    return;
  }
  if (inbound->shell != nullptr && inbound->session_id != session_id) {
    destroy_inbound(*inbound);
  }
  if (inbound->shell == nullptr) {
    inbound->shell = create_session(ShellTransport::System);
  }
  inbound->source = source;
  inbound->session_id = session_id;
  PeerState* peer = peer_state_for(source);
  if (peer != nullptr && peer_prefers_msh1(*peer)) {
    (void)send_msh1_frame(Target::S3, source, session_id, kMsh1TypeOpen, 0U,
                          nullptr, 0U);
  } else {
    (void)send_proto_line(build_frame("OPENED", Target::S3, source, session_id));
  }
}

void handle_close_request(const Target source, const uint32_t session_id) {
  InboundSession* inbound = inbound_session_for(source);
  if (inbound == nullptr || inbound->session_id != session_id) {
    return;
  }
  destroy_inbound(*inbound);
}

void handle_exec_request_line(const Target source, const uint32_t session_id, const std::string& line) {
  InboundSession* inbound = inbound_session_for(source);
  if (inbound == nullptr) {
    return;
  }
  if (inbound->shell == nullptr || inbound->session_id != session_id) {
    handle_open_request(source, session_id);
  }
  inbound = inbound_session_for(source);
  if (inbound == nullptr || inbound->shell == nullptr) {
    return;
  }

  std::string output;
  const bool ok = execute_session_line(inbound->shell, line.c_str(), &output, false);
  send_output_chunks(Target::S3, source, session_id, output);
  PeerState* peer = peer_state_for(source);
  if (peer != nullptr && peer_prefers_msh1(*peer)) {
    const char rc = ok ? '0' : '1';
    (void)send_msh1_frame(Target::S3, source, session_id, kMsh1TypeFinal, 0U,
                          reinterpret_cast<const uint8_t*>(&rc), 1U);
  } else {
    (void)send_proto_line(build_frame("RC", Target::S3, source, session_id, ok ? "0" : "1"));
  }
  if (session_close_requested(inbound->shell)) {
    destroy_inbound(*inbound);
  }
}

void handle_exec_request(const Target source, const uint32_t session_id, const std::string& payload_hex) {
  std::string line;
  if (!hex_decode(payload_hex, &line)) {
    (void)send_proto_line(build_frame("RC", Target::S3, source, session_id, "1"));
    return;
  }
  handle_exec_request_line(source, session_id, line);
}

void mark_peer_protocol(const Target source) {
  PeerState* peer = peer_state_for(source);
  if (peer == nullptr) {
    return;
  }
  peer->peer_seen = true;
  peer->peer_protocol_seen = true;
  peer->last_rx_ms = mros::platform::mros_millis();
  if (peer->detail.empty()) {
    peer->detail = std::string("protocol active on ") + peer->route;
  }
}

void mark_peer_msh1(const Target source) {
  PeerState* peer = peer_state_for(source);
  if (peer == nullptr) {
    return;
  }
  mark_peer_protocol(source);
  peer->msh1_ready = true;
  g_tunnel_metrics.remote_cap_msh1 = true;
  peer->detail = std::string("MSH1 binary tunnel active on ") + peer->route;
}

}  // namespace

const char* bridge_mode_name(const BridgeMode mode) {
  switch (mode) {
    case BridgeMode::Listen:
      return "listen";
    case BridgeMode::On:
      return "on";
    case BridgeMode::Off:
    default:
      return "off";
  }
}

bool parse_bridge_mode(const std::string& text, BridgeMode* out_mode) {
  if (out_mode == nullptr) {
    return false;
  }
  const std::string normalized = lower_copy(text);
  if (normalized == "off" || normalized == "0" || normalized == "disable" || normalized == "disabled") {
    *out_mode = BridgeMode::Off;
    return true;
  }
  if (normalized == "listen" || normalized == "status" || normalized == "hello") {
    *out_mode = BridgeMode::Listen;
    return true;
  }
  if (normalized == "on" || normalized == "1" || normalized == "enable" || normalized == "enabled") {
    *out_mode = BridgeMode::On;
    return true;
  }
  return false;
}

BridgeMode bridge_mode() {
  if (!lock_remote(pdMS_TO_TICKS(20))) {
    return BridgeMode::Off;
  }
  load_bridge_mode_locked();
  const BridgeMode mode = g_bridge_mode;
  unlock_remote();
  return mode;
}

bool set_bridge_mode(const BridgeMode mode) {
  if (!lock_remote()) {
    return false;
  }
  g_bridge_loaded = true;
  g_bridge_mode = mode;
  if (mode != BridgeMode::On) {
    destroy_peer_session(g_t41_peer);
    destroy_peer_session(g_c3_peer);
  }
  unlock_remote();
  return save_bridge_mode(mode);
}

const char* target_name(const Target target) {
  switch (target) {
    case Target::S3:
      return "s3";
    case Target::T41:
      return "t41";
    case Target::C3:
      return "c3";
    case Target::All:
      return "all";
    case Target::None:
    default:
      return "none";
  }
}

bool parse_target(const std::string& text, Target* out_target) {
  if (out_target == nullptr) {
    return false;
  }
  const std::string normalized = lower_copy(text);
  if (normalized == "s3") {
    *out_target = Target::S3;
    return true;
  }
  if (normalized == "t41") {
    *out_target = Target::T41;
    return true;
  }
  if (normalized == "c3") {
    *out_target = Target::C3;
    return true;
  }
  if (normalized == "all") {
    *out_target = Target::All;
    return true;
  }
  if (normalized == "none") {
    *out_target = Target::None;
    return true;
  }
  return false;
}

Target active_target(const ShellState& state) {
  switch (state.remote_target) {
    case 1U:
      return Target::S3;
    case 2U:
      return Target::T41;
    case 3U:
      return Target::C3;
    case 4U:
      return Target::All;
    default:
      return Target::None;
  }
}

void set_active_target(ShellState& state, const Target target) {
  switch (target) {
    case Target::S3:
      state.remote_target = 1U;
      break;
    case Target::T41:
      state.remote_target = 2U;
      break;
    case Target::C3:
      state.remote_target = 3U;
      break;
    case Target::All:
      state.remote_target = 4U;
      break;
    case Target::None:
    default:
      state.remote_target = 0U;
      break;
  }
}

void clear_active_target(ShellState& state) {
  state.remote_target = 0U;
}

std::string prompt_suffix(const ShellState& state) {
  const Target target = active_target(state);
  if (target == Target::None) {
    return {};
  }
  return std::string("[") + target_name(target) + "]";
}

bool connect_session(ShellState& state, const Target target, std::string* message) {
  if (message != nullptr) {
    message->clear();
  }
  if (target == Target::None) {
    clear_active_target(state);
    if (message != nullptr) {
      *message = "mshell remote: cleared active target";
    }
    return true;
  }
  if (target == Target::C3) {
    if (message != nullptr) {
      *message = "mshell remote: c3 topology is disabled in this firmware";
    }
    return false;
  }

  if (!lock_remote()) {
    if (message != nullptr) {
      *message = "mshell remote: mutex unavailable";
    }
    return false;
  }
  load_bridge_mode_locked();
  if (target != Target::S3 && target != Target::None && g_bridge_mode != BridgeMode::On) {
    const BridgeMode mode = g_bridge_mode;
    unlock_remote();
    if (message != nullptr) {
      *message = std::string("mshell remote: UART shell bridge is ") + bridge_mode_name(mode) +
                 "; use 'set uart-shell-bridge on' before remote exec";
    }
    return false;
  }

  std::string text;
  auto connect_one = [&](const Target one) -> bool {
    PeerState* peer = peer_state_for(one);
    if (peer == nullptr) {
      return false;
    }
    bool ok = true;
    if (one == Target::S3) {
      ensure_s3_local_session_locked(*peer);
      ok = peer->local_session != nullptr;
      peer->detail = ok ? "local loopback session armed" : "failed to create local loopback session";
    } else {
      ok = begin_remote_session_locked(*peer);
    }
    if (!text.empty()) {
      text += "\n";
    }
    text += std::string(target_name(one)) + ": " + (ok ? peer->detail : "failed to start session");
    return ok;
  };

  bool ok = false;
  if (target == Target::All) {
    const bool t41_ok = connect_one(Target::T41);
    ok = t41_ok;
  } else {
    ok = connect_one(target);
  }
  unlock_remote();

  if (ok) {
    set_active_target(state, target);
  }
  if (message != nullptr) {
    *message = text;
  }
  return ok;
}

bool disconnect_session(ShellState& state, const Target target, std::string* message) {
  if (message != nullptr) {
    message->clear();
  }
  if (!lock_remote()) {
    if (message != nullptr) {
      *message = "mshell remote: mutex unavailable";
    }
    return false;
  }

  std::string text;
  auto disconnect_one = [&](const Target one) {
    PeerState* peer = peer_state_for(one);
    if (peer == nullptr) {
      return;
    }
    if (one != Target::S3 && peer->session_id != 0U) {
      (void)send_proto_line(build_frame("CLOSE", Target::S3, one, peer->session_id));
    }
    destroy_peer_session(*peer);
    if (!text.empty()) {
      text += "\n";
    }
    text += std::string(target_name(one)) + ": session closed";
  };

  if (target == Target::All) {
    disconnect_one(Target::S3);
    disconnect_one(Target::T41);
    disconnect_one(Target::C3);
  } else {
    disconnect_one(target);
  }
  unlock_remote();

  if (target == Target::All || active_target(state) == target) {
    clear_active_target(state);
  }
  if (message != nullptr) {
    *message = text.empty() ? "mshell remote: nothing to disconnect" : text;
  }
  return true;
}

std::string status_report(const Target target, const ShellState* active_state) {
  const Target active = active_state != nullptr ? active_target(*active_state) : Target::None;
  std::string out;
  out += "MSHELL2 bridge mode: ";
  out += bridge_mode_name(bridge_mode());
  out += "\n";
  out += "target route          status     active   last-rx  detail\n";
  out += "------ -------------  ---------  -------  -------  ------------------------------\n";

  if (lock_remote()) {
    g_s3_peer.detail = "accepts incoming mshell OPEN/EXEC over UART bridge";
    if (!g_t41_peer.peer_protocol_seen && g_t41_peer.peer_seen && g_t41_peer.detail.empty()) {
      g_t41_peer.detail = "uart peer active, mshell frames not seen yet";
    }
    g_c3_peer.detail = "topology disabled";
    if (target == Target::All || target == Target::S3) append_status_line(&out, g_s3_peer, active);
    if (target == Target::All || target == Target::T41) append_status_line(&out, g_t41_peer, active);
    if (target == Target::All || target == Target::C3) append_status_line(&out, g_c3_peer, active);
    unlock_remote();
  } else {
    out += "mshell remote mutex unavailable\n";
  }
  if (target == Target::All || target == Target::T41) {
    out += "\nremote fs mount provider root        state     write  peer-status       error\n";
    out += "--------- -------- ----        --------  -----  ----------------  ---------------------\n";
    for (const FsMount mount : {FsMount::T41, FsMount::T41Sdcard}) {
      FsMountSnapshot snap {};
      fs_snapshot(mount, &snap);
      char line[192] = {};
      std::snprintf(
          line,
          sizeof(line),
          "%-9s %-8s %-10s %-8s %-5s  %-16s  %s\n",
          snap.name,
          snap.provider,
          snap.root,
          snap.mounted ? "mounted" : "down",
          snap.writable ? "yes" : "no",
          snap.peer_status,
          snap.error_code);
      out += line;
    }
  }
  return out;
}

std::string devices_report(const ShellState* active_state) {
  return status_report(Target::All, active_state);
}

bool fs_parse_mount(const std::string& text, FsMount* out_mount) {
  const std::string normalized = lower_copy(text);
  if (normalized == "t41" || normalized == "/t41") {
    if (out_mount != nullptr) {
      *out_mount = FsMount::T41;
    }
    return true;
  }
  if (normalized == "t41-sdcard" || normalized == "/t41-sdcard" ||
      normalized == "t41sdcard" || normalized == "t41-sd" || normalized == "sdcard") {
    if (out_mount != nullptr) {
      *out_mount = FsMount::T41Sdcard;
    }
    return true;
  }
  return false;
}

bool fs_mount_for_path(const std::string& path, FsMount* out_mount) {
  if (path == "/t41" || path.rfind("/t41/", 0U) == 0U) {
    if (out_mount != nullptr) {
      *out_mount = FsMount::T41;
    }
    return true;
  }
  if (path == "/t41-sdcard" || path.rfind("/t41-sdcard/", 0U) == 0U) {
    if (out_mount != nullptr) {
      *out_mount = FsMount::T41Sdcard;
    }
    return true;
  }
  return false;
}

bool fs_is_remote_path(const std::string& path) {
  return fs_mount_for_path(path, nullptr);
}

const char* fs_mount_name(const FsMount mount) {
  return mount == FsMount::T41Sdcard ? "t41-sdcard" : "t41";
}

const char* fs_mount_root(const FsMount mount) {
  return mount == FsMount::T41Sdcard ? "/t41-sdcard" : "/t41";
}

const char* fs_provider_name(const FsMount mount) {
  return mount == FsMount::T41Sdcard ? "t41-sdcard" : "t41";
}

void fs_snapshot(const FsMount mount, FsMountSnapshot* out_snapshot) {
  if (out_snapshot == nullptr) {
    return;
  }
  FsMountSnapshot snap {};
  snap.mount = mount;
  snap.name = fs_mount_name(mount);
  snap.root = fs_mount_root(mount);
  snap.provider = fs_provider_name(mount);
  snap.remote = true;
  snap.peer_status = "mutex_unavailable";
  snap.error_code = "MUTEX_UNAVAILABLE";
  if (lock_remote(pdMS_TO_TICKS(20))) {
    load_bridge_mode_locked();
    RemoteFsState* fs = fs_state_for(mount);
    if (fs != nullptr) {
      snap.mounted = fs->mounted;
      snap.writable = fs->writable;
      snap.protocol_ready = fs->protocol_ready;
      snap.mounted_ms = fs->mounted_ms;
      snap.bridge_on = g_bridge_mode == BridgeMode::On;
      snap.peer_seen = g_t41_peer.peer_seen || g_t41_peer.peer_protocol_seen;
      snap.peer_status = fs_peer_status_locked(mount, *fs);
      snap.error_code = fs_error_code_locked(mount, *fs);
    }
    unlock_remote();
  }
  *out_snapshot = snap;
}

std::string fs_mounts_json(const bool include_local, const bool local_mounted) {
  FsMountSnapshot t41 {};
  FsMountSnapshot t41sd {};
  fs_snapshot(FsMount::T41, &t41);
  fs_snapshot(FsMount::T41Sdcard, &t41sd);
  std::string out = "{\"success\":true,\"mounts\":[";
  bool first = true;
  auto add_comma = [&]() {
    if (!first) {
      out += ",";
    }
    first = false;
  };
  if (include_local) {
    add_comma();
    out += "{\"name\":\"s3-espuser\",\"root\":\"/ESPUSER\",\"provider\":\"local\",";
    out += "\"remote\":false,\"mounted\":";
    out += local_mounted ? "true" : "false";
    out += ",\"writable\":true,\"peer_status\":\"ready\",\"error_code\":\"OK\"}";
  }
  auto append_remote = [&](const FsMountSnapshot& snap) {
    add_comma();
    out += "{\"name\":\"";
    out += snap.name;
    out += "\",\"root\":\"";
    out += snap.root;
    out += "\",\"provider\":\"";
    out += snap.provider;
    out += "\",\"remote\":true,\"mounted\":";
    out += snap.mounted ? "true" : "false";
    out += ",\"writable\":";
    out += snap.writable ? "true" : "false";
    out += ",\"bridge_on\":";
    out += snap.bridge_on ? "true" : "false";
    out += ",\"protocol_ready\":";
    out += snap.protocol_ready ? "true" : "false";
    out += ",\"peer_seen\":";
    out += snap.peer_seen ? "true" : "false";
    out += ",\"peer_status\":\"";
    out += snap.peer_status;
    out += "\",\"error_code\":\"";
    out += snap.error_code;
    out += "\",\"mounted_ms\":";
    out += std::to_string(snap.mounted_ms);
    out += "}";
  };
  append_remote(t41);
  append_remote(t41sd);
  out += "]}";
  return out;
}

bool fs_mount(const FsMount mount, std::string* message) {
  if (!lock_remote(pdMS_TO_TICKS(100))) {
    if (message != nullptr) {
      *message = "MUTEX_UNAVAILABLE";
    }
    return false;
  }
  load_bridge_mode_locked();
  RemoteFsState* fs = fs_state_for(mount);
  const bool bridge_on = g_bridge_mode == BridgeMode::On;
  const bool peer_seen = g_t41_peer.peer_seen || g_t41_peer.peer_protocol_seen;
  const bool protocol_ready = fs != nullptr && fs->protocol_ready && g_t41_peer.peer_protocol_seen;
  bool ok = false;
  const char* code = "REMOTE_UNAVAILABLE";
  if (!bridge_on) {
    code = "BRIDGE_DISABLED";
  } else if (!peer_seen) {
    code = "REMOTE_UNAVAILABLE";
  } else if (!protocol_ready) {
    code = "PEER_PROTOCOL_MISSING";
  } else if (fs != nullptr) {
    fs->mounted = true;
    fs->writable = true;
    fs->last_error = "OK";
    fs->mounted_ms = mros::platform::mros_millis();
    ok = true;
    code = "OK";
  }
  if (fs != nullptr && !ok) {
    fs->mounted = false;
    fs->writable = false;
    fs->last_error = code;
  }
  unlock_remote();
  if (message != nullptr) {
    *message = std::string(fs_mount_name(mount)) + ": " + code;
  }
  return ok;
}

bool fs_umount(const FsMount mount, std::string* message) {
  if (!lock_remote(pdMS_TO_TICKS(100))) {
    if (message != nullptr) {
      *message = "MUTEX_UNAVAILABLE";
    }
    return false;
  }
  RemoteFsState* fs = fs_state_for(mount);
  if (fs != nullptr) {
    fs->mounted = false;
    fs->writable = false;
    fs->last_error = "REMOTE_NOT_MOUNTED";
    fs->mounted_ms = 0U;
  }
  unlock_remote();
  if (message != nullptr) {
    *message = std::string(fs_mount_name(mount)) + ": unmounted";
  }
  return true;
}

void fs_umount_all() {
  std::string unused;
  (void)fs_umount(FsMount::T41, &unused);
  (void)fs_umount(FsMount::T41Sdcard, &unused);
}

std::string fs_error_json(
    const std::string& path,
    const char* op,
    const char* code,
    const char* message) {
  FsMount mount = FsMount::T41;
  (void)fs_mount_for_path(path, &mount);
  FsMountSnapshot snap {};
  fs_snapshot(mount, &snap);
  const char* error_code = (code != nullptr && code[0] != '\0') ? code : snap.error_code;
  const char* error_message = (message != nullptr && message[0] != '\0') ? message : error_code;
  std::string out = "{\"success\":false,\"remote\":true,\"provider\":\"";
  out += snap.provider;
  out += "\",\"mounted\":";
  out += snap.mounted ? "true" : "false";
  out += ",\"writable\":";
  out += snap.writable ? "true" : "false";
  out += ",\"peer_status\":\"";
  out += snap.peer_status;
  out += "\",\"error_code\":\"";
  out += error_code;
  out += "\",\"error\":\"";
  out += json_escape_copy(error_message);
  out += "\",\"op\":\"";
  out += op != nullptr ? op : "fs";
  out += "\",\"path\":\"";
  out += json_escape_copy(path);
  out += "\"}";
  return out;
}

std::string fs_list_json(const std::string& path, const size_t offset, const size_t limit) {
  FsMount mount = FsMount::T41;
  if (!fs_mount_for_path(path, &mount)) {
    return fs_error_json(path, "list", "INVALID_PROVIDER", "not a remote filesystem path");
  }
  FsMountSnapshot snap {};
  fs_snapshot(mount, &snap);
  if (!snap.mounted) {
    return fs_error_json(path, "list", "REMOTE_NOT_MOUNTED", "remote filesystem is not mounted");
  }
  if (!snap.protocol_ready) {
    return fs_error_json(path, "list", "PEER_PROTOCOL_MISSING", "t41 firmware does not expose MSHELL2 FS protocol yet");
  }
  (void)offset;
  (void)limit;
  return fs_error_json(path, "list", "PEER_PROTOCOL_MISSING", "MSHELL2 FS_LIST transport is waiting for peer firmware support");
}

void reset_bridge_state() {
  if (!lock_remote()) {
    return;
  }
  destroy_peer_session(g_s3_peer);
  destroy_peer_session(g_t41_peer);
  destroy_peer_session(g_c3_peer);
  destroy_inbound(g_inbound_t41);
  destroy_inbound(g_inbound_c3);
  g_t41_peer.peer_protocol_seen = false;
  g_c3_peer.peer_protocol_seen = false;
  g_t41_peer.msh1_ready = false;
  g_c3_peer.msh1_ready = false;
  g_t41_peer.detail.clear();
  g_c3_peer.detail.clear();
  g_tunnel_metrics = RemoteTunnelMetrics {};
  std::memset(g_diag_pending, 0, sizeof(g_diag_pending));
  g_diag_ping_sent = 0U;
  g_diag_ping_recv = 0U;
  g_diag_ping_timeout = 0U;
  g_diag_rtt_min_ms = 0xFFFFFFFFUL;
  g_diag_rtt_max_ms = 0U;
  g_diag_rtt_sum_ms = 0ULL;
  g_diag_last_clock_ack_hz = 0U;
  g_diag_last_clock_ack_nonce = 0U;
  g_diag_last_clock_ack_ok = false;
  g_diag_last_clock_ack_seen = false;
  g_fs_t41 = RemoteFsState {FsMount::T41, false, false, false, 0U, "REMOTE_NOT_MOUNTED"};
  g_fs_t41_sd = RemoteFsState {FsMount::T41Sdcard, false, false, false, 0U, "REMOTE_NOT_MOUNTED"};
  unlock_remote();
}

bool execute_active_line(
    ShellState& state,
    const std::string& line,
    std::string* output,
    std::string* error) {
  const Target target = active_target(state);
  if (target == Target::None) {
    if (error != nullptr) {
      *error = "mshell remote: no active target";
    }
    return false;
  }

  if (target == Target::All) {
    std::string combined;
    std::string local_error;
    bool any_ok = false;
    for (const Target one : {Target::T41, Target::C3}) {
      std::string one_output;
      std::string one_error;
      const bool ok = execute_on_target(state, one, line, &one_output, &one_error);
      if (!combined.empty()) {
        combined += "\n";
      }
      combined += "[";
      combined += target_name(one);
      combined += "]\n";
      combined += ok ? one_output : one_error;
      any_ok = any_ok || ok;
      if (!ok && local_error.empty()) {
        local_error = one_error;
      }
    }
    if (output != nullptr) {
      *output = combined;
    }
    if (!any_ok && error != nullptr) {
      *error = local_error.empty() ? "remote all: no target responded" : local_error;
    }
    return any_ok;
  }

  return execute_on_target(state, target, line, output, error);
}

void note_plain_uart_activity() {
  if (!lock_remote(pdMS_TO_TICKS(10))) {
    return;
  }
  g_t41_peer.peer_seen = true;
  g_t41_peer.last_rx_ms = mros::platform::mros_millis();
  if (!g_t41_peer.peer_protocol_seen) {
    g_t41_peer.detail = "uart peer active, mshell frames not seen yet";
  }
  unlock_remote();
}

bool handle_uart_line(const char* line) {
  if (line == nullptr) {
    return false;
  }
  std::string raw(line);
  if (raw.rfind("MSHELL:DIAG:", 0U) == 0U) {
    if (lock_remote(pdMS_TO_TICKS(20))) {
      diag_sweep_timeouts_locked();
      handle_diag_line_locked(raw.c_str());
      unlock_remote();
    }
    return true;
  }
  const bool proto2 = raw.rfind(kProtoPrefix, 0U) == 0U;
  const bool legacy = raw.rfind(kLegacyProtoPrefix, 0U) == 0U;
  if (!proto2 && !legacy) {
    return false;
  }

  const std::string body = raw.substr(proto2 ? std::strlen(kProtoPrefix) : std::strlen(kLegacyProtoPrefix));
  const std::vector<std::string> parts = split_fields(body, 7U);
  if (parts.size() < 4U) {
    return true;
  }

  Target source = Target::None;
  Target dest = Target::None;
  uint32_t session_id = 0U;
  if (!parse_target(parts[1], &source) || !parse_target(parts[2], &dest) || !parse_u32(parts[3], &session_id)) {
    return true;
  }

  if (!lock_remote(pdMS_TO_TICKS(20))) {
    return true;
  }
  load_bridge_mode_locked();
  const BridgeMode mode = g_bridge_mode;
  mark_peer_protocol(source);
  unlock_remote();

  const std::string verb = parts[0];
  if (mode == BridgeMode::Off) {
    return true;
  }
  if (mode == BridgeMode::Listen && verb != "HELLO" && verb != "PING" && verb != "PONG") {
    if (dest == Target::S3 && source != Target::None) {
      (void)send_proto_line(build_frame("ERROR", Target::S3, source, session_id, "listen-only"));
    }
    return true;
  }
  if (dest == Target::S3) {
    if (verb == "HELLO") {
      if (parts.size() >= 5U && parts[4].find("msh1") != std::string::npos &&
          lock_remote()) {
        mark_peer_msh1(source);
        unlock_remote();
      }
      return true;
    }
    if (verb == "FS_CAPS") {
      if (lock_remote()) {
        RemoteFsState* fs = nullptr;
        if (source == Target::T41 && parts.size() >= 5U) {
          FsMount mount = FsMount::T41;
          if (fs_parse_mount(parts[4], &mount)) {
            fs = fs_state_for(mount);
          }
        }
        if (fs != nullptr) {
          fs->protocol_ready = true;
          fs->writable = parts.size() >= 6U ? (parts[5].find("write") != std::string::npos) : true;
          fs->last_error = "OK";
          g_t41_peer.detail = "MSHELL2 remote filesystem capability advertised";
        }
        unlock_remote();
      }
      return true;
    }
    if (verb == "OPEN") {
      handle_open_request(source, session_id);
      return true;
    }
    if (verb == "CLOSE") {
      handle_close_request(source, session_id);
      return true;
    }
    if (verb == "EXEC" && parts.size() >= 5U) {
      handle_exec_request(source, session_id, parts[4]);
      return true;
    }
    if (verb == "OPENED") {
      if (lock_remote()) {
        PeerState* peer = peer_state_for(source);
        if (peer != nullptr && peer->session_id == session_id) {
          peer->session_open = true;
          peer->session_pending = false;
          peer->detail = std::string("session open via ") + peer->route;
        }
        unlock_remote();
      }
      return true;
    }
    if (verb == "OUT" && parts.size() >= 7U) {
      bool more = false;
      std::string chunk;
      if (parse_bool01(parts[5], &more) && hex_decode(parts[6], &chunk) && lock_remote()) {
        PeerState* peer = peer_state_for(source);
        if (peer != nullptr && peer->session_id == session_id) {
          append_peer_output_locked(*peer,
                                    reinterpret_cast<const uint8_t*>(chunk.data()),
                                    chunk.size());
          peer->last_rx_ms = mros::platform::mros_millis();
          peer->detail = std::string("receiving output via ") + peer->route;
          (void)more;
        }
        unlock_remote();
      }
      return true;
    }
    if (verb == "RC" && parts.size() >= 5U) {
      if (lock_remote()) {
        PeerState* peer = peer_state_for(source);
        if (peer != nullptr && peer->session_id == session_id) {
          peer->last_exit_code = std::atoi(parts[4].c_str());
          peer->exec_ready = true;
          peer->exec_pending = false;
          peer->last_rx_ms = mros::platform::mros_millis();
          peer->detail = std::string("response ready via ") + peer->route;
        }
        unlock_remote();
      }
      return true;
    }
  }

  return true;
}

bool handle_uart_binary_frame(const uint8_t* data, const size_t len) {
  if (data == nullptr || len < kMsh1HeaderLen ||
      std::memcmp(data, "MSH1", 4U) != 0) {
    return false;
  }
  if (data[4] != kMsh1Version || data[5] != kMsh1HeaderLen) {
    g_tunnel_metrics.remote_cobs_decode_errors++;
    return true;
  }
  const uint8_t type = data[6];
  const uint32_t seq = msh1_get_u32(data, 8U);
  const uint32_t session_id = msh1_get_u32(data, 20U);
  const uint32_t payload_len = msh1_get_u32(data, 28U);
  if (payload_len + kMsh1HeaderLen != len || payload_len < 4U) {
    g_tunnel_metrics.remote_cobs_decode_errors++;
    return true;
  }
  const uint8_t* payload = data + kMsh1HeaderLen;
  const Target source = target_from_id(payload[0]);
  const Target dest = target_from_id(payload[1]);
  const uint8_t* body = payload + 4U;
  const size_t body_len = payload_len - 4U;
  if (source == Target::None || dest == Target::None) {
    g_tunnel_metrics.remote_cobs_decode_errors++;
    return true;
  }

  g_tunnel_metrics.remote_bin_frames++;
  g_tunnel_metrics.remote_bin_bytes += static_cast<uint32_t>(len);

  BridgeMode mode = BridgeMode::Off;
  if (lock_remote(pdMS_TO_TICKS(20))) {
    load_bridge_mode_locked();
    mode = g_bridge_mode;
    mark_peer_msh1(source);
    unlock_remote();
  }
  if (mode == BridgeMode::Off) {
    return true;
  }
  if (type == kMsh1TypeHello) {
    return true;
  }
  if (mode == BridgeMode::Listen && type != kMsh1TypeHello) {
    const char msg[] = "listen-only";
    (void)send_msh1_frame(Target::S3, source, session_id, kMsh1TypeError,
                          seq, reinterpret_cast<const uint8_t*>(msg),
                          sizeof(msg) - 1U);
    return true;
  }
  if (dest != Target::S3) {
    return true;
  }

  if (type == kMsh1TypeOpen) {
    PeerState* peer = peer_state_for(source);
    if (peer != nullptr && lock_remote(pdMS_TO_TICKS(20))) {
      peer->session_open = true;
      peer->session_pending = false;
      peer->session_id = session_id;
      peer->detail = std::string("session open via MSH1 ") + peer->route;
      unlock_remote();
    }
    handle_open_request(source, session_id);
    return true;
  }
  if (type == kMsh1TypeClose) {
    handle_close_request(source, session_id);
    return true;
  }
  if (type == kMsh1TypeExec) {
    handle_exec_request_line(
        source, session_id,
        std::string(reinterpret_cast<const char*>(body), body_len));
    return true;
  }
  if (type == kMsh1TypeStdout) {
    if (lock_remote(pdMS_TO_TICKS(20))) {
      PeerState* peer = peer_state_for(source);
      if (peer != nullptr && peer->session_id == session_id) {
        append_peer_output_locked(*peer, body, body_len);
        peer->last_rx_ms = mros::platform::mros_millis();
        peer->detail = std::string("receiving binary output via ") + peer->route;
      }
      unlock_remote();
    }
    return true;
  }
  if (type == kMsh1TypeFinal) {
    if (lock_remote(pdMS_TO_TICKS(20))) {
      PeerState* peer = peer_state_for(source);
      if (peer != nullptr && peer->session_id == session_id) {
        peer->last_exit_code = (body_len > 0U && body[0] == '0') ? 0 : 1;
        peer->exec_ready = true;
        peer->exec_pending = false;
        peer->last_rx_ms = mros::platform::mros_millis();
        peer->detail = std::string("binary response ready via ") + peer->route;
      }
      unlock_remote();
    }
    return true;
  }
  if (type == kMsh1TypeFsCaps) {
    if (source == Target::T41 && lock_remote(pdMS_TO_TICKS(20))) {
      RemoteFsState* fs = &g_fs_t41;
      fs->protocol_ready = true;
      fs->writable = true;
      fs->last_error = "OK";
      g_t41_peer.detail = "MSH1 remote filesystem capability advertised";
      unlock_remote();
    }
    return true;
  }
  return true;
}

void note_uart_binary_decode_error() {
  g_tunnel_metrics.remote_cobs_decode_errors++;
}

void get_tunnel_metrics(RemoteTunnelMetrics* out_metrics) {
  if (out_metrics == nullptr) {
    return;
  }
  *out_metrics = g_tunnel_metrics;
  if (lock_remote(pdMS_TO_TICKS(5))) {
    out_metrics->remote_cap_msh1 = g_tunnel_metrics.remote_cap_msh1 ||
                                   g_t41_peer.msh1_ready ||
                                   g_c3_peer.msh1_ready;
    unlock_remote();
  }
}

void reset_diag_metrics() {
  if (!lock_remote(pdMS_TO_TICKS(20))) {
    return;
  }
  std::memset(g_diag_pending, 0, sizeof(g_diag_pending));
  g_diag_ping_sent = 0U;
  g_diag_ping_recv = 0U;
  g_diag_ping_timeout = 0U;
  g_diag_rtt_min_ms = 0xFFFFFFFFUL;
  g_diag_rtt_max_ms = 0U;
  g_diag_rtt_sum_ms = 0ULL;
  g_diag_last_clock_ack_hz = 0U;
  g_diag_last_clock_ack_nonce = 0U;
  g_diag_last_clock_ack_ok = false;
  g_diag_last_clock_ack_seen = false;
  unlock_remote();
}

void get_diag_snapshot(RemoteDiagSnapshot* out_snapshot) {
  if (out_snapshot == nullptr) return;
  if (!lock_remote(pdMS_TO_TICKS(20))) return;
  diag_sweep_timeouts_locked();
  out_snapshot->ping_sent = g_diag_ping_sent;
  out_snapshot->ping_recv = g_diag_ping_recv;
  out_snapshot->ping_timeout = g_diag_ping_timeout;
  out_snapshot->rtt_min_ms = g_diag_ping_recv == 0U ? 0U : g_diag_rtt_min_ms;
  out_snapshot->rtt_avg_ms = g_diag_ping_recv == 0U ? 0U : static_cast<uint32_t>(g_diag_rtt_sum_ms / g_diag_ping_recv);
  out_snapshot->rtt_max_ms = g_diag_rtt_max_ms;
  out_snapshot->last_clock_ack_hz = g_diag_last_clock_ack_hz;
  out_snapshot->last_clock_ack_nonce = g_diag_last_clock_ack_nonce;
  out_snapshot->last_clock_ack_ok = g_diag_last_clock_ack_ok;
  out_snapshot->last_clock_ack_seen = g_diag_last_clock_ack_seen;
  unlock_remote();
}

bool send_diag_ping(const uint32_t id, const uint32_t t_ms) {
  char out[96] = {};
  std::snprintf(out, sizeof(out), "MSHELL:DIAG:PING:%lu:%lu",
                static_cast<unsigned long>(id),
                static_cast<unsigned long>(t_ms));
  if (!send_proto_line(out)) return false;
  if (lock_remote(pdMS_TO_TICKS(20))) {
    diag_mark_ping_sent_locked(id, t_ms);
    unlock_remote();
  }
  return true;
}

bool send_diag_clock_prep(const uint32_t hz, const uint32_t nonce) {
  char out[96] = {};
  std::snprintf(out, sizeof(out), "MSHELL:DIAG:CLOCK_PREP:%lu:%lu",
                static_cast<unsigned long>(hz),
                static_cast<unsigned long>(nonce));
  return send_proto_line(out);
}

bool send_diag_clock_commit(const uint32_t hz, const uint32_t nonce) {
  char out[96] = {};
  std::snprintf(out, sizeof(out), "MSHELL:DIAG:CLOCK_COMMIT:%lu:%lu",
                static_cast<unsigned long>(hz),
                static_cast<unsigned long>(nonce));
  return send_proto_line(out);
}

bool send_diag_clock_rollback(const uint32_t hz, const uint32_t nonce) {
  char out[96] = {};
  std::snprintf(out, sizeof(out), "MSHELL:DIAG:CLOCK_ROLLBACK:%lu:%lu",
                static_cast<unsigned long>(hz),
                static_cast<unsigned long>(nonce));
  return send_proto_line(out);
}

bool consume_last_clock_ack(uint32_t* out_hz, uint32_t* out_nonce, bool* out_ok) {
  if (!lock_remote(pdMS_TO_TICKS(20))) return false;
  if (!g_diag_last_clock_ack_seen) {
    unlock_remote();
    return false;
  }
  if (out_hz != nullptr) *out_hz = g_diag_last_clock_ack_hz;
  if (out_nonce != nullptr) *out_nonce = g_diag_last_clock_ack_nonce;
  if (out_ok != nullptr) *out_ok = g_diag_last_clock_ack_ok;
  g_diag_last_clock_ack_seen = false;
  unlock_remote();
  return true;
}

}  // namespace mros::shell::remote
