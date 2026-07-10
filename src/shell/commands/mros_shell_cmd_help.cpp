#include "src/shell/mros_shell_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace mros::shell {
namespace {

struct HelpTableLayout {
  size_t total_width = 80U;
  size_t group_width = 10U;
  size_t command_width = 18U;
  size_t capability_width = 9U;
  size_t summary_width = 36U;
  bool show_group = true;
  bool show_capability = true;
};

size_t bounded_width(const ShellState& state) {
  return std::max<size_t>(56U, std::min<size_t>(140U, shell_terminal_columns(state)));
}

std::string pad_right(std::string text, const size_t width) {
  if (text.size() > width) {
    if (width <= 1U) {
      return text.substr(0U, width);
    }
    text.resize(width - 1U);
    text += "~";
    return text;
  }
  if (text.size() < width) {
    text += std::string(width - text.size(), ' ');
  }
  return text;
}

std::string truncate_to(std::string text, const size_t width) {
  if (text.size() <= width) {
    return text;
  }
  if (width <= 1U) {
    return text.substr(0U, width);
  }
  text.resize(width - 1U);
  text += "~";
  return text;
}

HelpTableLayout help_table_layout(const ShellState& state) {
  HelpTableLayout layout {};
  layout.total_width = bounded_width(state);
  const ShellCompletionCache& cache = shell_completion_cache();
  layout.command_width = std::max<size_t>(8U, std::min<size_t>(20U, cache.max_command_name_width));
  if (layout.total_width < 70U) {
    layout.show_group = false;
    layout.show_capability = false;
    layout.group_width = 0U;
    layout.capability_width = 0U;
  } else if (layout.total_width < 92U) {
    layout.capability_width = 7U;
  }
  const size_t fixed =
      (layout.show_group ? layout.group_width + 2U : 0U) +
      layout.command_width + 2U +
      (layout.show_capability ? layout.capability_width + 2U : 0U);
  layout.summary_width = fixed < layout.total_width ? layout.total_width - fixed : 18U;
  layout.summary_width = std::max<size_t>(18U, layout.summary_width);
  return layout;
}

std::string lower_copy(const std::string& value) {
  std::string out = value;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

bool contains_ci(const char* haystack, const std::string& needle) {
  if (needle.empty()) return true;
  return lower_copy(haystack != nullptr ? haystack : "").find(needle) != std::string::npos;
}

const char* help_category_for(const char* command) {
  const std::string name = command != nullptr ? command : "";
  if (name == "ls" || name == "tree" || name == "cat" || name == "head" ||
      name == "tail" || name == "wc" || name == "du" || name == "stat" ||
      name == "find" || name == "cp" || name == "mv" || name == "rm" ||
      name == "touch" || name == "mkdir" || name == "df" || name == "mount" ||
      name == "umount" ||
      name == "sync" || name == "sha256sum" || name == "hexdump" ||
      name == "strings" || name == "cmp") {
    return "Dosya";
  }
  if (name == "grep" || name == "sort" || name == "uniq" || name == "cut" ||
      name == "tr" || name == "xargs" || name == "tee" || name == "basename" ||
      name == "dirname" || name == "realpath") {
    return "Metin";
  }
  if (name == "wifi" || name == "ping" || name == "ssh" || name == "mshell") {
    return "Ağ";
  }
  if (name == "robot" || name == "mros" || name == "project") {
    return "Robot";
  }
  if (name == "update-system" || name == "mros-deuscara-update" ||
      name == "mros7dofs3-update" || name == "lsblk") {
    return "Güncelleme";
  }
  if (name == "ps" || name == "mtop" || name == "htop" || name == "free" ||
      name == "status" || name == "log" || name == "mfetch" || name == "espfetch" || name == "uptime" ||
      name == "uname" || name == "date") {
    return "Tanı";
  }
  if (name == "help" || name == "man" || name == "set" || name == "env" ||
      name == "export" || name == "printenv" || name == "history" ||
      name == "which" || name == "read" || name == "source" || name == "script" ||
      name == "sh" ||
      name == "clear" || name == "echo" || name == "watch" || name == "config") {
    return "Kabuk";
  }
  if (name == "sudo" || name == "passwd" || name == "change" || name == "su" ||
      name == "exit" || name == "whoami" || name == "id" || name == "groups" ||
      name == "chmod" || name == "chown") {
    return "Oturum";
  }
  return "Diğer";
}

bool is_alias_command(const ShellCommandRegistration& command) {
  const char* name = command.name != nullptr ? command.name : "";
  return std::strcmp(name, "EOF") == 0 || std::strcmp(name, "eof") == 0 ||
         std::strcmp(name, "script") == 0 || std::strcmp(name, "htop") == 0 ||
         std::strcmp(name, "espfetch") == 0 ||
         std::strcmp(name, "mros7dofs3-update") == 0 ||
         std::strcmp(name, "[") == 0;
}

const char* help_category_color(const char* category) {
  const std::string c = category != nullptr ? category : "";
  if (c == "Dosya") return "38;5;81;1";
  if (c == "Metin") return "38;5;151;1";
  if (c == "Ağ") return "38;5;117;1";
  if (c == "Robot") return "38;5;214;1";
  if (c == "Güncelleme") return "38;5;203;1";
  if (c == "Tanı") return "38;5;177;1";
  if (c == "Kabuk") return "38;5;112;1";
  if (c == "Oturum") return "38;5;223;1";
  return "38;5;246";
}

std::string capability_label(const uint32_t capabilities) {
  if ((capabilities & ShellCapabilityRoot) != 0U) {
    return "root";
  }
  if ((capabilities & ShellCapabilityUpdate) != 0U) {
    return "update";
  }
  if ((capabilities & ShellCapabilityRobot) != 0U) {
    return "robot";
  }
  if ((capabilities & ShellCapabilityDebug) != 0U) {
    return "debug";
  }
  if ((capabilities & ShellCapabilityNetwork) != 0U) {
    return "net";
  }
  if ((capabilities & ShellCapabilityWrite) != 0U) {
    return "write";
  }
  return "read";
}

const char* capability_color(const uint32_t capabilities) {
  if ((capabilities & ShellCapabilityRoot) != 0U) return "38;5;203;1";
  if ((capabilities & ShellCapabilityUpdate) != 0U) return "38;5;214;1";
  if ((capabilities & ShellCapabilityRobot) != 0U) return "38;5;117;1";
  if ((capabilities & ShellCapabilityDebug) != 0U) return "38;5;177;1";
  if ((capabilities & ShellCapabilityNetwork) != 0U) return "38;5;81;1";
  if ((capabilities & ShellCapabilityWrite) != 0U) return "38;5;223;1";
  return "38;5;246";
}

std::string command_display_name(const ShellCommandRegistration& command) {
  std::string out = command.name != nullptr ? command.name : "";
  if (is_alias_command(command)) {
    out += "*";
  }
  return out;
}

void print_help_table_header(ShellState& state, const HelpTableLayout& layout) {
  std::string line;
  if (layout.show_group) {
    line += pad_right("Grup", layout.group_width);
    line += "  ";
  }
  line += pad_right("Komut", layout.command_width);
  line += "  ";
  if (layout.show_capability) {
    line += pad_right("Yetki", layout.capability_width);
    line += "  ";
  }
  line += truncate_to("Açıklama", layout.summary_width);
  shell_write_line(state, shell_ansi_wrap(state, "38;5;246;1", line).c_str());
  shell_write_line(state, shell_ansi_wrap(state, "38;5;238", std::string(layout.total_width, '-')).c_str());
}

void print_help_table_row(
    ShellState& state,
    const HelpTableLayout& layout,
    const ShellCommandRegistration& command,
    const bool repeat_group) {
  const char* category = help_category_for(command.name);
  std::string line;
  if (layout.show_group) {
    const std::string group = repeat_group ? category : "";
    line += shell_ansi_wrap(state, help_category_color(category), pad_right(group, layout.group_width));
    line += "  ";
  }
  const std::string command_name = command_display_name(command);
  line += shell_ansi_wrap(state, "38;5;112;1", pad_right(command_name, layout.command_width));
  line += "  ";
  if (layout.show_capability) {
    const std::string cap = capability_label(command.capabilities);
    line += shell_ansi_wrap(state, capability_color(command.capabilities), pad_right(cap, layout.capability_width));
    line += "  ";
  }
  line += shell_ansi_wrap(
      state,
      "38;5;246",
      truncate_to(command.summary != nullptr ? command.summary : "", layout.summary_width));
  shell_write_line(state, line.c_str());
}

void print_help_table(ShellState& state, const std::vector<const ShellCommandRegistration*>& rows) {
  const HelpTableLayout layout = help_table_layout(state);
  print_help_table_header(state, layout);
  std::string previous_category;
  for (const ShellCommandRegistration* command : rows) {
    if (command == nullptr) continue;
    const std::string category = help_category_for(command->name);
    const bool repeat_group = category != previous_category || !layout.show_group;
    print_help_table_row(state, layout, *command, repeat_group);
    previous_category = category;
  }
}

void print_help_syntax(ShellState& state) {
  shell_write_line(state, shell_ansi_wrap(state, "38;5;81;1", "MROS shell syntax").c_str());
  shell_write_line(state, "Kullanım kalıbı:");
  shell_write_line(state, "  command [subcommand] [options] [arguments]");
  shell_write_line(state, "");
  shell_write_line(state, "Temel kurallar:");
  shell_write_line(state, "  help                 Komut tablosunu gösterir.");
  shell_write_line(state, "  help <komut>         Komutun kısa yardımını açar.");
  shell_write_line(state, "  man <komut>          Komutun manuel sayfasını açar.");
  shell_write_line(state, "  help --search TERM   Komut adı ve açıklamalarında arama yapar.");
  shell_write_line(state, "  --help veya -h       Çoğu komutta yerel yardım sayfasını açar.");
  shell_write_line(state, "");
  shell_write_line(state, "MROS komut aileleri:");
  shell_write_line(state, "  mros perf status           Sistem performans metrikleri.");
  shell_write_line(state, "  mros rtos status           Task, stack ve deadline özeti.");
  shell_write_line(state, "  mros wifi diag             WiFi fast-path ve reconnect özeti.");
  shell_write_line(state, "  mros security status       Oturum, yetki ve audit özeti.");
  shell_write_line(state, "  robot math benchmark       Robot matematik süre ve solver özeti.");
  shell_write_line(state, "  mshell list devices        S3/t41 shell hedeflerini listeler (C3 devre disi).");
  shell_write_line(state, "  mshell connect <s3|t41>    Uygunsa uzak shell hedefini acar.");
  shell_write_line(state, "");
  shell_write_line(state, "Operatörler ve akış:");
  shell_write_line(state, "  cmd1 && cmd2         cmd1 başarılıysa cmd2 çalışır.");
  shell_write_line(state, "  cmd1 ; cmd2          Komutları sırayla çalıştırır.");
  shell_write_line(state, "  command | grep text  stdout borulama destekleyen komutlara aktarılır.");
  shell_write_line(state, "  VAR=value command    Komut öncesi geçici ortam değeri.");
  shell_write_line(state, "  export NAME=value    Kalıcı shell ortam değişkeni.");
  shell_write_line(state, "");
  shell_write_line(state, "Yollar:");
  shell_write_line(state, "  /                    Sanal kök.");
  shell_write_line(state, "  /fs                  LittleFS bağlama alias'ı.");
  shell_write_line(state, "  /ESPUSER             Kullanıcı tarafından yazılabilir alan.");
  shell_write_line(state, "");
  shell_write_line(state, "Yetki etiketleri:");
  shell_write_line(state, "  read   güvenli okuma/tanı komutu");
  shell_write_line(state, "  write  dosya, ayar veya kullanıcı verisi değiştirir");
  shell_write_line(state, "  net    ağ bağlantısı veya uzak erişim kullanır");
  shell_write_line(state, "  robot  robot hareket/safety yüzeyine erişir");
  shell_write_line(state, "  update firmware/update akışına erişir");
  shell_write_line(state, "  root   sadece root/sudo oturumunda çalışır");
}

}  // namespace

void shell_help_help(ShellState& state) {
  shell_write_line(state, "Usage: help [command|syntax]");
  shell_write_line(state, "       help --search TERM");
  shell_write_line(state, "       help --all");
  shell_write_line(state, "Show adaptive mshell command tables, search commands, or open syntax help.");
}

int shell_cmd_help(ShellContext& ctx) {
  if (ctx.args.size() > 3U) {
    shell_write_line(ctx.state, "help: too many arguments");
    return 1;
  }

  if (ctx.args.size() >= 2U && (ctx.args[1] == "--search" || ctx.args[1] == "-s")) {
    if (ctx.args.size() < 3U) {
      shell_write_line(ctx.state, "help: search term required");
      return 1;
    }
    const std::string needle = lower_copy(ctx.args[2]);
    size_t found = 0U;
    shell_write_line(ctx.state, shell_ansi_wrap(ctx.state, "38;5;81;1", "Search results").c_str());
    std::vector<const ShellCommandRegistration*> rows;
    for (const ShellCommandRegistration& command : shell_commands()) {
      if (!contains_ci(command.name, needle) && !contains_ci(command.summary, needle)) continue;
      rows.push_back(&command);
      ++found;
    }
    if (found == 0U) {
      shell_write_line(ctx.state, shell_ansi_wrap(ctx.state, "38;5;203", "No matching commands.").c_str());
    } else {
      print_help_table(ctx.state, rows);
    }
    return found == 0U ? 1 : 0;
  }

  if (ctx.args.size() == 2U && (ctx.args[1] == "--all" || ctx.args[1] == "-a")) {
    shell_write_line(ctx.state, shell_ansi_wrap(ctx.state, "38;5;81;1", "All commands").c_str());
    std::vector<const ShellCommandRegistration*> rows;
    for (const ShellCommandRegistration& command : shell_commands()) {
      rows.push_back(&command);
    }
    print_help_table(ctx.state, rows);
    return 0;
  }

  if (ctx.args.size() == 2U) {
    if (ctx.args[1] == "syntax") {
      print_help_syntax(ctx.state);
      return 0;
    }
    const ShellCommandRegistration* command = shell_find_command(ctx.args[1].c_str());
    if (command == nullptr) {
      shell_printf(ctx.state, "help: no such command: %s\n", ctx.args[1].c_str());
      return 1;
    }
    if (command->help_handler != nullptr) {
      command->help_handler(ctx.state);
    } else {
      shell_printf(
          ctx.state,
          "%s: %s\n",
          command->name != nullptr ? command->name : "(unknown)",
          command->summary != nullptr ? command->summary : "No help available.");
    }
    return 0;
  }

  shell_printf(
      ctx.state,
      "%s %s\n",
      shell_ansi_wrap(ctx.state, "38;5;81;1", kShellName).c_str(),
      shell_ansi_wrap(ctx.state, "38;5;223", kShellVersion).c_str());
  shell_write_line(
      ctx.state,
      shell_ansi_wrap(
          ctx.state,
          "38;5;246",
          "Kullanım: help <komut> | help syntax | help --search TERM | help --all").c_str());
  shell_write_line(ctx.state, shell_ansi_wrap(ctx.state, "38;5;246", "Aktif komut tablosu:").c_str());
  const std::vector<ShellCommandRegistration>& commands = shell_commands();
  const char* categories[] = {"Dosya", "Metin", "Kabuk", "Ağ", "Robot", "Tanı", "Güncelleme", "Oturum", "Diğer"};
  std::vector<const ShellCommandRegistration*> rows;
  for (const char* category : categories) {
    for (const ShellCommandRegistration& command : commands) {
      if (std::string(help_category_for(command.name)) == category) {
        rows.push_back(&command);
      }
    }
  }
  print_help_table(ctx.state, rows);
  shell_write_line(ctx.state, "");
  shell_write_line(
      ctx.state,
      shell_ansi_wrap(
          ctx.state,
          "38;5;246",
          "İpucu: 'help syntax', 'help wifi', 'man update-system', 'help --search file'; * = alias").c_str());
  return 0;
}

}  // namespace mros::shell
