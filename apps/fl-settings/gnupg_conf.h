// ~/.gnupg/gpg-agent.conf: which pinentry gpg-agent runs (plain logic, tested)
#pragma once
#include <string>

namespace gnupg_conf {
// the pinentry-program set in the text, "" when none
std::string pinentry(const std::string &conf);
// the text with pinentry-program set to path ("" removes it); other lines kept
std::string with_pinentry(const std::string &conf, const std::string &path);
// $GNUPGHOME/gpg-agent.conf, else ~/.gnupg/gpg-agent.conf
std::string conf_path();
} // namespace gnupg_conf
