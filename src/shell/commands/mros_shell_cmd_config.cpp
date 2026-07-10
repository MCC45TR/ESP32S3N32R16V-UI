#include "src/shell/mros_shell_internal.h"

#include <sys/stat.h>

#include <cstdio>
#include <string>
#include <vector>

namespace mros::shell {
namespace {

struct ConfigItem {
  std::string key;
  std::string value;
};

std::string config_path(const ShellState& state) {
  return shell_storage_user_root(state) + "/mshell_config.txt";
}

void ensure_user_root(const ShellState& state) {
  mkdir(shell_storage_user_root(state).c_str(), 0775);
}

std::string trim(std::string text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
    text.erase(text.begin());
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
    text.pop_back();
  }
  return text;
}

std::vector<ConfigItem> load_config(const ShellState& state) {
  std::vector<ConfigItem> items;
  FILE* file = std::fopen(config_path(state).c_str(), "rb");
  if (file == nullptr) return items;
  char line[256] = {};
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    std::string raw = trim(line);
    if (raw.empty() || raw.front() == '#') continue;
    const size_t eq = raw.find('=');
    if (eq == std::string::npos) continue;
    items.push_back({trim(raw.substr(0U, eq)), trim(raw.substr(eq + 1U))});
  }
  std::fclose(file);
  return items;
}

bool save_config(const ShellState& state, const std::vector<ConfigItem>& items) {
  if (!shell_is_storage_mounted(state)) return false;
  ensure_user_root(state);
  FILE* file = std::fopen(config_path(state).c_str(), "wb");
  if (file == nullptr) return false;
  for (const ConfigItem& item : items) {
    const std::string line = item.key + "=" + item.value + "\n";
    if (std::fwrite(line.data(), 1U, line.size(), file) != line.size()) {
      std::fclose(file);
      return false;
    }
  }
  std::fclose(file);
  return true;
}

std::string json_escape(const std::string& text) {
  std::string out;
  for (const char ch : text) {
    if (ch == '"' || ch == '\\') out.push_back('\\');
    if (ch == '\n') {
      out += "\\n";
    } else if (ch != '\r') {
      out.push_back(ch);
    }
  }
  return out;
}

}  // namespace

void shell_help_config(ShellState& state) {
  shell_write_line(state, "Usage: config <list|get|set|export|import|diff> [args]");
  shell_write_line(state, "Manage persistent mshell key/value settings under /ESPUSER.");
  shell_write_line(state, "  config list");
  shell_write_line(state, "  config get KEY");
  shell_write_line(state, "  config set KEY VALUE");
  shell_write_line(state, "  config export /ESPUSER/config.json");
  shell_write_line(state, "  config import /ESPUSER/config.json");
  shell_write_line(state, "  config diff factory");
}

