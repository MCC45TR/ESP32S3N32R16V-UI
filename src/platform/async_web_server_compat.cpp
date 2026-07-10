#include "ESPAsyncWebServer.h"

#include "sdkconfig.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>
#include <unistd.h>

extern "C" {
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
}

namespace {

constexpr char kTag[] = "AsyncCompat";

static AsyncWebServer* g_active_server = nullptr;
static std::mutex g_server_mutex;

constexpr int kHttpdInternalSockets = 3;
#ifndef MROS_HTTPD_MAX_OPEN_SOCKETS
#define MROS_HTTPD_MAX_OPEN_SOCKETS 10
#endif
#ifndef MROS_HTTPD_STACK_SIZE
#define MROS_HTTPD_STACK_SIZE 8192
#endif
#ifndef MROS_HTTPD_LRU_PURGE_ENABLE
#define MROS_HTTPD_LRU_PURGE_ENABLE 1
#endif
constexpr int kPreferredOpenSockets = MROS_HTTPD_MAX_OPEN_SOCKETS;
constexpr int kPreferredHttpdStackSize = MROS_HTTPD_STACK_SIZE;
constexpr bool kPreferredLruPurgeEnable = MROS_HTTPD_LRU_PURGE_ENABLE != 0;

struct StatusLine {
  int code;
  const char* text;
};

constexpr StatusLine kStatusLines[] = {
    {200, "200 OK"},
    {202, "202 Accepted"},
    {302, "302 Found"},
    {400, "400 Bad Request"},
    {401, "401 Unauthorized"},
    {403, "403 Forbidden"},
    {404, "404 Not Found"},
    {409, "409 Conflict"},
    {413, "413 Payload Too Large"},
    {429, "429 Too Many Requests"},
    {500, "500 Internal Server Error"},
    {503, "503 Service Unavailable"},
};

const char* http_status_text(const int code) {
  for (const auto& entry : kStatusLines) {
    if (entry.code == code) {
      return entry.text;
    }
  }
  return "200 OK";
}

String url_decode(const String& in) {
  String out;
  out.reserve(in.length());
  for (unsigned int i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '+') {
      out += ' ';
      continue;
    }
    if (c == '%' && (i + 2U) < in.length()) {
      char hex[3] = {in[i + 1U], in[i + 2U], '\0'};
      char* end = nullptr;
      const long value = std::strtol(hex, &end, 16);
      if (end != nullptr && *end == '\0') {
        out += static_cast<char>(value);
        i += 2U;
        continue;
      }
    }
    out += c;
  }
  return out;
}

void parse_urlencoded(const String& body,
                      const bool post,
                      std::vector<AsyncWebParameter>* out) {
  if (out == nullptr) {
    return;
  }
  out->clear();
  if (body.isEmpty()) {
    return;
  }

  unsigned int pos = 0U;
  while (pos < body.length()) {
    int amp = body.indexOf('&', pos);
    if (amp < 0) {
      amp = static_cast<int>(body.length());
    }
    const int eq = body.indexOf('=', pos);
    if (eq >= 0 && eq < amp) {
      const String key = url_decode(body.substring(pos, static_cast<unsigned int>(eq)));
      const String value =
          url_decode(body.substring(static_cast<unsigned int>(eq + 1), amp));
      out->emplace_back(key, value, post);
    } else {
      const String key = url_decode(body.substring(pos, amp));
      out->emplace_back(key, String(), post);
    }
    pos = static_cast<unsigned int>(amp + 1);
  }
}

String read_request_body(httpd_req_t* request) {
  String body;
  if (request == nullptr || request->content_len <= 0) {
    return body;
  }
  body.reserve(static_cast<unsigned int>(request->content_len));
  std::array<char, 1024> buffer = {};
  int remaining = request->content_len;
  while (remaining > 0) {
    const int to_read = std::min<int>(remaining, static_cast<int>(buffer.size()));
    const int read = httpd_req_recv(request, buffer.data(), to_read);
    if (read <= 0) {
      break;
    }
    body.concat(buffer.data(), static_cast<unsigned int>(read));
    remaining -= read;
  }
  return body;
}

