#include "src/shell/mros_shell_internal.h"
#include "src/shell/mshell_remote.h"
#include "src/shell/mshell_runtime.h"
#include "src/platform/mros_time.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace mros::shell {
namespace {

bool is_help_arg(const std::string& arg) {
  return arg == "--help" || arg == "-h";
}

constexpr const char* kRawJsonPrefix = "@@RAW_JSON@@";

struct ApiParam {
  std::string key;
  std::string value;
};

struct ApiSpec {
  const char* name;
  const char* method;
  const char* command;
  const char* summary;
  uint32_t capabilities;
};

const ApiSpec kApiSpecs[] = {
    {"system.status", "GET", "status --json", "Compact system health snapshot", ShellCapabilityRead},
    {"system.info", "GET", "mfetch --json", "Firmware, recovery, heap and library metadata", ShellCapabilityRead},
    {"wifi.status", "GET", "wifi info", "WiFi STA/AP state and reconnect diagnostics", ShellCapabilityNetwork},
    {"wifi.scan", "POST", "wifi scan", "Queue/list a WiFi scan using the cached scan path", ShellCapabilityNetwork},
    {"files.list", "GET", "ls -l {path}", "List local or mounted provider path", ShellCapabilityRead},
    {"files.mounts", "GET", "mount status", "List local and remote filesystem mount state", ShellCapabilityRead},
    {"robot.status", "GET", "robot status --json", "Robot control state summary", ShellCapabilityRobot},
    {"robot.math.status", "GET", "robot math status --json", "Robot math backend/solver state", ShellCapabilityRobot},
    {"robot.math.benchmark", "POST", "robot math benchmark --json", "Run bounded math benchmark", ShellCapabilityRobot},
    {"robot.fk", "POST", "robot math fk solve {joints} --json", "Run FK for seven joint values", ShellCapabilityRobot},
    {"robot.ik", "POST", "robot math ik preview {x} {y} {z} --json", "Preview IK for a Cartesian target", ShellCapabilityRobot},
    {"devices.status", "GET", "devices status", "Passive device/link snapshot", ShellCapabilityRead},
    {"devices.test", "POST", "devices test {target}", "Run active lightweight device diagnostic", ShellCapabilityDebug},
    {"security.status", "GET", "mros security status", "Security/session/audit state", ShellCapabilityDebug},
    {"report.create", "POST", "mros report create", "Create bounded support report", ShellCapabilityDebug},
    {"audit.list", "GET", "mros audit list", "List shell/security audit ring", ShellCapabilityDebug},
};

const uint32_t kApiSpecCount = sizeof(kApiSpecs) / sizeof(kApiSpecs[0]);
uint32_t g_api_request_seq = 0U;

std::string lower_copy(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

std::string json_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8U);
  for (const char ch : text) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          char tmp[8] = {};
          std::snprintf(tmp, sizeof(tmp), "\\u%04x", static_cast<unsigned char>(ch));
          out += tmp;
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

std::string shell_quote(const std::string& value) {
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '\\' || ch == '"') out.push_back('\\');
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

const ApiSpec* find_api_spec(const std::string& name) {
  const std::string key = lower_copy(name);
  for (const ApiSpec& spec : kApiSpecs) {
    if (key == spec.name) {
      return &spec;
    }
  }
  return nullptr;
}

const char* risk_for_caps(const uint32_t caps) {
  if ((caps & ShellCapabilityRoot) != 0U || (caps & ShellCapabilityUpdate) != 0U) return "critical";
  if ((caps & ShellCapabilityRobot) != 0U || (caps & ShellCapabilityDebug) != 0U) return "high";
  if ((caps & ShellCapabilityWrite) != 0U || (caps & ShellCapabilityNetwork) != 0U) return "medium";
  return "low";
}

const char* group_for_command(const char* name, const uint32_t caps) {
  if (name == nullptr) return "other";
  const std::string n = lower_copy(name);
  if (n == "ls" || n == "cat" || n == "cp" || n == "mv" || n == "rm" || n == "mkdir" ||
      n == "touch" || n == "find" || n == "stat" || n == "mount" || n == "umount" ||
      n == "lsblk" || n == "df" || n == "du") return "files";
  if (n == "wifi" || n == "ping" || n == "ssh" || n == "devices") return "network";
  if (n == "robot" || n == "mros") return "robot";
  if (n == "help" || n == "man" || n == "mshell" || n == "config" || n == "set" ||
      n == "history" || n == "watch") return "shell";
  if (n == "su" || n == "sudo" || n == "passwd" || n == "change" || n == "whoami" ||
      n == "id" || n == "groups") return "security";
  if ((caps & ShellCapabilityUpdate) != 0U) return "update";
  if ((caps & ShellCapabilityRoot) != 0U) return "system";
  return "utility";
}

std::string path_to_api_name(std::string path) {
  if (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  for (char& ch : path) {
    if (ch == '/') ch = '.';
  }
  return lower_copy(path);
}

std::string param_value(const std::vector<ApiParam>& params, const char* key, const char* fallback = "") {
  for (const ApiParam& param : params) {
    if (param.key == key) {
      return param.value;
    }
  }
  return fallback != nullptr ? fallback : "";
}

std::string render_template(std::string templ, const std::vector<ApiParam>& params) {
  const struct DefaultParam {
    const char* key;
    const char* value;
  } defaults[] = {
      {"path", "/ESPUSER"},
      {"target", "all"},
      {"joints", "0 0 0 0 0 0 0"},
      {"x", "0"},
      {"y", "0"},
      {"z", "0"},
  };
  for (const DefaultParam& def : defaults) {
    std::string value = param_value(params, def.key, def.value);
    const std::string token = std::string("{") + def.key + "}";
    size_t pos = 0U;
    while ((pos = templ.find(token, pos)) != std::string::npos) {
      const bool quote = std::strcmp(def.key, "path") == 0;
      const std::string replacement = quote ? shell_quote(value) : value;
      templ.replace(pos, token.size(), replacement);
      pos += replacement.size();
    }
  }
  return templ;
}

void write_json(ShellContext& ctx, const std::string& json) {
  std::string out = ctx.json_output ? kRawJsonPrefix : "";
  out += json;
  if (out.empty() || out.back() != '\n') out.push_back('\n');
  shell_write(ctx.state, out.c_str());
}

int write_call_error(
    ShellContext& ctx,
    const char* api,
    const char* code,
    const char* message,
    const remote::Target target = remote::Target::S3) {
  if (!ctx.json_output) {
    shell_printf(ctx.state, "mshell call: %s\n", message != nullptr ? message : code);
    return 1;
  }
  const uint32_t request_id = ++g_api_request_seq;
  std::string json = "{\"ok\":false,\"shell\":\"";
  json += kShellName;
  json += "\",\"version\":\"";
  json += kShellVersion;
  json += "\",\"request_id\":";
  json += std::to_string(request_id);
  json += ",\"api\":\"";
  json += json_escape(api != nullptr ? api : "");
  json += "\",\"target\":\"";
  json += remote::target_name(target);
  json += "\",\"error_code\":\"";
  json += code != nullptr ? code : "INTERNAL_ERROR";
  json += "\",\"error\":\"";
  json += json_escape(message != nullptr ? message : "");
  json += "\"}";
  write_json(ctx, json);
  return 1;
}

std::string scripts_dir(const ShellState& state) {
  return shell_storage_user_root(state) + "/scripts";
}

std::string join_args(const std::vector<std::string>& args, const size_t start) {
  std::string out;
  for (size_t i = start; i < args.size(); ++i) {
    if (!out.empty()) out.push_back(' ');
    out += args[i];
  }
  return out;
}

std::string script_path(const ShellState& state, const std::string& name) {
  return scripts_dir(state) + "/" + name + ".msh";
}

bool safe_script_name(const std::string& name) {
  if (name.empty()) return false;
  for (const char ch : name) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    if (!ok) return false;
  }
  return true;
}

void print_feature_table(ShellState& state) {
  shell_write_line(state, "feature                 status       note");
  shell_write_line(state, "----------------------  -----------  --------------------------------------");
  shell_write_line(state, "aliases                 ready        mshell alias list/add/remove, LittleFS");
  shell_write_line(state, "pipeline                ready        cmd | grep text");
  shell_write_line(state, "redirection             ready        cmd > /ESPUSER/file.txt");
  shell_write_line(state, "json output             ready        cmd --json");
  shell_write_line(state, "api call wrapper        ready        mshell call system.status --json");
  shell_write_line(state, "api schema registry     ready        mshell schema api|commands --json");
  shell_write_line(state, "doctor/perf             ready        mshell doctor, mshell perf");
  shell_write_line(state, "background jobs         ready        mshell job start|list|log|cancel");
  shell_write_line(state, "transactions            ready        mshell tx begin|stage|commit|rollback");
  shell_write_line(state, "error explain           ready        mshell explain PEER_PROTOCOL_MISSING");
  shell_write_line(state, "watch/monitor           ready        mros watch -n 5 <subcommand>");
  shell_write_line(state, "diagnostic bundle       ready        mros diag bundle");
  shell_write_line(state, "self tests              ready        mros test spi|uart|i2c|all");
  shell_write_line(state, "firmware update check   staged       mros-deuscara-update check");
  shell_write_line(state, "scripts/profiles        ready        mshell script save/run/list");
  shell_write_line(state, "uart remote bridge      ready        mshell connect|disconnect|status t41|s3|all (c3 disabled)");
  shell_write_line(state, "persistent history      session      in-shell history command is ready");
  shell_write_line(state, "background jobs         ready        fixed pool, bounded logs");
  shell_write_line(state, "admin unlock            ready        su/sudo and multi-user support enabled");
  shell_write_line(state, "flow control            ready        ; && || if/then/else/fi for/do/done");
}

void append_release_feature_json(std::string* out, const char* name, const char* status, const char* evidence, bool* first) {
  if (out == nullptr || first == nullptr) {
    return;
  }
  if (!*first) {
    *out += ",";
  }
  *first = false;
  *out += "{\"name\":\"";
  *out += json_escape(name != nullptr ? name : "");
  *out += "\",\"status\":\"";
  *out += json_escape(status != nullptr ? status : "");
  *out += "\",\"evidence\":\"";
  *out += json_escape(evidence != nullptr ? evidence : "");
  *out += "\"}";
}

std::string release_manifest_json() {
  std::string out;
  out.reserve(2048U);
  out += "{\"ok\":true,\"shell\":\"";
  out += kShellName;
  out += "\",\"version\":\"";
  out += kShellVersion;
  out += "\",\"release_line\":\"";
  out += kShellReleaseLine;
  out += "\",\"release_status\":\"";
  out += kShellReleaseStatus;
  out += "\",\"protocol\":\"";
  out += kShellProtocolName;
  out += "\",\"formats\":[\"";
  out += kShellBinaryFormat;
  out += "\",\"";
  out += kShellCborFormat;
  out += "\",\"";
  out += kShellJsonFormat;
  out += "\"],\"compatibility\":\"legacy /ws-shell and shell-json-v1 retained\",";
  out += "\"release_quality\":\"host-gated\",\"required_gates\":[";
  out += "\"shell_bin_v1_frame_test\",\"shell_control_cbor_test\",\"shell_stream_cost_test\",";
  out += "\"shell_virtual_terminal_test\",\"shell_ux_contract_test\",";
  out += "\"shell_slow_client_backpressure_test\",\"mshell_remote_tunnel_test\",";
  out += "\"shell_v02_release_quality_test\"],\"features\":[";
  bool first = true;
  append_release_feature_json(&out, "binary_stream", "done", "MSH1 shell-bin-v1 stream path with JSON fallback", &first);
  append_release_feature_json(&out, "typed_control", "done", "SCB1 shell-cbor-v1 control frame fixtures", &first);
  append_release_feature_json(&out, "backpressure", "done", "seq/ack/credit counters and slow-client fixture", &first);
  append_release_feature_json(&out, "completion", "done", "ranked command/path/option suggestion contract", &first);
  append_release_feature_json(&out, "history_search", "done", "browser reverse/fuzzy history metadata contract", &first);
  append_release_feature_json(&out, "terminal_render", "done", "virtual terminal, ANSI clear and local-first input tests", &first);
  append_release_feature_json(&out, "remote_tunnel", "done", "COBS MSH1 tunnel fixture with text fallback", &first);
  append_release_feature_json(&out, "legacy_compat", "done", "/ws-shell and shell-json-v1 unchanged", &first);
  out += "]}";
  return out;
}

void print_release_manifest_text(ShellState& state) {
  shell_printf(state, "%s %s (%s, %s)\n", kShellName, kShellVersion, kShellReleaseLine, kShellReleaseStatus);
  shell_printf(state, "protocol      : %s\n", kShellProtocolName);
  shell_printf(state, "formats       : %s, %s, %s\n", kShellBinaryFormat, kShellCborFormat, kShellJsonFormat);
  shell_write_line(state, "compatibility : legacy /ws-shell and shell-json-v1 retained");
  shell_write_line(state, "");
  shell_write_line(state, "release gate                 status  evidence");
  shell_write_line(state, "---------------------------  ------  ----------------------------------------");
  shell_write_line(state, "binary stream                done    MSH1 shell-bin-v1 stream path");
  shell_write_line(state, "typed control                done    SCB1 shell-cbor-v1 fixtures");
  shell_write_line(state, "backpressure                 done    seq/ack/credit slow-client fixture");
  shell_write_line(state, "completion/history UX        done    shell_ux_contract_test");
  shell_write_line(state, "terminal render/input        done    virtual terminal + local input tests");
  shell_write_line(state, "remote tunnel                done    COBS MSH1 tunnel fixture");
  shell_write_line(state, "legacy compatibility         done    /ws-shell and shell-json-v1 preserved");
}

int handle_release(ShellContext& ctx) {
  const bool json = ctx.json_output || (ctx.args.size() >= 3U && ctx.args[2] == "--json");
  if (json) {
    write_json(ctx, release_manifest_json());
    return 0;
  }
  print_release_manifest_text(ctx.state);
  return 0;
}

int handle_alias(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage:");
    shell_write_line(ctx.state, "  mshell alias list");
    shell_write_line(ctx.state, "  mshell alias add \"alias=command fragment\"");
    shell_write_line(ctx.state, "  mshell alias remove \"alias\"");
    shell_write_line(ctx.state, "  mshell alias save");
    shell_write_line(ctx.state, "  mshell alias reload");
    return ctx.args.size() < 3U ? 1 : 0;
  }

