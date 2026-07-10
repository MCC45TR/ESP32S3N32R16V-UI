#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_partition.h>

namespace mros::debug {

const esp_partition_t* coredump_partition_handle();
bool coredump_json(char* buffer, size_t capacity);
esp_err_t coredump_clear();
bool coredump_present();
size_t coredump_partition_size();

bool heap_trace_json(char* buffer, size_t capacity);
esp_err_t heap_trace_start();
esp_err_t heap_trace_stop();
void heap_trace_clear();

}  // namespace mros::debug