bool drain_request_body(httpd_req_t* request, size_t remaining) {
  if (request == nullptr) {
    return false;
  }
  std::array<char, 512> buffer = {};
  while (remaining > 0U) {
    const size_t to_read = std::min(buffer.size(), remaining);
    const int read = httpd_req_recv(request, buffer.data(), to_read);
    if (read <= 0) {
      return false;
    }
    remaining -= static_cast<size_t>(read);
  }
  return true;
}

struct WsSendWork {
  AsyncWebSocket* owner = nullptr;
  httpd_handle_t handle = nullptr;
  int socket_fd = -1;
  char* payload = nullptr;
  size_t len = 0U;
  httpd_ws_type_t type = HTTPD_WS_TYPE_TEXT;
};

void ws_send_work(void* arg) {
  auto* work = static_cast<WsSendWork*>(arg);
  if (work == nullptr) {
    return;
  }

  esp_err_t result = ESP_ERR_INVALID_ARG;
  if (work->handle != nullptr && work->socket_fd >= 0) {
    httpd_ws_frame_t frame = {};
    frame.final = true;
    frame.type = work->type;
    frame.payload = reinterpret_cast<uint8_t*>(work->payload);
    frame.len = work->len;
    result = httpd_ws_send_frame_async(work->handle, work->socket_fd, &frame);
  }

  if (work->owner != nullptr) {
    work->owner->note_send_result(work->socket_fd, result);
  }
  if (work->payload != nullptr) {
    heap_caps_free(work->payload);
  }
  delete work;
}

