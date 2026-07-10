#pragma once

class AsyncWebServerRequest;

bool web_async_send_littlefs_stream_psram(AsyncWebServerRequest* request,
                                          const char* path,
                                          const char* content_type,
                                          bool no_cache_headers = true,
                                          const char* cache_control_override = nullptr,
                                          const char* content_encoding = nullptr,
                                          const char* etag = nullptr,
                                          const char* content_disposition = nullptr);
