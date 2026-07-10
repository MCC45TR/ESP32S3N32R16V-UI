#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "WString.h"

extern "C" {
#include <esp_http_server.h>
}

class AsyncWebParameter {
 public:
  AsyncWebParameter(const String& name, const String& value, bool post)
      : name_(name), value_(value), post_(post) {}

  const String& name() const { return name_; }
  const String& value() const { return value_; }
  bool isPost() const { return post_; }

 private:
  String name_;
  String value_;
  bool post_ = false;
};

class AsyncWebServerResponse {
 public:
  using ChunkCallback = std::function<size_t(uint8_t*, size_t, size_t)>;

  AsyncWebServerResponse(int code, const String& content_type, const String& body);
  AsyncWebServerResponse(int code, const String& content_type, ChunkCallback callback);

  void addHeader(const String& key, const String& value);
  void addHeader(const char* key, const String& value);
  void addHeader(const char* key, const char* value);

 private:
  friend class AsyncWebServerRequest;

  int code_ = 200;
  String content_type_;
  String body_;
  ChunkCallback chunk_callback_;
  std::vector<std::pair<String, String>> headers_;
  bool chunked_ = false;
};

class AsyncWebServer;

struct AsyncWebDiagnostics {
  uint32_t route_count = 0;
  uint32_t ws_handler_count = 0;
  uint32_t active_ws_clients = 0;
  uint32_t total_ws_connects = 0;
  uint32_t total_ws_disconnects = 0;
  uint32_t total_ws_rx_frames = 0;
  uint32_t total_ws_tx_frames = 0;
  uint32_t dropped_ws_tx_frames = 0;
  uint32_t total_http_errors = 0;
  uint32_t total_ws_errors = 0;
  uint32_t active_http_sockets = 0;
  uint32_t total_socket_closes = 0;
  uint32_t max_body_size = 0;
  uint32_t max_ws_frame_size = 0;
  int last_http_error = 0;
  int last_ws_error = 0;
  int max_open_sockets = 0;
  int lwip_max_sockets = 0;
  int httpd_stack_size = 0;
  bool lru_purge_enabled = false;
};

class AsyncWebServerRequest {
 public:
  bool hasParam(const char* name, bool post = false);
  const AsyncWebParameter* getParam(const char* name, bool post = false);
  bool hasHeader(const char* name) const;
  String header(const char* name) const;
  String host() const;
  String url() const;
  httpd_method_t method() const;

  AsyncWebServerResponse* beginResponse(int code, const char* content_type,
                                        const String& body);
  AsyncWebServerResponse* beginResponse(int code, const char* content_type,
                                        const char* body);
  AsyncWebServerResponse* beginChunkedResponse(
      const char* content_type, AsyncWebServerResponse::ChunkCallback callback);

  void send(int code);
  void send(int code, const char* content_type, const char* body);
  void send(int code, const char* content_type, const String& body);
  void send(AsyncWebServerResponse* response);
  void redirect(const String& location);

 private:
  friend class AsyncWebServer;

  AsyncWebServerRequest(AsyncWebServer* owner,
                        httpd_req_t* request,
                        bool can_read_body,
                        bool body_preconsumed);

  bool ensure_query_params_loaded();
  bool ensure_post_params_loaded();
  bool ensure_body_loaded();
  void mark_body_preconsumed();
  bool send_response(AsyncWebServerResponse& response);

  AsyncWebServer* owner_ = nullptr;
  httpd_req_t* request_ = nullptr;
  bool query_loaded_ = false;
  bool post_loaded_ = false;
  bool body_loaded_ = false;
  bool body_preconsumed_ = false;
  bool can_read_body_ = false;
  bool responded_ = false;
  String cached_body_;
  std::string response_status_storage_;
  std::string response_type_storage_;
  std::vector<std::string> response_header_storage_;
  std::vector<AsyncWebParameter> query_params_;
  std::vector<AsyncWebParameter> post_params_;
};

enum AwsEventType {
  WS_EVT_CONNECT = 0,
  WS_EVT_DISCONNECT = 1,
  WS_EVT_DATA = 2,
};

enum AwsFrameType {
  WS_TEXT = HTTPD_WS_TYPE_TEXT,
  WS_BINARY = HTTPD_WS_TYPE_BINARY,
};

enum AwsClientStatus {
  WS_DISCONNECTED = 0,
  WS_CONNECTED = 1,
};