  const std::string& sub = ctx.args[2];
  if (sub == "list") {
    const std::vector<ShellAliasRecord>& aliases = shell_aliases();
    if (aliases.empty()) {
      shell_write_line(ctx.state, "(no aliases)");
      return 0;
    }
    shell_write_line(ctx.state, "alias                 command");
    shell_write_line(ctx.state, "--------------------  ----------------------------------------");
    for (const ShellAliasRecord& alias : aliases) {
      shell_printf(ctx.state, "%-20s  %s\n", alias.name.c_str(), alias.value.c_str());
    }
    return 0;
  }

  if (sub == "add") {
    if (ctx.args.size() < 4U) {
      shell_write_line(ctx.state, "mshell alias add: expected \"alias=command fragment\"");
      return 1;
    }
    const std::string& spec = ctx.args[3];
    const size_t eq = spec.find('=');
    if (eq == std::string::npos || eq == 0U || (eq + 1U) >= spec.size()) {
      shell_write_line(ctx.state, "mshell alias add: expected \"alias=command fragment\"");
      return 1;
    }
    const std::string name = spec.substr(0U, eq);
    std::string value = spec.substr(eq + 1U);
    for (size_t i = 4U; i < ctx.args.size(); ++i) {
      value += " ";
      value += ctx.args[i];
    }

    std::string error;
    if (!shell_alias_add(name, value, &error)) {
      shell_printf(ctx.state, "mshell alias add: %s\n", error.c_str());
      return 1;
    }
    shell_printf(ctx.state, "mshell alias: saved %s='%s'\n", name.c_str(), value.c_str());
    return 0;
  }

