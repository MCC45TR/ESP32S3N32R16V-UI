#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mros::platform {

bool mros_file_read_all(const char* path, std::string* out);
bool mros_file_write_all(const char* path, std::string_view data);
bool mros_file_append_all(const char* path, std::string_view data);
bool mros_file_write_all_atomic(const char* final_path, const char* temp_path,
                                std::string_view data);
bool mros_file_read_lines(const char* path, std::vector<std::string>* lines,
                          size_t max_lines = 0U);

}  // namespace mros::platform
