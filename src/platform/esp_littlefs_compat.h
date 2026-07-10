#pragma once

extern "C" {

#if defined(__has_include)
#if __has_include(<esp_littlefs.h>)
#include <esp_littlefs.h>
#else
#error "esp_littlefs.h is required for this ESP-IDF 6 LittleFS build"
#endif
#else
#include <esp_littlefs.h>
#endif

}
