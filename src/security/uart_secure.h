#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mros::security::uart_secure {

struct Status {
  bool enabled = false;
  bool key_loaded = false;
  bool secure_required = true;
  uint32_t tx_sequence = 0U;
  uint32_t rx_sequence = 0U;
  uint32_t auth_failures = 0U;
  uint32_t replay_rejects = 0U;
};

void init();
bool enable_with_psk_hex(const char* hex, std::string* error = nullptr);
bool disable(std::string* error = nullptr);
Status status();
std::string status_text(bool json = false);
bool self_test(std::string* error = nullptr);

bool seal(uint8_t frame_type, const uint8_t* plaintext, size_t plaintext_len,
          std::vector<uint8_t>* out, std::string* error = nullptr);
bool open(const uint8_t* frame, size_t frame_len, std::vector<uint8_t>* plaintext,
          uint8_t* frame_type, std::string* error = nullptr);

}  // namespace mros::security::uart_secure
