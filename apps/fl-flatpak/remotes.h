// Flatpak sources (remotes): what fl-flatpak shows and the flatpak command
// lines that change them (plain logic, unit tested without libflatpak).
#pragma once
#include <string>
#include <vector>

namespace fp {

struct Remote {
  std::string name, title, url, installation; // installation: "system" / "user"
  int prio = 1;
  bool disabled = false, gpg_verify = true;
  int installed_refs = 0; // apps and runtimes installed from it
};

// the command lines (flatpak talks to its system helper, which asks polkit)
// gpg_key: a key file to import, "" for none given, "-" for an unsigned source
std::vector<std::string> remote_add_cmd(const std::string &installation, const std::string &name,
                                        const std::string &url, const std::string &title, const std::string &gpg_key);
std::vector<std::string> remote_modify_cmd(const Remote &r);
std::vector<std::string> remote_delete_cmd(const Remote &r, bool force);
// a usable remote name from a title or URL ("Flathub Beta" -> "flathub-beta",
// https://dl.flathub.org/beta-repo/flathub-beta.flatpakrepo -> "flathub-beta")
std::string remote_name_for(const std::string &title_or_url);

} // namespace fp
