#include "src/shell/mros_shell_internal.h"
#include "src/shell/mshell_remote.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace mros::shell {
namespace {

bool starts_with_path(const std::string& value, const std::string& prefix) {
  if (prefix.empty()) {
    return false;
  }
  if (value == prefix) {
    return true;
  }
  return value.size() > prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0 &&
         value[prefix.size()] == '/';
}

bool is_protected_user_storage_path(const ShellState& state, const std::string& normalized) {
  const std::string user_root = shell_storage_user_root(state);
  const std::string auth_dir = user_root + "/auth";
  return normalized == auth_dir || starts_with_path(normalized, auth_dir) ||
         normalized == user_root + "/auth_credentials.dat" ||
         normalized == user_root + "/auth_credentials.tmp";
}

std::vector<std::string> split_path_segments(const std::string& path) {
  std::vector<std::string> segments;
  size_t cursor = 0U;
  while (cursor < path.size()) {
    while (cursor < path.size() && path[cursor] == '/') {
      ++cursor;
    }
    if (cursor >= path.size()) {
      break;
    }
    size_t next = cursor;
    while (next < path.size() && path[next] != '/') {
      ++next;
    }
    segments.emplace_back(path.substr(cursor, next - cursor));
    cursor = next;
  }
  return segments;
}

std::string normalize_absolute_path(const std::string& path) {
  std::vector<std::string> stack;
  const std::vector<std::string> segments = split_path_segments(path);
  for (const std::string& segment : segments) {
    if (segment.empty() || segment == ".") {
      continue;
    }
    if (segment == "..") {
      if (!stack.empty()) {
        stack.pop_back();
      }
      continue;
    }
    stack.push_back(segment);
  }

  if (stack.empty()) {
    return "/";
  }

  std::string normalized;
  for (const std::string& segment : stack) {
    normalized.push_back('/');
    normalized.append(segment);
  }
  return normalized;
}

std::string normalize_storage_aliases(const ShellState& state, const std::string& path) {
  const std::string normalized = normalize_absolute_path(path);
  const std::string mount_point = shell_storage_mount_path(state);
  const std::string user_root = shell_storage_user_root(state);

  if (normalized == "/fs" || normalized == "/littlefs") {
    return mount_point;
  }
  if (normalized == "/ESPUSER") {
    return user_root;
  }
  if (starts_with_path(normalized, "/fs")) {
    return mount_point + normalized.substr(std::strlen("/fs"));
  }
  if (starts_with_path(normalized, "/littlefs")) {
    return mount_point + normalized.substr(std::strlen("/littlefs"));
  }
  if (starts_with_path(normalized, "/ESPUSER")) {
    return user_root + normalized.substr(std::strlen("/ESPUSER"));
  }
  return normalized;
}

std::string make_child_path(const std::string& parent, const std::string& child_name) {
  if (parent == "/") {
    return "/" + child_name;
  }
  return parent + "/" + child_name;
}

void append_virtual_entry(
    const std::string& name,
    const std::string& display_name,
    const std::string& path,
    std::vector<ShellFsEntry>* entries) {
  if (entries == nullptr) {
    return;
  }
  ShellFsEntry entry {};
  entry.name = name;
  entry.display_name = display_name;
  entry.path = path;
  entry.is_dir = true;
  entry.is_virtual = true;
  entry.has_stat = false;
  entries->push_back(entry);
}

}  // namespace

std::string shell_storage_mount_path(const ShellState& state) {
  return normalize_absolute_path(
      state.config.storage_mount_point != nullptr ? state.config.storage_mount_point
                                                  : "/littlefs");
}

std::string shell_storage_user_root(const ShellState& state) {
  return shell_storage_mount_path(state) + "/ESPUSER";
}

std::string shell_normalize_path(const ShellState& state, const std::string& input) {
  if (input.empty()) {
    return state.cwd.empty() ? "/" : state.cwd;
  }

  if (!input.empty() && input.front() == '/') {
    return normalize_storage_aliases(state, input);
  }

  if (state.cwd == "/") {
    return normalize_storage_aliases(state, "/" + input);
  }

  return normalize_storage_aliases(state, state.cwd + "/" + input);
}

std::string shell_parent_path(const std::string& path) {
  const std::string normalized = normalize_absolute_path(path);
  if (normalized == "/") {
    return "/";
  }

  const size_t pos = normalized.find_last_of('/');
  if (pos == std::string::npos || pos == 0U) {
    return "/";
  }
  return normalized.substr(0U, pos);
}