char* alloc_ws_payload(const size_t len) {
  if (len == 0U) {
    return nullptr;
  }
  char* ptr = static_cast<char*>(
      heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (ptr != nullptr) {
    return ptr;
  }
  return static_cast<char*>(
      heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

struct WsCloseWork {
  httpd_handle_t handle = nullptr;
  int socket_fd = -1;
};

void ws_close_work(void* arg) {
  auto* work = static_cast<WsCloseWork*>(arg);
  if (work == nullptr) {
    return;
  }
  if (work->handle != nullptr && work->socket_fd >= 0) {
    httpd_sess_trigger_close(work->handle, work->socket_fd);
  }
  delete work;
}

void close_dispatch(httpd_handle_t, int socket_fd) {
  {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    if (g_active_server != nullptr) {
      g_active_server->handle_close(socket_fd);
    }
  }
  close(socket_fd);
}

esp_err_t ws_dispatch(httpd_req_t* request) {
  if (request == nullptr || request->user_ctx == nullptr) {
    return ESP_FAIL;
  }
  auto* ws = static_cast<AsyncWebSocket*>(request->user_ctx);
  return ws->handle_request(request);
}

}  // namespace

namespace {

esp_err_t route_dispatcher(httpd_req_t* request) {
  if (request == nullptr || request->user_ctx == nullptr) {
    return ESP_FAIL;
  }
  auto* route = static_cast<AsyncWebServer::Route*>(request->user_ctx);
  return route->owner->dispatch(route, request);
}

}  // namespace

AsyncWebServerResponse::AsyncWebServerResponse(const int code,
                                               const String& content_type,
                                               const String& body)
    : code_(code), content_type_(content_type), body_(body), chunked_(false) {}

AsyncWebServerResponse::AsyncWebServerResponse(const int code,
                                               const String& content_type,
                                               ChunkCallback callback)
    : code_(code),
      content_type_(content_type),
      chunk_callback_(std::move(callback)),
      chunked_(true) {}

void AsyncWebServerResponse::addHeader(const String& key, const String& value) {
  headers_.emplace_back(key, value);
}

void AsyncWebServerResponse::addHeader(const char* key, const String& value) {
  addHeader(String(key), value);
}

void AsyncWebServerResponse::addHeader(const char* key, const char* value) {
  addHeader(String(key), String(value));
}

AsyncWebServerRequest::AsyncWebServerRequest(AsyncWebServer* owner,
                                             httpd_req_t* request,
                                             const bool can_read_body,
                                             const bool body_preconsumed)
    : owner_(owner),
      request_(request),
      body_preconsumed_(body_preconsumed),
      can_read_body_(can_read_body) {}

bool AsyncWebServerRequest::ensure_query_params_loaded() {
  if (query_loaded_) {
    return true;
  }
  query_loaded_ = true;

  const size_t query_len = httpd_req_get_url_query_len(request_);
  if (query_len == 0U) {
    return true;
  }

  std::string buffer(query_len + 1U, '\0');
  if (httpd_req_get_url_query_str(request_, buffer.data(),
                                  buffer.size()) != ESP_OK) {
    return false;
  }

  parse_urlencoded(String(buffer.c_str()), false, &query_params_);
  return true;
}

bool AsyncWebServerRequest::ensure_body_loaded() {
  if (body_loaded_) {
    return true;
  }
  body_loaded_ = true;
  if (!can_read_body_ || body_preconsumed_) {
    return false;
  }
  cached_body_ = read_request_body(request_);
  return true;
}

bool AsyncWebServerRequest::ensure_post_params_loaded() {
  if (post_loaded_) {
    return true;
  }
  post_loaded_ = true;
  if (!ensure_body_loaded()) {
    return false;
  }
  parse_urlencoded(cached_body_, true, &post_params_);
  return true;
}

bool AsyncWebServerRequest::hasParam(const char* name, const bool post) {
  return getParam(name, post) != nullptr;
}

const AsyncWebParameter* AsyncWebServerRequest::getParam(const char* name,
                                                         const bool post) {
  const String key(name);
  std::vector<AsyncWebParameter>* params = nullptr;
  if (post) {
    if (!ensure_post_params_loaded()) {
      return nullptr;
    }
    params = &post_params_;
  } else {
    if (!ensure_query_params_loaded()) {
      return nullptr;
    }
    params = &query_params_;
  }

  for (auto& param : *params) {
    if (param.name() == key) {
      return &param;
    }
  }
  return nullptr;
}

bool AsyncWebServerRequest::hasHeader(const char* name) const {
  return httpd_req_get_hdr_value_len(request_, name) > 0;
}

String AsyncWebServerRequest::header(const char* name) const {
  const size_t len = httpd_req_get_hdr_value_len(request_, name);
  if (len == 0U) {
    return String();
  }
  std::string buffer(len + 1U, '\0');
  if (httpd_req_get_hdr_value_str(request_, name, buffer.data(),
                                  buffer.size()) != ESP_OK) {
    return String();
  }
  return String(buffer.c_str());
}

String AsyncWebServerRequest::host() const { return header("Host"); }

String AsyncWebServerRequest::url() const {
  return request_ != nullptr ? String(request_->uri) : String();
}

httpd_method_t AsyncWebServerRequest::method() const {
  return request_ != nullptr ? static_cast<httpd_method_t>(request_->method) : HTTP_GET;
}

AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(
    const int code, const char* content_type, const String& body) {
  return new AsyncWebServerResponse(code, String(content_type), body);
}

AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(
    const int code, const char* content_type, const char* body) {
  return new AsyncWebServerResponse(code, String(content_type), String(body));
}

AsyncWebServerResponse* AsyncWebServerRequest::beginChunkedResponse(
    const char* content_type, AsyncWebServerResponse::ChunkCallback callback) {
  return new AsyncWebServerResponse(200, String(content_type), std::move(callback));
}

void AsyncWebServerRequest::send(const int code) {
  send(code, "text/plain", "");
}

void AsyncWebServerRequest::send(const int code, const char* content_type,
                                 const char* body) {
  AsyncWebServerResponse response(code, String(content_type), String(body));
  send_response(response);
}

void AsyncWebServerRequest::send(const int code, const char* content_type,
                                 const String& body) {
  AsyncWebServerResponse response(code, String(content_type), body);
  send_response(response);
}

void AsyncWebServerRequest::send(AsyncWebServerResponse* response) {
  if (response == nullptr) {
    return;
  }
  send_response(*response);
  delete response;
}

void AsyncWebServerRequest::redirect(const String& location) {
  AsyncWebServerResponse response(302, "text/plain", "");
  response.addHeader("Location", location);
  response.addHeader("Cache-Control", "no-store");
  send_response(response);
}

bool AsyncWebServerRequest::send_response(AsyncWebServerResponse& response) {
  if (responded_ || request_ == nullptr) {
    return false;
  }
  responded_ = true;

  response_status_storage_ = http_status_text(response.code_);
  response_type_storage_.assign(response.content_type_.c_str(),
                                response.content_type_.length());
  response_header_storage_.clear();
  response_header_storage_.reserve((response.headers_.size() + 1U) * 2U);
  for (const auto& header : response.headers_) {
    response_header_storage_.emplace_back(header.first.c_str(),
                                          header.first.length());
    response_header_storage_.emplace_back(header.second.c_str(),
                                          header.second.length());
  }
  response_header_storage_.emplace_back("Connection");
  response_header_storage_.emplace_back("close");

  httpd_resp_set_status(request_, response_status_storage_.c_str());
  httpd_resp_set_type(request_, response_type_storage_.c_str());
  for (size_t i = 0; i + 1U < response_header_storage_.size(); i += 2U) {
    httpd_resp_set_hdr(request_,
                       response_header_storage_[i].c_str(),
                       response_header_storage_[i + 1U].c_str());
  }

  if (!response.chunked_) {
    const size_t body_len = response.body_.length();
    const char* body = (body_len > 0U) ? response.body_.c_str() : nullptr;
    const esp_err_t err = httpd_resp_send(request_, body, body_len);
    const bool ok = err == ESP_OK;
    if (!ok && owner_ != nullptr) {
      owner_->record_http_error(err);
    }
    if (ok) {
      httpd_sess_trigger_close(request_->handle, httpd_req_to_sockfd(request_));
    }
    return ok;
  }

  std::vector<uint8_t> buffer(2048U);
  size_t index = 0U;
  while (true) {
    const size_t chunk_len =
        response.chunk_callback_(buffer.data(), buffer.size(), index);
    if (chunk_len == 0U) {
      break;
    }
    if (httpd_resp_send_chunk(
            request_, reinterpret_cast<const char*>(buffer.data()),
            chunk_len) != ESP_OK) {
      if (owner_ != nullptr) {
        owner_->record_http_error(ESP_FAIL);
      }
      return false;
    }
    index += chunk_len;
  }
  const esp_err_t err = httpd_resp_send_chunk(request_, nullptr, 0U);
  const bool ok = err == ESP_OK;
  if (!ok && owner_ != nullptr) {
    owner_->record_http_error(err);
  }
  if (ok) {
    httpd_sess_trigger_close(request_->handle, httpd_req_to_sockfd(request_));
  }
  return ok;
}

void AsyncWebServerRequest::mark_body_preconsumed() {
  body_preconsumed_ = true;
  can_read_body_ = false;
}

AsyncWebSocketClient::AsyncWebSocketClient(AsyncWebSocket* owner,
                                           const int socket_fd)
    : owner_(owner), socket_fd_(socket_fd) {}

uint32_t AsyncWebSocketClient::id() const {
  return static_cast<uint32_t>(socket_fd_);
}

int AsyncWebSocketClient::status() const {
  if (owner_ != nullptr) {
    std::lock_guard<std::mutex> lock(owner_->mutex_);
    return connected_ ? WS_CONNECTED : WS_DISCONNECTED;
  }
  return connected_ ? WS_CONNECTED : WS_DISCONNECTED;
}

void AsyncWebSocketClient::text(const String& message) {
  text(message.c_str(), message.length());
}

void AsyncWebSocketClient::text(const char* message) {
  text(message, message != nullptr ? std::strlen(message) : 0U);
}

void AsyncWebSocketClient::text(const char* message, const size_t len) {
  sendFrame(reinterpret_cast<const uint8_t*>(message), len, HTTPD_WS_TYPE_TEXT);
}

void AsyncWebSocketClient::binary(const uint8_t* data, const size_t len) {
  sendFrame(data, len, HTTPD_WS_TYPE_BINARY);
}

void AsyncWebSocketClient::sendFrame(const uint8_t* data,
                                     const size_t len,
                                     const httpd_ws_type_t type) {
  if (data == nullptr && len > 0U) {
    return;
  }
  httpd_handle_t handle = nullptr;
  int socket_fd = -1;
  {
    if (owner_ == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(owner_->mutex_);
    if (owner_->handle_ == nullptr || !connected_) {
      return;
    }
    handle = owner_->handle_;
    socket_fd = socket_fd_;
  }

  auto* work = new (std::nothrow) WsSendWork();
  if (work == nullptr) {
    owner_->note_send_result(socket_fd, ESP_ERR_NO_MEM);
    return;
  }

  work->owner = owner_;
  work->handle = handle;
  work->socket_fd = socket_fd;
  work->len = len;
  work->type = type;
  if (work->len > 0U) {
    work->payload = alloc_ws_payload(work->len);
    if (work->payload == nullptr) {
      delete work;
      owner_->note_send_result(socket_fd, ESP_ERR_NO_MEM);
      return;
    }
    std::memcpy(work->payload, data, work->len);
  }

  const esp_err_t err = httpd_queue_work(handle, ws_send_work, work);
  if (err != ESP_OK) {
    if (work->payload != nullptr) {
      heap_caps_free(work->payload);
    }
    delete work;
    owner_->note_send_result(socket_fd, err);
  }
}

void AsyncWebSocketClient::close() {
  httpd_handle_t handle = nullptr;
  int socket_fd = -1;
  {
    if (owner_ == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(owner_->mutex_);
    if (owner_->handle_ == nullptr || !connected_) {
      return;
    }
    connected_ = false;
    handle = owner_->handle_;
    socket_fd = socket_fd_;
  }

  auto* work = new (std::nothrow) WsCloseWork();
  if (work == nullptr) {
    httpd_sess_trigger_close(handle, socket_fd);
    return;
  }
  work->handle = handle;
  work->socket_fd = socket_fd;
  if (httpd_queue_work(handle, ws_close_work, work) != ESP_OK) {
    delete work;
    httpd_sess_trigger_close(handle, socket_fd);
  }
}

AsyncWebSocket::AsyncWebSocket(const char* uri) : uri_(uri) {}

void AsyncWebSocket::onEvent(AwsEventHandler handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  handler_ = std::move(handler);
}

size_t AsyncWebSocket::count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return clients_.size();
}

AsyncWebSocketClient* AsyncWebSocket::client(const uint32_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = clients_.find(static_cast<int>(id));
  return (it == clients_.end()) ? nullptr : it->second.get();
}

void AsyncWebSocket::cleanupClients() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = clients_.begin(); it != clients_.end();) {
    if (!it->second->connected_) {
      it = clients_.erase(it);
    } else {
      ++it;
    }
  }
}

const char* AsyncWebSocket::url() const { return uri_.c_str(); }

void AsyncWebSocket::attach_server(httpd_handle_t handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  handle_ = handle;
}

esp_err_t AsyncWebSocket::handle_request(httpd_req_t* request) {
  if (request->method == HTTP_GET) {
    const int fd = httpd_req_to_sockfd(request);
    AsyncWebSocketClient* raw = nullptr;
    AwsEventHandler handler;
    bool emit_connect = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = clients_.find(fd);
      if (it == clients_.end()) {
        auto client = std::make_unique<AsyncWebSocketClient>(this, fd);
        raw = client.get();
        clients_[fd] = std::move(client);
        emit_connect = true;
        total_connects_++;
      } else {
        raw = it->second.get();
        if (!it->second->connected_) {
          emit_connect = true;
          total_connects_++;
        }
        it->second->connected_ = true;
      }
      handler = handler_;
    }
    if (emit_connect && handler) {
      handler(this, raw, WS_EVT_CONNECT, nullptr, nullptr, 0U);
    }
    return ESP_OK;
  }

  const int fd = httpd_req_to_sockfd(request);
  AsyncWebSocketClient* client = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto client_it = clients_.find(fd);
    if (client_it == clients_.end()) {
      auto inserted = std::make_unique<AsyncWebSocketClient>(this, fd);
      client = inserted.get();
      clients_.emplace(fd, std::move(inserted));
      total_connects_++;
    } else {
      client = client_it->second.get();
      client_it->second->connected_ = true;
    }
  }

  httpd_ws_frame_t frame = {};
  esp_err_t err = httpd_ws_recv_frame(request, &frame, 0U);
  if (err != ESP_OK) {
    note_error(err);
    return err;
  }

  std::vector<uint8_t> payload(frame.len);
  frame.payload = payload.empty() ? nullptr : payload.data();
  if (frame.len > 0U) {
    err = httpd_ws_recv_frame(request, &frame, frame.len);
    if (err != ESP_OK) {
      note_error(err);
      return err;
    }
  }

  std::vector<uint8_t> complete_payload;
  AwsEventHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& pending = pending_frames_[fd];
    pending.insert(pending.end(), payload.begin(), payload.end());
    max_frame_size_ = std::max<uint32_t>(
        max_frame_size_, static_cast<uint32_t>(pending.size()));
    if (!frame.final) {
      return ESP_OK;
    }
    complete_payload.swap(pending);
    pending.clear();
    total_rx_frames_++;
    handler = handler_;
  }

  AwsFrameInfo info = {};
  info.final = true;
  info.index = 0U;
  info.len = complete_payload.size();
  info.opcode = static_cast<uint8_t>(frame.type);

  if (handler) {
    handler(this, client, WS_EVT_DATA, &info,
            complete_payload.empty() ? nullptr : complete_payload.data(),
            complete_payload.size());
  }
  return ESP_OK;
}

