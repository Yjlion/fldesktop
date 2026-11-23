#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"

#include <FL/Fl.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_BMP_Image.H>
#include <FL/Fl_GIF_Image.H>
#include <FL/Fl_ICO_Image.H>
#include <FL/Fl_JPEG_Image.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_PNM_Image.H>
#include <FL/Fl_SVG_Image.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/Fl_XPM_Image.H>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
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

// FLTK renders SVG with nanosvg, which ignores masks, filters and clip paths;
// icons using them come out as black blobs, so such files are skipped.
bool svg_usable(const std::string &path) {
  static std::map<std::string, bool> cache;
  auto it = cache.find(path);
  if (it != cache.end()) return it->second;
  std::string text;
  bool ok = read_file(path, text, 1 << 20) && text.find("<mask") == std::string::npos &&
            text.find("<filter") == std::string::npos && text.find("<clipPath") == std::string::npos;
  cache[path] = ok;
  return ok;
}

bool usable_file(const std::string &p) {
  if (!file_exists(p)) return false;
  return !ends_with(p, ".svg") || svg_usable(p);
}

std::string lookup_in_theme(Theme *t, const std::string &name, int size) {
  std::string best;
  int best_dist = INT_MAX;
  for (auto &d : t->dirs) {
    int dist = dir_distance(d, size);
    if (dist >= best_dist) continue;
    for (auto &b : t->bases) {
      for (auto ext : kExts) {
        std::string p = b + "/" + d.subdir + "/" + name + ext;
        if (usable_file(p)) {
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

static std::string &theme_override() {
  static std::string name;
  return name;
}

std::string default_icon_theme() {
  for (auto &b : icon_bases())
    if (file_exists(path_join(b, "fldesktop/index.theme"))) return "fldesktop";
  return "Adwaita";
}

std::string default_cursor_theme() {
  for (auto &b : icon_bases())
    if (is_dir(path_join(b, "fldesktop-cursors/cursors"))) return "fldesktop-cursors";
  return "Adwaita";
}

void set_icon_theme_name(const std::string &name) { theme_override() = name; }

std::string icon_theme_name() {
  static std::string name;
  if (!theme_override().empty()) return theme_override();
  if (name.empty()) {
    Ini cfg;
    cfg.load(desktop_config_path());
    name = cfg.get("General", "IconTheme", "");
    if (name.empty()) name = default_icon_theme();
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
        if (usable_file(p)) { r = p; break; }
      }
      if (!r.empty()) break;
    }
  }
  cache[key] = r;
  return r;
}

// FLTK's cairo drawing does not blend gray+alpha (depth 2) images correctly
// (the transparent parts come out black), so expand them to RGBA.
static Fl_Image *normalize_depth(Fl_Image *img) {
  Fl_RGB_Image *rgb = dynamic_cast<Fl_RGB_Image *>(img);
  if (!rgb || rgb->d() != 2 || rgb->count() < 1 || !rgb->data()[0]) return img;
  int w = rgb->data_w(), h = rgb->data_h();
  int ld = rgb->ld() ? rgb->ld() : w * 2;
  const unsigned char *src = (const unsigned char *)rgb->data()[0];
  unsigned char *dst = new unsigned char[(size_t)w * h * 4];
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const unsigned char *s = src + (size_t)y * ld + x * 2;
      unsigned char *d = dst + ((size_t)y * w + x) * 4;
      d[0] = d[1] = d[2] = s[0];
      d[3] = s[1];
    }
  Fl_RGB_Image *n = new Fl_RGB_Image(dst, w, h, 4);
  n->alloc_array = 1;
  delete img;
  return n;
}

Fl_Image *load_image(const std::string &path) {
  static bool registered = false;
  if (!registered) { fl_register_images(); registered = true; }
  std::string ext = extension(path);
  Fl_Image *img = nullptr;
  if (ext == "svg" || ext == "svgz") img = new Fl_SVG_Image(path.c_str());
  else if (ext == "xpm") img = new Fl_XPM_Image(path.c_str());
  else if (ext == "png") img = new Fl_PNG_Image(path.c_str());
  // the usual formats directly: Fl_Shared_Image keeps a list of loaded files
  // that copies made from it must not outlive
  else if (ext == "jpg" || ext == "jpeg" || ext == "jpe") img = new Fl_JPEG_Image(path.c_str());
  else if (ext == "gif") img = new Fl_GIF_Image(path.c_str());
  else if (ext == "bmp") img = new Fl_BMP_Image(path.c_str());
  else if (ext == "ico") img = new Fl_ICO_Image(path.c_str());
  else if (ext == "pnm" || ext == "pbm" || ext == "pgm" || ext == "ppm") img = new Fl_PNM_Image(path.c_str());
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
  return normalize_depth(img);
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

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string recolor_symbolic_svg(const std::string &svg, const std::string &hex) {
  std::string out;
  out.reserve(svg.size());
  size_t i = 0;
  while (i < svg.size()) {
    if (svg.compare(i, 12, "currentColor") == 0) {
      out += hex;
      i += 12;
      continue;
    }
    char prev = i ? svg[i - 1] : ' ';
    if (svg[i] == '#' && (prev == '"' || prev == '\'' || prev == ':' || prev == ' ' || prev == '=' || prev == ',')) {
      size_t n = 0;
      while (i + 1 + n < svg.size() && hexval(svg[i + 1 + n]) >= 0 && n < 7) n++;
      bool term = i + 1 + n >= svg.size() || !isalnum((unsigned char)svg[i + 1 + n]);
      if ((n == 3 || n == 6) && term) {
        int c[3];
        for (int k = 0; k < 3; k++)
          c[k] = n == 3 ? hexval(svg[i + 1 + k]) * 17 : hexval(svg[i + 1 + 2 * k]) * 16 + hexval(svg[i + 2 + 2 * k]);
        int lo = std::min(c[0], std::min(c[1], c[2])), hi = std::max(c[0], std::max(c[1], c[2]));
        // neutral grays (the "foreground" of symbolic icons) take the color;
        // colored parts (warnings, errors) and white highlights stay
        if (hi - lo <= 16 && hi < 0xf0) {
          out += hex;
          i += 1 + n;
          continue;
        }
      }
    }
    out += svg[i++];
  }
  return out;
}

Fl_Image *symbolic_icon(const std::string &name, int size, Fl_Color color) {
  static std::map<std::string, Fl_Image *> cache;
  uchar r, g, b;
  Fl::get_color(color, r, g, b);
  char hex[8];
  snprintf(hex, sizeof hex, "#%02x%02x%02x", r, g, b);
  std::string key = name + "@" + std::to_string(size) + hex;
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  Fl_Image *img = nullptr;
  std::string p = ends_with(name, "-symbolic") ? icon_path(name, size) : icon_path(name + "-symbolic", size);
  std::string svg;
  if (!p.empty() && extension(p) == "svg" && read_file(p, svg, 1 << 20)) {
    svg = recolor_symbolic_svg(svg, hex);
    auto *s = new Fl_SVG_Image(nullptr, svg.c_str());
    if (s->fail()) delete s;
    else {
      s->scale(size, size, 1, 1);
      img = s;
    }
  }
  if (!img) img = icon(name, size); // a full color icon of that name
  cache[key] = img;
  return img;
}

Fl_Image *icon_first(const std::vector<std::string> &names, int size) {
  for (auto &n : names)
    if (Fl_Image *i = icon(n, size)) return i;
  return nullptr;
}

} // namespace fld
