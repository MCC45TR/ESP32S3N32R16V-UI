#include "src/shell/mros_shell_internal.h"

#include "src/security/ssh_identity.h"

namespace mros::shell {
namespace {

void print_identity(ShellState& state) {
  const mros::ssh::IdentityConfig cfg = mros::ssh::identity_get();
  shell_printf(state, "display name : %s\n", state.session_display_name.c_str());
  shell_printf(state, "username     : %s\n", state.session_username.c_str());
  shell_printf(state, "device name  : %s\n", cfg.device_name.c_str());
  shell_printf(state, "role         : %s\n", state.root_session ? "root" : "user");
  shell_printf(state, "admin        : %s\n", state.session_admin ? "yes" : "no");
  shell_printf(state, "sudo         : %s\n", state.session_can_sudo ? "yes" : "no");
}

void print_users(ShellState& state) {
  const std::vector<mros::ssh::UserAccount> users = mros::ssh::list_users();
  shell_write_line(state, "display name              username          role      sudo  flags");
  shell_write_line(state, "------------------------  ----------------  --------  ----  ----------------");
  for (const mros::ssh::UserAccount& user : users) {
    std::string role = user.root ? "root" : (user.admin ? "admin" : "normal");
    std::string flags;
    if (user.primary) {
      flags = "primary";
    }
    if (user.root) {
      flags = flags.empty() ? "built-in" : flags + ",built-in";
    }
    shell_printf(
        state,
        "%-24s  %-16s  %-8s  %-4s  %s\n",
        user.display_name.c_str(),
        user.username.c_str(),
        role.c_str(),
        user.sudo ? "yes" : "no",
        flags.c_str());
  }
}

}  // namespace

void shell_help_change(ShellState& state) {
  shell_write_line(state, "Usage:");
  shell_write_line(state, "  change");
  shell_write_line(state, "  change users");
  shell_write_line(state, "  change name \"Display Name\"");
  shell_write_line(state, "  change username \"username\"");
  shell_write_line(state, "  change devicename \"DEUSCARA-S3V-LAB\"");
  shell_write_line(state, "  change passwd username \"NEW_PASS\" \"NEW_PASS\"");
  shell_write_line(state, "  change passwd root \"NEW_PASS\" \"NEW_PASS\"");
  shell_write_line(state, "  change add user \"NAME\" \"username\" \"PASS\" \"PASS\" --admin|--normal [--sudo|--no-sudo]");
}

int shell_cmd_change(ShellContext& ctx) {
  mros::ssh::identity_init();
  if (ctx.args.size() == 1U) {
    print_identity(ctx.state);
    return 0;
  }
  if (ctx.args.size() < 3U) {
    shell_help_change(ctx.state);
    return 1;
  }

  const std::string& sub = ctx.args[1];
  if (sub == "users") {
    print_users(ctx.state);
    return 0;
  }
  if (sub == "name") {
    if (!mros::ssh::set_display_name(String(ctx.args[2].c_str()))) {
      shell_write_line(ctx.state, "change: display name must be 1-64 visible ASCII characters");
      return 1;
    }
    if (!ctx.state.root_session) {
      ctx.state.session_display_name = ctx.args[2];
    }
    shell_write_line(ctx.state, "change: display name saved");
    return 0;
  }
  if (sub == "username") {
    if (!mros::ssh::set_username(String(ctx.args[2].c_str()))) {
      shell_write_line(ctx.state, "change: username must use lowercase a-z, digits or _ only");
      return 1;
    }
    if (!ctx.state.root_session) {
      ctx.state.session_username = ctx.args[2];
    }
    shell_write_line(ctx.state, "change: username saved");
    return 0;
  }
  if (sub == "devicename") {
    if (!mros::ssh::set_device_name(String(ctx.args[2].c_str()))) {
      shell_write_line(ctx.state, "change: devicename must be ASCII, no spaces, 3-32 chars");
      return 1;
    }
    shell_write_line(ctx.state, "change: device name saved");
    return 0;
  }
  if (sub == "passwd") {
    if (ctx.args.size() < 5U) {
      shell_write_line(ctx.state,
                       "change: confirmation required: change passwd username \"PASS\" \"PASS\"");
      return 1;
    }
    if (ctx.args[3] != ctx.args[4]) {
      shell_write_line(ctx.state, "change: password confirmation mismatch");
      return 1;
    }

    mros::ssh::IdentityConfig cfg = mros::ssh::identity_get();
    String user = String(ctx.args[2].c_str());
    if (user == "username") {
      user = cfg.username;
    }
    if (user == mros::ssh::root_username() && !ctx.state.root_session) {
      shell_write_line(ctx.state, "change: root password changes require su first");
      return 1;
    }
    if (!mros::ssh::set_password_for_user(user, String(ctx.args[3].c_str()))) {
      shell_write_line(ctx.state, "change: password must be 8-96 chars and user must exist");
      return 1;
    }
    shell_write_line(ctx.state, "change: password hash saved");
    return 0;
  }
  if (sub == "add") {
    if (!ctx.state.root_session && !ctx.state.session_admin) {
      shell_write_line(ctx.state, "change: add user requires admin or root privileges");
      return 1;
    }
    if (ctx.args.size() < 7U || ctx.args[2] != "user") {
      shell_help_change(ctx.state);
      return 1;
    }
    if (ctx.args[5] != ctx.args[6]) {
      shell_write_line(ctx.state, "change: password confirmation mismatch");
      return 1;
    }

    bool admin = false;
    bool have_role = false;
    bool sudo_enabled = false;
    bool sudo_explicit = false;
    for (size_t i = 7U; i < ctx.args.size(); ++i) {
      if (ctx.args[i] == "--admin") {
        admin = true;
        have_role = true;
        if (!sudo_explicit) {
          sudo_enabled = true;
        }
        continue;
      }
      if (ctx.args[i] == "--normal") {
        admin = false;
        have_role = true;
        if (!sudo_explicit) {
          sudo_enabled = false;
        }
        continue;
      }
      if (ctx.args[i] == "--sudo") {
        sudo_enabled = true;
        sudo_explicit = true;
        continue;
      }
      if (ctx.args[i] == "--no-sudo") {
        sudo_enabled = false;
        sudo_explicit = true;
        continue;
      }
      shell_printf(ctx.state, "change: unknown option '%s'\n", ctx.args[i].c_str());
      return 1;
    }
    if (!have_role) {
      shell_write_line(ctx.state, "change: choose either --admin or --normal");
      return 1;
    }
    if (!admin && sudo_enabled && !ctx.state.root_session) {
      shell_write_line(ctx.state, "change: only root may grant sudo to a normal account");
      return 1;
    }
    if (!mros::ssh::add_user(
            String(ctx.args[3].c_str()),
            String(ctx.args[4].c_str()),
            String(ctx.args[5].c_str()),
            admin,
            sudo_enabled)) {
      shell_write_line(
          ctx.state,
          "change: failed to add user (check username uniqueness and password length 8-96)");
      return 1;
    }
    shell_printf(
        ctx.state,
        "change: added user '%s' (%s, sudo=%s)\n",
        ctx.args[4].c_str(),
        admin ? "admin" : "normal",
        sudo_enabled ? "yes" : "no");
    return 0;
  }

  shell_printf(ctx.state, "change: unknown command '%s'\n", sub.c_str());
  return 1;
}

void shell_help_su(ShellState& state) {
  shell_write_line(state, "Usage: su \"root-password\"");
  shell_write_line(state, "Switch the current shell role to root. Direct SSH root login stays disabled.");
}

int shell_cmd_su(ShellContext& ctx) {
  if (ctx.args.size() < 2U) {
    shell_write_line(ctx.state, "su: password argument required in this non-interactive shell");
    return 1;
  }
  if (!mros::ssh::verify_password(mros::ssh::root_username(), String(ctx.args[1].c_str()))) {
    shell_write_line(ctx.state, "su: authentication failed");
    audit_record("su-failed", ctx.state.session_username.c_str());
    return 1;
  }
  if (!shell_can_enter_root(ctx.state)) {
    shell_write_line(ctx.state, "su: another root shell session is already active");
    audit_record("su-denied", "root session limit");
    return 1;
  }
  ctx.state.root_session = true;
  ctx.state.session_admin = true;
  ctx.state.session_can_sudo = true;
  ctx.state.capability_mask = kShellCapabilityRoot;
  audit_record("su", ctx.state.session_username.c_str());
  shell_write_line(ctx.state, "su: root role active");
  return 0;
}

void shell_help_exit(ShellState& state) {
  shell_write_line(state, "Usage: exit");
  shell_write_line(state, "Leave root role, or close an SSH session when transport support is active.");
}

int shell_cmd_exit(ShellContext& ctx) {
  if (ctx.state.root_session) {
    ctx.state.root_session = false;
    mros::ssh::UserAccount user;
    if (mros::ssh::get_user(String(ctx.state.session_username.c_str()), &user)) {
      ctx.state.session_admin = user.admin;
      ctx.state.session_can_sudo = user.sudo;
      ctx.state.capability_mask = user.admin ? kShellCapabilityAdmin : kShellCapabilityUserDefault;
    }
    audit_record("exit-root", ctx.state.session_username.c_str());
    shell_write_line(ctx.state, "exit: returned to user role");
    return 0;
  }
  if (ctx.transport == ShellTransport::Ssh || ctx.state.active_transport == ShellTransport::Ssh) {
    ctx.state.close_requested = true;
    shell_write_line(ctx.state, "exit: closing SSH session");
    return 0;
  }
  shell_write_line(ctx.state, "exit: no nested root role is active");
  return 0;
}

}  // namespace mros::shell
