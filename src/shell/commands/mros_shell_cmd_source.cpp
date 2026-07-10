#include "src/shell/mros_shell_internal.h"

#include <cstdio>

#include <string>

namespace mros::shell {
namespace {

struct SourceOptions {
  bool verbose = false;
  bool keep_going = false;
  std::string path;
};

std::string trim_copy(const std::string& input) {
  size_t start = 0U;
  while (start < input.size() && (input[start] == ' ' || input[start] == '\t' || input[start] == '\r' || input[start] == '\n')) {
    ++start;
  }
  size_t end = input.size();
  while (end > start &&
         (input[end - 1U] == ' ' || input[end - 1U] == '\t' || input[end - 1U] == '\r' || input[end - 1U] == '\n')) {
    --end;
  }
  return input.substr(start, end - start);
}

bool parse_source_args(ShellContext& ctx, SourceOptions* options, bool* help_requested) {
  if (options == nullptr || help_requested == nullptr) {
    return false;
  }

  *help_requested = false;
  for (size_t i = 1U; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg == "--help" || arg == "-h") {
      *help_requested = true;
      return true;
    }
    if (arg == "-v" || arg == "--verbose") {
      options->verbose = true;
      continue;
    }
    if (arg == "-k" || arg == "--keep-going") {
      options->keep_going = true;
      continue;
    }
    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      shell_printf(ctx.state, "source: unknown option '%s'\n", arg.c_str());
      return false;
    }
    if (!options->path.empty()) {
      shell_printf(ctx.state, "source: extra operand '%s'\n", arg.c_str());
      return false;
    }
    options->path = arg;
  }

  if (options->path.empty()) {
    shell_write_line(ctx.state, "source: missing script file operand");
    return false;
  }
  return true;
}

int run_script_file(
    ShellState& state,
    const ShellTransport transport,
    const SourceOptions& options) {
  std::string actual_path;
  std::string error;
  if (!shell_openable_file_path(state, options.path, &actual_path, &error)) {
    shell_printf(state, "source: cannot open '%s': %s\n", options.path.c_str(), error.c_str());
    return 1;
  }

  FILE* file = std::fopen(actual_path.c_str(), "rb");
  if (file == nullptr) {
    shell_printf(state, "source: failed to open '%s'\n", options.path.c_str());
    return 1;
  }

  int result = 0;
  size_t line_no = 0U;
  size_t executed = 0U;
  size_t failed = 0U;
  char line_buffer[512] = {};
  while (std::fgets(line_buffer, sizeof(line_buffer), file) != nullptr) {
    ++line_no;
    const std::string line = trim_copy(line_buffer);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    if (options.verbose) {
      shell_printf(state, "source:%lu> %s\n", static_cast<unsigned long>(line_no), line.c_str());
    }
    ++executed;
    const bool ok = execute_line_on_state(state, line.c_str(), false, transport);
    if (!ok) {
      ++failed;
      result = 1;
      if (!options.keep_going) {
        break;
      }
    }
  }
  std::fclose(file);

  if (failed > 0U) {
    shell_printf(
        state,
        "source: %lu commands executed, %lu failed\n",
        static_cast<unsigned long>(executed),
        static_cast<unsigned long>(failed));
  }
  return result;
}

}  // namespace

void shell_help_source(ShellState& state) {
  shell_write_line(state, "Usage: source [OPTION]... FILE");
  shell_write_line(state, "Run commands from FILE line by line.");
  shell_write_line(state, "  -v, --verbose              print each script line before execution");
  shell_write_line(state, "  -k, --keep-going           continue when a line fails");
}

void shell_help_sh(ShellState& state) {
  shell_write_line(state, "Usage: sh [OPTION]... FILE");
  shell_write_line(state, "Run a shell script file with the mshell script interpreter.");
  shell_write_line(state, "Examples:");
  shell_write_line(state, "  sh ./script_name.sh");
  shell_write_line(state, "  sh -k /ESPUSER/scripts/snc-test.msh");
  shell_write_line(state, "Options:");
  shell_write_line(state, "  -v, --verbose              print each script line before execution");
  shell_write_line(state, "  -k, --keep-going           continue when a line fails");
}

int shell_cmd_source(ShellContext& ctx) {
  SourceOptions options {};
  bool help_requested = false;
  if (!parse_source_args(ctx, &options, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_source(ctx.state);
    return 0;
  }

  return run_script_file(ctx.state, ctx.transport, options);
}

int shell_cmd_sh(ShellContext& ctx) {
  SourceOptions options {};
  bool help_requested = false;
  if (!parse_source_args(ctx, &options, &help_requested)) {
    return 1;
  }
  if (help_requested) {
    shell_help_sh(ctx.state);
    return 0;
  }

  return run_script_file(ctx.state, ctx.transport, options);
}

}  // namespace mros::shell