void AsyncWebSocket::handle_disconnect(const int socket_fd) {
  AsyncWebSocketClient* raw = nullptr;
  AwsEventHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(socket_fd);
    if (it == clients_.end()) {
      return;
    }
    if (it->second->connected_) {
      total_disconnects_++;
    }
    it->second->connected_ = false;
    raw = it->second.get();
    handler = handler_;
    pending_frames_.erase(socket_fd);
  }
  if (handler) {
    handler(this, raw, WS_EVT_DISCONNECT, nullptr, nullptr, 0U);
  }
}

void AsyncWebSocket::remove_client(const int socket_fd) {
  handle_disconnect(socket_fd);
  std::lock_guard<std::mutex> lock(mutex_);
  clients_.erase(socket_fd);
}

void AsyncWebSocket::note_send_result(const int socket_fd,
                                      const esp_err_t result) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (result == ESP_OK) {
    total_tx_frames_++;
    return;
  }
  dropped_tx_frames_++;
  total_errors_++;
  last_error_ = static_cast<int>(result);
  auto it = clients_.find(socket_fd);
  if (it != clients_.end()) {
    it->second->connected_ = false;
  }
}

void AsyncWebSocket::note_error(const esp_err_t result) {
  if (result == ESP_OK) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  total_errors_++;
  last_error_ = static_cast<int>(result);
}

