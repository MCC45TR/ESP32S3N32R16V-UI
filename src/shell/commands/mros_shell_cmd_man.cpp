#include "src/shell/mros_shell_internal.h"

#include <cstring>

namespace mros::shell {
namespace {

const char* canonical_manual_name(const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
  if (std::strcmp(name, "[") == 0) return "test";
  if (std::strcmp(name, "script") == 0) return "source";
  if (std::strcmp(name, "htop") == 0) return "mtop";
  if (std::strcmp(name, "espfetch") == 0) return "mfetch";
  if (std::strcmp(name, "mros7dofs3-update") == 0) return "mros-deuscara-update";
  if (std::strcmp(name, "EOF") == 0 || std::strcmp(name, "eof") == 0) return "EOF";
  return name;
}

}  // namespace

void shell_help_man(ShellState& state) {
  shell_write_line(state, "Usage: man COMMAND");
  shell_write_line(state, "Show the manual/help page for a registered shell command.");
  shell_write_line(state, "Examples:");
  shell_write_line(state, "  man robot");
  shell_write_line(state, "  man mfetch");
}

int shell_cmd_man(ShellContext& ctx) {
  if (ctx.args.size() != 2U || ctx.args[1] == "--help" || ctx.args[1] == "-h") {
    shell_help_man(ctx.state);
    return ctx.args.size() == 2U ? 0 : 1;
  }
  const char* requested = ctx.args[1].c_str();
  const char* canonical = canonical_manual_name(requested);
  const ShellCommandRegistration* command = shell_find_command(canonical);
  if (command == nullptr) {
    shell_printf(ctx.state, "man: no manual entry for %s\n", ctx.args[1].c_str());
    return 1;
  }
  if (canonical != requested && ctx.args[1] != canonical) {
    shell_printf(ctx.state, "%s is an alias for %s\n\n", requested, canonical);
  }
  shell_printf(
      ctx.state,
      "%s - %s\n\n",
      command->name != nullptr ? command->name : "(unknown)",
      command->summary != nullptr ? command->summary : "No summary available.");
  if (command->help_handler != nullptr) {
    command->help_handler(ctx.state);
  } else {
    shell_write_line(ctx.state, "No detailed manual page is available.");
  }
  return 0;
}

}  // namespace mros::shell