  if (sub == "remove" || sub == "rm") {
    if (ctx.args.size() < 4U) {
      shell_write_line(ctx.state, "mshell alias remove: alias name required");
      return 1;
    }
    std::string error;
    if (!shell_alias_remove(ctx.args[3], &error)) {
      shell_printf(ctx.state, "mshell alias remove: %s\n", error.c_str());
      return 1;
    }
    shell_printf(ctx.state, "mshell alias: removed %s\n", ctx.args[3].c_str());
    return 0;
  }

  if (sub == "save") {
    std::string error;
    if (!shell_alias_save(&error)) {
      shell_printf(ctx.state, "mshell alias save: %s\n", error.c_str());
      return 1;
    }
    shell_write_line(ctx.state, "mshell alias: saved");
    return 0;
  }

  if (sub == "reload") {
    std::string error;
    if (!shell_alias_reload(&error)) {
      shell_printf(ctx.state, "mshell alias reload: %s\n", error.c_str());
      return 1;
    }
    shell_printf(ctx.state, "mshell alias: reloaded %u aliases\n",
                 static_cast<unsigned>(shell_aliases().size()));
    return 0;
  }

  shell_printf(ctx.state, "mshell alias: unknown command '%s'\n", sub.c_str());
  return 1;
}

int handle_script(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage:");
    shell_write_line(ctx.state, "  mshell script list");
    shell_write_line(ctx.state, "  mshell script save NAME \"cmd1; cmd2\"");
    shell_write_line(ctx.state, "  mshell script run NAME [--keep-going]");
    shell_write_line(ctx.state, "  mshell profile safe|debug");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  const std::string& sub = ctx.args[2];
  if (sub == "list") {
    std::vector<ShellFsEntry> entries;
    std::string error;
    if (!shell_read_directory(ctx.state, scripts_dir(ctx.state), false, false, &entries, &error)) {
      shell_write_line(ctx.state, "(no scripts)");
      return 0;
    }
    shell_write_line(ctx.state, "script");
    shell_write_line(ctx.state, "--------------------");
    for (const ShellFsEntry& entry : entries) {
      if (!entry.is_dir) shell_write_line(ctx.state, entry.name.c_str());
    }
    return 0;
  }
  if (sub == "save") {
    if (ctx.args.size() < 5U || !safe_script_name(ctx.args[3])) {
      shell_write_line(ctx.state, "mshell script save: expected NAME and command text");
      return 1;
    }
    mkdir(scripts_dir(ctx.state).c_str(), 0775);
    const std::string body = join_args(ctx.args, 4U);
    FILE* file = std::fopen(script_path(ctx.state, ctx.args[3]).c_str(), "wb");
    if (file == nullptr) {
      shell_write_line(ctx.state, "mshell script save: cannot open file");
      return 1;
    }
    std::string expanded;
    size_t start = 0U;
    while (start <= body.size()) {
      const size_t semi = body.find(';', start);
      const bool has = semi != std::string::npos;
      std::string line = body.substr(start, has ? semi - start : body.size() - start);
      expanded += line;
      expanded += "\n";
      if (!has) break;
      start = semi + 1U;
    }
    const bool ok = std::fwrite(expanded.data(), 1U, expanded.size(), file) == expanded.size();
    std::fclose(file);
    shell_printf(ctx.state, "mshell script: %s %s\n", ok ? "saved" : "failed", ctx.args[3].c_str());
    return ok ? 0 : 1;
  }
  if (sub == "run") {
    if (ctx.args.size() < 4U || !safe_script_name(ctx.args[3])) {
      shell_write_line(ctx.state, "mshell script run: script name required");
      return 1;
    }
    bool keep_going = false;
    for (size_t i = 4U; i < ctx.args.size(); ++i) {
      if (ctx.args[i] == "-k" || ctx.args[i] == "--keep-going") {
        keep_going = true;
        continue;
      }
      shell_printf(ctx.state, "mshell script run: unknown option '%s'\n", ctx.args[i].c_str());
      return 1;
    }
    const std::string cmd = std::string("source ") + (keep_going ? "-k " : "") +
                            "/ESPUSER/scripts/" + ctx.args[3] + ".msh";
    return execute_line(cmd.c_str(), false, ctx.transport) ? 0 : 1;
  }
  shell_printf(ctx.state, "mshell script: unknown command '%s'\n", sub.c_str());
  return 1;
}

