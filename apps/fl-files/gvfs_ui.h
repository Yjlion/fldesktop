// fl-files: the dialogs of network locations (GVfs)
#pragma once
#include <string>
#include <vector>

#include "gvclient.h"

// A location asks for a user name and password (modal)
gv::PasswordReply gvfs_password_dialog(const gv::PasswordRequest &q);
// A question while mounting or unmounting: the chosen answer, -1 to cancel
int gvfs_question_dialog(const std::string &message, const std::vector<std::string> &choices);

// Connect to Server: the address entered ("" when cancelled). recent is shown
// to choose from (and can be cleared); bookmark tells whether to bookmark it.
std::string connect_dialog(std::vector<std::string> &recent, bool &bookmark);
