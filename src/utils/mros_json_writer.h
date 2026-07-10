#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cstdio>
#include <cstring>

namespace mros::utils {

void record_json_overflow();
uint32_t json_overflow_count();
void reset_json_overflow_count();

class FixedJsonWriter {
 public:
  FixedJsonWriter(char* buffer, const size_t capacity)
      : buffer_(buffer), capacity_(capacity) {
    reset();
  }

  void reset() {
    length_ = 0U;
    first_ = true;
    overflow_ = false;
    overflow_recorded_ = false;
    if (buffer_ != nullptr && capacity_ > 0U) {
      buffer_[0] = '\0';
    }
  }

  void begin() {
    reset();
    append_char('{');
  }

  void end() { append_char('}'); }

  void raw_field(const char* key, const char* value) {
    field_prefix(key);
    append_raw(value != nullptr ? value : "null");
  }

  void string_field(const char* key, const char* value) {
    field_prefix(key);
    append_char('"');
    append_escaped(value != nullptr ? value : "");
    append_char('"');
  }

  void bool_field(const char* key, const bool value) {
    raw_field(key, value ? "true" : "false");
  }

  void i32_field(const char* key, const int32_t value) {
    char tmp[24] = {};
    std::snprintf(tmp, sizeof(tmp), "%ld", static_cast<long>(value));
    raw_field(key, tmp);
  }

  void u32_field(const char* key, const uint32_t value) {
    char tmp[24] = {};
    std::snprintf(tmp, sizeof(tmp), "%lu", static_cast<unsigned long>(value));
    raw_field(key, tmp);
  }

  void u64_field(const char* key, const uint64_t value) {
    char tmp[32] = {};
    std::snprintf(tmp, sizeof(tmp), "%llu",
                  static_cast<unsigned long long>(value));
    raw_field(key, tmp);
  }

  void u32(const uint32_t value) {
    char tmp[24] = {};
    std::snprintf(tmp, sizeof(tmp), "%lu", static_cast<unsigned long>(value));
    append_raw(tmp);
  }

  void i32(const int32_t value) {
    char tmp[24] = {};
    std::snprintf(tmp, sizeof(tmp), "%ld", static_cast<long>(value));
    append_raw(tmp);
  }

  void float_field(const char* key, const float value, const unsigned decimals) {
    char tmp[32] = {};
    std::snprintf(tmp, sizeof(tmp), "%.*f", static_cast<int>(decimals),
                  static_cast<double>(value));
    raw_field(key, tmp);
  }

  void append_raw(const char* text) {
    if (text == nullptr) return;
    append_bytes(text, std::strlen(text));
  }

  void append_raw(const char* text, const size_t len) {
    if (text == nullptr) return;
    append_bytes(text, len);
  }

  void append_escaped(const char* text) {
    if (text == nullptr) return;
    for (const char* p = text; *p != '\0'; ++p) {
      append_escaped_char(*p);
    }
  }

  void append_escaped(const char* text, const size_t len) {
    if (text == nullptr) return;
    for (size_t i = 0U; i < len; ++i) {
      append_escaped_char(text[i]);
    }
  }

  const char* c_str() const {
    return (buffer_ != nullptr && capacity_ > 0U) ? buffer_ : "";
  }

  size_t length() const { return length_; }
  bool overflow() const { return overflow_; }
  size_t capacity() const { return capacity_; }

 private:
  void field_prefix(const char* key) {
    if (!first_) {
      append_char(',');
    }
    first_ = false;
    append_char('"');
    append_escaped(key != nullptr ? key : "");
    append_raw("\":");
  }

  void append_escaped_char(const char ch) {
    switch (ch) {
      case '"':
        append_raw("\\\"");
        break;
      case '\\':
        append_raw("\\\\");
        break;
      case '\b':
        append_raw("\\b");
        break;
      case '\f':
        append_raw("\\f");
        break;
      case '\n':
        append_raw("\\n");
        break;
      case '\r':
        append_raw("\\r");
        break;
      case '\t':
        append_raw("\\t");
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          char tmp[7] = {};
          std::snprintf(tmp, sizeof(tmp), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(ch)));
          append_raw(tmp);
        } else {
          append_char(ch);
        }
        break;
    }
  }

  void append_char(const char ch) {
    if (buffer_ == nullptr || capacity_ == 0U) {
      note_overflow();
      return;
    }
    if (length_ + 1U >= capacity_) {
      note_overflow();
      buffer_[capacity_ - 1U] = '\0';
      return;
    }
    buffer_[length_++] = ch;
    buffer_[length_] = '\0';
  }

  void append_bytes(const char* text, const size_t len) {
    if (text == nullptr || len == 0U) return;
    if (buffer_ == nullptr || capacity_ == 0U) {
      note_overflow();
      return;
    }
    const size_t room = (capacity_ > length_) ? (capacity_ - length_ - 1U) : 0U;
    if (len > room) {
      if (room > 0U) {
        std::memcpy(buffer_ + length_, text, room);
        length_ += room;
        buffer_[length_] = '\0';
      }
      note_overflow();
      return;
    }
    std::memcpy(buffer_ + length_, text, len);
    length_ += len;
    buffer_[length_] = '\0';
  }

  void note_overflow() {
    overflow_ = true;
    if (!overflow_recorded_) {
      overflow_recorded_ = true;
      record_json_overflow();
    }
  }

  char* buffer_ = nullptr;
  size_t capacity_ = 0U;
  size_t length_ = 0U;
  bool first_ = true;
  bool overflow_ = false;
  bool overflow_recorded_ = false;
};

}  // namespace mros::utils
