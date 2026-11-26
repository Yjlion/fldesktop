// fl-files: navigation, places and drives
#include "files_app.h"

// ---------------------------------------------------------------------------
// Navigation & chrome

std::vector<Place> g_place_list;

void fill_places() {
  g_place_list.clear();
  auto add = [](const std::string &l, const std::string &p, const std::string &i) {
    if (p == TRASH_URI || fld::is_dir(p)) g_place_list.push_back({l, p, i});
  };
  add("Home", fld::home_dir(), "user-home");
  add("Desktop", fld::user_dir("DESKTOP"), "user-desktop");
  add("Documents", fld::user_dir("DOCUMENTS"), "folder-documents");
  add("Downloads", fld::user_dir("DOWNLOAD"), "folder-download");
  add("Pictures", fld::user_dir("PICTURES"), "folder-pictures");
  add("Music", fld::user_dir("MUSIC"), "folder-music");
  add("Videos", fld::user_dir("VIDEOS"), "folder-videos");
  add("Trash", TRASH_URI, fld::trash_list().empty() ? "user-trash" : "user-trash-full");
  // GTK bookmarks
  std::string bm;
  if (fld::read_file(fld::config_home() + "/gtk-3.0/bookmarks", bm)) {
    for (auto &line : fld::split(bm, '\n')) {
#ifdef FLD_HAVE_GVFS
      if (gv::looks_like_uri(line)) { // a network location
        size_t sp = line.find(' ');
        std::string uri = line.substr(0, sp);
        Place p{sp == std::string::npos ? gv::display_name_for_uri(uri) : line.substr(sp + 1), "", "folder-remote"};
        p.uri = uri;
        p.bookmark = true;
        // mounted: its folder
        for (auto &m : gv::Client::get().state().mounts)
          if (!m.path.empty() && (fld::starts_with(uri, m.uri) || fld::starts_with(uri + "/", m.uri))) {
            std::string rest = uri.size() > m.uri.size() ? fld::uri_decode(uri.substr(m.uri.size())) : "";
            p.path = fld::path_join(m.path, rest);
            if (!rest.empty() && !fld::is_dir(p.path)) p.path.clear();
          }
        g_place_list.push_back(p);
        continue;
      }
#endif
      if (!fld::starts_with(line, "file://")) continue;
      size_t sp = line.find(' ');
      std::string p = fld::uri_decode(line.substr(7, sp == std::string::npos ? std::string::npos : sp - 7));
      std::string l = sp == std::string::npos ? fld::basename(p) : line.substr(sp + 1);
      add(l, p, "folder");
      if (!g_place_list.empty() && g_place_list.back().path == p) g_place_list.back().bookmark = true;
    }
  }
  add("File System", "/", "drive-harddisk");
  // drives: from UDisks2 (also unmounted ones), else the mounted file systems
  bool have_volumes = false;
#ifdef FLD_HAVE_UDISKS
  const ud::State &us = ud::Client::get().state();
  if (us.running) {
    have_volumes = true;
    for (auto &v : us.volumes) {
      Place p{v.label, v.mount_point, v.icon};
      p.volume = v.path;
      p.can_eject = v.can_eject || v.mounted();
      g_place_list.push_back(p);
    }
  }
#endif
#ifdef FLD_HAVE_GVFS
  // network locations, phones and cameras (GVfs)
  {
    const gv::State &gs = gv::Client::get().state();
    if (gs.running) {
      Place net{"Network", NETWORK_URI, "network-workgroup"};
      g_place_list.push_back(net);
    }
    for (auto &m : gs.mounts) {
      Place p{m.name, m.path, m.icon};
      p.uri = m.uri;
      p.gvfs_mount = m.uri;
      p.can_eject = m.can_unmount || m.can_eject;
      g_place_list.push_back(p);
    }
    for (auto &v : gs.volumes) {
      Place p{v.name, "", v.icon};
      p.gvfs_volume = v.id;
      g_place_list.push_back(p);
    }
  }
#endif
  if (!have_volumes) {
    std::string mounts;
    if (fld::read_file("/proc/mounts", mounts)) {
      for (auto &line : fld::split(mounts, '\n')) {
        auto f = fld::split(line, ' ');
        if (f.size() < 2) continue;
        std::string mp; // \040 escapes are octal
        for (size_t i = 0; i < f[1].size(); i++) {
          if (f[1][i] == '\\' && i + 3 < f[1].size()) { mp += (char)strtol(f[1].substr(i + 1, 3).c_str(), nullptr, 8); i += 3; }
          else mp += f[1][i];
        }
        if (fld::starts_with(mp, "/media/") || fld::starts_with(mp, "/run/media/") || fld::starts_with(mp, "/mnt/"))
          add(fld::basename(mp), mp, "drive-removable-media");
      }
    }
  }
  g_places->clear();
  for (auto &p : g_place_list) {
    g_places->add(("  " + fld::escape_at(p.label)).c_str());
    g_places->icon(g_places->size(), fld::icon_first({p.icon, "folder"}, 20));
  }
}

static std::string bookmarks_path() { return fld::config_home() + "/gtk-3.0/bookmarks"; }

// a bookmark's URI: file://..., or the network location of a remote folder
std::string bookmark_uri(const std::string &path) {
  std::string r = remote_uri(path);
  return r.empty() ? "file://" + fld::uri_encode_path(path) : r;
}
std::string bookmark_uri(const Place &p) { return p.uri.empty() ? bookmark_uri(p.path) : p.uri; }

