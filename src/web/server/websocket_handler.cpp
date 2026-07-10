#include "websocket_handler.h"
#include "web_server.h"
#include "WString.h"

namespace {

WebSocketTextCallback g_text_cb = nullptr;
void *g_text_ctx = nullptr;

}  // namespace

void websocket_handler_attach(void* ws) {
  (void)ws;
}

void websocket_handler_set_text_callback(WebSocketTextCallback cb, void *ctx) {
  g_text_cb = cb;
  g_text_ctx = ctx;
}

void websocket_handler_broadcast_json(const String &json) {
  (void)json;
}

uint32_t websocket_handler_client_count() {
  return web_server_total_ws_client_count();
}

void websocket_handler_service(unsigned long now_ms) {
  (void)now_ms;
}
