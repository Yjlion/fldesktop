#include "fld/mime.h"
#include "fld/ini.h"
#include "fld/util.h"

#include <algorithm>
#include <cstring>
#include <fnmatch.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace fld {

namespace {

struct Glob {
  int weight;
  std::string mime;
  std::string pattern;
  bool literal;      // no wildcard
  bool simple_ext;   // "*.ext"
  bool case_sensitive;
};

struct MimeDb {
  std::vector<Glob> globs;
  std::map<std::string, std::vector<const Glob *>> by_ext; // lower-case ext
  std::map<std::string, std::vector<std::string>> parents;
  std::map<std::string, std::string> aliases;
  std::map<std::string, std::string> generic_icons;
  std::map<std::string, std::string> icons;
  bool loaded = false;

  void load() {
    if (loaded) return;
    loaded = true;
    auto dirs = data_dirs();
    // later (lower priority) dirs first so earlier dirs override
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
      std::string base = path_join(*it, "mime");
      std::string text;
      if (read_file(base + "/globs2", text)) {
        for (auto &line : split(text, '\n')) {
          if (line.empty() || line[0] == '#') continue;
          auto f = split(line, ':', true);
          if (f.size() < 3) continue;
          Glob g;
          g.weight = atoi(f[0].c_str());
          g.mime = f[1];
          g.pattern = f[2];
          g.case_sensitive = f.size() > 3 && f[3].find("cs") != std::string::npos;
          g.literal = g.pattern.find_first_of("*?[") == std::string::npos;
          g.simple_ext = starts_with(g.pattern, "*.") &&
                         g.pattern.find_first_of("*?[", 1) == std::string::npos;
          globs.push_back(g);
        }
      }
      if (read_file(base + "/subclasses", text))
        for (auto &line : split(text, '\n')) {
          auto f = split(line, ' ');
          if (f.size() == 2) parents[f[0]].push_back(f[1]);
        }
      if (read_file(base + "/aliases", text))
        for (auto &line : split(text, '\n')) {
          auto f = split(line, ' ');
          if (f.size() == 2) aliases[f[0]] = f[1];
        }
      if (read_file(base + "/generic-icons", text))
        for (auto &line : split(text, '\n')) {
          auto f = split(line, ':');
          if (f.size() == 2) generic_icons[f[0]] = f[1];
        }
      if (read_file(base + "/icons", text))
        for (auto &line : split(text, '\n')) {
          auto f = split(line, ':');
          if (f.size() == 2) icons[f[0]] = f[1];
        }
    }
    for (auto &g : globs)
      if (g.simple_ext) by_ext[lower(g.pattern.substr(2))].push_back(&g);
  }
};

MimeDb &db() {
  static MimeDb d;
  d.load();
  return d;
}

std::string unalias(const std::string &m) {
  auto &a = db().aliases;
  auto it = a.find(m);
  return it == a.end() ? m : it->second;
}

} // namespace

std::string mime_from_name(const std::string &name_in) {
  MimeDb &d = db();
  std::string name = basename(name_in);
  std::string lname = lower(name);
  const Glob *best = nullptr;
  size_t best_len = 0;
  // 1. literal names
  for (auto &g : d.globs)
    if (g.literal && (g.case_sensitive ? g.pattern == name : lower(g.pattern) == lname))
      if (!best || g.weight > best->weight) best = &g;
  if (best) return best->mime;
  // 2. simple extensions: try longest extension first (tar.gz before gz)
  for (size_t p = lname.find('.'); p != std::string::npos; p = lname.find('.', p + 1)) {
    auto it = d.by_ext.find(lname.substr(p + 1));
    if (it == d.by_ext.end()) continue;
    for (auto g : it->second) {
      if (g->case_sensitive && !ends_with(name, g->pattern.substr(1))) continue;
      if (!best || g->weight > best->weight) best = g;
    }
    if (best) return best->mime;
  }
  // 3. full fnmatch for remaining patterns
  for (auto &g : d.globs) {
    if (g.literal || g.simple_ext) continue;
    bool m = g.case_sensitive ? fnmatch(g.pattern.c_str(), name.c_str(), 0) == 0
                              : fnmatch(lower(g.pattern).c_str(), lname.c_str(), 0) == 0;
    if (m && (!best || g.weight > best->weight ||
              (g.weight == best->weight && g.pattern.size() > best_len))) {
      best = &g;
      best_len = g.pattern.size();
    }
  }
  return best ? best->mime : "";
}