void add_bookmark_uri(const std::string &uri, const std::string &label) {
  std::string bm;
  fld::read_file(bookmarks_path(), bm);
  for (auto &line : fld::split(bm, '\n'))
    if (line == uri || fld::starts_with(line, uri + " ")) return; // already there
  if (!bm.empty() && bm.back() != '\n') bm += '\n';
  bm += uri + (label.empty() ? "" : " " + label) + "\n";
  fld::mkdir_p(fld::dirname(bookmarks_path()));
  if (!fld::write_file(bookmarks_path(), bm)) fl_alert("Could not save %s", bookmarks_path().c_str());
  fill_places();
  select_place_for(g_cwd);
}

void add_bookmark(const std::string &path) {
  if (path.empty() || path == TRASH_URI || path == NETWORK_URI) return;
  // a network share is named after its location, not the gvfs-fuse folder
  std::string name = remote_mount_name(path);
  add_bookmark_uri(bookmark_uri(path), name);
}

void remove_bookmark(const std::string &uri) {
  std::string bm, out;
  if (!fld::read_file(bookmarks_path(), bm)) return;
  for (auto &line : fld::split(bm, '\n'))
    if (line != uri && !fld::starts_with(line, uri + " ")) out += line + "\n";
  fld::write_file(bookmarks_path(), out);
  fill_places();
  select_place_for(g_cwd);
}

void rename_bookmark(const std::string &uri, const std::string &old_label) {
  std::string label = old_label;
  if (!fld::ask_text("Rename Bookmark", "Bookmark name:", label, -1, "Rename")) return;
  const char *n = label.c_str();
  std::string bm, out;
  if (!fld::read_file(bookmarks_path(), bm)) return;
  for (auto &line : fld::split(bm, '\n')) {
    if (line == uri || fld::starts_with(line, uri + " ")) out += uri + (*n ? " " + std::string(n) : "") + "\n";
    else out += line + "\n";
  }
  fld::write_file(bookmarks_path(), out);
  fill_places();
  select_place_for(g_cwd);
}

// ---- drives (UDisks2)

// Opens a place here, in a new window or in a new tab: drives are mounted
// (and unlocked) first
void open_place(const Place &p, PlaceWhere where) {
  auto go = [where](const std::string &path) {
    if (where == PLACE_WINDOW) fld::spawn({"fl-files", path});
    else if (where == PLACE_TAB) new_tab(path);
    else navigate(path);
  };
#ifdef FLD_HAVE_GVFS
  if (!p.gvfs_volume.empty()) { // a phone or camera
    std::string label = p.label;
    g_status->copy_label("Mounting…");
    gv::Client::get().mount_volume(p.gvfs_volume, [go, label](const std::string &err, const std::string &path) {
      update_status();
      if (err == gv::kCancelled) return;
      if (!err.empty()) {
        fl_alert("Could not open \"%s\":\n%s", fld::escape_at(label).c_str(), fld::escape_at(err).c_str());
        return;
      }
      go(path);
    });
    return;
  }
  if (p.path.empty() && !p.uri.empty()) { // a network location that is not mounted
    mount_and_go(p.uri, where);
    return;
  }
#endif
  if (!p.path.empty() || p.volume.empty()) { go(p.path); return; }
#ifdef FLD_HAVE_UDISKS
  ud::Client &c = ud::Client::get();
  const ud::Volume *v = c.state().volume(p.volume);
  if (!v) return;
  std::string vol = v->path;
  auto mount = [go](const std::string &vol) {
    g_status->copy_label("Mounting…");
    ud::Client::get().mount(vol, [go](const std::string &err, const std::string &mp) {
      update_status();
      if (!err.empty()) { fl_alert("Could not mount the volume:\n%s", fld::escape_at(err).c_str()); return; }
      go(mp);
    });
  };
  if (v->encrypted && v->cleartext.empty()) {
    std::string q = "Passphrase for " + v->label + ":";
    const char *pw = fl_password("%s", "", fld::escape_at(q).c_str());
    if (!pw) return;
    ud::Client::get().unlock(vol, pw, [mount](const std::string &err, const std::string &clear) {
      if (!err.empty()) { fl_alert("Could not unlock the volume:\n%s", fld::escape_at(err).c_str()); return; }
      mount(clear);
    });
    return;
  }
  mount(vol);
#else
  (void)go;
#endif
}

void unmount_place(const Place &p, bool eject) {
#ifdef FLD_HAVE_GVFS
  if (!p.gvfs_mount.empty()) {
    if (!p.path.empty()) leave_folder(p.path);
    std::string label = p.label;
    g_status->copy_label("Disconnecting…");
    gv::Client::get().unmount(p.gvfs_mount, eject, [label](const std::string &err) {
      update_status();
      if (!err.empty() && err != gv::kCancelled)
        fl_alert("Could not disconnect \"%s\":\n%s", fld::escape_at(label).c_str(), fld::escape_at(err).c_str());
    });
    return;
  }
#endif
#ifdef FLD_HAVE_UDISKS
  if (p.volume.empty()) return;
  // leave the drive first, or it is busy
  if (!p.path.empty()) leave_folder(p.path);
  std::string label = p.label;
  auto done = [label, eject](const std::string &err) {
    update_status();
    if (!err.empty())
      fl_alert("Could not %s \"%s\":\n%s", eject ? "eject" : "unmount", fld::escape_at(label).c_str(),
               fld::escape_at(err).c_str());
    else if (eject)
      g_status->copy_label(fld::escape_at("\"" + label + "\" can be removed safely.").c_str());
  };
  g_status->copy_label(eject ? "Ejecting…" : "Unmounting…");
  if (eject) ud::Client::get().eject(p.volume, done);
  else ud::Client::get().unmount(p.volume, done);
#endif
}

// Places list with a context menu