int handle_profile(ShellContext& ctx) {
  if (ctx.args.size() < 3U) {
    shell_write_line(ctx.state, "Usage: mshell profile <safe|debug>");
    return 1;
  }
  if (ctx.args[2] == "safe") {
    shell_write_line(ctx.state, "profile safe:");
    shell_write_line(ctx.state, "  mros health");
    shell_write_line(ctx.state, "  mros bus summary");
    shell_write_line(ctx.state, "  mros alerts");
    execute_line("mros health", false, ctx.transport);
    execute_line("mros bus summary", false, ctx.transport);
    return execute_line("mros alerts", false, ctx.transport) ? 0 : 2;
  }
  if (ctx.args[2] == "debug") {
    shell_write_line(ctx.state, "profile debug:");
    execute_line("mfetch --full", false, ctx.transport);
    execute_line("mros connections status", false, ctx.transport);
    execute_line("mros spi status all", false, ctx.transport);
    execute_line("mros uart status", false, ctx.transport);
    return execute_line("mros i2c status", false, ctx.transport) ? 0 : 2;
  }
  shell_printf(ctx.state, "mshell profile: unknown profile '%s'\n", ctx.args[2].c_str());
  return 1;
}

int handle_remote_action(ShellContext& ctx, const char* verb) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_printf(ctx.state, "Usage: mshell %s <s3|t41|all>\n", verb);
    return (ctx.args.size() < 3U) ? 1 : 0;
  }

  remote::Target target = remote::Target::None;
  if (!remote::parse_target(ctx.args[2], &target) || target == remote::Target::None) {
    shell_printf(ctx.state, "mshell %s: unknown target '%s'\n", verb, ctx.args[2].c_str());
    return 1;
  }
  if (target == remote::Target::C3) {
    shell_printf(ctx.state, "mshell %s: c3 topology is disabled in this system. Use s3 or t41.\n", verb);
    return 1;
  }

  if (std::strcmp(verb, "status") == 0) {
    const std::string text = remote::status_report(target, &ctx.state);
    shell_write(ctx.state, text.c_str());
    return 0;
  }

  std::string message;
  const bool ok = (std::strcmp(verb, "connect") == 0)
                      ? remote::connect_session(ctx.state, target, &message)
                      : remote::disconnect_session(ctx.state, target, &message);
  if (!message.empty()) {
    shell_write_line(ctx.state, message.c_str());
  }
  return ok ? 0 : 1;
}

std::string trim_copy(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  size_t start = 0U;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
  if (start > 0U) text.erase(0U, start);
  return text;
}

void print_api_table(ShellState& state) {
  shell_write_line(state, "api                         method  caps       command");
  shell_write_line(state, "--------------------------  ------  ---------  ----------------------------------------");
  for (const ApiSpec& spec : kApiSpecs) {
    shell_printf(
        state,
        "%-26s  %-6s  %-9s  %s\n",
        spec.name,
        spec.method,
        capabilities_text(spec.capabilities),
        spec.command);
  }
}

int handle_schema(ShellContext& ctx) {
  const bool want_json = ctx.json_output ||
                         (ctx.args.size() >= 4U && (ctx.args[3] == "--json" || ctx.args[2] == "--json"));
  const std::string subject = ctx.args.size() >= 3U ? lower_copy(ctx.args[2]) : "api";
  if (subject == "api" || subject == "openapi-lite") {
    if (!want_json) {
      if (subject == "openapi-lite") {
        shell_write_line(ctx.state, "Use --json to export openapi-lite schema.");
        return 0;
      }
      print_api_table(ctx.state);
      return 0;
    }
    std::string json = "{\"ok\":true,\"shell\":\"";
    json += kShellName;
    json += "\",\"version\":\"";
    json += kShellVersion;
    json += subject == "openapi-lite" ? "\",\"openapi\":\"3.0.0\",\"paths\":{" : "\",\"apis\":[";
    for (size_t i = 0U; i < kApiSpecCount; ++i) {
      const ApiSpec& spec = kApiSpecs[i];
      if (i > 0U) json += ",";
      if (subject == "openapi-lite") {
        json += "\"/";
        std::string path = spec.name;
        std::replace(path.begin(), path.end(), '.', '/');
        json += path;
        json += "\":{\"method\":\"";
        json += spec.method;
        json += "\",\"operationId\":\"";
        json += spec.name;
        json += "\",\"summary\":\"";
        json += json_escape(spec.summary);
        json += "\",\"x-mshell-command\":\"";
        json += json_escape(spec.command);
        json += "\",\"x-capabilities\":\"";
        json += capabilities_text(spec.capabilities);
        json += "\",\"x-risk\":\"";
        json += risk_for_caps(spec.capabilities);
        json += "\"}";
      } else {
        json += "{\"name\":\"";
        json += spec.name;
        json += "\",\"method\":\"";
        json += spec.method;
        json += "\",\"capabilities\":\"";
        json += capabilities_text(spec.capabilities);
        json += "\",\"risk\":\"";
        json += risk_for_caps(spec.capabilities);
        json += "\",\"command\":\"";
        json += json_escape(spec.command);
        json += "\",\"summary\":\"";
        json += json_escape(spec.summary);
        json += "\"}";
      }
    }
    json += subject == "openapi-lite" ? "}}" : "]}";
    write_json(ctx, json);
    return 0;
  }
  if (subject == "commands") {
    if (!want_json) {
      shell_write_line(ctx.state, "command                  caps       summary");
      shell_write_line(ctx.state, "-----------------------  ---------  ----------------------------------------");
      for (const ShellCommandRegistration& command : shell_commands()) {
        shell_printf(
            ctx.state,
            "%-23s  %-9s  %s\n",
            command.name != nullptr ? command.name : "-",
            capabilities_text(command.capabilities),
            command.summary != nullptr ? command.summary : "-");
      }
      return 0;
    }
    std::string json = "{\"ok\":true,\"shell\":\"";
    json += kShellName;
    json += "\",\"version\":\"";
    json += kShellVersion;
    json += "\",\"commands\":[";
    const auto& commands = shell_commands();
    for (size_t i = 0U; i < commands.size(); ++i) {
      const ShellCommandRegistration& command = commands[i];
      if (i > 0U) json += ",";
      json += "{\"name\":\"";
      json += json_escape(command.name != nullptr ? command.name : "");
      json += "\",\"capabilities\":\"";
      json += capabilities_text(command.capabilities);
      json += "\",\"group\":\"";
      json += json_escape(command.group != nullptr ? command.group : group_for_command(command.name, command.capabilities));
      json += "\",\"risk\":\"";
      json += json_escape(command.risk != nullptr ? command.risk : risk_for_caps(command.capabilities));
      json += "\",\"json\":";
      json += command.supports_json ? "true" : "false";
      json += ",\"job\":";
      json += command.supports_job ? "true" : "false";
      json += ",\"summary\":\"";
      json += json_escape(command.summary != nullptr ? command.summary : "");
      json += "\"}";
    }
    json += "]}";
    write_json(ctx, json);
    return 0;
  }
  shell_write_line(ctx.state, "Usage: mshell schema api|commands|openapi-lite [--json]");
  return 1;
}

