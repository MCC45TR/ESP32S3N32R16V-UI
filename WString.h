#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

class __FlashStringHelper;

#ifndef FPSTR
#define FPSTR(str_pointer) (reinterpret_cast<const __FlashStringHelper *>(str_pointer))
#endif

#ifndef F
#define F(string_literal) (string_literal)
#endif

class String;
using StringSumHelper = String;

class String {
 public:
  String() = default;
  String(const char* cstr) : data_(cstr != nullptr ? cstr : "") {}
  String(const char* cstr, unsigned int length)
      : data_(cstr != nullptr ? std::string(cstr, length) : std::string()) {}
  String(const std::string& value) : data_(value) {}
  String(std::string&& value) : data_(std::move(value)) {}
  String(const String&) = default;
  String(String&&) noexcept = default;
  explicit String(char c) : data_(1, c) {}
  explicit String(unsigned char value, unsigned char base = 10)
      : data_(formatUnsigned(value, base)) {}
  explicit String(int value, unsigned char base = 10)
      : data_(formatSigned(value, base)) {}
  explicit String(unsigned int value, unsigned char base = 10)
      : data_(formatUnsigned(value, base)) {}
  explicit String(long value, unsigned char base = 10)
      : data_(formatSigned(value, base)) {}
  explicit String(unsigned long value, unsigned char base = 10)
      : data_(formatUnsigned(value, base)) {}
  explicit String(long long value, unsigned char base = 10)
      : data_(formatSigned(value, base)) {}
  explicit String(unsigned long long value, unsigned char base = 10)
      : data_(formatUnsigned(value, base)) {}
  explicit String(float value, unsigned int decimalPlaces = 2)
      : data_(formatFloat(static_cast<double>(value), decimalPlaces)) {}
  explicit String(double value, unsigned int decimalPlaces = 2)
      : data_(formatFloat(value, decimalPlaces)) {}

  ~String() = default;

  String& operator=(const String&) = default;
  String& operator=(String&&) noexcept = default;

  String& operator=(const char* cstr) {
    data_ = (cstr != nullptr) ? cstr : "";
    return *this;
  }

  String& operator=(const std::string& value) {
    data_ = value;
    return *this;
  }

  bool reserve(unsigned int size) {
    data_.reserve(size);
    return true;
  }

  unsigned int length() const { return static_cast<unsigned int>(data_.size()); }
  void clear() { data_.clear(); }
  bool isEmpty() const { return data_.empty(); }

  bool concat(const String& rhs) {
    data_ += rhs.data_;
    return true;
  }

  bool concat(const char* cstr) {
    if (cstr == nullptr) {
      return false;
    }
    data_ += cstr;
    return true;
  }

  bool concat(const char* cstr, unsigned int length) {
    if (cstr == nullptr) {
      return false;
    }
    data_.append(cstr, length);
    return true;
  }

  bool concat(char c) {
    data_.push_back(c);
    return true;
  }

  bool concat(unsigned char value) { return concat(String(value)); }
  bool concat(int value) { return concat(String(value)); }
  bool concat(unsigned int value) { return concat(String(value)); }
  bool concat(long value) { return concat(String(value)); }
  bool concat(unsigned long value) { return concat(String(value)); }
  bool concat(long long value) { return concat(String(value)); }
  bool concat(unsigned long long value) { return concat(String(value)); }
  bool concat(float value) { return concat(String(value)); }
  bool concat(double value) { return concat(String(value)); }

  String& operator+=(const String& rhs) {
    concat(rhs);
    return *this;
  }

  String& operator+=(const char* cstr) {
    concat(cstr);
    return *this;
  }

  String& operator+=(char c) {
    concat(c);
    return *this;
  }

  String& operator+=(unsigned char value) {
    concat(value);
    return *this;
  }

  String& operator+=(int value) {
    concat(value);
    return *this;
  }

  String& operator+=(unsigned int value) {
    concat(value);
    return *this;
  }

  String& operator+=(long value) {
    concat(value);
    return *this;
  }

  String& operator+=(unsigned long value) {
    concat(value);
    return *this;
  }

  String& operator+=(long long value) {
    concat(value);
    return *this;
  }

  String& operator+=(unsigned long long value) {
    concat(value);
    return *this;
  }

