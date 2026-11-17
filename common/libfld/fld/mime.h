// shared-mime-info based type detection and default applications
#pragma once
#include <string>
#include <vector>
#include "fld/desktop_entry.h"

namespace fld {

// Mime type of a file: inode/directory, glob match on the name, then a small
// content sniff (text vs. binary, ELF, scripts). Never returns "".
std::string mime_type(const std::string &path);
// Mime type based on the file name only (or "" if no glob matches).
std::string mime_from_name(const std::string &name);
// Icon names to try for a mime type, most specific first.
std::vector<std::string> mime_icon_names(const std::string &mime);
// Human readable description ("PNG image"), falls back to the mime type.
std::string mime_description(const std::string &mime);
// Parent types (text/x-csrc -> text/plain), most specific first, excluding mime itself.
std::vector<std::string> mime_parents(const std::string &mime);

// Applications that can open the mime type; the default (if any) first.
std::vector<DesktopEntry> apps_for_mime(const std::string &mime);
bool default_app_for_mime(const std::string &mime, DesktopEntry &out);
bool set_default_app_for_mime(const std::string &mime, const std::string &desktop_id);

// Opens a file or URL with its default application (falls back to xdg-open).
bool open_path(const std::string &path);
// Archives and compressed files (zip, tar.*, 7z, rar, iso, ...) that
// fl-archive opens.
bool is_archive_mime(const std::string &mime);

} // namespace fld
