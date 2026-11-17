// SSH dialogs and actions for fl-term
#pragma once
#include <string>
#include <vector>

#include "ssh_profiles.h"

namespace ssh_ui {

std::string profiles_path();          // ~/.config/fldesktop/ssh.ini
std::string ssh_config_path();        // ~/.ssh/config
std::vector<ssh::Profile> profiles(); // saved + ~/.ssh/config hosts

// Resolves a --ssh argument: a profile name, or user@host[:port].
ssh::Profile resolve(const std::string &arg);

// Opens a new terminal window connected to the profile.
void connect(const ssh::Profile &p);
// The "Connect" dialog; `initial` pre-fills it (e.g. to edit a profile).
void connect_dialog(const ssh::Profile *initial = nullptr);
void manage_dialog();
void copy_key_dialog();
void generate_key();
// Mounts the host with sshfs (in a terminal, so passwords can be typed) and
// opens fl-files there.
void browse_files(const ssh::Profile &p);
void browse_files_dialog();
// Mount points under $XDG_RUNTIME_DIR/fl-sshfs that are currently mounted.
std::vector<std::string> sshfs_mounts();
void unmount(const std::string &mountpoint);
bool have_sshfs();

} // namespace ssh_ui