int handle_call(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage:");
    shell_write_line(ctx.state, "  mshell call <api.name> [key=value...] [--target s3|t41] [--json|--text] [--dry-run]");
    shell_write_line(ctx.state, "  mshell call GET /files/list path=/ESPUSER --json");
    shell_write_line(ctx.state, "Examples:");
    shell_write_line(ctx.state, "  mshell call system.status");
    shell_write_line(ctx.state, "  mshell call files.list path=/t41-sdcard");
    shell_write_line(ctx.state, "  mshell call robot.ik x=300 y=300 z=250");
    return ctx.args.size() < 3U ? 1 : 0;
  }

  size_t index = 2U;
  std::string http_method = "CALL";
  std::string api_name;
  const std::string first = lower_copy(ctx.args[index]);
  if ((first == "get" || first == "post" || first == "put" || first == "delete") && (index + 1U) < ctx.args.size()) {
    http_method = ctx.args[index];
    std::transform(http_method.begin(), http_method.end(), http_method.begin(), [](unsigned char ch) {
      return static_cast<char>(std::toupper(ch));
    });
    api_name = path_to_api_name(ctx.args[index + 1U]);
    index += 2U;
  } else {
    api_name = path_to_api_name(ctx.args[index]);
    ++index;
  }

  std::vector<ApiParam> params;
  remote::Target target = remote::Target::S3;
  bool dry_run = false;
  bool text_mode = false;
  for (; index < ctx.args.size(); ++index) {
    const std::string& arg = ctx.args[index];
    if (arg == "--dry-run") {
      dry_run = true;
      continue;
    }
    if (arg == "--text") {
      text_mode = true;
      continue;
    }
    if (arg == "--json") {
      text_mode = false;
      continue;
    }
    if (arg == "--target" && (index + 1U) < ctx.args.size()) {
      if (!remote::parse_target(ctx.args[index + 1U], &target) ||
          target == remote::Target::None ||
          target == remote::Target::All ||
          target == remote::Target::C3) {
        return write_call_error(ctx, api_name.c_str(), "INVALID_ARGUMENT", "invalid target");
      }
      ++index;
      continue;
    }
    const size_t eq = arg.find('=');
    if (eq != std::string::npos && eq > 0U) {
      params.push_back({lower_copy(arg.substr(0U, eq)), arg.substr(eq + 1U)});
      continue;
    }
    return write_call_error(ctx, api_name.c_str(), "INVALID_ARGUMENT", "expected key=value or flag");
  }

  const ApiSpec* spec = find_api_spec(api_name);
  if (spec == nullptr) {
    return write_call_error(ctx, api_name.c_str(), "INVALID_ARGUMENT", "unknown api (try: mshell schema api)");
  }
  if ((ctx.state.capability_mask & spec->capabilities) != spec->capabilities) {
    return write_call_error(ctx, spec->name, "CAPABILITY_DENIED", capabilities_text(spec->capabilities), target);
  }

  const uint32_t request_id = ++g_api_request_seq;
  const std::string command = render_template(spec->command, params);
  if (dry_run) {
    std::string json = "{\"ok\":true,\"dry_run\":true,\"api\":\"";
    json += spec->name;
    json += "\",\"request_id\":";
    json += std::to_string(request_id);
    json += ",\"method\":\"";
    json += http_method;
    json += "\",\"target\":\"";
    json += remote::target_name(target);
    json += "\",\"command\":\"";
    json += json_escape(command);
    json += "\"}";
    write_json(ctx, json);
    return 0;
  }

  const uint32_t started = mros::platform::mros_millis();
  std::string output;
  std::string error;
  bool ok = false;
  const remote::Target previous_target = remote::active_target(ctx.state);
  if (target == remote::Target::S3) {
    ok = execute_line_capture(command.c_str(), &output, false, ctx.transport);
  } else {
    std::string message;
    ok = remote::connect_session(ctx.state, target, &message);
    if (ok) {
      ok = remote::execute_active_line(ctx.state, command, &output, &error);
    } else {
      error = message;
    }
    if (previous_target == remote::Target::None) {
      remote::clear_active_target(ctx.state);
    } else {
      remote::set_active_target(ctx.state, previous_target);
    }
  }
  const uint32_t elapsed = mros::platform::mros_millis() - started;
  output = trim_copy(output);
  error = trim_copy(error);
  if (text_mode) {
    if (!output.empty()) shell_write_line(ctx.state, output.c_str());
    if (!ok && !error.empty()) shell_write_line(ctx.state, error.c_str());
    return ok ? 0 : 1;
  }

  const bool raw_json_output = !output.empty() && (output.front() == '{' || output.front() == '[');
  std::string json = "{\"ok\":";
  json += ok ? "true" : "false";
  json += ",\"shell\":\"";
  json += kShellName;
  json += "\",\"version\":\"";
  json += kShellVersion;
  json += "\",\"request_id\":";
  json += std::to_string(request_id);
  json += ",\"api\":\"";
  json += spec->name;
  json += "\",\"method\":\"";
  json += http_method;
  json += "\",\"target\":\"";
  json += remote::target_name(target);
  json += "\",\"command\":\"";
  json += json_escape(command);
  json += "\",\"duration_ms\":";
  json += std::to_string(elapsed);
  json += ",\"error_code\":\"";
  json += ok ? "OK" : (target != remote::Target::S3 ? "PEER_PROTOCOL_MISSING" : "COMMAND_FAILED");
  json += "\"";
  if (raw_json_output) {
    json += ",\"data\":";
    json += output;
  } else {
    json += ",\"output\":\"";
    json += json_escape(output);
    json += "\"";
  }
  if (!error.empty()) {
    json += ",\"error\":\"";
    json += json_escape(error);
    json += "\"";
  }
  json += "}";
  write_json(ctx, json);
  return ok ? 0 : 1;
}

