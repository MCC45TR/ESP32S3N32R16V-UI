#pragma once

#include <sys/stat.h>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "src/shell/mros_shell.h"

namespace mros::shell {

struct ShellState {
  ShellConfig config {};
  std::string cwd = "/";
  std::string previous_cwd = "/";
  std::string* captured_output = nullptr;
  ShellStreamCallback captured_stream_callback = nullptr;
  void* captured_stream_user_data = nullptr;
  bool json_output_default = false;
  uint32_t command_timeout_ms = 0U;
  char serial_input_buffer[256] = {};
  size_t serial_input_length = 0U;
  uint32_t last_serial_activity_ms = 0U;
  bool last_output_ended_with_newline = true;
  bool root_session = false;
  bool close_requested = false;
  bool initialized = false;
  ShellTransport active_transport = ShellTransport::SerialConsole;
  std::string session_username;
  std::string session_display_name;
  bool session_admin = false;
  bool session_can_sudo = false;
  uint32_t capability_mask = kShellCapabilityUserDefault;
  std::vector<std::string> history;
  std::vector<std::string> dir_stack;
  std::map<std::string, std::string> env_vars;
  bool heredoc_active = false;
  std::string heredoc_delimiter;
  std::string heredoc_command;
  std::string heredoc_buffer;
  uint16_t terminal_columns = 80U;
  uint16_t terminal_rows = 24U;
  uint8_t remote_target = 0U;
};

struct ShellOutputSink {
  ShellState* state = nullptr;
  size_t bytes_written = 0U;
  bool dropped = false;
};

struct ShellCompletionCache {
  std::vector<std::string> command_names;
  std::vector<std::string> alias_names;
  std::vector<std::string> all_names;
  size_t max_command_name_width = 0U;
  uint32_t revision = 0U;
};

struct ShellContext {
  ShellState& state;
  const std::vector<std::string>& args;
  const std::string* stdin_buffer = nullptr;
  bool json_output = false;
  ShellTransport transport = ShellTransport::Current;
};

using ShellCommandHandler = int (*)(ShellContext& ctx);
using ShellCommandHelpHandler = void (*)(ShellState& state);

struct ShellCommandRegistration {
  const char* name = nullptr;
  ShellCommandHandler handler = nullptr;
  const char* summary = nullptr;
  ShellCommandHelpHandler help_handler = nullptr;
  uint32_t capabilities = ShellCapabilityRead;
  const char* group = nullptr;
  const char* usage = nullptr;
  const char* examples = nullptr;
  const char* risk = nullptr;
  bool supports_json = false;
  bool supports_job = true;
};

struct ShellFsEntry {
  std::string name;
  std::string display_name;
  std::string path;
  bool is_dir = false;
  bool is_virtual = false;
  bool has_stat = false;
  struct stat info {};
};

void shell_write(ShellState& state, const char* text);
void shell_write_line(ShellState& state, const char* text);
void shell_printf(ShellState& state, const char* format, ...);
ShellOutputSink shell_output_sink(ShellState& state);
void shell_sink_write(ShellOutputSink& sink, const char* text, size_t len);
void shell_sink_write_cstr(ShellOutputSink& sink, const char* text);
void shell_sink_printf(ShellOutputSink& sink, const char* format, ...);
std::string shell_prompt(const ShellState& state);
bool shell_is_storage_mounted(const ShellState& state);
bool shell_supports_ansi(const ShellState& state);
size_t shell_terminal_columns(const ShellState& state);
size_t shell_terminal_rows(const ShellState& state);
std::string shell_ansi_wrap(const ShellState& state, const char* sgr, const std::string& text);
std::string shell_ansi_reset();

std::string shell_normalize_path(const ShellState& state, const std::string& input);
std::string shell_storage_mount_path(const ShellState& state);
std::string shell_storage_user_root(const ShellState& state);
std::string shell_parent_path(const std::string& path);
std::string shell_basename(const std::string& path);
bool shell_is_virtual_root(const std::string& path);
bool shell_is_storage_path(const ShellState& state, const std::string& path);
bool shell_is_user_visible_path(const ShellState& state, const std::string& path);
bool shell_is_user_writable_path(const ShellState& state, const std::string& path);

bool shell_path_exists(
    const ShellState& state,
    const std::string& path,
    bool* is_dir,
    struct stat* info,
    std::string* error);

bool shell_read_directory(
    const ShellState& state,
    const std::string& dir_path,
    bool include_dot,
    bool include_dotdot,
    std::vector<ShellFsEntry>* entries,
    std::string* error);

bool shell_openable_file_path(
    const ShellState& state,
    const std::string& path,
    std::string* actual_path,
    std::string* error);

const std::vector<ShellCommandRegistration>& shell_commands();
const ShellCommandRegistration* shell_find_command(const char* name);
bool shell_can_enter_root(const ShellState& state);
const ShellCompletionCache& shell_completion_cache();
void shell_invalidate_completion_cache();
const std::vector<const char*>& shell_setting_names();
bool execute_line_on_state(
    ShellState& state,
    const char* line,
    bool echo_command,
    ShellTransport transport);

int shell_cmd_ls(ShellContext& ctx);
int shell_cmd_tree(ShellContext& ctx);
int shell_cmd_cat(ShellContext& ctx);
int shell_cmd_head(ShellContext& ctx);
int shell_cmd_tail(ShellContext& ctx);
int shell_cmd_wc(ShellContext& ctx);
int shell_cmd_more(ShellContext& ctx);
int shell_cmd_less(ShellContext& ctx);
int shell_cmd_nl(ShellContext& ctx);
int shell_cmd_xxd(ShellContext& ctx);
int shell_cmd_sed(ShellContext& ctx);
int shell_cmd_awk(ShellContext& ctx);
int shell_cmd_fold(ShellContext& ctx);
int shell_cmd_fmt(ShellContext& ctx);
int shell_cmd_expand(ShellContext& ctx);
int shell_cmd_unexpand(ShellContext& ctx);
int shell_cmd_column(ShellContext& ctx);
int shell_cmd_bc(ShellContext& ctx);
int shell_cmd_units(ShellContext& ctx);
int shell_cmd_pushd(ShellContext& ctx);
int shell_cmd_popd(ShellContext& ctx);
int shell_cmd_dirs(ShellContext& ctx);
int shell_cmd_du(ShellContext& ctx);
int shell_cmd_stat(ShellContext& ctx);
int shell_cmd_find(ShellContext& ctx);
int shell_cmd_sha256sum(ShellContext& ctx);
int shell_cmd_file(ShellContext& ctx);
int shell_cmd_od(ShellContext& ctx);
int shell_cmd_cksum(ShellContext& ctx);
int shell_cmd_md5sum(ShellContext& ctx);
int shell_cmd_crc32(ShellContext& ctx);
int shell_cmd_hexdump(ShellContext& ctx);
int shell_cmd_sleep(ShellContext& ctx);
int shell_cmd_tee(ShellContext& ctx);
int shell_cmd_eof(ShellContext& ctx);
int shell_cmd_sort(ShellContext& ctx);
int shell_cmd_uniq(ShellContext& ctx);
int shell_cmd_cut(ShellContext& ctx);
int shell_cmd_tr(ShellContext& ctx);
int shell_cmd_xargs(ShellContext& ctx);
int shell_cmd_basename(ShellContext& ctx);
int shell_cmd_dirname(ShellContext& ctx);
int shell_cmd_realpath(ShellContext& ctx);
int shell_cmd_cmp(ShellContext& ctx);
int shell_cmd_strings(ShellContext& ctx);
int shell_cmd_sync(ShellContext& ctx);
int shell_cmd_env(ShellContext& ctx);
int shell_cmd_export(ShellContext& ctx);
int shell_cmd_printenv(ShellContext& ctx);
int shell_cmd_history(ShellContext& ctx);
int shell_cmd_which(ShellContext& ctx);
int shell_cmd_test(ShellContext& ctx);
int shell_cmd_read(ShellContext& ctx);
int shell_cmd_true(ShellContext& ctx);
int shell_cmd_false(ShellContext& ctx);
int shell_cmd_whoami(ShellContext& ctx);
int shell_cmd_id(ShellContext& ctx);
int shell_cmd_groups(ShellContext& ctx);
int shell_cmd_chmod(ShellContext& ctx);
int shell_cmd_chown(ShellContext& ctx);
int shell_cmd_sudo(ShellContext& ctx);
int shell_cmd_passwd(ShellContext& ctx);
int shell_cmd_cd(ShellContext& ctx);
int shell_cmd_espfetch(ShellContext& ctx);
int shell_cmd_grep(ShellContext& ctx);
int shell_cmd_mount(ShellContext& ctx);
int shell_cmd_umount(ShellContext& ctx);
int shell_cmd_cp(ShellContext& ctx);
int shell_cmd_mv(ShellContext& ctx);
int shell_cmd_rm(ShellContext& ctx);
int shell_cmd_touch(ShellContext& ctx);
int shell_cmd_mkdir(ShellContext& ctx);
int shell_cmd_help(ShellContext& ctx);
int shell_cmd_man(ShellContext& ctx);
int shell_cmd_set(ShellContext& ctx);
int shell_cmd_reboot(ShellContext& ctx);
int shell_cmd_poweroff(ShellContext& ctx);
int shell_cmd_htop(ShellContext& ctx);
int shell_cmd_echo(ShellContext& ctx);
int shell_cmd_uptime(ShellContext& ctx);
int shell_cmd_status(ShellContext& ctx);
int shell_cmd_log(ShellContext& ctx);
int shell_cmd_clear(ShellContext& ctx);
int shell_cmd_uname(ShellContext& ctx);
int shell_cmd_date(ShellContext& ctx);
int shell_cmd_pwd(ShellContext& ctx);
int shell_cmd_espnow(ShellContext& ctx);
int shell_cmd_robot(ShellContext& ctx);
int shell_cmd_project(ShellContext& ctx);
int shell_cmd_ps(ShellContext& ctx);
int shell_cmd_df(ShellContext& ctx);
int shell_cmd_free(ShellContext& ctx);
int shell_cmd_kill(ShellContext& ctx);
int shell_cmd_ping(ShellContext& ctx);
int shell_cmd_wifi(ShellContext& ctx);
int shell_cmd_devices(ShellContext& ctx);
int shell_cmd_dpm(ShellContext& ctx);
int shell_cmd_coredump(ShellContext& ctx);
int shell_cmd_heap_trace(ShellContext& ctx);
int shell_cmd_watch(ShellContext& ctx);
int shell_cmd_config(ShellContext& ctx);
int shell_cmd_mros(ShellContext& ctx);
int shell_cmd_mshell(ShellContext& ctx);
int shell_cmd_mros7dofs3_update(ShellContext& ctx);
int shell_cmd_medit(ShellContext& ctx);
int shell_cmd_tar(ShellContext& ctx);
int shell_cmd_gzip(ShellContext& ctx);
int shell_cmd_gunzip(ShellContext& ctx);
int shell_cmd_zcat(ShellContext& ctx);
int shell_cmd_curl(ShellContext& ctx);
int shell_cmd_wget(ShellContext& ctx);
int shell_cmd_hostname(ShellContext& ctx);
int shell_cmd_who(ShellContext& ctx);
int shell_cmd_w(ShellContext& ctx);
int shell_cmd_last(ShellContext& ctx);
int shell_cmd_dmesg(ShellContext& ctx);
int shell_cmd_logger(ShellContext& ctx);
int shell_cmd_journalctl(ShellContext& ctx);
int shell_cmd_systemctl(ShellContext& ctx);
int shell_cmd_service(ShellContext& ctx);
int shell_cmd_gpio(ShellContext& ctx);
int shell_cmd_gpioget(ShellContext& ctx);
int shell_cmd_gpioset(ShellContext& ctx);
int shell_cmd_gpioinfo(ShellContext& ctx);
int shell_cmd_i2cdetect(ShellContext& ctx);
int shell_cmd_i2cget(ShellContext& ctx);
int shell_cmd_i2cset(ShellContext& ctx);
int shell_cmd_i2cdump(ShellContext& ctx);
int shell_cmd_pwm(ShellContext& ctx);
int shell_cmd_adc(ShellContext& ctx);
int shell_cmd_spi(ShellContext& ctx);
int shell_cmd_uart(ShellContext& ctx);
int shell_cmd_ssh(ShellContext& ctx);
int shell_cmd_change(ShellContext& ctx);
int shell_cmd_su(ShellContext& ctx);
int shell_cmd_exit(ShellContext& ctx);
int shell_cmd_update_system(ShellContext& ctx);
int shell_cmd_update_c6(ShellContext& ctx);
int shell_cmd_lsblk(ShellContext& ctx);
int shell_cmd_source(ShellContext& ctx);
int shell_cmd_sh(ShellContext& ctx);
int shell_cmd_math(ShellContext& ctx);

void shell_help_ls(ShellState& state);
void shell_help_tree(ShellState& state);
void shell_help_cat(ShellState& state);
void shell_help_head(ShellState& state);
void shell_help_tail(ShellState& state);
void shell_help_wc(ShellState& state);
void shell_help_more(ShellState& state);
void shell_help_less(ShellState& state);
void shell_help_nl(ShellState& state);
void shell_help_xxd(ShellState& state);
void shell_help_sed(ShellState& state);
void shell_help_awk(ShellState& state);
void shell_help_fold(ShellState& state);
void shell_help_fmt(ShellState& state);
void shell_help_expand(ShellState& state);
void shell_help_unexpand(ShellState& state);
void shell_help_column(ShellState& state);
void shell_help_bc(ShellState& state);
void shell_help_units(ShellState& state);
void shell_help_pushd(ShellState& state);
void shell_help_popd(ShellState& state);
void shell_help_dirs(ShellState& state);
void shell_help_du(ShellState& state);
void shell_help_stat(ShellState& state);
void shell_help_find(ShellState& state);
void shell_help_sha256sum(ShellState& state);
void shell_help_file(ShellState& state);
void shell_help_od(ShellState& state);
void shell_help_cksum(ShellState& state);
void shell_help_md5sum(ShellState& state);
void shell_help_crc32(ShellState& state);
void shell_help_hexdump(ShellState& state);
void shell_help_sleep(ShellState& state);
void shell_help_tee(ShellState& state);
void shell_help_eof(ShellState& state);
void shell_help_sort(ShellState& state);
void shell_help_uniq(ShellState& state);
void shell_help_cut(ShellState& state);
void shell_help_tr(ShellState& state);
void shell_help_xargs(ShellState& state);
void shell_help_basename(ShellState& state);
void shell_help_dirname(ShellState& state);
void shell_help_realpath(ShellState& state);
void shell_help_cmp(ShellState& state);
void shell_help_strings(ShellState& state);
void shell_help_sync(ShellState& state);
void shell_help_env(ShellState& state);
void shell_help_export(ShellState& state);
void shell_help_printenv(ShellState& state);
void shell_help_history(ShellState& state);
void shell_help_which(ShellState& state);
void shell_help_test(ShellState& state);
void shell_help_read(ShellState& state);
void shell_help_true(ShellState& state);
void shell_help_false(ShellState& state);
void shell_help_whoami(ShellState& state);
void shell_help_id(ShellState& state);
void shell_help_groups(ShellState& state);
void shell_help_chmod(ShellState& state);
void shell_help_chown(ShellState& state);
void shell_help_sudo(ShellState& state);
void shell_help_passwd(ShellState& state);
void shell_help_cd(ShellState& state);
void shell_help_espfetch(ShellState& state);
void shell_help_grep(ShellState& state);
void shell_help_mount(ShellState& state);
void shell_help_umount(ShellState& state);
void shell_help_cp(ShellState& state);
void shell_help_mv(ShellState& state);
void shell_help_rm(ShellState& state);
void shell_help_touch(ShellState& state);
void shell_help_mkdir(ShellState& state);
void shell_help_help(ShellState& state);
void shell_help_man(ShellState& state);
void shell_help_set(ShellState& state);
void shell_help_reboot(ShellState& state);
void shell_help_poweroff(ShellState& state);
void shell_help_htop(ShellState& state);
void shell_help_echo(ShellState& state);
void shell_help_uptime(ShellState& state);
void shell_help_status(ShellState& state);
void shell_help_log(ShellState& state);
void shell_help_clear(ShellState& state);
void shell_help_uname(ShellState& state);
void shell_help_date(ShellState& state);
void shell_help_pwd(ShellState& state);
void shell_help_espnow(ShellState& state);
void shell_help_robot(ShellState& state);
void shell_help_project(ShellState& state);
void shell_help_ps(ShellState& state);
void shell_help_df(ShellState& state);
void shell_help_free(ShellState& state);
void shell_help_kill(ShellState& state);
void shell_help_ping(ShellState& state);
void shell_help_wifi(ShellState& state);
void shell_help_devices(ShellState& state);
void shell_help_dpm(ShellState& state);
void shell_help_coredump(ShellState& state);
void shell_help_heap_trace(ShellState& state);
void shell_help_watch(ShellState& state);
void shell_help_config(ShellState& state);
void shell_help_mros(ShellState& state);
void shell_help_mshell(ShellState& state);
void shell_help_mros7dofs3_update(ShellState& state);
void shell_help_medit(ShellState& state);
void shell_help_tar(ShellState& state);
void shell_help_gzip(ShellState& state);
void shell_help_gunzip(ShellState& state);
void shell_help_zcat(ShellState& state);
void shell_help_curl(ShellState& state);
void shell_help_wget(ShellState& state);
void shell_help_hostname(ShellState& state);
void shell_help_who(ShellState& state);
void shell_help_w(ShellState& state);
void shell_help_last(ShellState& state);
void shell_help_dmesg(ShellState& state);
void shell_help_logger(ShellState& state);
void shell_help_journalctl(ShellState& state);
void shell_help_systemctl(ShellState& state);
void shell_help_service(ShellState& state);
void shell_help_gpio(ShellState& state);
void shell_help_gpioget(ShellState& state);
void shell_help_gpioset(ShellState& state);
void shell_help_gpioinfo(ShellState& state);
void shell_help_i2cdetect(ShellState& state);
void shell_help_i2cget(ShellState& state);
void shell_help_i2cset(ShellState& state);
void shell_help_i2cdump(ShellState& state);
void shell_help_pwm(ShellState& state);
void shell_help_adc(ShellState& state);
void shell_help_spi(ShellState& state);
void shell_help_uart(ShellState& state);
void shell_help_ssh(ShellState& state);
void shell_help_change(ShellState& state);
void shell_help_su(ShellState& state);
void shell_help_exit(ShellState& state);
void shell_help_update_system(ShellState& state);
void shell_help_update_c6(ShellState& state);
void shell_help_lsblk(ShellState& state);
void shell_help_source(ShellState& state);
void shell_help_sh(ShellState& state);
void shell_help_math(ShellState& state);

struct ShellAliasRecord {
  std::string name;
  std::string value;
};

const std::vector<ShellAliasRecord>& shell_aliases();
bool shell_alias_add(const std::string& name, const std::string& value, std::string* error);
bool shell_alias_remove(const std::string& name, std::string* error);
bool shell_alias_save(std::string* error);
bool shell_alias_reload(std::string* error);

}  // namespace mros::shell
