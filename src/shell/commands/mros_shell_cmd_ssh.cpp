#include "src/shell/mros_shell_internal.h"

#include <cstdlib>

#include "src/net/ssh_service.h"

namespace mros::shell {
namespace {

bool parse_port(const std::vector<std::string>& args, uint16_t* port) {
  if (port == nullptr) {
    return false;
  }
  *port = 22U;
  for (size_t i = 0; i + 1U < args.size(); ++i) {
    if (args[i] == "-P" || args[i] == "-p" || args[i] == "--port") {
      char* end = nullptr;
      const unsigned long value = std::strtoul(args[i + 1U].c_str(), &end, 10);
      if (end == args[i + 1U].c_str() || (end != nullptr && *end != '\0') ||
          value == 0UL || value > 65535UL) {
        return false;
      }
      *port = static_cast<uint16_t>(value);
      return true;
    }
  }
  return true;
}

}  // namespace

void shell_help_ssh(ShellState& state) {
  shell_write_line(state, "Usage:");
  shell_write_line(state, "  ssh connect user@host [-P port]");
  shell_write_line(state, "");
  shell_write_line(state, "Outbound SSH client command. SCP/SFTP are not part of v1.");
}

int shell_cmd_ssh(ShellContext& ctx) {
  if (ctx.args.size() < 2U || ctx.args[1] == "--help" || ctx.args[1] == "-h") {
    shell_help_ssh(ctx.state);
    return ctx.args.size() < 2U ? 1 : 0;
  }
  if (ctx.args[1] != "connect") {
    shell_printf(ctx.state, "ssh: unknown command '%s'\n", ctx.args[1].c_str());
    return 1;
  }
  if (ctx.args.size() < 3U) {
    shell_write_line(ctx.state, "ssh: target required: ssh connect user@host -P 5463");
    return 1;
  }
  const std::string& target = ctx.args[2];
  const size_t at = target.find('@');
  if (at == std::string::npos || at == 0U || at == target.size() - 1U) {
    shell_write_line(ctx.state, "ssh: target must be user@host");
    return 1;
  }
  uint16_t port = 22U;
  if (!parse_port(ctx.args, &port)) {
    shell_write_line(ctx.state, "ssh: invalid port");
    return 1;
  }
  shell_printf(ctx.state,
               "ssh: outbound client queued target=%s port=%u\n",
               target.c_str(),
               static_cast<unsigned>(port));
  shell_write_line(ctx.state,
                   "ssh: client backend will activate when wolfSSH transport is linked");
  return 0;
}

}  // namespace mros::shell
