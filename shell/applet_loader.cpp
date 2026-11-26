#include "applet_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <set>

#include "fld/util.h"

namespace applets {

bool valid_id(const char *s) {
  if (!s || !*s || strlen(s) > 64) return false;
  for (const char *p = s; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
  return true;
}

std::vector<std::string> search_dirs(const std::string &libdir) {
  std::vector<std::string> dirs;
  if (const char *p = getenv("FLD_APPLET_PATH"))
    for (auto &d : fld::split(p, ':'))
      if (!d.empty()) dirs.push_back(d);
  dirs.push_back(fld::home_dir() + "/.local/lib/fldesktop/applets");
  dirs.push_back(libdir + "/fldesktop/applets");
  return dirs;
}

std::vector<Loaded> load(const std::vector<std::string> &dirs, const std::vector<std::string> &disabled,
                         const fld_applet_host &host, std::vector<std::string> *errors) {
  std::vector<Loaded> out;
  std::set<std::string> seen;
  auto problem = [&](const std::string &m) {
    fprintf(stderr, "fl-panel: applet %s\n", m.c_str());
    if (errors) errors->push_back(m);
  };
  for (auto &dir : dirs) {
    std::vector<std::string> files = fld::list_dir(dir);
    std::sort(files.begin(), files.end());
    for (auto &f : files) {
      if (f.size() < 4 || f.compare(f.size() - 3, 3, ".so") != 0) continue;
      std::string path = dir + "/" + f;
      void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (!h) {
        const char *e = dlerror();
        problem(path + ": " + (e ? e : "cannot be loaded"));
        continue;
      }
      auto entry = (fld_applet_entry_fn)dlsym(h, FLD_APPLET_ENTRY);
      const fld_applet *a = entry ? entry() : nullptr;
      if (!a) {
        problem(path + ": no " FLD_APPLET_ENTRY "()");
        dlclose(h);
        continue;
      }
      if (a->abi_version != FLD_APPLET_ABI_VERSION) {
        problem(path + ": built for applet ABI " + std::to_string(a->abi_version) + ", the panel has " +
                std::to_string(FLD_APPLET_ABI_VERSION));
        dlclose(h);
        continue;
      }
      if (!valid_id(a->id) || !a->create) {
        problem(path + ": no valid id or create()");
        dlclose(h);
        continue;
      }
      std::string id = a->id;
      if (seen.count(id)) {  // an earlier directory has it (the user's copy wins)
        dlclose(h);
        continue;
      }
      seen.insert(id);
      if (std::find(disabled.begin(), disabled.end(), id) != disabled.end()) {
        dlclose(h);
        continue;
      }
      Loaded l;
      l.id = id;
      l.name = a->name ? a->name : id;
      l.path = path;
      l.info = a;
      l.widget = a->create(&host);
      if (!l.widget) {
        problem(path + ": create() made nothing");
        continue;  // stays loaded: it may have started something
      }
      out.push_back(l);  // the library stays loaded for the panel's lifetime
    }
  }
  return out;
}

}  // namespace applets