struct AwsFrameInfo {
  bool final = true;
  size_t index = 0U;
  size_t len = 0U;
  uint8_t opcode = WS_TEXT;
};

class AsyncWebSocket;

class AsyncWebSocketClient {
 public:
  uint32_t id() const;
  int status() const;
  void text(const String& message);
  void text(const char* message);
  void text(const char* message, size_t len);
  void binary(const uint8_t* data, size_t len);
  void close();
  AsyncWebSocketClient(AsyncWebSocket* owner, int socket_fd);

 private:
  friend class AsyncWebSocket;

  void sendFrame(const uint8_t* data, size_t len, httpd_ws_type_t type);

  AsyncWebSocket* owner_ = nullptr;
  int socket_fd_ = -1;
  bool connected_ = true;
};

using AwsEventHandler = std::function<void(AsyncWebSocket*,
                                           AsyncWebSocketClient*,
                                           AwsEventType,
                                           void*,
                                           uint8_t*,
                                           size_t)>;

class AsyncWebSocket {
 public:
  explicit AsyncWebSocket(const char* uri);

  void onEvent(AwsEventHandler handler);
  size_t count() const;
  AsyncWebSocketClient* client(uint32_t id);
  void cleanupClients();
  const char* url() const;
  esp_err_t handle_request(httpd_req_t* request);
  void accumulate_diagnostics(AsyncWebDiagnostics* out) const;
  void note_send_result(int socket_fd, esp_err_t result);
  void note_error(esp_err_t result);

 private:
  friend class AsyncWebServer;
  friend class AsyncWebSocketClient;

  void attach_server(httpd_handle_t handle);
  void handle_disconnect(int socket_fd);
  void remove_client(int socket_fd);

  String uri_;
  httpd_handle_t handle_ = nullptr;
  AwsEventHandler handler_;
  mutable std::mutex mutex_;
  std::map<int, std::unique_ptr<AsyncWebSocketClient>> clients_;
  std::map<int, std::vector<uint8_t>> pending_frames_;
  uint32_t total_connects_ = 0;
  uint32_t total_disconnects_ = 0;
  uint32_t total_rx_frames_ = 0;
  uint32_t total_tx_frames_ = 0;
  uint32_t dropped_tx_frames_ = 0;
  uint32_t total_errors_ = 0;
  uint32_t max_frame_size_ = 0;
  int last_error_ = 0;
};

namespace AsyncURIMatcher {

struct Prefix {
  String value;
};

Prefix prefix(const char* uri_prefix);

}  // namespace AsyncURIMatcher

using ArRequestHandlerFunction = std::function<void(AsyncWebServerRequest*)>;
using ArBodyHandlerFunction =
    std::function<void(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t)>;

class AsyncWebServer {
 public:
  struct Route {
    AsyncWebServer* owner = nullptr;
    String uri;
    http_method method = HTTP_GET;
    ArRequestHandlerFunction on_request;
    ArBodyHandlerFunction on_body;
    bool prefix = false;
  };

  explicit AsyncWebServer(uint16_t port);

  void on(const char* uri,
          http_method method,
          ArRequestHandlerFunction on_request);
  void on(const char* uri,
          http_method method,
          ArRequestHandlerFunction on_request,
          void*,
          ArBodyHandlerFunction on_body);
  void on(const AsyncURIMatcher::Prefix& prefix,
          http_method method,
          ArRequestHandlerFunction on_request);
  void onNotFound(ArRequestHandlerFunction on_request);
  void addHandler(AsyncWebSocket* handler);
  void begin();
  void handle_close(int socket_fd);
  esp_err_t dispatch(Route* route, httpd_req_t* request);
  AsyncWebDiagnostics diagnostics() const;
  void record_http_error(esp_err_t err);

 private:
  friend class AsyncWebServerRequest;

  uint16_t port_ = 80U;
  httpd_handle_t handle_ = nullptr;
  std::vector<std::unique_ptr<Route>> routes_;
  std::vector<AsyncWebSocket*> ws_handlers_;
  ArRequestHandlerFunction not_found_handler_;
  uint32_t total_http_errors_ = 0;
  uint32_t total_socket_closes_ = 0;
  uint32_t max_body_size_ = 0;
  int last_http_error_ = 0;
  int max_open_sockets_ = 0;
};
