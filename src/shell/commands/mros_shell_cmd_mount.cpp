#include "src/shell/mros_shell_internal.h"
#include "src/shell/mshell_remote.h"

#include <cstring>
#include <string>

namespace mros::shell {
namespace {

bool is_mount_target(const char* text) {
  return text != nullptr &&
         (::strcmp(text, "fs") == 0 || ::strcmp(text, "/fs") == 0 ||
          ::strcmp(text, "littlefs") == 0 || ::strcmp(text, "/littlefs") == 0 ||
          ::strcmp(text, "status") == 0 || ::strcmp(text, "t41") == 0 ||
          ::strcmp(text, "/t41") == 0 || ::strcmp(text, "t41-sdcard") == 0 ||
          ::strcmp(text, "/t41-sdcard") == 0);
}

void print_mount_status(ShellState& state) {
  if (shell_is_storage_mounted(state)) {
    shell_printf(
        state,
        "%s on /fs type littlefs (rw)\n",
        state.config.storage_mount_point != nullptr ? state.config.storage_mount_point : "/littlefs");
  } else {
    shell_write_line(state, "mount: LittleFS is not mounted");
  }
  for (const auto mount : {remote::FsMount::T41, remote::FsMount::T41Sdcard}) {
    remote::FsMountSnapshot snap {};
    remote::fs_snapshot(mount, &snap);
    shell_printf(
        state,
        "%s on %s type remote-mshell (%s,%s) peer=%s error=%s\n",
        snap.name,
        snap.root,
        snap.mounted ? "mounted" : "down",
        snap.writable ? "rw" : "ro",
        snap.peer_status,
        snap.error_code);
  }
}

}  // namespace

void shell_help_mount(ShellState& state) {
  shell_write_line(state, "Usage: mount [fs|/fs|littlefs|/littlefs|t41|t41-sdcard|status]");
  shell_write_line(state, "Validate local LittleFS or mount a t41 remote filesystem provider.");
  shell_write_line(state, "Remote mounts require: set uart-shell-bridge on");
}

int shell_cmd_mount(ShellContext& ctx) {
  bool status_only = false;
  remote::FsMount remote_mount = remote::FsMount::T41;
  bool has_remote_target = false;

  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help" || arg == "-h") {
      shell_help_mount(ctx.state);
      return 0;
    }
    remote::FsMount parsed = remote::FsMount::T41;
    if (remote::fs_parse_mount(arg, &parsed)) {
      remote_mount = parsed;
      has_remote_target = true;
      continue;
    }
    if (!is_mount_target(arg.c_str())) {
      shell_printf(ctx.state, "mount: unsupported target '%s'\n", arg.c_str());
      return 1;
    }
    if (arg == "status") {
      status_only = true;
    }
  }

  if (has_remote_target) {
    std::string message;
    const bool ok = remote::fs_mount(remote_mount, &message);
    shell_write_line(ctx.state, message.c_str());
    return ok ? 0 : 1;
  }

  if (status_only) {
    print_mount_status(ctx.state);
    return shell_is_storage_mounted(ctx.state) ? 0 : 1;
  }

  if (shell_is_storage_mounted(ctx.state)) {
    print_mount_status(ctx.state);
    return 0;
  }

  if (ctx.state.config.mount_storage_callback == nullptr) {
    shell_write_line(ctx.state, "mount: LittleFS mount callback is not configured");
    return 1;
  }

  char message[192] = {};
  const bool ok =
      ctx.state.config.mount_storage_callback(message, sizeof(message), ctx.state.config.user_data);
  shell_write_line(ctx.state, message[0] != '\0' ? message : (ok ? "LittleFS ready." : "LittleFS mount failed."));
  return ok ? 0 : 1;
}

void shell_help_umount(ShellState& state) {
  shell_write_line(state, "Usage: umount t41|t41-sdcard|all");
  shell_write_line(state, "Disconnects shell/web visibility for remote t41 filesystem mounts.");
}

int shell_cmd_umount(ShellContext& ctx) {
  if (ctx.args.size() <= 1U || ctx.args[1] == "--help" || ctx.args[1] == "-h") {
    shell_help_umount(ctx.state);
    return ctx.args.size() <= 1U ? 1 : 0;
  }
  bool ok_all = true;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "all") {
      remote::fs_umount_all();
      shell_write_line(ctx.state, "remote filesystem mounts: unmounted");
      continue;
    }
    remote::FsMount mount = remote::FsMount::T41;
    if (!remote::fs_parse_mount(arg, &mount)) {
      shell_printf(ctx.state, "umount: unsupported target '%s'\n", arg.c_str());
      ok_all = false;
      continue;
    }
    std::string message;
    const bool ok = remote::fs_umount(mount, &message);
    shell_write_line(ctx.state, message.c_str());
    ok_all = ok_all && ok;
  }
  return ok_all ? 0 : 1;
}

}  // namespace mros::shell
