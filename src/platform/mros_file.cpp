#include "src/platform/mros_file.h"

#include <cstdio>

#include "src/platform/mros_fs.h"

namespace mros::platform {

bool mros_file_read_all(const char* path, std::string* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();

  FILE* file = mros_fs_open(path, "rb");
  if (file == nullptr) {
    return false;
  }

  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }
  const long file_size = std::ftell(file);
  if (file_size < 0) {
    std::fclose(file);
    return false;
  }
  if (std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }

  out->assign(static_cast<size_t>(file_size), '\0');
  if (file_size > 0) {
    const size_t read = std::fread(out->data(), 1U, static_cast<size_t>(file_size), file);
    if (read != static_cast<size_t>(file_size)) {
      std::fclose(file);
      out->clear();
      return false;
    }
  }
  std::fclose(file);
  return true;
}

bool mros_file_write_all(const char* path, const std::string_view data) {
  FILE* file = mros_fs_open(path, "wb");
  if (file == nullptr) {
    return false;
  }
  const size_t written = std::fwrite(data.data(), 1U, data.size(), file);
  std::fclose(file);
  return written == data.size();
}

bool mros_file_append_all(const char* path, const std::string_view data) {
  FILE* file = mros_fs_open(path, "ab");
  if (file == nullptr) {
    return false;
  }
  const size_t written = std::fwrite(data.data(), 1U, data.size(), file);
  std::fclose(file);
  return written == data.size();
}

bool mros_file_write_all_atomic(const char* final_path, const char* temp_path,
                                const std::string_view data) {
  if (final_path == nullptr || temp_path == nullptr) {
    return false;
  }
  if (!mros_file_write_all(temp_path, data)) {
    return false;
  }
  if (mros_fs_rename(temp_path, final_path)) {
    return true;
  }
  (void)mros_fs_remove(temp_path);
  return false;
}

bool mros_file_read_lines(const char* path, std::vector<std::string>* lines,
                          const size_t max_lines) {
  if (lines == nullptr) {
    return false;
  }
  lines->clear();

  std::string raw;
  if (!mros_file_read_all(path, &raw)) {
    return false;
  }

  size_t start = 0U;
  while (start <= raw.size()) {
    size_t end = raw.find('\n', start);
    if (end == std::string::npos) {
      end = raw.size();
    }
    std::string line = raw.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines->push_back(std::move(line));
    if (max_lines > 0U && lines->size() >= max_lines) {
      break;
    }
    if (end == raw.size()) {
      break;
    }
    start = end + 1U;
  }

  return true;
}

}  // namespace mros::platform
