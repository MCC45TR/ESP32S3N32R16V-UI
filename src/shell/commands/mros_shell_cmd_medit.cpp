#include "src/shell/mros_shell_internal.h"

#include <cstdio>
#include <string>

namespace mros::shell {
namespace {

std::string shortcut_help() {
  return
      "medit shortcuts\n"
      "---------------\n"
      "Ctrl+O  save file\n"
      "Ctrl+X  exit editor\n"
      "Ctrl+K  cut current line\n"
      "Ctrl+U  paste cut line\n"
      "Ctrl+W  search text\n"
      "Ctrl+G  show help\n";
}

}  // namespace

void shell_help_medit(ShellState& state) {
  shell_write_line(state, "Usage: medit FILE");
  shell_write_line(state, "SSH-only text editor for files under /ESPUSER.");
  shell_write_line(state, "Serial and web console calls are rejected so the editor cannot trap the UI.");
  shell_write_line(state, "Shortcuts: Ctrl+O save, Ctrl+X exit, Ctrl+K cut, Ctrl+U paste, Ctrl+W search, Ctrl+G help.");
}

int shell_cmd_medit(ShellContext& ctx) {
  if (ctx.args.size() < 2U || ctx.args[1] == "--help" || ctx.args[1] == "-h") {
    shell_help_medit(ctx.state);
    return ctx.args.size() < 2U ? 1 : 0;
  }
  if (ctx.transport != ShellTransport::Ssh && ctx.state.active_transport != ShellTransport::Ssh) {
    shell_write_line(ctx.state, "medit: only available from an SSH shell session");
    shell_write_line(ctx.state, "medit: web and serial are blocked to avoid trapping the console");
    return 2;
  }

  const std::string path = shell_normalize_path(ctx.state, ctx.args[1]);
  if (!shell_is_user_writable_path(ctx.state, path)) {
    shell_write_line(ctx.state, "medit: file must be inside /ESPUSER");
    return 1;
  }

  if (ctx.args.size() >= 3U && ctx.args[2] == "--keys") {
    shell_write(ctx.state, shortcut_help().c_str());
    return 0;
  }

  shell_write_line(ctx.state, "medit: SSH transport is allowed, but full-screen editor loop is staged.");
  shell_write_line(ctx.state, "medit: planned mode will use raw SSH pty input and the shortcuts below.");
  shell_write(ctx.state, shortcut_help().c_str());
  shell_printf(ctx.state, "medit: target=%s\n", path.c_str());
  return 2;
}

}  // namespace mros::shell
