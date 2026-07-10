#include "src/platform/mros_http_client.h"

#include <cctype>
#include <cstring>
#include <string>

#include <esp_err.h>

#if defined(__has_include)
#if __has_include(<esp_crt_bundle.h>)
#include <esp_crt_bundle.h>
#define MROS_HAS_ESP_CRT_BUNDLE 1
#else
#define MROS_HAS_ESP_CRT_BUNDLE 0
#endif
#else
#include <esp_crt_bundle.h>
#define MROS_HAS_ESP_CRT_BUNDLE 1
#endif

namespace mros::platform {

namespace {

bool is_redirect_status(const int status_code) {
  return status_code == 301 || status_code == 302 || status_code == 303 ||
         status_code == 307 || status_code == 308;
}

bool url_has_scheme(const char* url, const char* scheme) {
  if (url == nullptr || scheme == nullptr) {
    return false;
  }
  for (size_t i = 0U; scheme[i] != '\0'; ++i) {
    const unsigned char url_ch = static_cast<unsigned char>(url[i]);
    const unsigned char scheme_ch = static_cast<unsigned char>(scheme[i]);
    if (url_ch == 0U ||
        std::tolower(url_ch) != std::tolower(scheme_ch)) {
      return false;
    }
  }
  return true;
}

std::string lower_copy(std::string text) {
  for (char& ch : text) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return text;
}

std::string strip_trailing_dots(std::string host) {
  while (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  return host;
}

bool has_suffix(const std::string& text, const char* suffix) {
  if (suffix == nullptr) {
    return false;
  }
  const size_t suffix_len = std::strlen(suffix);
  return text.size() >= suffix_len &&
         text.compare(text.size() - suffix_len, suffix_len, suffix) == 0;
}

bool parse_ipv4_literal(const std::string& host, uint8_t octets[4]) {
  size_t start = 0U;
  for (int part = 0; part < 4; ++part) {
    const size_t dot = host.find('.', start);
    const size_t end = dot == std::string::npos ? host.size() : dot;
    if ((part < 3 && dot == std::string::npos) ||
        (part == 3 && dot != std::string::npos) || end <= start ||
        (end - start) > 3U) {
      return false;
    }
    int value = 0;
    for (size_t i = start; i < end; ++i) {
      const unsigned char ch = static_cast<unsigned char>(host[i]);
      if (!std::isdigit(ch)) {
        return false;
      }
      value = (value * 10) + static_cast<int>(ch - '0');
      if (value > 255) {
        return false;
      }
    }
    octets[part] = static_cast<uint8_t>(value);
    start = end + 1U;
  }
  return start == host.size() + 1U;
}

bool is_blocked_ipv4_literal(const uint8_t octets[4]) {
  const uint8_t a = octets[0];
  const uint8_t b = octets[1];
  if (a == 0U || a == 10U || a == 127U) return true;
  if (a == 100U && b >= 64U && b <= 127U) return true;
  if (a == 169U && b == 254U) return true;
  if (a == 172U && b >= 16U && b <= 31U) return true;
  if (a == 192U && b == 168U) return true;
  if (a == 198U && (b == 18U || b == 19U)) return true;
  if (a >= 224U) return true;
  return false;
}

bool is_all_digits(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (const unsigned char ch : text) {
    if (!std::isdigit(ch)) {
      return false;
    }
  }
  return true;
}

bool extract_url_host(const char* url, std::string* host) {
  if (url == nullptr || host == nullptr) {
    return false;
  }
  const char* scheme_end = std::strstr(url, "://");
  if (scheme_end == nullptr) {
    return false;
  }
  const char* start = scheme_end + 3;
  const char* end = start;
  while (*end != '\0' && *end != '/' && *end != '?' && *end != '#') {
    ++end;
  }
  if (end == start) {
    return false;
  }
  std::string authority(start, static_cast<size_t>(end - start));
  if (authority.find('@') != std::string::npos) {
    return false;
  }
  if (authority[0] == '[') {
    const size_t close = authority.find(']');
    if (close == std::string::npos || close <= 1U) {
      return false;
    }
    *host = authority.substr(1U, close - 1U);
    return true;
  }
  const size_t first_colon = authority.find(':');
  const size_t last_colon = authority.rfind(':');
  if (first_colon != std::string::npos && first_colon != last_colon) {
    return false;
  }
  *host = first_colon == std::string::npos ? authority : authority.substr(0U, first_colon);
  return !host->empty();
}

bool has_blocked_private_host(const char* url) {
  std::string host;
  if (!extract_url_host(url, &host)) {
    return true;
  }
  host = strip_trailing_dots(lower_copy(host));
  if (host.empty()) return true;
  if (host == "localhost" || has_suffix(host, ".localhost") ||
      has_suffix(host, ".local") || has_suffix(host, ".lan") ||
      has_suffix(host, ".home.arpa")) {
    return true;
  }
  if (host.find(':') != std::string::npos || host.find('%') != std::string::npos) {
    return true;
  }
  if (is_all_digits(host) || host.rfind("0x", 0U) == 0U) {
    return true;
  }
  uint8_t octets[4] = {};
  if (parse_ipv4_literal(host, octets)) {
    return is_blocked_ipv4_literal(octets);
  }
  return false;
}

}  // namespace

bool mros_http_client_begin_get(const char* url, const HttpClientConfig& config,
                                HttpClientStream* stream) {
  if (url == nullptr || stream == nullptr) {
    return false;
  }
  if (!config.allow_insecure_tls && !url_has_scheme(url, "https://")) {
    return false;
  }
  if (!config.allow_private_hosts && has_blocked_private_host(url)) {
    return false;
  }

  mros_http_client_close(stream);

  esp_http_client_config_t http_config = {};
  http_config.url = url;
  http_config.method = HTTP_METHOD_GET;
  http_config.timeout_ms = config.timeout_ms;
  http_config.buffer_size = static_cast<int>(config.buffer_size);
  http_config.keep_alive_enable = true;
  http_config.skip_cert_common_name_check = config.allow_insecure_tls;
#if MROS_HAS_ESP_CRT_BUNDLE && defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  http_config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

  stream->handle = esp_http_client_init(&http_config);
  if (stream->handle == nullptr) {
    return false;
  }

  for (int redirect_count = 0; redirect_count <= config.max_redirects; ++redirect_count) {
    if (esp_http_client_open(stream->handle, 0) != ESP_OK) {
      mros_http_client_close(stream);
      return false;
    }

    stream->content_length = esp_http_client_fetch_headers(stream->handle);
    stream->status_code = esp_http_client_get_status_code(stream->handle);
    if (!is_redirect_status(stream->status_code)) {
      return true;
    }
    if (!config.allow_insecure_tls || config.max_redirects <= 0) {
      mros_http_client_close(stream);
      return false;
    }

    if (esp_http_client_set_redirection(stream->handle) != ESP_OK) {
      mros_http_client_close(stream);
      return false;
    }
    esp_http_client_close(stream->handle);
  }

  mros_http_client_close(stream);
  return false;
}

int mros_http_client_read(HttpClientStream* stream, void* buffer,
                          const size_t buffer_size) {
  if (stream == nullptr || stream->handle == nullptr || buffer == nullptr ||
      buffer_size == 0U) {
    return -1;
  }
  return esp_http_client_read(stream->handle, static_cast<char*>(buffer),
                              static_cast<int>(buffer_size));
}

bool mros_http_client_connected(const HttpClientStream& stream) {
  return stream.handle != nullptr &&
         !esp_http_client_is_complete_data_received(stream.handle);
}

void mros_http_client_close(HttpClientStream* stream) {
  if (stream == nullptr) {
    return;
  }
  if (stream->handle != nullptr) {
    esp_http_client_close(stream->handle);
    esp_http_client_cleanup(stream->handle);
  }
  stream->handle = nullptr;
  stream->status_code = -1;
  stream->content_length = -1;
}

}  // namespace mros::platform