  String& operator+=(float value) {
    concat(value);
    return *this;
  }

  String& operator+=(double value) {
    concat(value);
    return *this;
  }

  explicit operator bool() const { return true; }

  int compareTo(const String& rhs) const {
    if (data_ < rhs.data_) {
      return -1;
    }
    if (data_ > rhs.data_) {
      return 1;
    }
    return 0;
  }

  bool equals(const String& rhs) const { return data_ == rhs.data_; }
  bool equals(const char* cstr) const { return data_ == (cstr != nullptr ? cstr : ""); }

  bool operator==(const String& rhs) const { return equals(rhs); }
  bool operator==(const char* cstr) const { return equals(cstr); }
  bool operator!=(const String& rhs) const { return !equals(rhs); }
  bool operator!=(const char* cstr) const { return !equals(cstr); }
  bool operator<(const String& rhs) const { return data_ < rhs.data_; }
  bool operator>(const String& rhs) const { return data_ > rhs.data_; }
  bool operator<=(const String& rhs) const { return data_ <= rhs.data_; }
  bool operator>=(const String& rhs) const { return data_ >= rhs.data_; }

  bool equalsIgnoreCase(const String& rhs) const {
    if (data_.size() != rhs.data_.size()) {
      return false;
    }
    for (size_t i = 0; i < data_.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(data_[i])) !=
          std::tolower(static_cast<unsigned char>(rhs.data_[i]))) {
        return false;
      }
    }
    return true;
  }

  unsigned char equalsConstantTime(const String& rhs) const {
    if (data_.size() != rhs.data_.size()) {
      return 0;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < data_.size(); ++i) {
      diff |= static_cast<unsigned char>(data_[i] ^ rhs.data_[i]);
    }
    return diff == 0 ? 1 : 0;
  }

  bool startsWith(const String& prefix) const { return startsWith(prefix, 0U); }

  bool startsWith(const char* prefix) const { return startsWith(String(prefix)); }

  bool startsWith(const String& prefix, unsigned int offset) const {
    if (offset > data_.size()) {
      return false;
    }
    const size_t prefix_len = prefix.data_.size();
    if (offset + prefix_len > data_.size()) {
      return false;
    }
    return data_.compare(offset, prefix_len, prefix.data_) == 0;
  }

  bool endsWith(const String& suffix) const {
    if (suffix.data_.size() > data_.size()) {
      return false;
    }
    return data_.compare(data_.size() - suffix.data_.size(), suffix.data_.size(),
                         suffix.data_) == 0;
  }

  bool endsWith(const char* suffix) const { return endsWith(String(suffix)); }

  char charAt(unsigned int index) const {
    if (index >= data_.size()) {
      return '\0';
    }
    return data_[index];
  }

  void setCharAt(unsigned int index, char c) {
    if (index < data_.size()) {
      data_[index] = c;
    }
  }

  char operator[](unsigned int index) const { return charAt(index); }

  char& operator[](unsigned int index) {
    if (index >= data_.size()) {
      static char dummy = '\0';
      return dummy;
    }
    return data_[index];
  }

  void getBytes(unsigned char* buffer, unsigned int bufsize,
                unsigned int index = 0) const {
    if (buffer == nullptr || bufsize == 0U) {
      return;
    }
    const size_t start = std::min<size_t>(index, data_.size());
    const size_t copy_len = std::min<size_t>(bufsize - 1U, data_.size() - start);
    if (copy_len > 0U) {
      std::memcpy(buffer, data_.data() + start, copy_len);
    }
    buffer[copy_len] = '\0';
  }

  void toCharArray(char* buffer, unsigned int bufsize,
                   unsigned int index = 0) const {
    getBytes(reinterpret_cast<unsigned char*>(buffer), bufsize, index);
  }

  const char* c_str() const { return data_.c_str(); }

  String substring(unsigned int beginIndex) const {
    return substring(beginIndex, length());
  }

  String substring(unsigned int beginIndex, unsigned int endIndex) const {
    if (beginIndex >= data_.size() || beginIndex >= endIndex) {
      return String();
    }
    const size_t safe_end = std::min<size_t>(endIndex, data_.size());
    return String(data_.substr(beginIndex, safe_end - beginIndex));
  }

  int indexOf(char ch, unsigned int fromIndex = 0U) const {
    const size_t pos = data_.find(ch, fromIndex);
    return (pos == std::string::npos) ? -1 : static_cast<int>(pos);
  }

  int indexOf(const String& needle, unsigned int fromIndex = 0U) const {
    const size_t pos = data_.find(needle.data_, fromIndex);
    return (pos == std::string::npos) ? -1 : static_cast<int>(pos);
  }

  int indexOf(const char* needle, unsigned int fromIndex = 0U) const {
    return indexOf(String(needle), fromIndex);
  }

  int lastIndexOf(char ch) const {
    const size_t pos = data_.rfind(ch);
    return (pos == std::string::npos) ? -1 : static_cast<int>(pos);
  }

  int lastIndexOf(const String& needle) const {
    const size_t pos = data_.rfind(needle.data_);
    return (pos == std::string::npos) ? -1 : static_cast<int>(pos);
  }

  long toInt() const { return std::strtol(data_.c_str(), nullptr, 10); }
  float toFloat() const { return std::strtof(data_.c_str(), nullptr); }
  double toDouble() const { return std::strtod(data_.c_str(), nullptr); }

  void trim() {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    auto begin_it = std::find_if(data_.begin(), data_.end(), not_space);
    auto end_it = std::find_if(data_.rbegin(), data_.rend(), not_space).base();
    if (begin_it >= end_it) {
      data_.clear();
      return;
    }
    data_ = std::string(begin_it, end_it);
  }

  void toLowerCase() {
    for (char& ch : data_) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
  }

  void toUpperCase() {
    for (char& ch : data_) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
  }

  void remove(unsigned int index) { remove(index, length() - index); }

  void remove(unsigned int index, unsigned int count) {
    if (index >= data_.size()) {
      return;
    }
    data_.erase(index, count);
  }

  void replace(const String& find, const String& replaceWith) {
    if (find.isEmpty()) {
      return;
    }
    size_t pos = 0U;
    while ((pos = data_.find(find.data_, pos)) != std::string::npos) {
      data_.replace(pos, find.data_.size(), replaceWith.data_);
      pos += replaceWith.data_.size();
    }
  }

  void replace(const char* find, const char* replaceWith) {
    replace(String(find), String(replaceWith));
  }

  const std::string& std_str() const { return data_; }
  std::string& std_str() { return data_; }

 private:
  std::string data_;

  template <typename T>
  static std::string formatUnsigned(T value, unsigned char base) {
    if (base < 2 || base > 36) {
      base = 10;
    }
    if (value == 0) {
      return "0";
    }
    std::string out;
    T current = value;
    while (current > 0) {
      const unsigned digit = static_cast<unsigned>(current % base);
      out.push_back(static_cast<char>(digit < 10 ? ('0' + digit)
                                                 : ('A' + (digit - 10))));
      current /= base;
    }
    std::reverse(out.begin(), out.end());
    return out;
  }

  template <typename T>
  static std::string formatSigned(T value, unsigned char base) {
    if (base == 10 && value < 0) {
      using UnsignedT = typename std::make_unsigned<T>::type;
      UnsignedT magnitude = static_cast<UnsignedT>(-(value + 1));
      magnitude += 1U;
      return std::string("-") + formatUnsigned(magnitude, base);
    }
    return formatUnsigned(static_cast<typename std::make_unsigned<T>::type>(value),
                          base);
  }

  static std::string formatFloat(double value, unsigned int decimalPlaces) {
    if (!std::isfinite(value)) {
      return "nan";
    }
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%.*f",
                  static_cast<int>(decimalPlaces), value);
    return std::string(buffer);
  }
};

inline String operator+(const String& lhs, const String& rhs) {
  String out(lhs);
  out += rhs;
  return out;
}

inline String operator+(const String& lhs, const char* rhs) {
  String out(lhs);
  out += rhs;
  return out;
}

inline String operator+(const char* lhs, const String& rhs) {
  String out(lhs);
  out += rhs;
  return out;
}

inline String operator+(const String& lhs, char rhs) {
  String out(lhs);
  out += rhs;
  return out;
}

inline String operator+(char lhs, const String& rhs) {
  String out(lhs);
  out += rhs;
  return out;
}