int handle_doctor(ShellContext& ctx) {
  const bool json = ctx.json_output || (ctx.args.size() >= 3U && ctx.args[2] == "--json");
  const bool storage = shell_is_storage_mounted(ctx.state);
  const uint32_t sessions = active_session_count();
  const uint32_t roots = active_root_session_count();
  remote::FsMountSnapshot t41 {};
  remote::FsMountSnapshot t41sd {};
  remote::fs_snapshot(remote::FsMount::T41, &t41);
  remote::fs_snapshot(remote::FsMount::T41Sdcard, &t41sd);
  if (json) {
    std::string out = "{\"ok\":true,\"shell\":\"";
    out += kShellName;
    out += "\",\"version\":\"";
    out += kShellVersion;
    out += "\",\"release_line\":\"";
    out += kShellReleaseLine;
    out += "\",\"release_status\":\"";
    out += kShellReleaseStatus;
    out += "\",\"storage_mounted\":";
    out += storage ? "true" : "false";
    out += ",\"sessions\":";
    out += std::to_string(sessions);
    out += ",\"session_capacity\":";
    out += std::to_string(session_capacity());
    out += ",\"root_sessions\":";
    out += std::to_string(roots);
    out += ",\"uart_bridge\":\"";
    out += remote::bridge_mode_name(remote::bridge_mode());
    out += "\",\"t41_fs\":\"";
    out += t41.error_code;
    out += "\",\"t41_sdcard\":\"";
    out += t41sd.error_code;
    out += "\"}";
    write_json(ctx, out);
    return 0;
  }
  shell_write_line(ctx.state, "check                    state      detail");
  shell_write_line(ctx.state, "-----------------------  ---------  ----------------------------------------");
  shell_printf(ctx.state, "%-23s  %-9s  %s\n", "version", "ok", kShellVersion);
  shell_printf(ctx.state, "%-23s  %-9s  %s %s\n", "release", "ok", kShellReleaseLine, kShellReleaseStatus);
  shell_printf(ctx.state, "%-23s  %-9s  %s\n", "storage", storage ? "ok" : "warn", storage ? "/fs ready" : "LittleFS not mounted");
  shell_printf(ctx.state, "%-23s  %-9s  %lu/%lu sessions\n", "sessions", sessions < session_capacity() ? "ok" : "full",
               static_cast<unsigned long>(sessions), static_cast<unsigned long>(session_capacity()));
  shell_printf(ctx.state, "%-23s  %-9s  %lu root sessions\n", "root", roots <= 1U ? "ok" : "warn", static_cast<unsigned long>(roots));
  shell_printf(ctx.state, "%-23s  %-9s  mode=%s\n", "uart bridge", remote::bridge_mode() == remote::BridgeMode::On ? "ok" : "off",
               remote::bridge_mode_name(remote::bridge_mode()));
  shell_printf(ctx.state, "%-23s  %-9s  %s\n", "t41 fs", t41.mounted ? "ok" : "info", t41.error_code);
  shell_printf(ctx.state, "%-23s  %-9s  %s\n", "t41 sdcard", t41sd.mounted ? "ok" : "info", t41sd.error_code);
  return 0;
}

int handle_perf(ShellContext& ctx) {
  const bool json = ctx.json_output || (ctx.args.size() >= 3U && ctx.args[2] == "--json");
  if (json) {
    std::string out = "{\"ok\":true,\"shell\":\"";
    out += kShellName;
    out += "\",\"version\":\"";
    out += kShellVersion;
    out += "\",\"release_line\":\"";
    out += kShellReleaseLine;
    out += "\",\"release_status\":\"";
    out += kShellReleaseStatus;
    out += "\",\"commands\":";
    out += std::to_string(shell_commands().size());
    out += ",\"apis\":";
    out += std::to_string(kApiSpecCount);
    out += ",\"sessions\":";
    out += std::to_string(active_session_count());
    out += ",\"capacity\":";
    out += std::to_string(session_capacity());
    out += ",\"jobs_active\":";
    out += std::to_string(runtime::job_active_count());
    out += ",\"jobs_capacity\":";
    out += std::to_string(runtime::job_capacity());
    out += ",\"jobs_completed\":";
    out += std::to_string(runtime::job_completed_count());
    out += ",\"job_drops\":";
    out += std::to_string(runtime::job_drop_count());
    out += ",\"job_storage_bytes\":";
    out += std::to_string(runtime::job_storage_bytes());
    out += ",\"job_storage_allocated\":";
    out += runtime::job_storage_allocated() ? "true" : "false";
    out += ",\"job_storage_psram\":";
    out += runtime::job_storage_uses_psram() ? "true" : "false";
    out += ",\"tx_active\":";
    out += runtime::tx_active() ? "true" : "false";
    out += ",\"terminal_columns\":";
    out += std::to_string(shell_terminal_columns(ctx.state));
    out += "}";
    write_json(ctx, out);
    return 0;
  }
  shell_write_line(ctx.state, "metric                  value");
  shell_write_line(ctx.state, "----------------------  ----------------");
  shell_printf(ctx.state, "%-22s  %s\n", "version", kShellVersion);
  shell_printf(ctx.state, "%-22s  %s %s\n", "release", kShellReleaseLine, kShellReleaseStatus);
  shell_printf(ctx.state, "%-22s  %u\n", "commands", static_cast<unsigned>(shell_commands().size()));
  shell_printf(ctx.state, "%-22s  %u\n", "api endpoints", static_cast<unsigned>(kApiSpecCount));
  shell_printf(ctx.state, "%-22s  %lu/%lu\n", "sessions", static_cast<unsigned long>(active_session_count()),
               static_cast<unsigned long>(session_capacity()));
  shell_printf(ctx.state, "%-22s  %lu/%lu active, completed=%lu drops=%lu\n", "jobs",
               static_cast<unsigned long>(runtime::job_active_count()),
               static_cast<unsigned long>(runtime::job_capacity()),
               static_cast<unsigned long>(runtime::job_completed_count()),
               static_cast<unsigned long>(runtime::job_drop_count()));
  shell_printf(ctx.state, "%-22s  %lu bytes, %s\n", "job storage",
               static_cast<unsigned long>(runtime::job_storage_bytes()),
               runtime::job_storage_allocated()
                   ? (runtime::job_storage_uses_psram() ? "PSRAM" : "internal fallback")
                   : "lazy");
  shell_printf(ctx.state, "%-22s  %s\n", "transaction", runtime::tx_active() ? "active" : "idle");
  shell_printf(ctx.state, "%-22s  %u cols\n", "terminal width", static_cast<unsigned>(shell_terminal_columns(ctx.state)));
  shell_printf(ctx.state, "%-22s  %s\n", "uart bridge", remote::bridge_mode_name(remote::bridge_mode()));
  return 0;
}

