#include "fonts_util.h"

#include <algorithm>
#include <cstdlib>

#include "fld/util.h"

namespace fonts {

std::vector<Family> group(const std::vector<Face> &faces, const std::string &home) {
  std::map<std::string, Family> by;
  for (const Face &f : faces) {
    std::string name = fld::trim(f.family.substr(0, f.family.find(',')));
    if (name.empty()) continue;
    Family &fam = by[name];
    if (fam.name.empty()) {
      fam.name = name;
      fam.user = true;
    }
    Face g = f;
    g.family = name;
    g.style = fld::trim(g.style.substr(0, g.style.find(',')));
    // the same face twice (two formats of one font): keep one
    bool dup = false;
    for (const Face &o : fam.faces)
      if (o.style == g.style) dup = true;
    if (!dup) fam.faces.push_back(g);
    if (!is_user_font(f.file, home)) fam.user = false;
  }
  std::vector<Family> out;
  for (auto &p : by) {
    Family f = p.second;
    std::sort(f.faces.begin(), f.faces.end(), [](const Face &a, const Face &b) {
      if (a.slant != b.slant) return a.slant < b.slant;
      int da = std::abs(a.weight - 80), db = std::abs(b.weight - 80);
      if (da != db) return da < db;
      if (a.weight != b.weight) return a.weight < b.weight;
      return a.style < b.style;
    });
    std::string l = fld::lower(f.name);
    f.monospace = l.find("mono") != std::string::npos || l.find("code") != std::string::npos ||
                  l.find("courier") != std::string::npos || l.find("console") != std::string::npos;
    out.push_back(f);
  }
  std::sort(out.begin(), out.end(), [](const Family &a, const Family &b) { return fld::lower(a.name) < fld::lower(b.name); });
  return out;
}

bool is_font_file(const std::string &name) {
  std::string l = fld::lower(name);
  for (const char *e : {".ttf", ".otf", ".ttc", ".otc", ".pfb", ".pcf", ".pcf.gz", ".woff", ".woff2"})
    if (fld::ends_with(l, e)) return true;
  return false;
}

bool is_user_font(const std::string &file, const std::string &home) {
  for (const std::string &d : {home + "/.local/share/fonts/", home + "/.fonts/"})
    if (fld::starts_with(file, d)) return true;
  return false;
}

std::string install_path(const std::string &fonts_dir, const std::string &file) {
  std::string name = fld::basename(file);
  std::string p = fonts_dir + "/" + name;
  if (!fld::file_exists(p)) return p;
  size_t dot = name.find('.');
  std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
  std::string ext = dot == std::string::npos ? "" : name.substr(dot);
  for (int i = 2;; i++) {
    p = fonts_dir + "/" + stem + "-" + std::to_string(i) + ext;
    if (!fld::file_exists(p)) return p;
  }
}

} // namespace fonts
