#pragma once

#include <stdint.h>

#include <esp_partition.h>

namespace mros::recovery {

void web_set_phase(const char* state,
                   const char* phase,
                   const char* message,
                   uint32_t progress,
                   int32_t eta_sec,
                   bool busy);
void web_set_confirmation(const char* confirmation);
void web_set_filesystem_ready(bool ready);
void web_start(const esp_partition_t* app0_partition);
bool web_client_seen();

}  // namespace mros::recovery
