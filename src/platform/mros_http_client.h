#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_http_client.h>

namespace mros::platform {

struct HttpClientConfig {
  bool allow_insecure_tls = false;
  bool allow_private_hosts = false;
  int timeout_ms = 15000;
  int max_redirects = 0;
  size_t buffer_size = 1024U;
};

struct HttpClientStream {
  esp_http_client_handle_t handle = nullptr;
  int status_code = -1;
  int64_t content_length = -1;
};

bool mros_http_client_begin_get(const char* url, const HttpClientConfig& config,
                                HttpClientStream* stream);
int mros_http_client_read(HttpClientStream* stream, void* buffer, size_t buffer_size);
bool mros_http_client_connected(const HttpClientStream& stream);
void mros_http_client_close(HttpClientStream* stream);

}  // namespace mros::platform
