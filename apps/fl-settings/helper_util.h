// Shared by the helpers Settings runs as root through pkexec.
#pragma once
#include <string>
#include <sys/types.h>

namespace fld {

// Prints m on stdout (the caller shows it) and returns 1.
int helper_fail(const std::string &m);
// The user who ran pkexec ($PKEXEC_UID), else getuid().
uid_t helper_caller_uid();
// Reads a regular file owned by `uid` without following symlinks.
bool read_user_file(const char *path, uid_t uid, size_t max, std::string &out, std::string &err);
// Writes path.new, fsyncs and renames it over path (mode 0644).
bool write_atomic(const std::string &path, const std::string &data, mode_t mode = 0644);

}  // namespace fld