std::string shell_basename(const std::string& path) {
  const std::string normalized = normalize_absolute_path(path);
  if (normalized == "/") {
    return "/";
  }

  const size_t pos = normalized.find_last_of('/');
  if (pos == std::string::npos) {
    return normalized;
  }
  return normalized.substr(pos + 1U);
}

bool shell_is_virtual_root(const std::string& path) {
  return normalize_absolute_path(path) == "/";
}

bool shell_is_storage_path(const ShellState& state, const std::string& path) {
  const std::string normalized = normalize_absolute_path(path);
  return starts_with_path(normalized, shell_storage_mount_path(state));
}

bool shell_is_user_visible_path(const ShellState& state, const std::string& path) {
  const std::string normalized = normalize_absolute_path(path);
  const std::string mount_point = shell_storage_mount_path(state);
  const std::string user_root = shell_storage_user_root(state);
  if (is_protected_user_storage_path(state, normalized)) {
    return false;
  }
  return normalized == mount_point || normalized == user_root ||
         starts_with_path(normalized, user_root);
}

bool shell_is_user_writable_path(const ShellState& state, const std::string& path) {
  const std::string normalized = normalize_absolute_path(path);
  const std::string user_root = shell_storage_user_root(state);
  if (is_protected_user_storage_path(state, normalized)) {
    return false;
  }
  return normalized == user_root || starts_with_path(normalized, user_root);
}

bool shell_path_exists(
    const ShellState& state,
    const std::string& path,
    bool* is_dir,
    struct stat* info,
    std::string* error) {
  const std::string normalized = normalize_absolute_path(path);

  if (shell_is_virtual_root(normalized)) {
    if (is_dir != nullptr) {
      *is_dir = true;
    }
    if (info != nullptr) {
      *info = {};
    }
    return true;
  }

  if (!shell_is_storage_path(state, normalized)) {
    remote::FsMount remote_mount = remote::FsMount::T41;
    if (remote::fs_mount_for_path(normalized, &remote_mount)) {
      remote::FsMountSnapshot snap {};
      remote::fs_snapshot(remote_mount, &snap);
      if (!snap.mounted) {
        if (error != nullptr) {
          *error = std::string(snap.error_code) + " (mount " + remote::fs_mount_name(remote_mount) + " first)";
        }
        return false;
      }
      if (normalized == remote::fs_mount_root(remote_mount)) {
        if (is_dir != nullptr) {
          *is_dir = true;
        }
        if (info != nullptr) {
          *info = {};
        }
        return true;
      }
      if (error != nullptr) {
        *error = std::string(snap.error_code) + " (t41 MSHELL2 FS protocol required)";
      }
      return false;
    }
    if (error != nullptr) {
      *error = "No such file or directory";
    }
    return false;
  }

  if (!shell_is_storage_mounted(state)) {
    if (error != nullptr) {
      *error = "LittleFS is not mounted";
    }
    return false;
  }

  if (normalized == shell_storage_mount_path(state)) {
    if (is_dir != nullptr) {
      *is_dir = true;
    }
    if (info != nullptr) {
      *info = {};
    }
    return true;
  }

  if (!shell_is_user_visible_path(state, normalized)) {
    if (error != nullptr) {
      *error = "Permission denied";
    }
    return false;
  }

  struct stat local_info {};
  if (stat(normalized.c_str(), &local_info) != 0) {
    if (error != nullptr) {
      *error = ::strerror(errno);
    }
    return false;
  }

  if (is_dir != nullptr) {
    *is_dir = S_ISDIR(local_info.st_mode);
  }
  if (info != nullptr) {
    *info = local_info;
  }
  return true;
}

