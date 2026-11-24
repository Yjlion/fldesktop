#pragma once
#include <string>
// the password dialog of "fl-secretd --ask"; returns the exit status
int ask_main(const std::string &mode, const std::string &label, const std::string &message);