int handle_job(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage:");
    shell_write_line(ctx.state, "  mshell job start \"command\"");
    shell_write_line(ctx.state, "  mshell job list");
    shell_write_line(ctx.state, "  mshell job status");
    shell_write_line(ctx.state, "  mshell job log <id>");
    shell_write_line(ctx.state, "  mshell job cancel <id>");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  const std::string sub = lower_copy(ctx.args[2]);
  char json[4096] = {};
  if (sub == "list" || sub == "status") {
    if (!runtime::jobs_json(json, sizeof(json))) {
      shell_write_line(ctx.state, "{\"ok\":false,\"error_code\":\"TRUNCATED\"}");
      return 1;
    }
    if (ctx.json_output || sub == "status") {
      write_json(ctx, json);
    } else {
      shell_write_line(ctx.state, "id    state      duration  owner       command");
      shell_write_line(ctx.state, "----  ---------  --------  ----------  --------------------------------");
      shell_write_line(ctx.state, "Use --json for structured job details.");
    }
    return 0;
  }
  if (sub == "start") {
    if (ctx.args.size() < 4U) {
      shell_write_line(ctx.state, "mshell job start: command required");
      return 1;
    }
    const std::string command = join_args(ctx.args, 3U);
    uint32_t id = 0U;
    char error[96] = {};
    if (!runtime::job_start(command.c_str(), ctx.state.capability_mask, ctx.state.session_username.c_str(), &id, error, sizeof(error))) {
      shell_printf(ctx.state, "mshell job start: %s\n", error);
      return 1;
    }
    if (ctx.json_output) {
      std::string out = "{\"ok\":true,\"job_id\":";
      out += std::to_string(id);
      out += ",\"state\":\"queued\"}";
      write_json(ctx, out);
    } else {
      shell_printf(ctx.state, "mshell job: queued id=%lu\n", static_cast<unsigned long>(id));
    }
    return 0;
  }
  if (sub == "log") {
    if (ctx.args.size() < 4U) {
      shell_write_line(ctx.state, "mshell job log: id required");
      return 1;
    }
    const uint32_t id = static_cast<uint32_t>(std::strtoul(ctx.args[3].c_str(), nullptr, 10));
    if (!runtime::job_log_json(id, json, sizeof(json))) {
      shell_write_line(ctx.state, "{\"ok\":false,\"error_code\":\"TRUNCATED\"}");
      return 1;
    }
    write_json(ctx, json);
    return 0;
  }
  if (sub == "cancel") {
    if (ctx.args.size() < 4U) {
      shell_write_line(ctx.state, "mshell job cancel: id required");
      return 1;
    }
    const uint32_t id = static_cast<uint32_t>(std::strtoul(ctx.args[3].c_str(), nullptr, 10));
    char error[96] = {};
    const bool ok = runtime::job_cancel(id, error, sizeof(error));
    if (ctx.json_output) {
      std::string out = "{\"ok\":";
      out += ok ? "true" : "false";
      out += ",\"job_id\":";
      out += std::to_string(id);
      out += ",\"error_code\":\"";
      out += error;
      out += "\"}";
      write_json(ctx, out);
    } else {
      shell_printf(ctx.state, "mshell job cancel: %s\n", error);
    }
    return ok ? 0 : 1;
  }
  shell_printf(ctx.state, "mshell job: unknown command '%s'\n", sub.c_str());
  return 1;
}

int handle_tx(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mshell tx begin|status|stage|commit|rollback");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  const std::string sub = lower_copy(ctx.args[2]);
  char error[96] = {};
  if (sub == "status") {
    char json[512] = {};
    if (!runtime::tx_json(json, sizeof(json))) return 1;
    write_json(ctx, json);
    return 0;
  }
  if (sub == "begin") {
    uint32_t id = 0U;
    const bool ok = runtime::tx_begin(ctx.state.session_username.c_str(), &id, error, sizeof(error));
    shell_printf(ctx.state, "mshell tx begin: %s id=%lu\n", error, static_cast<unsigned long>(id));
    return ok ? 0 : 1;
  }
  if (sub == "stage") {
    const std::string note = ctx.args.size() >= 4U ? join_args(ctx.args, 3U) : "";
    const bool ok = runtime::tx_stage(note.c_str(), error, sizeof(error));
    shell_printf(ctx.state, "mshell tx stage: %s\n", error);
    return ok ? 0 : 1;
  }
  if (sub == "commit") {
    const bool ok = runtime::tx_commit(error, sizeof(error));
    shell_printf(ctx.state, "mshell tx commit: %s\n", error);
    return ok ? 0 : 1;
  }
  if (sub == "rollback") {
    const bool ok = runtime::tx_rollback(error, sizeof(error));
    shell_printf(ctx.state, "mshell tx rollback: %s\n", error);
    return ok ? 0 : 1;
  }
  shell_printf(ctx.state, "mshell tx: unknown command '%s'\n", sub.c_str());
  return 1;
}

