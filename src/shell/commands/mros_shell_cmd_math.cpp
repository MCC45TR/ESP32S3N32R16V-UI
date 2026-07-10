#include "src/shell/mros_shell_internal.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "src/platform/mros_time.h"

namespace mros::shell {
namespace {

struct MathRecord {
  uint32_t id = 0U;
  bool ok = false;
  double result = 0.0;
  uint32_t elapsed_ms = 0U;
  std::string expression;
  std::string message;
};

struct MathState {
  uint32_t success_count = 0U;
  uint32_t failure_count = 0U;
  uint32_t sequence = 0U;
  bool has_last_result = false;
  double last_result = 0.0;
  std::vector<MathRecord> history;
};

constexpr size_t kMathHistoryLimit = 24U;
MathState g_math_state {};

class MathParser {
 public:
  MathParser(const char* input, const bool has_ans, const double ans_value)
      : cursor_(input != nullptr ? input : ""), has_ans_(has_ans), ans_value_(ans_value) {}

  bool parse(double* out_value, std::string* out_error) {
    if (out_value == nullptr || out_error == nullptr) {
      return false;
    }

    double value = 0.0;
    if (!parse_expression(&value)) {
      *out_error = error_;
      return false;
    }
    skip_spaces();
    if (*cursor_ != '\0') {
      *out_error = "unexpected trailing input";
      return false;
    }
    *out_value = value;
    out_error->clear();
    return true;
  }

 private:
  const char* cursor_ = nullptr;
  bool has_ans_ = false;
  double ans_value_ = 0.0;
  std::string error_;

  void skip_spaces() {
    while (*cursor_ == ' ' || *cursor_ == '\t' || *cursor_ == '\r' || *cursor_ == '\n') {
      ++cursor_;
    }
  }

  bool consume(const char ch) {
    skip_spaces();
    if (*cursor_ == ch) {
      ++cursor_;
      return true;
    }
    return false;
  }

  bool parse_expression(double* out_value) {
    if (!parse_term(out_value)) {
      return false;
    }

    for (;;) {
      if (consume('+')) {
        double rhs = 0.0;
        if (!parse_term(&rhs)) {
          return false;
        }
        *out_value += rhs;
        continue;
      }
      if (consume('-')) {
        double rhs = 0.0;
        if (!parse_term(&rhs)) {
          return false;
        }
        *out_value -= rhs;
        continue;
      }
      break;
    }
    return true;
  }

  bool parse_term(double* out_value) {
    if (!parse_power(out_value)) {
      return false;
    }

    for (;;) {
      if (consume('*')) {
        double rhs = 0.0;
        if (!parse_power(&rhs)) {
          return false;
        }
        *out_value *= rhs;
        continue;
      }
      if (consume('/')) {
        double rhs = 0.0;
        if (!parse_power(&rhs)) {
          return false;
        }
        if (rhs == 0.0) {
          error_ = "division by zero";
          return false;
        }
        *out_value /= rhs;
        continue;
      }
      if (consume('%')) {
        double rhs = 0.0;
        if (!parse_power(&rhs)) {
          return false;
        }
        if (rhs == 0.0) {
          error_ = "modulo by zero";
          return false;
        }
        *out_value = std::fmod(*out_value, rhs);
        continue;
      }
      break;
    }
    return true;
  }

  bool parse_power(double* out_value) {
    if (!parse_unary(out_value)) {
      return false;
    }
    if (consume('^')) {
      double exponent = 0.0;
      if (!parse_power(&exponent)) {
        return false;
      }
      *out_value = std::pow(*out_value, exponent);
    }
    return true;
  }

  bool parse_unary(double* out_value) {
    if (consume('+')) {
      return parse_unary(out_value);
    }
    if (consume('-')) {
      if (!parse_unary(out_value)) {
        return false;
      }
      *out_value = -*out_value;
      return true;
    }
    return parse_primary(out_value);
  }

  bool parse_identifier(std::string* out_identifier) {
    if (out_identifier == nullptr) {
      return false;
    }
    skip_spaces();
    if (!(std::isalpha(static_cast<unsigned char>(*cursor_)) || *cursor_ == '_')) {
      return false;
    }
    const char* start = cursor_;
    ++cursor_;
    while (std::isalnum(static_cast<unsigned char>(*cursor_)) || *cursor_ == '_') {
      ++cursor_;
    }
    *out_identifier = std::string(start, static_cast<size_t>(cursor_ - start));
    return true;
  }

  bool eval_constant(const std::string& identifier, double* out_value) {
    if (out_value == nullptr) {
      return false;
    }
    if (identifier == "pi" || identifier == "PI") {
      *out_value = 3.14159265358979323846;
      return true;
    }
    if (identifier == "e" || identifier == "E") {
      *out_value = 2.71828182845904523536;
      return true;
    }
    if (identifier == "ans") {
      if (!has_ans_) {
        error_ = "ans is not available yet";
        return false;
      }
      *out_value = ans_value_;
      return true;
    }
    return false;
  }

