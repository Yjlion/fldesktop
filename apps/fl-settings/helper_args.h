// The commands fl-settings-helper accepts, and the checks on their arguments
// made before anything runs as root.
//
//   fl-settings-helper system greeter-ini <new.ini> [<background image>]
//   fl-settings-helper system timezone <Region/City>
//   fl-settings-helper system set-time "YYYY-MM-DD HH:MM[:SS]"
//   fl-settings-helper system ntp on|off
//   fl-settings-helper system hostname <name>
//   fl-settings-helper system avahi on|off
//   fl-settings-helper system publish <id> on|off
//   fl-settings-helper users add <name> <full name> 0|1          (password on stdin)
//   fl-settings-helper users delete <name> keep|remove-home
//   fl-settings-helper users modify <name> <full name> 0|1 <groups a,b|->
//   fl-settings-helper users password <name>                     (password on stdin)
//   fl-settings-helper users picture <name> <image file|->
//   fl-settings-helper users autologin <name|->
//
// The first word picks the polkit action (exec.argv1 in the policy):
// "system" is org.fldesktop.settings.login-screen, "users" is
// org.fldesktop.settings.users.
#pragma once
#include <string>
#include <vector>

namespace fld {
namespace helper {

// args: the words after the program name. Returns an empty string when the
// command is on the list and its arguments are well formed, else the reason.
std::string check(const std::vector<std::string> &args);

// The usage text listing every command.
const char *usage();

}  // namespace helper
}  // namespace fld
