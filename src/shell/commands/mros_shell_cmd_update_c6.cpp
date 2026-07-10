#include "src/shell/mros_shell_internal.h"

#include <cstring>
#include <string>

#include "src/platform/mros_time.h"

namespace mros::shell {
namespace {

const char* c6_state_name(const ShellC6UpdateState state) {
  switch (state) {
    case ShellC6UpdateState::Queued:
      return "queued";
    case ShellC6UpdateState::Preparing:
      return "preparing";
    case ShellC6UpdateState::Validating:
      return "validating";
    case ShellC6UpdateState::Transferring:
      return "transferring";
    case ShellC6UpdateState::Finalizing:
      return "finalizing";
    case ShellC6UpdateState::Activating:
      return "activating";
    case ShellC6UpdateState::Reconnecting:
      return "reconnecting";
    case ShellC6UpdateState::Completed:
      return "completed";
    case ShellC6UpdateState::NotRequired:
      return "not_required";
    case ShellC6UpdateState::Failed:
      return "failed";
    case ShellC6UpdateState::Idle:
    default:
      return "idle";
  }
}

bool path_has_bin_extension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  return dot != std::string::npos && path.substr(dot) == ".bin";
}

bool ensure_storage_mounted_for_path(ShellState& state, const std::string& path) {
  if (!shell_is_storage_path(state, path)) {
    return true;
  }
  if (shell_is_storage_mounted(state)) {
    return true;
  }
  if (state.config.mount_storage_callback == nullptr) {
    shell_write_line(state, "update-c6: LittleFS mount callback tanimli degil");
    return false;
  }
  char message[192] = {};
  const bool ok =
      state.config.mount_storage_callback(message, sizeof(message), state.config.user_data);
  shell_write_line(
      state,
      message[0] != '\0' ? message : (ok ? "LittleFS baglandi." : "LittleFS baglanamadi."));
  return ok;
}

}  // namespace

void shell_help_update_c6(ShellState& state) {
  shell_write_line(state, "Usage: update-c6 default");
  shell_write_line(state, "       update-c6 /fs/path/to/fw.bin");
  shell_write_line(state, "default -> lfs_c6:/c6/fw/current.bin, path -> LittleFS veya yerel .bin dosyasi.");
  shell_write_line(state, "Write a firmware image to the onboard ESP32-C6 coprocessor.");
}

int shell_cmd_update_c6(ShellContext& ctx) {
  if (ctx.args.size() != 2U) {
    shell_help_update_c6(ctx.state);
    return 1;
  }

  if (ctx.args[1] == "--help" || ctx.args[1] == "-h") {
    shell_help_update_c6(ctx.state);
    return 0;
  }

  if (ctx.state.config.c6_update_start_callback == nullptr ||
      ctx.state.config.c6_update_snapshot_callback == nullptr) {
    shell_write_line(ctx.state, "update-c6: C6 update callbacklari tanimli degil");
    return 1;
  }

  ShellC6UpdateRequest request {};
  std::string resolved_path;
  if (ctx.args[1] == "default") {
    request.source = ShellC6UpdateSourceKind::Default;
  } else {
    request.source = ShellC6UpdateSourceKind::File;
    resolved_path = shell_normalize_path(ctx.state, ctx.args[1]);
    if (!ensure_storage_mounted_for_path(ctx.state, resolved_path)) {
      return 1;
    }

    bool is_dir = false;
    std::string error;
    if (!shell_path_exists(ctx.state, resolved_path, &is_dir, nullptr, &error)) {
      shell_printf(
          ctx.state,
          "update-c6: '%s' bulunamadi: %s\n",
          resolved_path.c_str(),
          error.c_str());
      return 1;
    }
    if (is_dir) {
      shell_write_line(ctx.state, "update-c6: klasor yerine .bin firmware dosyasi verin");
      return 1;
    }
    if (!path_has_bin_extension(resolved_path)) {
      shell_write_line(ctx.state, "update-c6: sadece .bin dosyalari desteklenir");
      return 1;
    }
    request.path = resolved_path.c_str();
  }

  char message[192] = {};
  if (!ctx.state.config.c6_update_start_callback(
          request,
          message,
          sizeof(message),
          ctx.state.config.user_data)) {
    shell_write_line(
        ctx.state,
        message[0] != '\0' ? message : "update-c6: istek baslatilamadi");
    return 1;
  }
  if (message[0] != '\0') {
    shell_write_line(ctx.state, message);
  }

  ShellC6UpdateSnapshot snapshot {};
  uint32_t last_revision = UINT32_MAX;
  const uint32_t started_at = mros::platform::mros_millis();
  for (;;) {
    if (!ctx.state.config.c6_update_snapshot_callback(
            &snapshot,
            ctx.state.config.user_data)) {
      shell_write_line(ctx.state, "update-c6: durum okunamadi");
      return 1;
    }

    if (snapshot.revision != last_revision) {
      shell_printf(
          ctx.state,
          "[%3u%%] %-13s %s\n",
          static_cast<unsigned>(snapshot.progress),
          c6_state_name(snapshot.state),
          snapshot.status_text[0] != '\0' ? snapshot.status_text : snapshot.detail_text);
      if (snapshot.detail_text[0] != '\0' &&
          std::strcmp(snapshot.detail_text, snapshot.status_text) != 0) {
        shell_printf(ctx.state, "         %s\n", snapshot.detail_text);
      }
      last_revision = snapshot.revision;
    }

    if (!snapshot.busy) {
      if (snapshot.last_result[0] != '\0') {
        shell_write_line(ctx.state, snapshot.last_result);
      }
      return snapshot.state == ShellC6UpdateState::Completed ||
                     snapshot.state == ShellC6UpdateState::NotRequired
                 ? 0
                 : 1;
    }

    if ((mros::platform::mros_millis() - started_at) > 300000UL) {
      shell_write_line(ctx.state, "update-c6: zaman asimi");
      return 1;
    }
    mros::platform::mros_delay_ms(150);
  }
}

}  // namespace mros::shell