  bool eval_function(const std::string& name, const std::vector<double>& args, double* out_value) {
    if (out_value == nullptr) {
      return false;
    }

    auto require_argc = [&](const size_t expected) -> bool {
      if (args.size() != expected) {
        error_ = "function '" + name + "' expects " + std::to_string(static_cast<unsigned long>(expected)) +
                 " argument(s)";
        return false;
      }
      return true;
    };

    if (name == "sin") {
      if (!require_argc(1U)) {
        return false;
      }
      *out_value = std::sin(args[0]);
      return true;
    }
    if (name == "cos") {
      if (!require_argc(1U)) {
        return false;
      }
      *out_value = std::cos(args[0]);
      return true;
    }
    if (name == "tan") {
      if (!require_argc(1U)) {
        return false;
      }
      *out_value = std::tan(args[0]);
      return true;
    }
    if (name == "sqrt") {
      if (!require_argc(1U)) {
        return false;
      }
      if (args[0] < 0.0) {
        error_ = "sqrt domain error";
        return false;
      }
      *out_value = std::sqrt(args[0]);
      return true;
    }
    if (name == "abs") {
      if (!require_argc(1U)) {
        return false;
      }
      *out_value = std::fabs(args[0]);
      return true;
    }
    if (name == "log") {
      if (!require_argc(1U)) {
        return false;
      }
      if (args[0] <= 0.0) {
        error_ = "log domain error";
        return false;
      }
      *out_value = std::log10(args[0]);
      return true;
    }
    if (name == "ln") {
      if (!require_argc(1U)) {
        return false;
      }
      if (args[0] <= 0.0) {
        error_ = "ln domain error";
        return false;
      }
      *out_value = std::log(args[0]);
      return true;
    }
    if (name == "exp") {
      if (!require_argc(1U)) {
        return false;
      }
      *out_value = std::exp(args[0]);
      return true;
    }
    if (name == "floor") {
      if (!require_argc(1U)) {
        return false;
      }
      *out_value = std::floor(args[0]);
      return true;
    }
    if (name == "ceil") {
      if (!require_argc(1U)) {
        return false;
      }
      *out_value = std::ceil(args[0]);
      return true;
    }
    if (name == "round") {
      if (!require_argc(1U)) {
        return false;
      }
      *out_value = std::round(args[0]);
      return true;
    }
    if (name == "pow") {
      if (!require_argc(2U)) {
        return false;
      }
      *out_value = std::pow(args[0], args[1]);
      return true;
    }
    if (name == "min") {
      if (!require_argc(2U)) {
        return false;
      }
      *out_value = std::fmin(args[0], args[1]);
      return true;
    }
    if (name == "max") {
      if (!require_argc(2U)) {
        return false;
      }
      *out_value = std::fmax(args[0], args[1]);
      return true;
    }

    error_ = "unknown function '" + name + "'";
    return false;
  }

  bool parse_primary(double* out_value) {
    if (consume('(')) {
      if (!parse_expression(out_value)) {
        return false;
      }
      if (!consume(')')) {
        error_ = "missing closing ')'";
        return false;
      }
      return true;
    }

    std::string identifier;
    if (parse_identifier(&identifier)) {
      if (consume('(')) {
        std::vector<double> args;
        skip_spaces();
        if (!consume(')')) {
          for (;;) {
            double arg_value = 0.0;
            if (!parse_expression(&arg_value)) {
              return false;
            }
            args.push_back(arg_value);
            if (consume(')')) {
              break;
            }
            if (!consume(',')) {
              error_ = "expected ',' or ')' in function arguments";
              return false;
            }
          }
        }
        return eval_function(identifier, args, out_value);
      }
      if (eval_constant(identifier, out_value)) {
        return true;
      }
      error_ = "unknown identifier '" + identifier + "'";
      return false;
    }

    skip_spaces();
    char* end = nullptr;
    const double value = std::strtod(cursor_, &end);
    if (end == cursor_) {
      error_ = "expected number or expression";
      return false;
    }
    cursor_ = end;
    *out_value = value;
    return true;
  }
};

std::string join_tokens(const std::vector<std::string>& args, const size_t start_index) {
  std::string output;
  for (size_t i = start_index; i < args.size(); ++i) {
    if (!output.empty()) {
      output.push_back(' ');
    }
    output += args[i];
  }
  return output;
}

void push_history(const MathRecord& record) {
  if (g_math_state.history.size() >= kMathHistoryLimit) {
    g_math_state.history.erase(g_math_state.history.begin());
  }
  g_math_state.history.push_back(record);
}

void print_math_status(ShellState& state) {
  shell_printf(
      state,
      "math: total=%lu ok=%lu fail=%lu\n",
      static_cast<unsigned long>(g_math_state.sequence),
      static_cast<unsigned long>(g_math_state.success_count),
      static_cast<unsigned long>(g_math_state.failure_count));
  if (g_math_state.has_last_result) {
    shell_printf(state, "last_result=%.10g\n", g_math_state.last_result);
  } else {
    shell_write_line(state, "last_result=none");
  }
}

bool parse_size_arg(const std::string& text, size_t* out_value) {
  if (out_value == nullptr) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || (end != nullptr && *end != '\0') || parsed == 0U) {
    return false;
  }
  *out_value = static_cast<size_t>(parsed);
  return true;
}

}  // namespace