std::string mime_type(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    if (lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) return "inode/symlink";
    std::string m = mime_from_name(path);
    return m.empty() ? "application/octet-stream" : m;
  }
  if (S_ISDIR(st.st_mode)) return "inode/directory";
  if (S_ISCHR(st.st_mode)) return "inode/chardevice";
  if (S_ISBLK(st.st_mode)) return "inode/blockdevice";
  if (S_ISFIFO(st.st_mode)) return "inode/fifo";
  if (S_ISSOCK(st.st_mode)) return "inode/socket";
  std::string m = mime_from_name(path);
  if (!m.empty()) return m;
  if (st.st_size == 0) return "application/x-zerosize";
  // content sniff
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return "application/octet-stream";
  unsigned char buf[4096];
  size_t n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  if (n >= 4 && !memcmp(buf, "\x7f" "ELF", 4)) {
    // ET_DYN (3) could be a PIE executable or a shared library
    bool exec = (st.st_mode & 0111) != 0;
    return exec ? "application/x-executable" : "application/x-sharedlib";
  }
  if (n >= 2 && buf[0] == '#' && buf[1] == '!') {
    std::string first((char *)buf, std::min<size_t>(n, 128));
    if (first.find("python") != std::string::npos) return "text/x-python";
    if (first.find("sh") != std::string::npos) return "application/x-shellscript";
    return "text/plain";
  }
  if (n >= 8 && !memcmp(buf, "\x89PNG", 4)) return "image/png";
  if (n >= 3 && !memcmp(buf, "\xff\xd8\xff", 3)) return "image/jpeg";
  if (n >= 4 && !memcmp(buf, "GIF8", 4)) return "image/gif";
  if (n >= 5 && !memcmp(buf, "%PDF-", 5)) return "application/pdf";
  bool text = true;
  for (size_t i = 0; i < n; i++)
    if (buf[i] == 0 || (buf[i] < 32 && !strchr("\t\n\r\f\v\x1b\b", buf[i]))) { text = false; break; }
  return text ? "text/plain" : "application/octet-stream";
}

std::vector<std::string> mime_parents(const std::string &mime) {
  std::vector<std::string> out;
  std::vector<std::string> q{unalias(mime)};
  std::set<std::string> seen{q[0]};
  while (!q.empty()) {
    std::string m = q.front();
    q.erase(q.begin());
    auto it = db().parents.find(m);
    if (it == db().parents.end()) continue;
    for (auto &p : it->second)
      if (seen.insert(p).second) { out.push_back(p); q.push_back(p); }
  }
  if (starts_with(mime, "text/") && !seen.count("text/plain")) out.push_back("text/plain");
  return out;
}

std::vector<std::string> mime_icon_names(const std::string &mime_in) {
  std::string mime = unalias(mime_in);
  std::vector<std::string> v;
  if (mime == "inode/directory") return {"folder", "inode-directory"};
  auto &d = db();
  auto it = d.icons.find(mime);
  if (it != d.icons.end()) v.push_back(it->second);
  std::string n = mime;
  std::replace(n.begin(), n.end(), '/', '-');
  v.push_back(n);
  auto g = d.generic_icons.find(mime);
  if (g != d.generic_icons.end()) v.push_back(g->second);
  size_t s = mime.find('/');
  std::string media = mime.substr(0, s);
  v.push_back(media + "-x-generic");
  if (media == "application") {
    if (mime == "application/x-executable" || mime == "application/x-sharedlib")
      v.insert(v.begin(), "application-x-executable");
  }
  v.push_back("text-x-generic");
  v.push_back("unknown");
  return v;
}

std::string mime_description(const std::string &mime_in) {
  static std::map<std::string, std::string> cache;
  std::string mime = unalias(mime_in);
  auto it = cache.find(mime);
  if (it != cache.end()) return it->second;
  std::string r = mime;
  if (mime == "inode/directory") r = "Folder";
  else {
    for (auto &d : data_dirs()) {
      std::string xml;
      if (!read_file(path_join(d, "mime/" + mime + ".xml"), xml)) continue;
      size_t p = xml.find("<comment>");
      if (p != std::string::npos) {
        size_t e = xml.find("</comment>", p);
        if (e != std::string::npos) r = xml.substr(p + 9, e - p - 9);
      }
      break;
    }
  }
  cache[mime] = r;
  return r;
}

