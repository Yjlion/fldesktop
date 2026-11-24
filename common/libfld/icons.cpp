#include "fld/icons.h"
#include "fld/accents.h"
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
#include <unordered_map>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <memory>

namespace fld {

namespace {

struct ThemeDir {
  std::string subdir;
  int size = 48, min = 48, max = 48, threshold = 2, scale = 1;
  enum { Fixed, Scalable, Threshold } type = Threshold;
};

// Where each icon name is in a theme, from listing its directories once
// (instead of trying every directory x extension for every name: a missing
// icon cost ~700 stat() calls in hicolor alone).
struct IconFile {
  unsigned short dir; // ThemeDir index
  unsigned char base, ext;
};

struct Theme {
  std::string name;
  std::vector<std::string> bases;   // existing <base>/<name> directories
  std::vector<ThemeDir> dirs;
  std::vector<std::string> parents;
  bool indexed = false;
  double indexed_at = 0;
  std::unordered_map<std::string, std::vector<IconFile>> files;
  std::vector<std::pair<std::string, time_t>> listed; // directory, mtime
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

double mono_seconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

time_t dir_mtime(const std::string &d) {
  struct stat st;
  return stat(d.c_str(), &st) == 0 ? st.st_mtime : 0;
}

int ext_index(const char *name, size_t len) {
  for (int e = 0; e < 3; e++) {
    size_t el = strlen(kExts[e]);
    if (len > el && strcmp(name + len - el, kExts[e]) == 0) return e;
  }
  return -1;
}

void index_theme(Theme *t) {
  t->files.clear();
  t->listed.clear();
  for (size_t di = 0; di < t->dirs.size() && di < 65535; di++)
    for (size_t bi = 0; bi < t->bases.size() && bi < 255; bi++) {
      std::string d = t->bases[bi] + "/" + t->dirs[di].subdir;
      DIR *dir = opendir(d.c_str());
      if (!dir) continue;
      t->listed.push_back({d, dir_mtime(d)});
      while (struct dirent *e = readdir(dir)) {
        size_t len = strlen(e->d_name);
        int ext = ext_index(e->d_name, len);
        if (ext < 0) continue;
        t->files[std::string(e->d_name, len - strlen(kExts[ext]))].push_back(
            {(unsigned short)di, (unsigned char)bi, (unsigned char)ext});
      }
      closedir(dir);
    }
  // in the order the lookup prefers: directory, base, extension
  for (auto &f : t->files)
    std::sort(f.second.begin(), f.second.end(), [](const IconFile &a, const IconFile &b) {
      if (a.dir != b.dir) return a.dir < b.dir;
      if (a.base != b.base) return a.base < b.base;
      return a.ext < b.ext;
    });
  t->indexed = true;
  t->indexed_at = mono_seconds();
}

// icons installed while the program runs (a new application): a name not
// found looks again, at most every 10 s, whether a directory changed
bool reindex_if_changed(Theme *t) {
  double now = mono_seconds();
  if (now - t->indexed_at < 10) return false;
  t->indexed_at = now;
  for (auto &l : t->listed)
    if (dir_mtime(l.first) != l.second) {
      index_theme(t);
      return true;
    }
  return false;
}

// fldesktop's own themes are checked when they are built (tests/artwork-check.py)
bool trusted_svg(const std::string &path) {
  return path.find("/icons/fldesktop") != std::string::npos;
}

std::string lookup_in_theme(Theme *t, const std::string &name, int size) {
  if (!t->indexed) index_theme(t);
  auto it = t->files.find(name);
  if (it == t->files.end() && reindex_if_changed(t)) it = t->files.find(name);
  if (it == t->files.end()) return "";
  std::string best;
  int best_dist = INT_MAX;
  for (const IconFile &f : it->second) {
    int dist = dir_distance(t->dirs[f.dir], size);
    if (dist >= best_dist) continue;
    std::string p = t->bases[f.base] + "/" + t->dirs[f.dir].subdir + "/" + name + kExts[f.ext];
    if (f.ext == 1 && !trusted_svg(p) && !svg_usable(p)) continue;
    best = p;
    best_dist = dist;
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
    // follows the accent even when the stored IconTheme went stale
    name = effective_icon_theme(cfg, [](const std::string &t) {
      for (auto &b : icon_bases())
        if (file_exists(path_join(path_join(b, t), "index.theme"))) return true;
      return false;
    });
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
    // the pixmaps directories, listed once
    static std::vector<std::pair<std::string, std::vector<std::string>>> pixmaps;
    static bool listed = false;
    if (!listed) {
      listed = true;
      for (auto &d : data_dirs()) {
        std::string pd = path_join(d, "pixmaps");
        std::vector<std::string> names = list_dir(pd);
        if (!names.empty()) pixmaps.push_back({pd, names});
      }
    }
    for (auto &pd : pixmaps) {
      for (auto ext : kExts)
        if (std::binary_search(pd.second.begin(), pd.second.end(), n + ext) &&
            usable_file(pd.first + "/" + n + ext)) {
          r = pd.first + "/" + n + ext;
          break;
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
