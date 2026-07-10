#pragma once

#include <stdint.h>

class String;

typedef void (*WebSocketTextCallback)(uint32_t client_id, const String &text, void *ctx);

void websocket_handler_attach(void* ws);
void websocket_handler_set_text_callback(WebSocketTextCallback cb, void *ctx);
void websocket_handler_broadcast_json(const String &json);
uint32_t websocket_handler_client_count();
void websocket_handler_service(unsigned long now_ms);