void shell_help_math(ShellState& state) {
  shell_write_line(state, "Usage: math [EXPRESSION]");
  shell_write_line(state, "       math eval EXPRESSION");
  shell_write_line(state, "       math status");
  shell_write_line(state, "       math history [-n NUM]");
  shell_write_line(state, "       math watch [COUNT]");
  shell_write_line(state, "       math clear");
  shell_write_line(state, "Examples: math 12*(7-3), math eval pow(2,10), math status");
}

int shell_cmd_math(ShellContext& ctx) {
  if (ctx.args.size() >= 2U && (ctx.args[1] == "--help" || ctx.args[1] == "-h")) {
    shell_help_math(ctx.state);
    return 0;
  }

  if (ctx.args.size() >= 2U && ctx.args[1] == "status") {
    print_math_status(ctx.state);
    return 0;
  }

  if (ctx.args.size() >= 2U && ctx.args[1] == "clear") {
    g_math_state.history.clear();
    g_math_state.sequence = 0U;
    g_math_state.success_count = 0U;
    g_math_state.failure_count = 0U;
    g_math_state.has_last_result = false;
    g_math_state.last_result = 0.0;
    shell_write_line(ctx.state, "math: history and counters cleared");
    return 0;
  }

  if (ctx.args.size() >= 2U && ctx.args[1] == "history") {
    size_t count = 10U;
    if (ctx.args.size() == 3U || ctx.args.size() > 4U) {
      shell_write_line(ctx.state, "math: usage: math history [-n NUM]");
      return 1;
    }
    if (ctx.args.size() == 4U) {
      if (ctx.args[2] != "-n") {
        shell_write_line(ctx.state, "math: usage: math history [-n NUM]");
        return 1;
      }
      if (!parse_size_arg(ctx.args[3], &count)) {
        shell_write_line(ctx.state, "math: invalid history count");
        return 1;
      }
    }
    if (g_math_state.history.empty()) {
      shell_write_line(ctx.state, "math: no history yet");
      return 0;
    }
    const size_t start = g_math_state.history.size() > count ? (g_math_state.history.size() - count) : 0U;
    for (size_t i = start; i < g_math_state.history.size(); ++i) {
      const MathRecord& item = g_math_state.history[i];
      if (item.ok) {
        shell_printf(
            ctx.state,
            "#%lu OK  %s = %.10g (%lums)\n",
            static_cast<unsigned long>(item.id),
            item.expression.c_str(),
            item.result,
            static_cast<unsigned long>(item.elapsed_ms));
      } else {
        shell_printf(
            ctx.state,
            "#%lu ERR %s -> %s (%lums)\n",
            static_cast<unsigned long>(item.id),
            item.expression.c_str(),
            item.message.c_str(),
            static_cast<unsigned long>(item.elapsed_ms));
      }
    }
    return 0;
  }

  if (ctx.args.size() >= 2U && ctx.args[1] == "watch") {
    size_t count = 8U;
    if (ctx.args.size() == 3U && !parse_size_arg(ctx.args[2], &count)) {
      shell_write_line(ctx.state, "math: invalid watch count");
      return 1;
    }
    if (ctx.args.size() > 3U) {
      shell_write_line(ctx.state, "math: usage: math watch [COUNT]");
      return 1;
    }
    for (size_t i = 0U; i < count; ++i) {
      shell_printf(ctx.state, "watch[%lu] ", static_cast<unsigned long>(i + 1U));
      print_math_status(ctx.state);
      if ((i + 1U) < count) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
      }
    }
    return 0;
  }

  size_t expression_start = 1U;
  if (ctx.args.size() >= 2U && ctx.args[1] == "eval") {
    expression_start = 2U;
  }
  if (expression_start >= ctx.args.size()) {
    shell_write_line(ctx.state, "math: missing expression");
    return 1;
  }

  const std::string expression = join_tokens(ctx.args, expression_start);
  MathParser parser(expression.c_str(), g_math_state.has_last_result, g_math_state.last_result);
  double result_value = 0.0;
  std::string error;
  const uint32_t started_ms = mros::platform::mros_millis();
  const bool ok = parser.parse(&result_value, &error);
  const uint32_t elapsed_ms = mros::platform::mros_millis() - started_ms;

  MathRecord record {};
  record.id = ++g_math_state.sequence;
  record.ok = ok;
  record.result = result_value;
  record.elapsed_ms = elapsed_ms;
  record.expression = expression;
  record.message = ok ? "ok" : (error.empty() ? "parse error" : error);
  push_history(record);

  if (!ok) {
    ++g_math_state.failure_count;
    shell_printf(ctx.state, "math: %s\n", record.message.c_str());
    return 1;
  }

  ++g_math_state.success_count;
  g_math_state.has_last_result = true;
  g_math_state.last_result = result_value;
  shell_printf(ctx.state, "%.10g\n", result_value);
  return 0;
}

}  // namespace mros::shell