int handle_explain(ShellContext& ctx) {
  if (ctx.args.size() < 3U || is_help_arg(ctx.args[2])) {
    shell_write_line(ctx.state, "Usage: mshell explain <ERROR_CODE>");
    return ctx.args.size() < 3U ? 1 : 0;
  }
  const std::string code = lower_copy(ctx.args[2]);
  if (code == "bridge_disabled") {
    shell_write_line(ctx.state, "BRIDGE_DISABLED: UART shell bridge is off. Use: set uart-shell-bridge on");
  } else if (code == "peer_protocol_missing") {
    shell_write_line(ctx.state, "PEER_PROTOCOL_MISSING: peer is reachable only after matching MSHELL2/FS firmware is installed.");
  } else if (code == "remote_not_mounted") {
    shell_write_line(ctx.state, "REMOTE_NOT_MOUNTED: run mount t41 or mount t41-sdcard before remote file operations.");
  } else if (code == "capability_denied") {
    shell_write_line(ctx.state, "CAPABILITY_DENIED: current user/session lacks the command capability. Use su/sudo or adjust roles.");
  } else if (code == "command_failed") {
    shell_write_line(ctx.state, "COMMAND_FAILED: wrapped command returned non-zero. Re-run with --text for raw output.");
  } else {
    shell_write_line(ctx.state, "Unknown code. Useful codes: BRIDGE_DISABLED, PEER_PROTOCOL_MISSING, REMOTE_NOT_MOUNTED, CAPABILITY_DENIED, COMMAND_FAILED");
  }
  return 0;
}

}  // namespace

void shell_help_mshell(ShellState& state) {
  shell_write_line(state, "Usage: mshell <command> [args]");
  shell_write_line(state, "Manage mshell runtime features.");
  shell_write_line(state, "");
  shell_write_line(state, "Alias management:");
  shell_write_line(state, "  mshell alias list");
  shell_write_line(state, "  mshell alias add \"alias=command fragment\"");
  shell_write_line(state, "  mshell alias remove \"alias\"");
  shell_write_line(state, "  mshell alias reload");
  shell_write_line(state, "");
  shell_write_line(state, "Feature status:");
  shell_write_line(state, "  mshell version");
  shell_write_line(state, "  mshell release [--json]");
  shell_write_line(state, "  mshell schema api|commands|openapi-lite [--json]");
  shell_write_line(state, "  mshell call <api.name> [key=value...] [--target s3|t41] [--json|--text]");
  shell_write_line(state, "  mshell job start|list|status|log|cancel");
  shell_write_line(state, "  mshell tx begin|status|stage|commit|rollback");
  shell_write_line(state, "  mshell doctor [--json]");
  shell_write_line(state, "  mshell perf [--json]");
  shell_write_line(state, "  mshell explain <ERROR_CODE>");
  shell_write_line(state, "  mshell features");
  shell_write_line(state, "  mshell list devices");
  shell_write_line(state, "  mshell connect <s3|t41|all>");
  shell_write_line(state, "  mshell disconnect <s3|t41|all>");
  shell_write_line(state, "  mshell status <s3|t41|all>");
  shell_write_line(state, "  mshell history status");
  shell_write_line(state, "  mshell jobs list");
  shell_write_line(state, "  mshell script list|save|run");
  shell_write_line(state, "  mshell profile safe|debug");
}

int shell_cmd_mshell(ShellContext& ctx) {
  if (ctx.args.size() < 2U || is_help_arg(ctx.args[1])) {
    shell_help_mshell(ctx.state);
    return ctx.args.size() < 2U ? 1 : 0;
  }

  const std::string& top = ctx.args[1];
  if (top == "version" || top == "--version" || top == "-V") {
    if (ctx.json_output) {
      std::string json = "{\"ok\":true,\"shell\":\"";
      json += kShellName;
      json += "\",\"version\":\"";
      json += kShellVersion;
      json += "\",\"release_line\":\"";
      json += kShellReleaseLine;
      json += "\",\"release_status\":\"";
      json += kShellReleaseStatus;
      json += "\",\"protocol\":\"";
      json += kShellProtocolName;
      json += "\"}";
      write_json(ctx, json);
    } else {
      shell_printf(ctx.state, "%s %s\n", kShellName, kShellVersion);
    }
    return 0;
  }
  if (top == "release") {
    return handle_release(ctx);
  }
  if (top == "schema") {
    return handle_schema(ctx);
  }
  if (top == "call" || top == "api") {
    return handle_call(ctx);
  }
  if (top == "job" || top == "jobs") {
    return handle_job(ctx);
  }
  if (top == "tx") {
    return handle_tx(ctx);
  }
  if (top == "doctor") {
    return handle_doctor(ctx);
  }
  if (top == "perf") {
    return handle_perf(ctx);
  }
  if (top == "explain") {
    return handle_explain(ctx);
  }
  if (top == "list" && ctx.args.size() >= 3U && ctx.args[2] == "devices") {
    const std::string text = remote::devices_report(&ctx.state);
    shell_write(ctx.state, text.c_str());
    return 0;
  }
  if (top == "alias") {
    return handle_alias(ctx);
  }
  if (top == "script") {
    return handle_script(ctx);
  }
  if (top == "profile") {
    return handle_profile(ctx);
  }
  if (top == "connect" || top == "disconnect") {
    return handle_remote_action(ctx, top.c_str());
  }
  if (top == "status") {
    if (ctx.args.size() >= 3U) {
      return handle_remote_action(ctx, "status");
    }
    const std::string text = remote::status_report(remote::Target::All, &ctx.state);
    shell_write(ctx.state, text.c_str());
    return 0;
  }
  if (top == "features") {
    print_feature_table(ctx.state);
    return 0;
  }
  if (top == "history" && ctx.args.size() >= 3U && ctx.args[2] == "status") {
    shell_write_line(ctx.state, "history scope       : current shell session");
    shell_write_line(ctx.state, "web console history : browser-side + shell session");
    shell_write_line(ctx.state, "persistent file     : not enabled yet");
    return 0;
  }
  shell_printf(ctx.state, "mshell: unknown command '%s'\n", top.c_str());
  return 1;
}

}  // namespace mros::shell