int shell_cmd_config(ShellContext& ctx) {
  if (ctx.args.size() < 2U || ctx.args[1] == "--help" || ctx.args[1] == "-h") {
    shell_help_config(ctx.state);
    return ctx.args.size() < 2U ? 1 : 0;
  }

  const std::string& sub = ctx.args[1];
  std::vector<ConfigItem> items = load_config(ctx.state);

  if (sub == "list") {
    shell_write_line(ctx.state, "key                         value");
    shell_write_line(ctx.state, "--------------------------  ----------------------------------------");
    shell_printf(ctx.state, "%-26s  %s\n", "shell.output-mode", "text");
    shell_printf(ctx.state, "%-26s  %lu\n", "shell.cmd-timeout-ms", static_cast<unsigned long>(ctx.state.command_timeout_ms));
    for (const ConfigItem& item : items) {
      shell_printf(ctx.state, "%-26s  %s\n", item.key.c_str(), item.value.c_str());
    }
    return 0;
  }

  if (sub == "get") {
    if (ctx.args.size() < 3U) {
      shell_write_line(ctx.state, "config get: key required");
      return 1;
    }
    const std::string& key = ctx.args[2];
    if (key == "shell.output-mode") {
      shell_printf(ctx.state, "%s=%s\n", key.c_str(), "text");
      return 0;
    }
    if (key == "shell.cmd-timeout-ms") {
      shell_printf(ctx.state, "%s=%lu\n", key.c_str(), static_cast<unsigned long>(ctx.state.command_timeout_ms));
      return 0;
    }
    for (const ConfigItem& item : items) {
      if (item.key == key) {
        shell_printf(ctx.state, "%s=%s\n", item.key.c_str(), item.value.c_str());
        return 0;
      }
    }
    shell_write_line(ctx.state, "config get: key not found");
    return 1;
  }

  if (sub == "set") {
    if (ctx.args.size() < 4U) {
      shell_write_line(ctx.state, "config set: key and value required");
      return 1;
    }
    const std::string key = ctx.args[2];
    std::string value = ctx.args[3];
    for (size_t i = 4U; i < ctx.args.size(); ++i) {
      value += " ";
      value += ctx.args[i];
    }
    bool found = false;
    for (ConfigItem& item : items) {
      if (item.key == key) {
        item.value = value;
        found = true;
        break;
      }
    }
    if (!found) items.push_back({key, value});
    if (!save_config(ctx.state, items)) {
      shell_write_line(ctx.state, "config set: save failed");
      return 1;
    }
    shell_printf(ctx.state, "config: saved %s=%s\n", key.c_str(), value.c_str());
    return 0;
  }

  if (sub == "export") {
    if (ctx.args.size() < 3U) {
      shell_write_line(ctx.state, "config export: path required");
      return 1;
    }
    const std::string path = shell_normalize_path(ctx.state, ctx.args[2]);
    if (!shell_is_user_writable_path(ctx.state, path)) {
      shell_write_line(ctx.state, "config export: path must be inside /ESPUSER");
      return 1;
    }
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
      shell_write_line(ctx.state, "config export: cannot open file");
      return 1;
    }
    std::string json = "{\n";
    json += "  \"shell.output-mode\":\"";
    json += "text";
    json += "\",\n  \"shell.cmd-timeout-ms\":\"";
    json += std::to_string(ctx.state.command_timeout_ms);
    json += "\"";
    for (const ConfigItem& item : items) {
      json += ",\n  \"";
      json += json_escape(item.key);
      json += "\":\"";
      json += json_escape(item.value);
      json += "\"";
    }
    json += "\n}\n";
    const bool ok = std::fwrite(json.data(), 1U, json.size(), file) == json.size();
    std::fclose(file);
    shell_printf(ctx.state, "config export: %s %s\n", ok ? "wrote" : "failed", ctx.args[2].c_str());
    return ok ? 0 : 1;
  }

  if (sub == "import") {
    shell_write_line(ctx.state, "config import: staged; use config set for now");
    shell_write_line(ctx.state, "config import will parse JSON presets in the next firmware step.");
    return 2;
  }

  if (sub == "diff" && ctx.args.size() >= 3U && ctx.args[2] == "factory") {
    shell_write_line(ctx.state, "key                         factory           current");
    shell_write_line(ctx.state, "--------------------------  ----------------  ----------------");
    shell_printf(ctx.state, "%-26s  %-16s  %s\n", "shell.output-mode", "text", "text");
    shell_printf(ctx.state, "%-26s  %-16s  %lu\n", "shell.cmd-timeout-ms", "0", static_cast<unsigned long>(ctx.state.command_timeout_ms));
    for (const ConfigItem& item : items) {
      shell_printf(ctx.state, "%-26s  %-16s  %s\n", item.key.c_str(), "(unset)", item.value.c_str());
    }
    return 0;
  }

  shell_printf(ctx.state, "config: unknown command '%s'\n", sub.c_str());
  return 1;
}

}  // namespace mros::shell
