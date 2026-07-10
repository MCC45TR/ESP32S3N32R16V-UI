#include "src/security/uart_secure.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#include "src/platform/mros_nvs.h"

namespace mros::security::uart_secure {
namespace {

#ifndef MROS_REQUIRE_SECURE_UART
#define MROS_REQUIRE_SECURE_UART 1
#endif

constexpr uint8_t kMagic[4] = {'M', 'U', 'S', '1'};
constexpr uint8_t kVersion = 1U;
constexpr size_t kKeyLen = 32U;
constexpr size_t kNonceLen = 12U;
constexpr size_t kTagLen = 16U;
constexpr size_t kHeaderLen = 4U + 1U + 1U + 4U + kNonceLen;
constexpr uint32_t kTxSequenceReserveWindow = 1024U;
constexpr uint32_t kTxSequenceReserveMargin = 16U;
constexpr size_t kRememberedRxSessions = 8U;
constexpr uint32_t kTeensyTxDomain = 0x54584D52UL;
constexpr uint32_t kEspTxDomain = 0x52584D52UL;

SemaphoreHandle_t g_mutex = nullptr;
bool g_initialized = false;
bool g_enabled = false;
bool g_key_loaded = false;
uint8_t g_tx_key[kKeyLen] = {};
uint8_t g_rx_key[kKeyLen] = {};
uint32_t g_boot_salt = 0U;
uint32_t g_tx_sequence = 0U;
uint32_t g_tx_sequence_reservation_limit = 0U;
uint32_t g_rx_sequence = 0U;
uint32_t g_rx_session_salt = 0U;
bool g_rx_session_valid = false;
uint32_t g_seen_rx_sessions[kRememberedRxSessions] = {};
size_t g_seen_rx_session_count = 0U;
uint32_t g_auth_failures = 0U;
uint32_t g_replay_rejects = 0U;

class StateLock {
 public:
  StateLock() : locked_(g_mutex != nullptr &&
                        xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {}
  ~StateLock() { if (locked_) xSemaphoreGive(g_mutex); }
  bool locked() const { return locked_; }
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;
 private:
  bool locked_ = false;
};

void secure_zero(void* data, const size_t len) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  for (size_t i = 0U; bytes != nullptr && i < len; ++i) bytes[i] = 0U;
}

uint32_t load32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8U) |
         (static_cast<uint32_t>(p[2]) << 16U) |
         (static_cast<uint32_t>(p[3]) << 24U);
}

void store32_le(uint8_t* p, const uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8U);
  p[2] = static_cast<uint8_t>(value >> 16U);
  p[3] = static_cast<uint8_t>(value >> 24U);
}

uint8_t hex_value(const char ch) {
  if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
  if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(10U + ch - 'a');
  if (ch >= 'A' && ch <= 'F') return static_cast<uint8_t>(10U + ch - 'A');
  return 0xFFU;
}

bool parse_hex32(const char* hex, uint8_t out[kKeyLen]) {
  if (hex == nullptr || std::strlen(hex) != kKeyLen * 2U) return false;
  for (size_t i = 0U; i < kKeyLen; ++i) {
    const uint8_t hi = hex_value(hex[i * 2U]);
    const uint8_t lo = hex_value(hex[i * 2U + 1U]);
    if (hi == 0xFFU || lo == 0xFFU) return false;
    out[i] = static_cast<uint8_t>((hi << 4U) | lo);
  }
  return true;
}

bool derive_keys(const uint8_t psk[kKeyLen]) {
  static const uint8_t salt[] = "mros-uart-secure-v1 hkdf salt";
  static const uint8_t tx_info[] = "mros-uart-secure-v1 teensy-rx";
  static const uint8_t rx_info[] = "mros-uart-secure-v1 teensy-tx";
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr) return false;
  const int tx_rc = mbedtls_hkdf(md, salt, sizeof(salt) - 1U, psk, kKeyLen,
                                tx_info, sizeof(tx_info) - 1U, g_tx_key, sizeof(g_tx_key));
  const int rx_rc = mbedtls_hkdf(md, salt, sizeof(salt) - 1U, psk, kKeyLen,
                                rx_info, sizeof(rx_info) - 1U, g_rx_key, sizeof(g_rx_key));
  g_key_loaded = tx_rc == 0 && rx_rc == 0;
  return g_key_loaded;
}

