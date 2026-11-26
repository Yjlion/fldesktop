// Users page model: local accounts and groups from /etc/passwd and
// /etc/group, the checks Settings and fl-settings-helper both make, and the
// greetd automatic login section. No UI, no root: unit tested.
#pragma once
#include <string>
#include <sys/types.h>
#include <vector>

namespace fld {
namespace users {

struct Group {
  std::string name;
  gid_t gid = 0;
  std::vector<std::string> members;
  bool has(const std::string &user) const;
};

struct Account {
  std::string name, real_name, home, shell;
  uid_t uid = 0;
  gid_t gid = 0;
  std::vector<std::string> groups;  // supplementary groups (from /etc/group)
  bool admin = false;               // member of the admin group
  bool in(const std::string &group) const;
};

// the lowest and highest uid of the people's accounts the page manages
constexpr uid_t kMinUid = 1000;
constexpr uid_t kMaxUid = 60000;

std::vector<Group> parse_group(const std::string &text);
// Login accounts with kMinUid <= uid < kMaxUid and a real shell, sorted by name.
std::vector<Account> parse_passwd(const std::string &text);
// Fills groups and admin of each account.
void attach_groups(std::vector<Account> &accounts, const std::vector<Group> &groups, const std::string &admin);
// "wheel" (Arch, Fedora) when it exists, else "sudo" (Debian), else "".
std::string admin_group(const std::vector<Group> &groups);
const Group *find_group(const std::vector<Group> &groups, const std::string &name);
const Account *find_account(const std::vector<Account> &accounts, const std::string &name);
// Groups a user can be added to or removed from on the page: the usual
// device and service groups that exist, plus other groups with gid >= 1000
// that are not a user's own (private) group. The admin group is left out
// (it is the account type). Sorted.
std::vector<std::string> editable_groups(const std::vector<Group> &groups, const std::vector<Account> &accounts,
                                         const std::string &admin);
int admin_count(const std::vector<Account> &accounts);

// Checks; false sets err to a sentence to show.
bool valid_user_name(const std::string &name, std::string &err);
bool valid_full_name(const std::string &name, std::string &err);
bool valid_password(const std::string &pw, std::string &err);
// "Ana María López" -> "ana"; "" when nothing usable
std::string suggest_user_name(const std::string &full_name);
// 0 weak, 1 fair, 2 good, 3 strong
int password_strength(const std::string &pw);
const char *strength_text(int s);

// The picture fl-lock and fl-greet show for a user (written by the helper).
std::string picture_path(const std::string &user);  // /var/lib/AccountsService/icons/<user>

// /etc/greetd/config.toml [initial_session]: the user logged in automatically
// at boot, "" when none.
std::string autologin_user(const std::string &toml);
// Returns the config with [initial_session] removed (user "") or set to
// user and command; other sections and comments are kept.
std::string set_autologin(const std::string &toml, const std::string &user, const std::string &command);

}  // namespace users
}  // namespace fld