namespace {

std::vector<std::string> mimeapps_files() {
  std::vector<std::string> v;
  std::string desk = lower(getenv_or("XDG_CURRENT_DESKTOP", ""));
  for (auto &c : config_dirs()) {
    if (!desk.empty()) v.push_back(path_join(c, desk + "-mimeapps.list"));
    v.push_back(path_join(c, "mimeapps.list"));
  }
  for (auto &d : data_dirs()) v.push_back(path_join(d, "applications/mimeapps.list"));
  return v;
}

void add_unique(std::vector<std::string> &v, const std::string &id) {
  if (!id.empty() && std::find(v.begin(), v.end(), id) == v.end()) v.push_back(id);
}

} // namespace

std::vector<DesktopEntry> apps_for_mime(const std::string &mime_in) {
  std::vector<std::string> types{unalias(mime_in)};
  for (auto &p : mime_parents(mime_in)) types.push_back(p);
  std::vector<std::string> ids, removed;
  for (auto &t : types) {
    for (auto &f : mimeapps_files()) {
      Ini ini;
      if (!ini.load(f)) continue;
      for (auto &id : split(ini.get("Default Applications", t), ';')) add_unique(ids, trim(id));
      for (auto &id : split(ini.get("Added Associations", t), ';')) add_unique(ids, trim(id));
      for (auto &id : split(ini.get("Removed Associations", t), ';')) add_unique(removed, trim(id));
    }
    for (auto &d : data_dirs()) {
      Ini cache;
      if (!cache.load(path_join(d, "applications/mimeinfo.cache"))) continue;
      for (auto &id : split(cache.get("MIME Cache", t), ';')) add_unique(ids, trim(id));
    }
  }
  std::vector<DesktopEntry> out;
  for (auto &id : ids) {
    if (std::find(removed.begin(), removed.end(), id) != removed.end()) continue;
    DesktopEntry e;
    if (find_application(id, e) && !e.hidden) out.push_back(e);
  }
  if (out.empty()) {
    // no mimeinfo.cache? scan all apps
    for (auto &e : all_applications(true))
      for (auto &t : types)
        if (std::find(e.mime_types.begin(), e.mime_types.end(), t) != e.mime_types.end()) {
          out.push_back(e);
          break;
        }
  }
  return out;
}

bool default_app_for_mime(const std::string &mime, DesktopEntry &out) {
  auto v = apps_for_mime(mime);
  if (v.empty()) return false;
  out = v[0];
  return true;
}

bool set_default_app_for_mime(const std::string &mime, const std::string &desktop_id) {
  std::string f = config_home() + "/mimeapps.list";
  Ini ini;
  ini.load(f);
  ini.set("Default Applications", mime, desktop_id + ";");
  std::string added = ini.get("Added Associations", mime);
  if (added.find(desktop_id) == std::string::npos)
    ini.set("Added Associations", mime, desktop_id + ";" + added);
  return ini.save(f);
}

bool open_path(const std::string &path) {
  if (path.find("://") != std::string::npos && !starts_with(path, "file://"))
    return spawn({"xdg-open", path});
  std::string p = starts_with(path, "file://") ? uri_decode(path.substr(7)) : path;
  std::string m = mime_type(p);
  if (m == "inode/directory") {
    DesktopEntry e;
    if (default_app_for_mime(m, e)) return e.launch({p});
    return spawn({"fl-files", p});
  }
  if (m == "application/x-desktop") {
    DesktopEntry e;
    if (e.load(p)) return e.launch();
  }
  DesktopEntry e;
  if (default_app_for_mime(m, e)) return e.launch({p});
  // our own fallbacks
  if (starts_with(m, "text/") || m == "application/x-shellscript" || m == "application/x-zerosize")
    return spawn({"fl-edit", p});
  if (starts_with(m, "image/")) return spawn({"fl-view", p});
  return spawn({"xdg-open", p});
}

} // namespace fld