void reset_rx_replay_state() {
  g_rx_sequence = 0U;
  g_rx_session_salt = 0U;
  g_rx_session_valid = false;
  g_seen_rx_session_count = 0U;
  std::memset(g_seen_rx_sessions, 0, sizeof(g_seen_rx_sessions));
}

bool accept_sequence(const uint8_t* frame, const uint32_t sequence, std::string* error) {
  const uint32_t session_salt = load32_le(frame + 10U);
  if (g_rx_session_valid && session_salt == g_rx_session_salt) {
    if (sequence <= g_rx_sequence) {
      if (error != nullptr) *error = "replay rejected frame";
      return false;
    }
    g_rx_sequence = sequence;
    return true;
  }
  for (size_t i = 0U; i < g_seen_rx_session_count; ++i) {
    if (g_seen_rx_sessions[i] == session_salt) {
      if (error != nullptr) *error = "replay rejected retired session";
      return false;
    }
  }
  if (g_rx_session_valid) {
    if (g_seen_rx_session_count < kRememberedRxSessions) ++g_seen_rx_session_count;
    for (size_t i = g_seen_rx_session_count - 1U; i > 0U; --i) {
      g_seen_rx_sessions[i] = g_seen_rx_sessions[i - 1U];
    }
    g_seen_rx_sessions[0] = g_rx_session_salt;
  }
  g_rx_session_salt = session_salt;
  g_rx_session_valid = true;
  g_rx_sequence = sequence;
  return true;
}

uint32_t reservation_limit() {
  return g_tx_sequence > UINT32_MAX - kTxSequenceReserveWindow
             ? UINT32_MAX
             : g_tx_sequence + kTxSequenceReserveWindow;
}

bool reserve_tx_sequences(mros::platform::NvsNamespace* open_nvs = nullptr) {
  const uint32_t limit = reservation_limit();
  bool ok = false;
  if (open_nvs != nullptr && open_nvs->is_open()) {
    ok = open_nvs->set_u32("next_tx_seq", limit);
  } else {
    mros::platform::NvsNamespace nvs;
    if (nvs.open("uartsec", false)) ok = nvs.set_u32("next_tx_seq", limit);
  }
  if (ok) g_tx_sequence_reservation_limit = limit;
  return ok;
}

bool ensure_sequence(std::string* error) {
  if (g_tx_sequence == UINT32_MAX) {
    if (error != nullptr) *error = "uart secure tx sequence exhausted; rotate key";
    return false;
  }
  if (g_tx_sequence_reservation_limit <= g_tx_sequence ||
      g_tx_sequence_reservation_limit - g_tx_sequence <= kTxSequenceReserveMargin) {
    if (!reserve_tx_sequences()) {
      if (error != nullptr) *error = "failed to reserve persistent tx sequence window";
      return false;
    }
  }
  return true;
}

void save_config(const uint8_t* psk) {
  mros::platform::NvsNamespace nvs;
  if (!nvs.open("uartsec", false)) return;
  (void)nvs.set_bool("enabled", g_enabled);
  if (g_tx_sequence_reservation_limit > g_tx_sequence) {
    (void)nvs.set_u32("next_tx_seq", g_tx_sequence_reservation_limit);
  }
  if (psk != nullptr) (void)nvs.set_blob("psk", psk, kKeyLen);
}

void load_config() {
  for (uint8_t attempt = 0U; attempt < 8U && g_boot_salt == 0U; ++attempt) {
    g_boot_salt = esp_random();
  }
  mros::platform::NvsNamespace nvs;
  if (!nvs.open("uartsec", false)) return;
  uint32_t next = 0U;
  if (nvs.get_u32("next_tx_seq", &next)) g_tx_sequence = next;
  (void)reserve_tx_sequences(&nvs);
  bool enabled = false;
  if (nvs.get_bool("enabled", &enabled)) g_enabled = enabled;
  uint8_t psk[kKeyLen] = {};
  if (nvs.get_blob("psk", psk, sizeof(psk))) (void)derive_keys(psk);
  secure_zero(psk, sizeof(psk));
  if (MROS_REQUIRE_SECURE_UART != 0 && (!g_enabled || !g_key_loaded)) g_enabled = false;
}

