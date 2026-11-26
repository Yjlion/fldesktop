// fl-panel: finds and loads applet plugins (fld_applet.h).
#pragma once
#include <string>
#include <vector>

#include "fld_applet.h"

namespace applets {

struct Loaded {
  std::string id, name, path;
  const fld_applet *info = nullptr;
  void *widget = nullptr;  // Fl_Widget*
};

// The directories searched, first wins for an id: $FLD_APPLET_PATH
// (colon separated), ~/.local/lib/fldesktop/applets, then libdir.
std::vector<std::string> search_dirs(const std::string &libdir);

// Loads every applet in dirs that is not disabled and creates its widget in
// the current FLTK group. Problems (no entry point, other ABI version, a
// duplicate id) are reported on stderr and in errors.
std::vector<Loaded> load(const std::vector<std::string> &dirs, const std::vector<std::string> &disabled,
                         const fld_applet_host &host, std::vector<std::string> *errors = nullptr);

// whether s is a valid applet id ([a-z0-9-], 1-64)
bool valid_id(const char *s);

}  // namespace applets
