#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/util.h"

#include <FL/Fl.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_SVG_Image.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/Fl_XPM_Image.H>

#include <climits>
#include <cstdlib>
#include <map>
#include <memory>

namespace fld {

namespace {

struct ThemeDir {
  std::string subdir;
  int size = 48, min = 48, max = 48, threshold = 2, scale = 1;
  enum { Fixed, Scalable, Threshold } type = Threshold;
};

struct Theme {
  std::string name;
  std::vector<std::string> bases;   // existing <base>/<name> directories
  std::vector<ThemeDir> dirs;
  std::vector<std::string> parents;
};

std::vector<std::string> icon_bases() {
  std::vector<std::string> b{home_dir() + "/.icons"};
  for (auto &d : data_dirs()) b.push_back(path_join(d, "icons"));
  return b;
}

std::map<std::string, std::unique_ptr<Theme>> &theme_cache() {
  static std::map<std::string, std::unique_ptr<Theme>> c;
  return c;
}

Theme *load_theme(const std::string &name) {
  auto &cache = theme_cache();
  auto it = cache.find(name);
  if (it != cache.end()) return it->second.get();
  auto t = std::make_unique<Theme>();
  t->name = name;
  Ini idx;
  bool have_index = false;
  for (auto &b : icon_bases()) {
    std::string d = path_join(b, name);
    if (!is_dir(d)) continue;
    t->bases.push_back(d);
    if (!have_index && idx.load(path_join(d, "index.theme"))) have_index = true;
  }
  if (!have_index) {
    cache[name] = nullptr;
    return nullptr;
  }
  const std::string g = "Icon Theme";
  for (auto &p : split(idx.get(g, "Inherits"), ',')) t->parents.push_back(trim(p));
  std::vector<std::string> dl = split(idx.get(g, "Directories"), ',');
  for (auto &s : split(idx.get(g, "ScaledDirectories"), ',')) dl.push_back(s);
  for (auto &sd : dl) {
    std::string s = trim(sd);
    if (s.empty() || !idx.has(s, "Size")) continue;
    ThemeDir d;
    d.subdir = s;
    d.size = idx.get_int(s, "Size", 48);
    d.scale = idx.get_int(s, "Scale", 1);
    d.min = idx.get_int(s, "MinSize", d.size);
    d.max = idx.get_int(s, "MaxSize", d.size);
    d.threshold = idx.get_int(s, "Threshold", 2);
    std::string ty = idx.get(s, "Type", "Threshold");
    d.type = ty == "Fixed" ? ThemeDir::Fixed : ty == "Scalable" ? ThemeDir::Scalable : ThemeDir::Threshold;
    t->dirs.push_back(d);
  }
  Theme *raw = t.get();
  cache[name] = std::move(t);
  return raw;
}

int dir_distance(const ThemeDir &d, int size) {
  switch (d.type) {
    case ThemeDir::Fixed: return abs(d.size * d.scale - size);
    case ThemeDir::Scalable:
      if (size < d.min * d.scale) return d.min * d.scale - size;
      if (size > d.max * d.scale) return size - d.max * d.scale;
      return 0;
    default:
      if (size < (d.size - d.threshold) * d.scale) return d.min * d.scale - size;
      if (size > (d.size + d.threshold) * d.scale) return size - d.max * d.scale;
      return 0;
  }
}

const char *kExts[] = {".png", ".svg", ".xpm"};

std::string lookup_in_theme(Theme *t, const std::string &name, int size) {
  std::string best;
  int best_dist = INT_MAX;
  for (auto &d : t->dirs) {
    int dist = dir_distance(d, size);
    if (dist >= best_dist) continue;
    for (auto &b : t->bases) {
      for (auto ext : kExts) {
        std::string p = b + "/" + d.subdir + "/" + name + ext;
        if (file_exists(p)) {
          best = p;
          best_dist = dist;
          goto next_dir;
        }
      }
    }
  next_dir:
    if (best_dist == 0) break;
  }
  return best;
}

std::string lookup_recursive(const std::string &theme, const std::string &name, int size,
                             int depth, std::vector<std::string> &visited) {
  if (depth > 8) return "";
  for (auto &v : visited) if (v == theme) return "";
  visited.push_back(theme);
  Theme *t = load_theme(theme);
  if (!t) return "";
  std::string r = lookup_in_theme(t, name, size);
  if (!r.empty()) return r;
  for (auto &p : t->parents) {
    r = lookup_recursive(p, name, size, depth + 1, visited);
    if (!r.empty()) return r;
  }
  return "";
}

} // namespace

std::string icon_theme_name() {
  static std::string name;
  if (name.empty()) {
    Ini cfg;
    cfg.load(fld_config_dir() + "/desktop.ini");
    name = cfg.get("General", "IconTheme", "Adwaita");
  }
  return name;
}

std::string icon_path(const std::string &name, int size) {
  if (name.empty()) return "";
  if (name[0] == '/') return file_exists(name) ? name : "";
  static std::map<std::string, std::string> cache;
  std::string key = name + "@" + std::to_string(size);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  // Some .desktop files give "foo.png" without a path
  std::string n = name;
  for (auto ext : kExts)
    if (ends_with(n, ext)) n = n.substr(0, n.size() - 4);
  std::vector<std::string> visited;
  std::string r = lookup_recursive(icon_theme_name(), n, size, 0, visited);
  if (r.empty()) r = lookup_recursive("hicolor", n, size, 0, visited);
  if (r.empty()) {
    for (auto &d : data_dirs()) {
      for (auto ext : kExts) {
        std::string p = path_join(d, "pixmaps/" + n + ext);
        if (file_exists(p)) { r = p; break; }
      }
      if (!r.empty()) break;
    }
  }
  cache[key] = r;
  return r;
}

Fl_Image *load_image(const std::string &path) {
  static bool registered = false;
  if (!registered) { fl_register_images(); registered = true; }
  std::string ext = extension(path);
  Fl_Image *img = nullptr;
  if (ext == "svg" || ext == "svgz") img = new Fl_SVG_Image(path.c_str());
  else if (ext == "xpm") img = new Fl_XPM_Image(path.c_str());
  else if (ext == "png") img = new Fl_PNG_Image(path.c_str());
  else {
    Fl_Shared_Image *s = Fl_Shared_Image::get(path.c_str());
    if (!s) return nullptr;
    if (s->fail() || s->w() <= 0) { s->release(); return nullptr; }
    img = s->copy();
    s->release();
  }
  if (img && (img->fail() || img->w() <= 0 || img->h() <= 0)) {
    delete img;
    return nullptr;
  }
  return img;
}

Fl_Image *icon(const std::string &name, int size) {
  static std::map<std::string, Fl_Image *> cache;
  std::string key = name + "@" + std::to_string(size);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  Fl_Image *img = nullptr;
  std::string p = icon_path(name, size);
  if (!p.empty()) img = load_image(p);
  if (img) {
    // FLTK 1.4: draw at size x size points, keeping full resolution for HiDPI
    if (img->w() != size || img->h() != size) {
      if (extension(p) == "svg") {
        img->scale(size, size, 1, 1);
      } else {
        // pre-scale raster images for quality, keeping 2x pixels for HiDPI
        int px = size * 2;
        if (img->w() > px || img->h() > px) {
          Fl_Image *c = img->copy(px, px);
          delete img;
          img = c;
        }
        img->scale(size, size, 1, 1);
      }
    }
  }
  cache[key] = img;
  return img;
}

Fl_Image *icon(const std::vector<std::string> &names, int size) {
  for (auto &n : names)
    if (Fl_Image *i = icon(n, size)) return i;
  return nullptr;
}

} // namespace fld
