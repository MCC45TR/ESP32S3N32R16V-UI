#include "src/shell/mros_shell_internal.h"

#include <string>

namespace mros::shell {
namespace {

struct UnameFlags {
  bool all = false;
  bool kernel_name = false;
  bool node_name = false;
  bool kernel_release = false;
  bool machine = false;
};

bool parse_uname_args(ShellContext& ctx, UnameFlags* flags, bool* help_requested) {
  if (flags == nullptr || help_requested == nullptr) {
    return false;
  }

  *help_requested = false;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help") {
      *help_requested = true;
      return true;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      for (size_t j = 1U; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'a':
            flags->all = true;
            break;
          case 'm':
            flags->machine = true;
            break;
          case 'n':
            flags->node_name = true;
            break;
          case 'r':
            flags->kernel_release = true;
            break;
          case 's':
            flags->kernel_name = true;
            break;
          default:
            shell_printf(ctx.state, "uname: invalid option -- '%c'\n", arg[j]);
            return false;
        }
      }
      continue;
    }
    shell_printf(ctx.state, "uname: extra operand '%s'\n", arg.c_str());
    return false;
  }
  return true;
}

void append_field(std::string* output, const char* value) {
  if (output == nullptr || value == nullptr || value[0] == '\0') {
    return;
  }
  if (!output->empty()) {
    output->push_back(' ');
  }
  output->append(value);
}

}  // namespace

void shell_help_uname(ShellState& state) {
  shell_write_line(state, "Usage: uname [OPTION]...");
  shell_write_line(state, "Print certain system information.");
  shell_write_line(state, "  -a                         print all information");
  shell_write_line(state, "  -m                         print the machine hardware name");
  shell_write_line(state, "  -n                         print the network node hostname");
  shell_write_line(state, "  -r                         print the system release");
  shell_write_line(state, "  -s                         print the kernel name");
}

int shell_cmd_uname(ShellContext& ctx) {
  UnameFlags flags {};
  bool help_requested = false;
  if (!parse_uname_args(ctx, &flags, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_uname(ctx.state);
    return 0;
  }

  if (!(flags.all || flags.kernel_name || flags.node_name || flags.kernel_release || flags.machine)) {
    flags.kernel_name = true;
  }
  if (flags.all) {
    flags.kernel_name = true;
    flags.node_name = true;
    flags.kernel_release = true;
    flags.machine = true;
  }

  const char* hostname =
      (ctx.state.config.hostname != nullptr && ctx.state.config.hostname[0] != '\0') ? ctx.state.config.hostname : "mros";
  const char* firmware_name =
      (ctx.state.config.firmware_name != nullptr && ctx.state.config.firmware_name[0] != '\0')
          ? ctx.state.config.firmware_name
          : "MROS";
  const char* board_name =
      (ctx.state.config.board_name != nullptr && ctx.state.config.board_name[0] != '\0')
          ? ctx.state.config.board_name
          : "ESP32";

  std::string output;
  if (flags.kernel_name) {
    append_field(&output, "MROS");
  }
  if (flags.node_name) {
    append_field(&output, hostname);
  }
  if (flags.kernel_release) {
    append_field(&output, firmware_name);
  }
  if (flags.machine) {
    append_field(&output, board_name);
  }
  shell_write_line(ctx.state, output.c_str());
  return 0;
}

}  // namespace mros::shell
