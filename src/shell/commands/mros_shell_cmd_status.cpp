#include "src/shell/mros_shell_internal.h"

#include <string>

namespace mros::shell {
namespace {

std::string json_escape(const char* text) {
  std::string out;
  if (text == nullptr) {
    return out;
  }
  for (const char* p = text; *p != '\0'; ++p) {
    switch (*p) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(*p);
        break;
    }
  }
  return out;
}

}  // namespace

void shell_help_status(ShellState& state) {
  shell_write_line(state, "Usage: status [--json]");
  shell_write_line(state, "Show a compact system status summary.");
}

int shell_cmd_status(ShellContext& ctx) {
  bool json = ctx.json_output;
  if (ctx.args.size() > 1U) {
    if (ctx.args.size() == 2U && (ctx.args[1] == "--help" || ctx.args[1] == "-h")) {
      shell_help_status(ctx.state);
      return 0;
    }
    if (ctx.args.size() == 2U && ctx.args[1] == "--json") {
      json = true;
    } else {
      shell_write_line(ctx.state, "status: too many arguments");
      return 1;
    }
  }

  if (ctx.state.config.status_callback == nullptr) {
    if (json) {
      shell_write_line(ctx.state, "{\"ok\":false,\"error\":\"status callback is not configured\"}");
      return 1;
    }
    shell_write_line(ctx.state, "status: status callback is not configured");
    return 1;
  }

  char message[1024] = {};
  const bool ok = ctx.state.config.status_callback(message, sizeof(message), ctx.state.config.user_data);
  if (json) {
    const std::string escaped = json_escape(message[0] != '\0' ? message : (ok ? "status: no status available" : "status: failed"));
    shell_printf(ctx.state, "{\"ok\":%s,\"status\":\"%s\"}\n", ok ? "true" : "false", escaped.c_str());
    return ok ? 0 : 1;
  }
  shell_write_line(
      ctx.state,
      message[0] != '\0' ? message : (ok ? "status: no status available" : "status: failed"));
  return ok ? 0 : 1;
}

}  // namespace mros::shell
