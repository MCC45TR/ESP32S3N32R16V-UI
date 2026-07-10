#include "web_async_stream.h"

#include "ESPAsyncWebServer.h"
#include <esp_heap_caps.h>

#include <cstdio>
#include <memory>

#include "src/platform/mros_fs.h"
#include "src/platform/mros_system.h"

namespace {

static constexpr size_t kApiLargeStreamChunkBytes = 8192U;

struct PsramFileStreamContext {
  FILE* file = nullptr;
  uint8_t* stage = nullptr;
  size_t stage_len = 0U;

  ~PsramFileStreamContext() {
    if (file != nullptr) {
      std::fclose(file);
      file = nullptr;
    }
    if (stage != nullptr) {
      heap_caps_free(stage);
      stage = nullptr;
    }
  }
};

static void* alloc_api_stream_stage(const size_t bytes) {
  if (bytes == 0U) {
    return nullptr;
  }
  if (mros::platform::mros_system_psram_total() > 0U) {
    void* ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != nullptr) {
      return ptr;
    }
  }
  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

}  // namespace

bool web_async_send_littlefs_stream_psram(AsyncWebServerRequest* request,
                                          const char* path,
                                          const char* content_type,
                                          const bool no_cache_headers,
                                          const char* cache_control_override,
                                          const char* content_encoding,
                                          const char* etag,
                                          const char* content_disposition) {
  if (request == nullptr || path == nullptr || content_type == nullptr) {
    return false;
  }

  if (!mros::platform::mros_fs_exists(path)) {
    request->send(404, "application/json",
                  "{\"success\":false,\"error\":\"FILE_NOT_FOUND\",\"message\":\"File not found\"}");
    return true;
  }

  auto ctx = std::make_shared<PsramFileStreamContext>();
  ctx->file = mros::platform::mros_fs_open(path, "rb");
  if (ctx->file == nullptr) {
    return false;
  }

  ctx->stage_len = kApiLargeStreamChunkBytes;
  ctx->stage = static_cast<uint8_t*>(alloc_api_stream_stage(ctx->stage_len));
  if (ctx->stage == nullptr) {
    return false;
  }

  AsyncWebServerResponse* response = request->beginChunkedResponse(
      content_type, [ctx](uint8_t* buffer, const size_t max_len, size_t index) -> size_t {
        (void)index;
        if (ctx->file == nullptr || max_len == 0U || ctx->stage == nullptr) {
          return 0U;
        }

        const size_t read_len = (max_len < ctx->stage_len) ? max_len : ctx->stage_len;
        const size_t rd = std::fread(ctx->stage, 1U, read_len, ctx->file);
        if (rd == 0U) {
          std::fclose(ctx->file);
          ctx->file = nullptr;
          return 0U;
        }

        memcpy(buffer, ctx->stage, rd);
        return rd;
      });

  if (response == nullptr) {
    return false;
  }

  if (cache_control_override != nullptr && cache_control_override[0] != '\0') {
    response->addHeader("Cache-Control", cache_control_override);
  } else if (no_cache_headers) {
    response->addHeader("Cache-Control",
                        "no-store, no-cache, must-revalidate, max-age=0");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
  }
  if (content_encoding != nullptr && content_encoding[0] != '\0') {
    response->addHeader("Content-Encoding", content_encoding);
  }
  if (etag != nullptr && etag[0] != '\0') {
    response->addHeader("ETag", etag);
  }
  if (content_disposition != nullptr && content_disposition[0] != '\0') {
    response->addHeader("Content-Disposition", content_disposition);
  }

  request->send(response);
  return true;
}
