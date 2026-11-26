#include "helper_args.h"

#include <cstring>

#include "fld/util.h"
#include "datetime_sys.h"
#include "sharing_util.h"
#include "users_model.h"

namespace fld {
namespace helper {

namespace {

using Args = std::vector<std::string>;
using Check = std::string (*)(const Args &);  // a[0] is the command

struct Command {
  const char *group, *name;
  size_t min, max;  // argument count, without the group and the command
  Check check;
};

std::string ok() { return ""; }

bool on_off(const std::string &s) { return s == "on" || s == "off"; }

// a file the helper reads as the calling user: absolute, not too long
std::string user_path(const std::string &p, const char *what) {
  if (p.empty() || p[0] != '/' || p.size() > 4096) return std::string(what) + " must be an absolute path";
  for (unsigned char c : p)
    if (c < 0x20 || c == 0x7f) return std::string(what) + " has control characters";
  return ok();
}

// Region/City as in /usr/share/zoneinfo: no "..", no leading /
std::string zone_name(const std::string &z) {
  if (z.empty() || z.size() > 128 || z[0] == '/' || z.find("..") != std::string::npos)
    return "not a time zone name: " + z;
  for (unsigned char c : z)
    if (!(isalnum(c) || c == '/' || c == '_' || c == '-' || c == '+')) return "not a time zone name: " + z;
  return ok();
}

std::string user_name(const std::string &n) {
  std::string err;
  return users::valid_user_name(n, err) ? ok() : err;
}

std::string group_list(const std::string &g) {
  if (g == "-") return ok();
  if (g.empty() || g.size() > 4096 || g.front() == ',' || g.back() == ',' || g.find(",,") != std::string::npos)
    return "not a list of groups";
  for (auto &name : split(g, ',')) {
    if (name.empty() || name.size() > 32 || name[0] == '-') return "not a group name: " + name;
    for (unsigned char c : name)
      if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return "not a group name: " + name;
  }
  return ok();
}

std::string admin_flag(const std::string &s) { return s == "0" || s == "1" ? ok() : "admin must be 0 or 1"; }

std::string full_name(const std::string &n) {
  std::string err;
  return users::valid_full_name(n, err) ? ok() : err;
}

const Command kCommands[] = {
    {"system", "greeter-ini", 1, 2,
     [](const Args &a) {
       std::string e = user_path(a[1], "the settings file");
       if (e.empty() && a.size() > 2) e = user_path(a[2], "the background");
       return e;
     }},
    {"system", "timezone", 1, 1, [](const Args &a) { return zone_name(a[1]); }},
    {"system", "set-time", 1, 1,
     [](const Args &a) { return dtsys::valid_time(a[1]) ? ok() : "not a date and time: " + a[1]; }},
    {"system", "ntp", 1, 1, [](const Args &a) { return on_off(a[1]) ? ok() : std::string("ntp on|off"); }},
    {"system", "hostname", 1, 1,
     [](const Args &a) { return sharing::valid_hostname(a[1]) ? ok() : "not a valid computer name: " + a[1]; }},
    {"system", "avahi", 1, 1, [](const Args &a) { return on_off(a[1]) ? ok() : std::string("avahi on|off"); }},
    {"system", "publish", 2, 2,
     [](const Args &a) {
       if (!sharing::find_published(a[1])) return "unknown service: " + a[1];
       return on_off(a[2]) ? ok() : std::string("publish <id> on|off");
     }},
    {"users", "add", 3, 3,
     [](const Args &a) {
       std::string e = user_name(a[1]);
       if (e.empty()) e = full_name(a[2]);
       if (e.empty()) e = admin_flag(a[3]);
       return e;
     }},
    {"users", "delete", 2, 2,
     [](const Args &a) {
       std::string e = user_name(a[1]);
       if (e.empty() && a[2] != "keep" && a[2] != "remove-home") e = "delete <name> keep|remove-home";
       return e;
     }},
    {"users", "modify", 4, 4,
     [](const Args &a) {
       std::string e = user_name(a[1]);
       if (e.empty()) e = full_name(a[2]);
       if (e.empty()) e = admin_flag(a[3]);
       if (e.empty()) e = group_list(a[4]);
       return e;
     }},
    {"users", "password", 1, 1, [](const Args &a) { return user_name(a[1]); }},
    {"users", "picture", 2, 2,
     [](const Args &a) {
       std::string e = user_name(a[1]);
       if (e.empty() && a[2] != "-") e = user_path(a[2], "the picture");
       return e;
     }},
    {"users", "autologin", 1, 1, [](const Args &a) { return a[1] == "-" ? ok() : user_name(a[1]); }},
};

}  // namespace

std::string check(const std::vector<std::string> &args) {
  if (args.size() < 2) return usage();
  for (auto &c : kCommands) {
    if (args[0] != c.group || args[1] != c.name) continue;
    size_t n = args.size() - 2;
    if (n < c.min || n > c.max) return std::string("wrong number of arguments for ") + c.group + " " + c.name;
    for (auto &a : args)
      if (a.size() > 8192) return "an argument is too long";
    return c.check(Args(args.begin() + 1, args.end()));
  }
  return "unknown command: " + args[0] + " " + args[1];
}

const char *usage() {
  return "usage: fl-settings-helper system greeter-ini <new.ini> [<image>] | system timezone <zone> | "
         "system set-time <time> | system ntp on|off | system hostname <name> | system avahi on|off | "
         "system publish <id> on|off | users add|delete|modify|password|picture|autologin ...";
}

}  // namespace helper
}  // namespace fld
