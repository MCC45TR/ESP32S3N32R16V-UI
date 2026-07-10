#pragma once

#include <array>
#include <cstdint>

#include "WString.h"

class IPAddress {
 public:
  IPAddress() : bytes_{0, 0, 0, 0} {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
      : bytes_{a, b, c, d} {}
  explicit IPAddress(uint32_t value)
      : bytes_{static_cast<uint8_t>(value & 0xFFU),
               static_cast<uint8_t>((value >> 8) & 0xFFU),
               static_cast<uint8_t>((value >> 16) & 0xFFU),
               static_cast<uint8_t>((value >> 24) & 0xFFU)} {}

  uint8_t operator[](size_t index) const { return bytes_[index]; }
  uint8_t& operator[](size_t index) { return bytes_[index]; }

  bool operator==(const IPAddress& rhs) const { return bytes_ == rhs.bytes_; }
  bool operator!=(const IPAddress& rhs) const { return !(*this == rhs); }

  bool isSet() const {
    return bytes_[0] != 0U || bytes_[1] != 0U || bytes_[2] != 0U ||
           bytes_[3] != 0U;
  }

  uint32_t toUInt() const {
    return static_cast<uint32_t>(bytes_[0]) |
           (static_cast<uint32_t>(bytes_[1]) << 8U) |
           (static_cast<uint32_t>(bytes_[2]) << 16U) |
           (static_cast<uint32_t>(bytes_[3]) << 24U);
  }

  String toString() const {
    String out(bytes_[0]);
    out += ".";
    out += String(bytes_[1]);
    out += ".";
    out += String(bytes_[2]);
    out += ".";
    out += String(bytes_[3]);
    return out;
  }

 private:
  std::array<uint8_t, 4> bytes_;
};