bool shell_read_directory(
    const ShellState& state,
    const std::string& dir_path,
    const bool include_dot,
    const bool include_dotdot,
    std::vector<ShellFsEntry>* entries,
    std::string* error) {
  if (entries == nullptr) {
    if (error != nullptr) {
      *error = "Invalid argument";
    }
    return false;
  }
  entries->clear();

  const std::string normalized = normalize_absolute_path(dir_path);
  if (shell_is_virtual_root(normalized)) {
    if (include_dot) {
      append_virtual_entry(".", ".", "/", entries);
    }
    if (include_dotdot) {
      append_virtual_entry("..", "..", "/", entries);
    }
    if (shell_is_storage_mounted(state)) {
      const std::string mount_point = normalize_absolute_path(
          state.config.storage_mount_point != nullptr ? state.config.storage_mount_point : "/littlefs");
      append_virtual_entry("fs", "fs", mount_point, entries);
      append_virtual_entry("littlefs", "littlefs", mount_point, entries);
    }
    append_virtual_entry("t41", "t41", "/t41", entries);
    append_virtual_entry("t41-sdcard", "t41-sdcard", "/t41-sdcard", entries);
    return true;
  }

  remote::FsMount remote_mount = remote::FsMount::T41;
  if (remote::fs_mount_for_path(normalized, &remote_mount)) {
    remote::FsMountSnapshot snap {};
    remote::fs_snapshot(remote_mount, &snap);
    if (!snap.mounted) {
      if (error != nullptr) {
        *error = std::string(snap.error_code) + " (mount " + remote::fs_mount_name(remote_mount) + " first)";
      }
      return false;
    }
    if (!snap.protocol_ready) {
      if (error != nullptr) {
        *error = std::string(snap.error_code) + " (t41 MSHELL2 FS protocol required)";
      }
      return false;
    }
    if (error != nullptr) {
      *error = "PEER_PROTOCOL_MISSING (FS_LIST transport pending peer support)";
    }
    return false;
  }

  if (normalized == shell_storage_mount_path(state)) {
    if (include_dot) {
      append_virtual_entry(".", ".", normalized, entries);
    }
    if (include_dotdot) {
      append_virtual_entry("..", "..", shell_parent_path(normalized), entries);
    }

    ShellFsEntry user_entry {};
    user_entry.name = "ESPUSER";
    user_entry.display_name = "ESPUSER";
    user_entry.path = shell_storage_user_root(state);
    user_entry.is_dir = true;
    struct stat info {};
    if (stat(user_entry.path.c_str(), &info) == 0) {
      user_entry.info = info;
      user_entry.has_stat = true;
    }
    entries->push_back(std::move(user_entry));
    return true;
  }

  bool is_dir = false;
  if (!shell_path_exists(state, normalized, &is_dir, nullptr, error)) {
    return false;
  }
  if (!is_dir) {
    if (error != nullptr) {
      *error = "Not a directory";
    }
    return false;
  }

  if (include_dot) {
    append_virtual_entry(".", ".", normalized, entries);
  }
  if (include_dotdot) {
    append_virtual_entry("..", "..", shell_parent_path(normalized), entries);
  }

  DIR* dir = opendir(normalized.c_str());
  if (dir == nullptr) {
    if (error != nullptr) {
      *error = ::strerror(errno);
    }
    return false;
  }

  dirent* entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    if (::strcmp(entry->d_name, ".") == 0 || ::strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    ShellFsEntry shell_entry {};
    shell_entry.name = entry->d_name;
    shell_entry.display_name = entry->d_name;
    shell_entry.path = make_child_path(normalized, entry->d_name);
    if (!shell_is_user_visible_path(state, shell_entry.path)) {
      continue;
    }
    shell_entry.is_virtual = false;

    struct stat info {};
    if (stat(shell_entry.path.c_str(), &info) == 0) {
      shell_entry.info = info;
      shell_entry.has_stat = true;
      shell_entry.is_dir = S_ISDIR(info.st_mode);
    } else {
      shell_entry.has_stat = false;
      shell_entry.is_dir = entry->d_type == DT_DIR;
    }

    entries->push_back(std::move(shell_entry));
  }

  closedir(dir);
  return true;
}

bool shell_openable_file_path(
    const ShellState& state,
    const std::string& path,
    std::string* actual_path,
    std::string* error) {
  const std::string normalized = normalize_absolute_path(path);
  bool is_dir = false;
  if (!shell_path_exists(state, normalized, &is_dir, nullptr, error)) {
    return false;
  }

  if (shell_is_virtual_root(normalized) || is_dir) {
    if (error != nullptr) {
      *error = "Is a directory";
    }
    return false;
  }

  if (!shell_is_user_writable_path(state, normalized)) {
    if (error != nullptr) {
      *error = "Permission denied";
    }
    return false;
  }

  if (actual_path != nullptr) {
    *actual_path = normalized;
  }
  return true;
}

}  // namespace mros::shell