void AsyncWebSocket::accumulate_diagnostics(AsyncWebDiagnostics* out) const {
  if (out == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  out->active_ws_clients += static_cast<uint32_t>(clients_.size());
  out->total_ws_connects += total_connects_;
  out->total_ws_disconnects += total_disconnects_;
  out->total_ws_rx_frames += total_rx_frames_;
  out->total_ws_tx_frames += total_tx_frames_;
  out->dropped_ws_tx_frames += dropped_tx_frames_;
  out->total_ws_errors += total_errors_;
  out->max_ws_frame_size = std::max(out->max_ws_frame_size, max_frame_size_);
  if (last_error_ != 0) {
    out->last_ws_error = last_error_;
  }
}

namespace AsyncURIMatcher {

Prefix prefix(const char* uri_prefix) { return Prefix{String(uri_prefix)}; }

}  // namespace AsyncURIMatcher

AsyncWebServer::AsyncWebServer(const uint16_t port) : port_(port) {}

void AsyncWebServer::on(const char* uri,
                        const http_method method,
                        ArRequestHandlerFunction on_request) {
  auto route = std::make_unique<Route>();
  route->owner = this;
  route->uri = uri;
  route->method = method;
  route->on_request = std::move(on_request);
  routes_.push_back(std::move(route));
}

void AsyncWebServer::on(const char* uri,
                        const http_method method,
                        ArRequestHandlerFunction on_request,
                        void*,
                        ArBodyHandlerFunction on_body) {
  auto route = std::make_unique<Route>();
  route->owner = this;
  route->uri = uri;
  route->method = method;
  route->on_request = std::move(on_request);
  route->on_body = std::move(on_body);
  routes_.push_back(std::move(route));
}

void AsyncWebServer::on(const AsyncURIMatcher::Prefix& prefix_value,
                        const http_method method,
                        ArRequestHandlerFunction on_request) {
  auto route = std::make_unique<Route>();
  route->owner = this;
  route->prefix = true;
  route->uri = prefix_value.value;
  route->method = method;
  route->on_request = std::move(on_request);
  routes_.push_back(std::move(route));
}

void AsyncWebServer::onNotFound(ArRequestHandlerFunction on_request) {
  not_found_handler_ = std::move(on_request);
}

void AsyncWebServer::addHandler(AsyncWebSocket* handler) {
  if (handler != nullptr) {
    ws_handlers_.push_back(handler);
  }
}

void AsyncWebServer::begin() {
  if (handle_ != nullptr) {
    return;
  }

  const int max_open_sockets =
      std::max(1, std::min(kPreferredOpenSockets,
                           CONFIG_LWIP_MAX_SOCKETS - kHttpdInternalSockets));
  max_open_sockets_ = max_open_sockets;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port_;
  config.stack_size = kPreferredHttpdStackSize;
  config.max_open_sockets = max_open_sockets;
  config.lru_purge_enable = kPreferredLruPurgeEnable;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.close_fn = close_dispatch;
  config.max_uri_handlers =
      static_cast<uint16_t>(routes_.size() + ws_handlers_.size() + 4U);

  const esp_err_t start_err = httpd_start(&handle_, &config);
  if (start_err != ESP_OK) {
    record_http_error(start_err);
    ESP_LOGE(kTag,
             "httpd_start failed: %s (LWIP_MAX_SOCKETS=%d, max_open_sockets=%d)",
             esp_err_to_name(start_err), CONFIG_LWIP_MAX_SOCKETS,
             max_open_sockets);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    g_active_server = this;
  }

  for (auto& route : routes_) {
    String uri = route->uri;
    if (route->prefix) {
      if (uri.endsWith("/")) {
        uri += "*";
      } else {
        uri += "/?*";
      }
    }
    httpd_uri_t handler = {};
    handler.uri = strdup(uri.c_str());
    handler.method = route->method;
    handler.handler = route_dispatcher;
    handler.user_ctx = route.get();
    const esp_err_t err = httpd_register_uri_handler(handle_, &handler);
    if (err != ESP_OK) {
      record_http_error(err);
      ESP_LOGE(kTag, "route register failed uri=%s err=%s", uri.c_str(),
               esp_err_to_name(err));
    }
  }

  for (auto* ws : ws_handlers_) {
    ws->attach_server(handle_);
    httpd_uri_t ws_handler = {};
    ws_handler.uri = ws->url();
    ws_handler.method = HTTP_GET;
    ws_handler.handler = ws_dispatch;
    ws_handler.user_ctx = ws;
    ws_handler.is_websocket = true;
    const esp_err_t err = httpd_register_uri_handler(handle_, &ws_handler);
    if (err != ESP_OK) {
      record_http_error(err);
      ESP_LOGE(kTag, "websocket register failed uri=%s err=%s", ws->url(),
               esp_err_to_name(err));
    }
  }

  if (not_found_handler_) {
    auto route = std::make_unique<Route>();
    route->owner = this;
    route->uri = "*";
    route->method = HTTP_GET;
    route->on_request = not_found_handler_;
    httpd_uri_t handler = {};
    handler.uri = strdup(route->uri.c_str());
    handler.method = HTTP_GET;
    handler.handler = route_dispatcher;
    handler.user_ctx = route.get();
    routes_.push_back(std::move(route));
    const esp_err_t err = httpd_register_uri_handler(handle_, &handler);
    if (err != ESP_OK) {
      record_http_error(err);
      ESP_LOGE(kTag, "not-found handler register failed err=%s",
               esp_err_to_name(err));
    }
  }
}

void AsyncWebServer::handle_close(const int socket_fd) {
  total_socket_closes_++;
  for (auto* ws : ws_handlers_) {
    ws->remove_client(socket_fd);
  }
}

esp_err_t AsyncWebServer::dispatch(Route* route, httpd_req_t* request) {
  if (route == nullptr || request == nullptr) {
    return ESP_FAIL;
  }

  AsyncWebServerRequest wrapped(this, request,
                                route->method != HTTP_GET &&
                                    route->on_body == nullptr,
                                false);

  if (route->on_body) {
    if (route->on_request) {
      route->on_request(&wrapped);
      if (wrapped.responded_) {
        return ESP_OK;
      }
    }

    const size_t total = static_cast<size_t>(request->content_len);
    max_body_size_ = std::max<uint32_t>(max_body_size_,
                                        static_cast<uint32_t>(total));
    std::array<uint8_t, 1024> buffer = {};
    size_t index = 0U;
    while (index < total) {
      const size_t to_read = std::min(buffer.size(), total - index);
      const int read =
          httpd_req_recv(request, reinterpret_cast<char*>(buffer.data()), to_read);
      if (read <= 0) {
        record_http_error(ESP_FAIL);
        return ESP_FAIL;
      }
      route->on_body(&wrapped, buffer.data(), static_cast<size_t>(read), index,
                     total);
      index += static_cast<size_t>(read);
      if (wrapped.responded_ && index < total) {
        if (!drain_request_body(request, total - index)) {
          record_http_error(ESP_FAIL);
          return ESP_FAIL;
        }
        index = total;
      }
    }
    wrapped.mark_body_preconsumed();
    return ESP_OK;
  }

  if (route->on_request) {
    route->on_request(&wrapped);
  }
  return ESP_OK;
}

void AsyncWebServer::record_http_error(const esp_err_t err) {
  if (err == ESP_OK) {
    return;
  }
  total_http_errors_++;
  last_http_error_ = static_cast<int>(err);
}

AsyncWebDiagnostics AsyncWebServer::diagnostics() const {
  AsyncWebDiagnostics out = {};
  out.route_count = static_cast<uint32_t>(routes_.size());
  out.ws_handler_count = static_cast<uint32_t>(ws_handlers_.size());
  out.total_http_errors = total_http_errors_;
  out.total_socket_closes = total_socket_closes_;
  out.last_http_error = last_http_error_;
  out.max_body_size = max_body_size_;
  out.max_open_sockets = max_open_sockets_;
  out.lwip_max_sockets = CONFIG_LWIP_MAX_SOCKETS;
  out.httpd_stack_size = kPreferredHttpdStackSize;
  out.lru_purge_enabled = kPreferredLruPurgeEnable;
  if (handle_ != nullptr && max_open_sockets_ > 0) {
    std::vector<int> client_fds(static_cast<size_t>(max_open_sockets_), -1);
    size_t fd_count = client_fds.size();
    if (httpd_get_client_list(handle_, &fd_count, client_fds.data()) == ESP_OK) {
      out.active_http_sockets = static_cast<uint32_t>(fd_count);
    }
  }
  for (const auto* ws : ws_handlers_) {
    if (ws != nullptr) {
      ws->accumulate_diagnostics(&out);
    }
  }
  return out;
}