bool crypt(const bool encrypt, const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen],
           const uint8_t* aad, const size_t aad_len, const uint8_t* input,
           const size_t input_len, const uint8_t* input_tag, uint8_t* output,
           uint8_t output_tag[kTagLen]) {
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  int rc = mbedtls_chachapoly_setkey(&ctx, key);
  if (rc == 0 && encrypt) {
    rc = mbedtls_chachapoly_encrypt_and_tag(&ctx, input_len, nonce, aad, aad_len,
                                            input, output, output_tag);
  } else if (rc == 0) {
    rc = mbedtls_chachapoly_auth_decrypt(&ctx, input_len, nonce, aad, aad_len,
                                         input_tag, input, output);
  }
  mbedtls_chachapoly_free(&ctx);
  return rc == 0;
}

}  // namespace

void init() {
  if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
  if (g_mutex == nullptr) return;
  StateLock lock;
  if (!lock.locked() || g_initialized) return;
  load_config();
  g_initialized = true;
}

bool enable_with_psk_hex(const char* hex, std::string* error) {
  init();
  StateLock lock;
  if (!lock.locked()) return false;
  uint8_t psk[kKeyLen] = {};
  if (!parse_hex32(hex, psk)) {
    if (error != nullptr) *error = "expected 64 hex chars";
    return false;
  }
  const bool ok = derive_keys(psk);
  if (ok) {
    g_enabled = true;
    reset_rx_replay_state();
    save_config(psk);
  } else if (error != nullptr) {
    *error = "HKDF-SHA256 key derivation failed";
  }
  secure_zero(psk, sizeof(psk));
  return ok;
}

bool disable(std::string* error) {
  init();
  StateLock lock;
  if (!lock.locked()) return false;
  if (MROS_REQUIRE_SECURE_UART != 0) {
    if (error != nullptr) *error = "secure UART is required by this build";
    return false;
  }
  g_enabled = false;
  save_config(nullptr);
  return true;
}

Status status() {
  init();
  StateLock lock;
  Status out;
  if (!lock.locked()) return out;
  out.enabled = g_enabled;
  out.key_loaded = g_key_loaded;
  out.secure_required = MROS_REQUIRE_SECURE_UART != 0;
  out.tx_sequence = g_tx_sequence;
  out.rx_sequence = g_rx_sequence;
  out.auth_failures = g_auth_failures;
  out.replay_rejects = g_replay_rejects;
  return out;
}

std::string status_text(const bool json) {
  const Status s = status();
  char text[320] = {};
  if (json) {
    std::snprintf(text, sizeof(text),
                  "{\"uart_crypto\":{\"enabled\":%s,\"key_loaded\":%s,\"secure_required\":%s,\"tx_sequence\":%lu,\"rx_sequence\":%lu,\"auth_failures\":%lu,\"replay_rejects\":%lu}}",
                  s.enabled ? "true" : "false", s.key_loaded ? "true" : "false",
                  s.secure_required ? "true" : "false",
                  static_cast<unsigned long>(s.tx_sequence), static_cast<unsigned long>(s.rx_sequence),
                  static_cast<unsigned long>(s.auth_failures), static_cast<unsigned long>(s.replay_rejects));
  } else {
    std::snprintf(text, sizeof(text),
                  "uart-crypto enabled=%s key_loaded=%s secure_required=%s tx_seq=%lu rx_seq=%lu auth_failures=%lu replay_rejects=%lu",
                  s.enabled ? "true" : "false", s.key_loaded ? "true" : "false",
                  s.secure_required ? "true" : "false",
                  static_cast<unsigned long>(s.tx_sequence), static_cast<unsigned long>(s.rx_sequence),
                  static_cast<unsigned long>(s.auth_failures), static_cast<unsigned long>(s.replay_rejects));
  }
  return text;
}

