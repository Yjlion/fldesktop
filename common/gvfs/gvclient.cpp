#include "gvclient.h"

#include <gio/gio.h>

#include <cstring>
#include <map>
#include <memory>

#include "fld/glib_bridge.h"
#include "fld/util.h"

namespace gv {
namespace {

// ---- GLib thread state --------------------------------------------------------
GVolumeMonitor *g_monitor = nullptr;
std::map<std::string, GVolume *> g_volumes; // Volume::id -> volume (refs)
guint g_rebuild_src = 0;

std::string take(char *s) {
  std::string r = s ? s : "";
  g_free(s);
  return r;
}

std::string icon_name(GIcon *icon) {
  std::string r;
  if (icon && G_IS_THEMED_ICON(icon)) {
    const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    if (names && names[0]) r = names[0];
  }
  return r;
}

std::string error_text(GError *e) {
  if (!e) return "";
  std::string r = g_error_matches(e, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED) ||
                           g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                       ? kCancelled
                       : e->message;
  g_error_free(e);
  return r;
}

void report(Done done, const std::string &err) {
  if (done) fld::ui_call([done, err] { done(err); });
}
void report(DoneValue done, const std::string &err, const std::string &value) {
  if (done) fld::ui_call([done, err, value] { done(err, value); });
}

// a block device: UDisks shows it (the panel and fl-files mount drives through UDisks)
bool has_device(GVolume *v) {
  return !take(g_volume_get_identifier(v, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE)).empty();
}

void rebuild() {
  State s;
  // GVfs is installed when GIO knows its kinds of locations (plain GIO has
  // file:// and a few others only)
  if (const gchar *const *sc = g_vfs_get_supported_uri_schemes(g_vfs_get_default()))
    for (int i = 0; sc[i]; i++)
      if (!strcmp(sc[i], "network") || !strcmp(sc[i], "sftp") || !strcmp(sc[i], "smb")) s.running = g_monitor != nullptr;
  for (auto &v : g_volumes) g_object_unref(v.second);
  g_volumes.clear();
  if (g_monitor) {
    GList *mounts = g_volume_monitor_get_mounts(g_monitor);
    for (GList *l = mounts; l; l = l->next) {
      GMount *m = G_MOUNT(l->data);
      GFile *root = g_mount_get_root(m);
      Mount mt;
      mt.uri = take(g_file_get_uri(root));
      // drives (file:// mounts) are UDisks' business; shadowed mounts are hidden ones
      if (!g_file_has_uri_scheme(root, "file") && !g_mount_is_shadowed(m)) {
        mt.name = take(g_mount_get_name(m));
        mt.path = take(g_file_get_path(root));
        GIcon *ic = g_mount_get_symbolic_icon(m);
        mt.icon = icon_name(ic);
        if (ic) g_object_unref(ic);
        if (mt.icon.empty() || fld::ends_with(mt.icon, "-symbolic")) {
          GIcon *full = g_mount_get_icon(m);
          std::string n = icon_name(full);
          if (full) g_object_unref(full);
          if (!n.empty()) mt.icon = n;
        }
        if (mt.icon.empty()) mt.icon = "folder-remote";
        mt.can_unmount = g_mount_can_unmount(m);
        mt.can_eject = g_mount_can_eject(m);
        s.mounts.push_back(mt);
      }
      g_object_unref(root);
    }
    g_list_free_full(mounts, g_object_unref);

    GList *vols = g_volume_monitor_get_volumes(g_monitor);
    for (GList *l = vols; l; l = l->next) {
      GVolume *v = G_VOLUME(l->data);
      GMount *m = g_volume_get_mount(v);
      if (m) {
        g_object_unref(m);
        continue;
      }
      if (has_device(v) || !g_volume_can_mount(v)) continue;
      Volume vol;
      vol.name = take(g_volume_get_name(v));
      vol.id = take(g_volume_get_uuid(v));
      if (vol.id.empty()) vol.id = take(g_volume_get_identifier(v, "class")) + ":" + vol.name;
      GIcon *ic = g_volume_get_icon(v);
      vol.icon = icon_name(ic);
      if (ic) g_object_unref(ic);
      if (vol.icon.empty()) vol.icon = "drive-removable-media";
      g_volumes[vol.id] = G_VOLUME(g_object_ref(v));
      s.volumes.push_back(vol);
    }
    g_list_free_full(vols, g_object_unref);
  }
  fld::ui_call([s] {
    Client &c = Client::get();
    c.state_ = s;
    if (c.on_changed) c.on_changed();
  });
}

void schedule_rebuild() {
  if (g_rebuild_src) return;
  g_rebuild_src = fld::glib_timeout(150, [](void *) -> int {
    g_rebuild_src = 0;
    rebuild();
    return G_SOURCE_REMOVE;
  }, nullptr);
}

// ---- mount operations: the questions go to the FLTK thread ---------------------

void answer_password(GMountOperation *op, const PasswordReply &r) {
  if (!r.ok) {
    g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
    return;
  }
  if (r.anonymous) g_mount_operation_set_anonymous(op, TRUE);
  else {
    g_mount_operation_set_username(op, r.user.c_str());
    g_mount_operation_set_domain(op, r.domain.c_str());
    g_mount_operation_set_password(op, r.password.c_str());
    g_mount_operation_set_password_save(op, r.remember == PasswordReply::FOREVER   ? G_PASSWORD_SAVE_PERMANENTLY
                                            : r.remember == PasswordReply::SESSION ? G_PASSWORD_SAVE_FOR_SESSION
                                                                                   : G_PASSWORD_SAVE_NEVER);
  }
  g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
}

void on_ask_password(GMountOperation *op, const char *message, const char *user, const char *domain,
                     GAskPasswordFlags flags, gpointer) {
  PasswordRequest q;
  q.message = message ? message : "";
  q.user = user ? user : "";
  q.domain = domain ? domain : "";
  q.need_user = flags & G_ASK_PASSWORD_NEED_USERNAME;
  q.need_domain = flags & G_ASK_PASSWORD_NEED_DOMAIN;
  q.need_password = flags & G_ASK_PASSWORD_NEED_PASSWORD;
  q.anonymous_ok = flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED;
  q.can_save = flags & G_ASK_PASSWORD_SAVING_SUPPORTED;
  g_object_ref(op);
  fld::ui_call([op, q] {
    PasswordReply r;
    if (Client::get().ask_password) r = Client::get().ask_password(q);
    fld::glib_call([op, r] {
      answer_password(op, r);
      g_object_unref(op);
    });
  });
}

void ask(GMountOperation *op, const char *message, const char *const *choices) {
  std::vector<std::string> ch;
  for (int i = 0; choices && choices[i]; i++) ch.push_back(choices[i]);
  std::string msg = message ? message : "";
  g_object_ref(op);
  fld::ui_call([op, msg, ch] {
    int a = Client::get().ask_question ? Client::get().ask_question(msg, ch) : -1;
    fld::glib_call([op, a] {
      if (a < 0) g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
      else {
        g_mount_operation_set_choice(op, a);
        g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
      }
      g_object_unref(op);
    });
  });
}

GMountOperation *new_operation() {
  GMountOperation *op = g_mount_operation_new();
  g_signal_connect(op, "ask-password", G_CALLBACK(on_ask_password), nullptr);
  g_signal_connect(op, "ask-question",
                   G_CALLBACK(+[](GMountOperation *op, const char *msg, const char **choices, gpointer) {
                     ask(op, msg, choices);
                   }),
                   nullptr);
  // a busy mount: which programs use it, unmount anyway?
  g_signal_connect(op, "show-processes",
                   G_CALLBACK(+[](GMountOperation *op, const char *msg, GArray *, const char **choices, gpointer) {
                     ask(op, msg, choices);
                   }),
                   nullptr);
  return op;
}

struct MountJob {
  GFile *file;
  GMountOperation *op;
  DoneValue done;
};

void mount_finished(GObject *src, GAsyncResult *res, gpointer data) {
  std::unique_ptr<MountJob> j((MountJob *)data);
  GError *e = nullptr;
  g_file_mount_enclosing_volume_finish(G_FILE(src), res, &e);
  std::string err;
  if (e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) g_clear_error(&e);
  if (e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
    char *sc = g_file_get_uri_scheme(G_FILE(src));
    err = std::string("This kind of location (") + (sc ? sc : "?") +
          ") is not supported. Install gvfs and its backend for it (gvfs-smb, gvfs-mtp, gvfs-nfs...).";
    g_free(sc);
    g_clear_error(&e);
  }
  if (err.empty()) err = error_text(e);
  std::string path;
  if (err.empty()) {
    path = take(g_file_get_path(j->file));
    if (path.empty())
      err = "The location is connected, but it cannot be opened as a folder: gvfs-fuse is not running "
            "(install gvfs-fuse, then log out and in again).";
  }
  g_object_unref(j->file);
  g_object_unref(j->op);
  report(j->done, err, path);
  schedule_rebuild();
}

void volume_mounted(GObject *src, GAsyncResult *res, gpointer data) {
  std::unique_ptr<DoneValue> done((DoneValue *)data);
  GVolume *v = G_VOLUME(src);
  GError *e = nullptr;
  g_volume_mount_finish(v, res, &e);
  std::string err = error_text(e), path;
  if (err.empty()) {
    GMount *m = g_volume_get_mount(v);
    if (m) {
      GFile *root = g_mount_get_root(m);
      path = take(g_file_get_path(root));
      g_object_unref(root);
      g_object_unref(m);
    }
    if (path.empty()) err = "The device is mounted, but it cannot be opened as a folder: gvfs-fuse is not running.";
  }
  report(*done, err, path);
  schedule_rebuild();
}

GMount *find_mount(const std::string &uri) {
  if (!g_monitor) return nullptr;
  GMount *found = nullptr;
  GList *mounts = g_volume_monitor_get_mounts(g_monitor);
  for (GList *l = mounts; l; l = l->next) {
    GFile *root = g_mount_get_root(G_MOUNT(l->data));
    if (!found && take(g_file_get_uri(root)) == uri) found = G_MOUNT(g_object_ref(l->data));
    g_object_unref(root);
  }
  g_list_free_full(mounts, g_object_unref);
  return found;
}

// listing a location without a local path
struct ListJob {
  std::vector<NetEntry> entries;
  std::function<void(const std::string &, std::vector<NetEntry>)> done;
};

void list_done(ListJob *j, const std::string &err) {
  auto done = j->done;
  auto entries = j->entries;
  delete j;
  if (done) fld::ui_call([done, err, entries] { done(err, entries); });
}

void next_files(GObject *src, GAsyncResult *res, gpointer data) {
  ListJob *j = (ListJob *)data;
  GFileEnumerator *en = G_FILE_ENUMERATOR(src);
  GError *e = nullptr;
  GList *infos = g_file_enumerator_next_files_finish(en, res, &e);
  if (e) {
    g_object_unref(en);
    list_done(j, error_text(e));
    return;
  }
  if (!infos) {
    g_object_unref(en);
    list_done(j, "");
    return;
  }
  for (GList *l = infos; l; l = l->next) {
    GFileInfo *fi = G_FILE_INFO(l->data);
    NetEntry n;
    const char *dn = g_file_info_get_display_name(fi);
    n.name = dn ? dn : g_file_info_get_name(fi);
    n.icon = icon_name(g_file_info_get_icon(fi));
    const char *t = g_file_info_get_attribute_string(fi, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
    if (t) n.target = t;
    else {
      GFile *child = g_file_enumerator_get_child(en, fi);
      n.target = take(g_file_get_uri(child));
      g_object_unref(child);
    }
    j->entries.push_back(n);
  }
  g_list_free_full(infos, g_object_unref);
  g_file_enumerator_next_files_async(en, 64, G_PRIORITY_DEFAULT, nullptr, next_files, j);
}

} // namespace

Client &Client::get() {
  static Client c;
  return c;
}

void Client::start() {
  static bool started = false;
  if (started) return;
  started = true;
  if (!fld::glib_running()) fld::glib_start();
  fld::glib_call([] {
    g_monitor = g_volume_monitor_get();
    for (const char *sig : {"mount-added", "mount-removed", "mount-changed", "volume-added", "volume-removed",
                            "volume-changed"})
      g_signal_connect(g_monitor, sig, G_CALLBACK(+[](GVolumeMonitor *, gpointer, gpointer) { schedule_rebuild(); }),
                       nullptr);
    rebuild();
  });
}

void Client::refresh() {
  fld::glib_call([] { schedule_rebuild(); });
}

void Client::mount_uri(const std::string &uri, DoneValue done) {
  start();
  fld::glib_call([uri, done] {
    MountJob *j = new MountJob{g_file_new_for_uri(uri.c_str()), new_operation(), done};
    g_file_mount_enclosing_volume(j->file, G_MOUNT_MOUNT_NONE, j->op, nullptr, mount_finished, j);
  });
}

void Client::mount_volume(const std::string &id, DoneValue done) {
  start();
  fld::glib_call([id, done] {
    auto it = g_volumes.find(id);
    if (it == g_volumes.end()) {
      report(done, "The device is gone.", "");
      return;
    }
    GMountOperation *op = new_operation();
    g_volume_mount(it->second, G_MOUNT_MOUNT_NONE, op, nullptr, volume_mounted, new DoneValue(done));
    g_object_unref(op); // the volume keeps it while mounting
  });
}

void Client::unmount(const std::string &uri, bool eject, Done done) {
  start();
  fld::glib_call([uri, eject, done] {
    GMount *m = find_mount(uri);
    if (!m) {
      report(done, "");
      schedule_rebuild();
      return;
    }
    GMountOperation *op = new_operation();
    auto *d = new Done(done);
    if (eject && g_mount_can_eject(m)) {
      g_mount_eject_with_operation(m, G_MOUNT_UNMOUNT_NONE, op, nullptr,
                                   +[](GObject *src, GAsyncResult *res, gpointer data) {
                                     std::unique_ptr<Done> done((Done *)data);
                                     GError *e = nullptr;
                                     g_mount_eject_with_operation_finish(G_MOUNT(src), res, &e);
                                     report(*done, error_text(e));
                                     schedule_rebuild();
                                   },
                                   d);
    } else {
      g_mount_unmount_with_operation(m, G_MOUNT_UNMOUNT_NONE, op, nullptr,
                                     +[](GObject *src, GAsyncResult *res, gpointer data) {
                                       std::unique_ptr<Done> done((Done *)data);
                                       GError *e = nullptr;
                                       g_mount_unmount_with_operation_finish(G_MOUNT(src), res, &e);
                                       report(*done, error_text(e));
                                       schedule_rebuild();
                                     },
                                     d);
    }
    g_object_unref(op);
    g_object_unref(m);
  });
}

void Client::list(const std::string &uri, std::function<void(const std::string &, std::vector<NetEntry>)> done) {
  start();
  fld::glib_call([uri, done] {
    GFile *f = g_file_new_for_uri(uri.c_str());
    ListJob *j = new ListJob{{}, done};
    g_file_enumerate_children_async(
        f, "standard::name,standard::display-name,standard::icon,standard::target-uri,standard::type",
        G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
          ListJob *j = (ListJob *)data;
          GError *e = nullptr;
          GFileEnumerator *en = g_file_enumerate_children_finish(G_FILE(src), res, &e);
          if (!en) {
            if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
              g_clear_error(&e);
              list_done(j, "it needs GVfs (install gvfs, gvfs-fuse and gvfs-smb)");
            } else list_done(j, error_text(e));
            return;
          }
          g_file_enumerator_next_files_async(en, 64, G_PRIORITY_DEFAULT, nullptr, next_files, j);
        },
        j);
    g_object_unref(f);
  });
}

} // namespace gv
