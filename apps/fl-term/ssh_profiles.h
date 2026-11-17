// SSH connection profiles for fl-term (no FLTK dependency)
#pragma once
#include <string>
#include <vector>

namespace ssh {

struct Profile {
  std::string name;      // display name / profile key
  std::string host;      // host name, address or ssh_config Host alias
  std::string user;
  int port = 0;          // 0: default
  std::string identity;  // private key file
  std::string jump;      // ProxyJump, e.g. user@bastion
  std::vector<std::string> forwards; // "L 8080:localhost:80", "R 9000:localhost:9000", "D 1080"
  std::string options;   // extra ssh arguments, shell-quoted
  std::string command;   // remote command; empty: login shell
  std::string scheme;    // fl-term color scheme for this connection; empty: default
  bool from_ssh_config = false; // read from ~/.ssh/config (not editable here)

  std::string target() const { return user.empty() ? host : user + "@" + host; }
};

// Builds the ssh command line for a profile.
std::vector<std::string> build_argv(const Profile &p);
// Parses "user@host:port" (or "host", "user@host") into a profile.
Profile parse_target(const std::string &s);

// Concrete Host entries of an OpenSSH client config (wildcard patterns and
// negations are skipped; Include is followed, relative to ~/.ssh).
std::vector<Profile> read_ssh_config(const std::string &path, const std::string &ssh_dir);

// Saved profiles in an INI file, one [Profile name] section each.
std::vector<Profile> load_profiles(const std::string &ini_path);
bool save_profiles(const std::string &ini_path, const std::vector<Profile> &profiles);

// All profiles: saved ones first, then ssh_config hosts that have no saved
// profile of the same name.
std::vector<Profile> all_profiles(const std::string &ini_path, const std::string &ssh_config,
                                  const std::string &ssh_dir);
// Finds a profile by name (case-sensitive), or nullptr.
const Profile *find(const std::vector<Profile> &v, const std::string &name);

} // namespace ssh