bool seal(const uint8_t frame_type, const uint8_t* plaintext, const size_t plaintext_len,
          std::vector<uint8_t>* out, std::string* error) {
  init();
  StateLock lock;
  if (!lock.locked() || out == nullptr || (plaintext == nullptr && plaintext_len != 0U)) return false;
  out->clear();
  if (!g_enabled) {
    if (MROS_REQUIRE_SECURE_UART == 0) {
      if (plaintext_len != 0U) out->assign(plaintext, plaintext + plaintext_len);
      return true;
    }
    if (error != nullptr) *error = "uart crypto required but not enabled";
    return false;
  }
  if (!g_key_loaded || !ensure_sequence(error)) return false;
  if (g_boot_salt == 0U) {
    if (error != nullptr) *error = "hardware RNG unavailable for secure UART nonce";
    return false;
  }
  const uint32_t sequence = ++g_tx_sequence;
  uint8_t nonce[kNonceLen] = {};
  store32_le(nonce, g_boot_salt);
  store32_le(nonce + 4U, kEspTxDomain);
  store32_le(nonce + 8U, sequence);
  out->resize(kHeaderLen + plaintext_len + kTagLen);
  std::memcpy(out->data(), kMagic, sizeof(kMagic));
  (*out)[4] = kVersion;
  (*out)[5] = frame_type;
  store32_le(out->data() + 6U, sequence);
  std::memcpy(out->data() + 10U, nonce, sizeof(nonce));
  uint8_t* cipher = out->data() + kHeaderLen;
  uint8_t* tag = out->data() + kHeaderLen + plaintext_len;
  if (!crypt(true, g_tx_key, nonce, out->data(), kHeaderLen, plaintext, plaintext_len,
             nullptr, cipher, tag)) {
    out->clear();
    if (error != nullptr) *error = "ChaCha20-Poly1305 encryption failed";
    return false;
  }
  return true;
}

bool open(const uint8_t* frame, const size_t frame_len, std::vector<uint8_t>* plaintext,
          uint8_t* frame_type, std::string* error) {
  init();
  StateLock lock;
  if (!lock.locked() || frame == nullptr || plaintext == nullptr) return false;
  plaintext->clear();
  if (!g_enabled) {
    if (MROS_REQUIRE_SECURE_UART == 0) {
      if (frame_len != 0U) plaintext->assign(frame, frame + frame_len);
      if (frame_type != nullptr) *frame_type = 0U;
      return true;
    }
    if (error != nullptr) *error = "uart crypto required but not enabled";
    return false;
  }
  if (!g_key_loaded || frame_len < kHeaderLen + kTagLen ||
      std::memcmp(frame, kMagic, sizeof(kMagic)) != 0 || frame[4] != kVersion ||
      (frame[5] != 1U && frame[5] != 2U)) {
    if (error != nullptr) *error = "invalid secure UART envelope";
    ++g_auth_failures;
    return false;
  }
  const uint32_t sequence = load32_le(frame + 6U);
  const uint8_t* nonce = frame + 10U;
  if (sequence == 0U || load32_le(nonce + 4U) != kTeensyTxDomain ||
      load32_le(nonce + 8U) != sequence) {
    if (error != nullptr) *error = "invalid secure UART nonce sequence";
    ++g_auth_failures;
    return false;
  }
  const size_t cipher_len = frame_len - kHeaderLen - kTagLen;
  plaintext->resize(cipher_len);
  const uint8_t* cipher = frame + kHeaderLen;
  const uint8_t* tag = frame + kHeaderLen + cipher_len;
  if (!crypt(false, g_rx_key, nonce, frame, kHeaderLen, cipher, cipher_len,
             tag, plaintext->data(), nullptr)) {
    plaintext->clear();
    ++g_auth_failures;
    if (error != nullptr) *error = "authentication failed";
    return false;
  }
  if (!accept_sequence(frame, sequence, error)) {
    plaintext->clear();
    ++g_replay_rejects;
    return false;
  }
  if (frame_type != nullptr) *frame_type = frame[5];
  return true;
}

bool self_test(std::string* error) {
  static const uint8_t key[32] = {
      0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
      0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f};
  static const uint8_t nonce[12] = {0x07,0x00,0x00,0x00,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
  static const uint8_t aad[12] = {0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7};
  static const uint8_t plain[] =
      "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
  static const uint8_t expected_tag[16] = {
      0x1a,0xe1,0x0b,0x59,0x4f,0x09,0xe2,0x6a,0x7e,0x90,0x2e,0xcb,0xd0,0x60,0x06,0x91};
  std::array<uint8_t, sizeof(plain) - 1U> cipher = {};
  uint8_t tag[16] = {};
  if (!crypt(true, key, nonce, aad, sizeof(aad), plain, cipher.size(), nullptr,
             cipher.data(), tag) || std::memcmp(tag, expected_tag, sizeof(tag)) != 0) {
    if (error != nullptr) *error = "RFC 8439 ChaCha20-Poly1305 test failed";
    return false;
  }
  if (error != nullptr) error->clear();
  return true;
}

}  // namespace mros::security::uart_secure
