// fl-users-helper: the root part of Settings > Users, run with pkexec.
//
//   fl-users-helper add <name> <full name> <admin 0|1>        (password on stdin)
//   fl-users-helper delete <name> keep|remove-home
//   fl-users-helper modify <name> <full name> <admin 0|1> <groups a,b|->
//   fl-users-helper password <name>                            (password on stdin)
//   fl-users-helper picture <name> <image file|->
//   fl-users-helper autologin <name|->
//
// Only accounts of people (uid 1000-59999 with a login shell) are touched.
// It refuses to delete the user who runs it or someone who is logged in, and
// to leave the computer without an administrator. Errors go to stdout.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "fld/avatar.h"
#include "fld/ini.h"
#include "fld/util.h"
#include "greeter_config.h"
#include "helper_util.h"
#include "users_model.h"

using namespace fld;
using namespace fld::users;

namespace {

const char *kGreetdConfig = "/etc/greetd/config.toml";

struct Db {
  std::vector<Group> groups;
  std::vector<Account> accounts;
  std::string admin;
};

Db load_db() {
  Db d;
  std::string p, g;
  read_file("/etc/passwd", p);
  read_file("/etc/group", g);
  d.groups = parse_group(g);
  d.accounts = parse_passwd(p);
  d.admin = admin_group(d.groups);
  attach_groups(d.accounts, d.groups, d.admin);
  return d;
}

// runs a tool by absolute path; its output becomes the error text
bool tool(const std::vector<std::string> &argv, std::string &err, const std::string &input = "") {
  std::string out;
  int rc = run_capture_input(argv, input, out, true);
  if (rc == 0) return true;
  err = trim(out);
  if (err.empty()) err = argv[0] + " failed (" + std::to_string(rc) + ")";
  return false;
}

std::string find_tool(const char *name) {
  for (const char *d : {"/usr/sbin/", "/usr/bin/", "/sbin/", "/bin/"}) {
    std::string p = std::string(d) + name;
    if (access(p.c_str(), X_OK) == 0) return p;
  }
  return "";
}

bool need_tools(std::string &err, std::initializer_list<const char *> names) {
  for (auto n : names)
    if (find_tool(n).empty()) {
      err = std::string(n) + " is not installed";
      return false;
    }
  return true;
}

std::string read_password() {
  std::string pw;
  char buf[256];
  ssize_t n;
  while (pw.size() < 4096 && ((n = read(0, buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR)))
    if (n > 0) pw.append(buf, (size_t)n);
  while (!pw.empty() && (pw.back() == '\n' || pw.back() == '\r')) pw.pop_back();
  return pw;
}

bool logged_in(const Account &a) {
  // systemd-logind keeps /run/user/<uid> while the user has a session
  return is_dir("/run/user/" + std::to_string(a.uid));
}

std::string login_shell() {
  for (const char *s : {"/bin/bash", "/usr/bin/bash", "/bin/zsh", "/bin/sh"})
    if (access(s, X_OK) == 0) return s;
  return "/bin/sh";
}

bool store_picture(const std::string &user, const std::string &img);

int cmd_add(int argc, char **argv) {
  if (argc != 5) return helper_fail("usage: add <name> <full name> <admin 0|1>");
  std::string name = argv[2], full = argv[3], err;
  bool admin = std::string(argv[4]) == "1";
  if (!valid_user_name(name, err) || !valid_full_name(full, err)) return helper_fail(err);
  std::string pw = read_password();
  if (!valid_password(pw, err)) return helper_fail(err);
  Db d = load_db();
  std::string all;
  read_file("/etc/passwd", all);
  for (auto &line : split(all, '\n'))
    if (starts_with(line, name + ":")) return helper_fail("There is already a user called " + name + ".");
  if (find_group(d.groups, name)) return helper_fail("There is already a group called " + name + ".");
  if (admin && d.admin.empty()) return helper_fail("This computer has no wheel or sudo group for administrators.");
  if (!need_tools(err, {"useradd", "chpasswd", "userdel"})) return helper_fail(err);
  std::vector<std::string> cmd{find_tool("useradd"), "-m", "-s", login_shell(), "-c", full};
  if (admin) cmd.insert(cmd.end(), {"-G", d.admin});
  cmd.insert(cmd.end(), {"--", name});
  if (!tool(cmd, err)) return helper_fail(err);
  if (!tool({find_tool("chpasswd")}, err, name + ":" + pw + "\n")) {
    std::string e2;
    tool({find_tool("userdel"), "-r", "--", name}, e2);  // do not leave an account without a password
    return helper_fail("Cannot set the password: " + err);
  }
  // new accounts start with a picture: their initial on a color of their own
  std::string png;
  if (initial_avatar_png(name, full, 256, png)) store_picture(name, png);
  return 0;
}

// a person's account that may be changed
const Account *target(const Db &d, const std::string &name, std::string &err) {
  const Account *a = find_account(d.accounts, name);
  if (!a) err = name + " is not a user account that Settings manages.";
  return a;
}

int cmd_delete(int argc, char **argv) {
  if (argc != 4) return helper_fail("usage: delete <name> keep|remove-home");
  std::string err, mode = argv[3];
  if (mode != "keep" && mode != "remove-home") return helper_fail("usage: delete <name> keep|remove-home");
  Db d = load_db();
  const Account *a = target(d, argv[2], err);
  if (!a) return helper_fail(err);
  if (a->uid == helper_caller_uid()) return helper_fail("You cannot remove your own account.");
  if (logged_in(*a)) return helper_fail(a->name + " is logged in. Log them out first.");
  if (a->admin && admin_count(d.accounts) <= 1) return helper_fail("This is the only administrator: it cannot be removed.");
  if (!need_tools(err, {"userdel"})) return helper_fail(err);
  std::vector<std::string> cmd{find_tool("userdel")};
  if (mode == "remove-home") cmd.push_back("-r");
  cmd.insert(cmd.end(), {"--", a->name});
  if (!tool(cmd, err)) return helper_fail(err);
  unlink(picture_path(a->name).c_str());
  std::string toml;
  if (read_file(kGreetdConfig, toml) && autologin_user(toml) == a->name)
    write_atomic(kGreetdConfig, set_autologin(toml, "", ""));
  return 0;
}

int cmd_modify(int argc, char **argv) {
  if (argc != 6) return helper_fail("usage: modify <name> <full name> <admin 0|1> <groups a,b|->");
  std::string err, full = argv[3], gl = argv[5];
  bool admin = std::string(argv[4]) == "1";
  Db d = load_db();
  const Account *a = target(d, argv[2], err);
  if (!a) return helper_fail(err);
  if (!valid_full_name(full, err)) return helper_fail(err);
  if (!need_tools(err, {"usermod", "gpasswd"})) return helper_fail(err);
  if (a->admin && !admin && admin_count(d.accounts) <= 1)
    return helper_fail("This is the only administrator: it has to stay one.");
  if (admin && d.admin.empty()) return helper_fail("This computer has no wheel or sudo group for administrators.");

  std::vector<std::pair<std::string, bool>> changes;  // group, add
  if (admin != a->admin) changes.push_back({d.admin, admin});
  if (gl != "-") {
    auto editable = editable_groups(d.groups, d.accounts, d.admin);
    std::vector<std::string> want = split(gl, ',');
    for (auto &w : want)
      if (std::find(editable.begin(), editable.end(), w) == editable.end())
        return helper_fail("The group " + w + " cannot be changed here.");
    for (auto &g : editable) {
      bool on = std::find(want.begin(), want.end(), g) != want.end();
      if (on != a->in(g)) changes.push_back({g, on});
    }
  }
  if (full != a->real_name && !tool({find_tool("usermod"), "-c", full, "--", a->name}, err)) return helper_fail(err);
  for (auto &c : changes)
    if (!tool({find_tool("gpasswd"), c.second ? "-a" : "-d", a->name, c.first}, err)) return helper_fail(err);
  return 0;
}

int cmd_password(int argc, char **argv) {
  if (argc != 3) return helper_fail("usage: password <name>");
  std::string err;
  Db d = load_db();
  const Account *a = target(d, argv[2], err);
  if (!a) return helper_fail(err);
  std::string pw = read_password();
  if (!valid_password(pw, err)) return helper_fail(err);
  if (!need_tools(err, {"chpasswd"})) return helper_fail(err);
  if (!tool({find_tool("chpasswd")}, err, a->name + ":" + pw + "\n")) return helper_fail(err);
  return 0;
}

// AccountsService (when installed) keeps its own record of the picture
void set_accounts_icon(const std::string &user, const std::string &icon) {
  std::string users_dir = "/var/lib/AccountsService/users";
  if (!is_dir(users_dir)) return;
  Ini rec;
  std::string p = users_dir + "/" + user;
  rec.load(p);
  rec.set("User", "Icon", icon);
  write_atomic(p, rec.to_string(), 0600);
}

// the picture the greeter, the lock screen and Settings show
bool store_picture(const std::string &user, const std::string &img) {
  std::string dest = picture_path(user);
  mkdir_p("/var/lib/AccountsService/icons", 0755);
  if (!write_atomic(dest, img)) return false;
  set_accounts_icon(user, dest);
  return true;
}

int cmd_picture(int argc, char **argv) {
  if (argc != 4) return helper_fail("usage: picture <name> <image file|->");
  std::string err;
  Db d = load_db();
  const Account *a = target(d, argv[2], err);
  if (!a) return helper_fail(err);
  if (std::string(argv[3]) == "-") {
    unlink(picture_path(a->name).c_str());
    set_accounts_icon(a->name, "");
    return 0;
  }
  std::string img;
  if (!read_user_file(argv[3], helper_caller_uid(), 8u << 20, img, err)) return helper_fail(err);
  std::string ext = image_type(img);
  if (ext != "png" && ext != "jpg") return helper_fail("The picture has to be a PNG or JPEG image.");
  if (!store_picture(a->name, img)) return helper_fail("Cannot write " + picture_path(a->name));
  return 0;
}

int cmd_autologin(int argc, char **argv) {
  if (argc != 3) return helper_fail("usage: autologin <name|->");
  std::string err, name = argv[2];
  if (name == "-") name.clear();
  if (!name.empty()) {
    Db d = load_db();
    if (!target(d, name, err)) return helper_fail(err);
  }
  std::string toml;
  if (!read_file(kGreetdConfig, toml))
    return helper_fail("greetd is not set up (no /etc/greetd/config.toml): automatic login needs it.");
  if (!write_atomic(kGreetdConfig, set_autologin(toml, name, "fl-session")))
    return helper_fail(std::string("Cannot write ") + kGreetdConfig);
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) return helper_fail("usage: fl-users-helper add|delete|modify|password|picture|autologin ...");
  if (geteuid() != 0) return helper_fail("must run as root (through pkexec)");
  umask(022);
  std::string c = argv[1];
  if (c == "add") return cmd_add(argc, argv);
  if (c == "delete") return cmd_delete(argc, argv);
  if (c == "modify") return cmd_modify(argc, argv);
  if (c == "password") return cmd_password(argc, argv);
  if (c == "picture") return cmd_picture(argc, argv);
  if (c == "autologin") return cmd_autologin(argc, argv);
  return helper_fail("unknown command " + c);
}
