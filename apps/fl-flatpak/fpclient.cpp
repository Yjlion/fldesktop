#include "fpclient.h"

#include <appstream.h>
#include <flatpak.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <sys/stat.h>
#include <thread>

#include "fld/glib_bridge.h"
#include "fld/ini.h"
#include "fld/util.h"

namespace fp {

void Job::cancel() {
  canceled->store(true);
  if (cancellable) g_cancellable_cancel((GCancellable *)cancellable.get());
}

namespace {

std::string err_text(GError *e) {
  if (!e) return "";
  if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return "Canceled.";
  std::string m = e->message;
  if (g_error_matches(e, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED)) return "It is already installed.";
  if (g_error_matches(e, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED)) return "It is not installed.";
  if (g_error_matches(e, FLATPAK_ERROR, FLATPAK_ERROR_REMOTE_NOT_FOUND)) return "The source was not found: " + m;
  if (g_error_matches(e, FLATPAK_ERROR, FLATPAK_ERROR_PERMISSION_DENIED))
    return "You are not allowed to do this.\n\n" + m;
  return m;
}

FlatpakInstallation *open_installation(const std::string &which, GError **e) {
  if (which == "user") return flatpak_installation_new_user(nullptr, e);
  return flatpak_installation_new_system(nullptr, e);
}

std::string g2s(const char *s) { return s ? s : ""; }

// --- AppStream ---------------------------------------------------------------------

void parse_appstream(const std::string &remote, GFile *dir, std::map<std::string, App> &out) {
  GFile *f = g_file_get_child(dir, "appstream.xml.gz");
  if (!g_file_query_exists(f, nullptr)) {
    g_object_unref(f);
    f = g_file_get_child(dir, "appstream.xml");
  }
  AsMetadata *md = as_metadata_new();
  as_metadata_set_format_style(md, AS_FORMAT_STYLE_CATALOG);
  GError *e = nullptr;
  if (!as_metadata_parse_file(md, f, AS_FORMAT_KIND_XML, &e)) {
    g_clear_error(&e);
    g_object_unref(md);
    g_object_unref(f);
    return;
  }
  gchar *dpath = g_file_get_path(dir);
  std::string base = dpath ? dpath : "";
  g_free(dpath);
  AsComponentBox *box = as_metadata_get_components(md);
  for (guint i = 0; i < as_component_box_len(box); i++) {
    AsComponent *c = as_component_box_index(box, i);
    AsComponentKind k = as_component_get_kind(c);
    if (k != AS_COMPONENT_KIND_DESKTOP_APP && k != AS_COMPONENT_KIND_CONSOLE_APP) continue;
    AsBundle *b = as_component_get_bundle(c, AS_BUNDLE_KIND_FLATPAK);
    if (!b) continue;
    App a;
    a.ref = g2s(as_bundle_get_id(b));
    a.id = ref_id(a.ref);
    if (a.id.empty() || out.count(a.id)) continue;
    a.remote = remote;
    a.in_catalog = true;
    a.name = g2s(as_component_get_name(c));
    a.summary = g2s(as_component_get_summary(c));
    a.description = strip_markup(g2s(as_component_get_description(c)));
    if (AsDeveloper *dev = as_component_get_developer(c)) a.developer = g2s(as_developer_get_name(dev));
    a.license = g2s(as_component_get_project_license(c));
    a.homepage = g2s(as_component_get_url(c, AS_URL_KIND_HOMEPAGE));
    if (GPtrArray *cats = as_component_get_categories(c))
      for (guint j = 0; j < cats->len; j++) a.categories.push_back((const char *)g_ptr_array_index(cats, j));
    if (AsReleaseList *rl = as_component_get_releases_plain(c))
      if (as_release_list_len(rl) > 0) a.version = g2s(as_release_get_version(as_release_list_index(rl, 0)));
    // cached icons: icons/<size>x<size>/<name>, prefer 64 then 128
    GPtrArray *icons = as_component_get_icons(c);
    int best = 0;
    for (guint j = 0; icons && j < icons->len; j++) {
      AsIcon *ic = AS_ICON(g_ptr_array_index(icons, j));
      if (as_icon_get_kind(ic) != AS_ICON_KIND_CACHED) continue;
      int w = (int)as_icon_get_width(ic);
      int score = w == 64 ? 3 : w == 128 ? 2 : 1;
      if (score <= best) continue;
      std::string p = base + "/icons/" + std::to_string(w) + "x" + std::to_string(w) + "/" + g2s(as_icon_get_name(ic));
      if (fld::file_exists(p)) {
        a.icon = p;
        best = score;
      }
    }
    if (a.name.empty()) a.name = a.id;
    out[a.id] = a;
  }
  g_object_unref(md);
  g_object_unref(f);
}

void run_bg(std::function<void()> fn) { std::thread(std::move(fn)).detach(); }

// --- transactions ---------------------------------------------------------------------

struct TxCtx {
  ProgressFn progress;
  std::string error;
  int total = 0, index = 0;
};

const char *op_verb(FlatpakTransactionOperationType t) {
  switch (t) {
  case FLATPAK_TRANSACTION_OPERATION_INSTALL:
  case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE: return "Installing";
  case FLATPAK_TRANSACTION_OPERATION_UPDATE: return "Updating";
  case FLATPAK_TRANSACTION_OPERATION_UNINSTALL: return "Removing";
  default: return "Working on";
  }
}

struct PCtx {
  TxCtx *c;
  std::string what;
};

gboolean on_ready(FlatpakTransaction *tx, gpointer d) {
  TxCtx *c = (TxCtx *)d;
  GList *ops = flatpak_transaction_get_operations(tx);
  c->total = (int)g_list_length(ops);
  g_list_free_full(ops, g_object_unref);
  return TRUE;
}

void on_progress_changed(FlatpakTransactionProgress *pr, gpointer d) {
  PCtx *pc = (PCtx *)d;
  if (!pc->c->progress) return;
  Progress x;
  int pct = flatpak_transaction_progress_get_progress(pr);
  // overall percent over all operations
  int n = std::max(1, pc->c->total);
  x.percent = ((pc->c->index - 1) * 100 + pct) / n;
  x.status = pc->what;
  auto fn = pc->c->progress;
  fld::ui_call([fn, x] { fn(x); });
}

void free_pctx(gpointer d, GClosure *) { delete (PCtx *)d; }

void on_new_operation(FlatpakTransaction *, FlatpakTransactionOperation *op, FlatpakTransactionProgress *p,
                      gpointer d) {
  TxCtx *c = (TxCtx *)d;
  c->index++;
  std::string what = std::string(op_verb(flatpak_transaction_operation_get_operation_type(op))) + " " +
                     ref_id(g2s(flatpak_transaction_operation_get_ref(op)));
  if (c->total > 1) what += " (" + std::to_string(c->index) + "/" + std::to_string(c->total) + ")";
  flatpak_transaction_progress_set_update_frequency(p, 300);
  g_signal_connect_data(p, "changed", G_CALLBACK(on_progress_changed), new PCtx{c, what}, free_pctx,
                        (GConnectFlags)0);
  if (c->progress) {
    Progress x;
    x.status = what;
    auto fn = c->progress;
    fld::ui_call([fn, x] { fn(x); });
  }
}

gboolean on_operation_error(FlatpakTransaction *, FlatpakTransactionOperation *op, GError *error, gint details,
                            gpointer d) {
  TxCtx *c = (TxCtx *)d;
  // a failed optional part (an extension) does not stop the rest
  if (details & FLATPAK_TRANSACTION_ERROR_DETAILS_NON_FATAL) return TRUE;
  if (c->error.empty()) c->error = ref_id(g2s(flatpak_transaction_operation_get_ref(op))) + ": " + err_text(error);
  return FALSE;
}

// runtimes from the source a .flatpakref names are fine
gboolean on_add_new_remote(FlatpakTransaction *, FlatpakTransactionRemoteReason, const char *, const char *,
                           const char *, gpointer) {
  return TRUE;
}

gint on_choose_remote(FlatpakTransaction *, const char *, const char *, const char *const *, gpointer) { return 0; }

void tx_connect(FlatpakTransaction *t, TxCtx *ctx) {
  g_signal_connect(t, "ready", G_CALLBACK(on_ready), ctx);
  g_signal_connect(t, "new-operation", G_CALLBACK(on_new_operation), ctx);
  g_signal_connect(t, "operation-error", G_CALLBACK(on_operation_error), ctx);
  g_signal_connect(t, "add-new-remote", G_CALLBACK(on_add_new_remote), nullptr);
  g_signal_connect(t, "choose-remote-for-ref", G_CALLBACK(on_choose_remote), nullptr);
}

// builds and runs a transaction in a worker thread
Job run_tx(const std::string &installation, ProgressFn progress, Done done,
           std::function<bool(FlatpakTransaction *, FlatpakInstallation *, GError **)> add) {
  Job job;
  GCancellable *cancel = g_cancellable_new();
  job.cancellable = std::shared_ptr<void>(cancel, [](void *p) { g_object_unref(p); });
  auto keep = job.cancellable;
  run_bg([installation, progress, done, add, keep] {
    GCancellable *cancel = (GCancellable *)keep.get();
    // libflatpak runs its signals in the thread-default context of the caller
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GError *e = nullptr;
    std::string err;
    FlatpakInstallation *inst = open_installation(installation, &e);
    FlatpakTransaction *t = inst ? flatpak_transaction_new_for_installation(inst, cancel, &e) : nullptr;
    TxCtx tc;
    tc.progress = progress;
    if (t) {
      flatpak_transaction_add_default_dependency_sources(t);
      tx_connect(t, &tc);
      if (add(t, inst, &e)) {
        if (!flatpak_transaction_run(t, cancel, &e) && tc.error.empty()) err = err_text(e);
      } else {
        err = err_text(e);
      }
    } else {
      err = err_text(e);
    }
    if (err.empty()) err = tc.error;
    if (err.empty() && g_cancellable_is_cancelled(cancel)) err = "Canceled.";
    g_clear_error(&e);
    if (t) g_object_unref(t);
    if (inst) g_object_unref(inst);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    fld::ui_call([done, err] { done(err); });
  });
  return job;
}

} // namespace

void load_catalog(bool refresh, std::function<void(const Catalog &)> done, ProgressFn progress) {
  run_bg([refresh, done, progress] {
    Catalog cat;
    std::map<std::string, App> apps;
    std::map<std::string, App> installed;
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    for (const char *which : {"system", "user"}) {
      GError *e = nullptr;
      FlatpakInstallation *inst = open_installation(which, &e);
      if (!inst) {
        g_clear_error(&e);
        continue;
      }
      GPtrArray *remotes = flatpak_installation_list_remotes(inst, nullptr, &e);
      g_clear_error(&e);
      for (guint i = 0; remotes && i < remotes->len; i++) {
        FlatpakRemote *r = FLATPAK_REMOTE(g_ptr_array_index(remotes, i));
        if (flatpak_remote_get_disabled(r) || flatpak_remote_get_noenumerate(r)) continue;
        std::string name = g2s(flatpak_remote_get_name(r));
        gchar *url = flatpak_remote_get_url(r);
        if (name == "flathub" || (url && strstr(url, "flathub.org"))) cat.have_flathub = true;
        g_free(url);
        cat.remotes.push_back(name + (std::string(which) == "user" ? " (user)" : ""));
        GFile *dir = flatpak_remote_get_appstream_dir(r, nullptr);
        GFile *xml = g_file_get_child(dir, "appstream.xml.gz");
        bool have = g_file_query_exists(xml, nullptr);
        g_object_unref(xml);
        if (refresh || !have) {
          if (progress) {
            Progress p;
            p.status = "Downloading the catalog of " + name;
            fld::ui_call([progress, p] { progress(p); });
          }
          gboolean changed = FALSE;
          if (!flatpak_installation_update_appstream_full_sync(inst, name.c_str(), nullptr, nullptr, nullptr,
                                                               &changed, nullptr, &e)) {
            if (cat.error.empty()) cat.error = "Could not download the catalog of " + name + ": " + err_text(e);
            g_clear_error(&e);
          }
        }
        if (progress) {
          Progress p;
          p.status = "Reading the catalog of " + name;
          fld::ui_call([progress, p] { progress(p); });
        }
        parse_appstream(name, dir, apps);
        g_object_unref(dir);
      }
      if (remotes) g_ptr_array_unref(remotes);
      // installed apps
      GPtrArray *refs = flatpak_installation_list_installed_refs_by_kind(inst, FLATPAK_REF_KIND_APP, nullptr, &e);
      g_clear_error(&e);
      for (guint i = 0; refs && i < refs->len; i++) {
        FlatpakInstalledRef *r = FLATPAK_INSTALLED_REF(g_ptr_array_index(refs, i));
        App a;
        a.id = g2s(flatpak_ref_get_name(FLATPAK_REF(r)));
        gchar *full = flatpak_ref_format_ref(FLATPAK_REF(r));
        a.ref = g2s(full);
        g_free(full);
        a.installed = true;
        a.installation = which;
        a.remote = g2s(flatpak_installed_ref_get_origin(r));
        a.name = g2s(flatpak_installed_ref_get_appdata_name(r));
        a.summary = g2s(flatpak_installed_ref_get_appdata_summary(r));
        a.version = g2s(flatpak_installed_ref_get_appdata_version(r));
        a.license = g2s(flatpak_installed_ref_get_appdata_license(r));
        a.installed_size = flatpak_installed_ref_get_installed_size(r);
        // the exported icon of an installed app
        std::string dep = g2s(flatpak_installed_ref_get_deploy_dir(r));
        for (const char *sz : {"64x64", "128x128", "scalable"}) {
          std::string p = dep + "/export/share/icons/hicolor/" + sz + "/apps/" + a.id +
                          (std::string(sz) == "scalable" ? ".svg" : ".png");
          if (a.icon.empty() && fld::file_exists(p)) a.icon = p;
        }
        if (a.name.empty()) a.name = a.id;
        installed[a.id] = a;
      }
      if (refs) g_ptr_array_unref(refs);
      // updates (asks the remotes)
      GPtrArray *ups = flatpak_installation_list_installed_refs_for_update(inst, nullptr, &e);
      g_clear_error(&e);
      for (guint i = 0; ups && i < ups->len; i++) {
        FlatpakInstalledRef *r = FLATPAK_INSTALLED_REF(g_ptr_array_index(ups, i));
        std::string id = g2s(flatpak_ref_get_name(FLATPAK_REF(r)));
        auto it = installed.find(id);
        if (it != installed.end() && flatpak_ref_get_kind(FLATPAK_REF(r)) == FLATPAK_REF_KIND_APP)
          it->second.update = true;
      }
      if (ups) g_ptr_array_unref(ups);
      g_object_unref(inst);
    }
    // installed runtimes and extensions (shown in their own list)
    std::vector<App> runtimes;
    for (const char *which : {"system", "user"}) {
      GError *e = nullptr;
      FlatpakInstallation *inst = open_installation(which, &e);
      g_clear_error(&e);
      if (!inst) continue;
      GPtrArray *refs = flatpak_installation_list_installed_refs_by_kind(inst, FLATPAK_REF_KIND_RUNTIME, nullptr, &e);
      g_clear_error(&e);
      for (guint i = 0; refs && i < refs->len; i++) {
        FlatpakInstalledRef *r = FLATPAK_INSTALLED_REF(g_ptr_array_index(refs, i));
        App a;
        a.id = g2s(flatpak_ref_get_name(FLATPAK_REF(r)));
        gchar *full = flatpak_ref_format_ref(FLATPAK_REF(r));
        a.ref = g2s(full);
        g_free(full);
        a.runtime = true;
        a.installed = true;
        a.installation = which;
        a.remote = g2s(flatpak_installed_ref_get_origin(r));
        a.name = g2s(flatpak_installed_ref_get_appdata_name(r));
        if (a.name.empty()) a.name = a.id;
        a.version = g2s(flatpak_ref_get_branch(FLATPAK_REF(r)));
        a.summary = g2s(flatpak_installed_ref_get_appdata_summary(r));
        if (a.summary.empty()) a.summary = "Runtime " + a.ref;
        a.installed_size = flatpak_installed_ref_get_installed_size(r);
        runtimes.push_back(a);
      }
      if (refs) g_ptr_array_unref(refs);
      g_object_unref(inst);
    }
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    // merge: catalog data, installed state
    for (auto &kv : installed) {
      auto it = apps.find(kv.first);
      if (it == apps.end()) {
        apps[kv.first] = kv.second;
        continue;
      }
      App &a = it->second;
      a.installed = true;
      a.update = kv.second.update;
      a.installation = kv.second.installation;
      a.ref = kv.second.ref;
      a.installed_size = kv.second.installed_size;
      if (a.icon.empty()) a.icon = kv.second.icon;
    }
    for (auto &kv : apps) cat.apps.push_back(kv.second);
    for (auto &r : runtimes) cat.apps.push_back(r);
    std::sort(cat.apps.begin(), cat.apps.end(),
              [](const App &a, const App &b) { return fld::lower(a.name) < fld::lower(b.name); });
    fld::ui_call([done, cat] { done(cat); });
  });
}

Job install(const App &app, bool user, ProgressFn progress, Done done) {
  std::string remote = app.remote, ref = app.ref;
  return run_tx(user ? "user" : "system", progress, done,
                [remote, ref](FlatpakTransaction *t, FlatpakInstallation *, GError **e) {
                  return (bool)flatpak_transaction_add_install(t, remote.c_str(), ref.c_str(), nullptr, e);
                });
}

Job uninstall(const App &app, ProgressFn progress, Done done) {
  std::string ref = app.ref;
  return run_tx(app.installation.empty() ? "system" : app.installation, progress, done,
                [ref](FlatpakTransaction *t, FlatpakInstallation *, GError **e) {
                  return (bool)flatpak_transaction_add_uninstall(t, ref.c_str(), e);
                });
}

Job update(const std::vector<App> &apps, ProgressFn progress, Done done) {
  // one transaction per installation, one after the other
  std::map<std::string, std::vector<std::string>> by;
  for (auto &a : apps)
    if (a.installed) by[a.installation.empty() ? "system" : a.installation].push_back(a.ref);
  Job job;
  GCancellable *cancel = g_cancellable_new();
  job.cancellable = std::shared_ptr<void>(cancel, [](void *p) { g_object_unref(p); });
  auto steps = std::make_shared<std::vector<std::pair<std::string, std::vector<std::string>>>>(by.begin(), by.end());
  auto next = std::make_shared<std::function<void(size_t, std::string)>>();
  *next = [steps, progress, done, next](size_t i, std::string err) {
    if (!err.empty() || i >= steps->size()) {
      done(err);
      return;
    }
    auto refs = (*steps)[i].second;
    run_tx((*steps)[i].first, progress, [next, i](const std::string &e) { (*next)(i + 1, e); },
           [refs](FlatpakTransaction *t, FlatpakInstallation *, GError **e) {
             for (auto &r : refs)
               if (!flatpak_transaction_add_update(t, r.c_str(), nullptr, nullptr, e)) return false;
             return true;
           });
  };
  (*next)(0, "");
  return job;
}

Job install_file(const std::string &path, bool user, ProgressFn progress, Done done) {
  std::string ext = fld::extension(path);
  if (ext == "flatpakrepo") {
    // a source, not a transaction
    std::string p = path;
    run_bg([p, user, done] {
      std::string data, err;
      GError *e = nullptr;
      if (!fld::read_file(p, data)) err = "Cannot read " + p;
      else {
        GBytes *b = g_bytes_new(data.data(), data.size());
        std::string name = fld::basename(p);
        name = name.substr(0, name.rfind('.'));
        FlatpakRemote *r = flatpak_remote_new_from_file(name.c_str(), b, &e);
        FlatpakInstallation *inst = r ? open_installation(user ? "user" : "system", &e) : nullptr;
        if (!r || !inst || !flatpak_installation_add_remote(inst, r, TRUE, nullptr, &e)) err = err_text(e);
        if (r) g_object_unref(r);
        if (inst) g_object_unref(inst);
        g_bytes_unref(b);
        g_clear_error(&e);
      }
      fld::ui_call([done, err] { done(err); });
    });
    return {};
  }
  std::string p = path;
  return run_tx(user ? "user" : "system", progress, done,
                [p, ext](FlatpakTransaction *t, FlatpakInstallation *, GError **e) {
                  if (ext == "flatpak") {
                    GFile *f = g_file_new_for_path(p.c_str());
                    bool ok = flatpak_transaction_add_install_bundle(t, f, nullptr, e);
                    g_object_unref(f);
                    return ok;
                  }
                  std::string data;
                  if (!fld::read_file(p, data)) {
                    g_set_error(e, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot read %s", p.c_str());
                    return false;
                  }
                  GBytes *b = g_bytes_new(data.data(), data.size());
                  bool ok = flatpak_transaction_add_install_flatpakref(t, b, e);
                  g_bytes_unref(b);
                  return ok;
                });
}

void add_flathub(bool user, Done done) {
  run_bg([user, done] {
    std::vector<std::string> cmd = {"flatpak", "remote-add", "--if-not-exists"};
    if (user) cmd.push_back("--user");
    cmd.push_back("flathub");
    cmd.push_back("https://dl.flathub.org/repo/flathub.flatpakrepo");
    std::string out;
    int rc = fld::which("flatpak").empty() ? -1 : fld::run_capture(cmd, out);
    std::string err = rc == 0 ? "" : rc < 0 ? "The flatpak command is not installed." : "Adding Flathub failed.\n\n" + out;
    fld::ui_call([done, err] { done(err); });
  });
}

void launch(const App &app, Done done) {
  std::vector<std::string> cmd = {"flatpak", "run"};
  if (app.installation == "user") cmd.push_back("--user");
  cmd.push_back(app.id);
  bool ok = fld::spawn(cmd);
  if (done) done(ok ? "" : "Could not start " + app.id);
}

std::string metadata(const App &app) {
  if (!app.installed) return "";
  GError *e = nullptr;
  FlatpakInstallation *inst = open_installation(app.installation.empty() ? "system" : app.installation, &e);
  std::string out;
  if (inst) {
    auto parts = fld::split(app.ref, '/', true);
    FlatpakInstalledRef *r = flatpak_installation_get_installed_ref(
        inst, FLATPAK_REF_KIND_APP, app.id.c_str(), parts.size() > 2 ? parts[2].c_str() : nullptr,
        parts.size() > 3 ? parts[3].c_str() : nullptr, nullptr, &e);
    if (r) {
      GBytes *b = flatpak_installed_ref_load_metadata(r, nullptr, &e);
      if (b) {
        gsize n = 0;
        const char *d = (const char *)g_bytes_get_data(b, &n);
        out.assign(d, n);
        g_bytes_unref(b);
      }
      g_object_unref(r);
    }
    g_object_unref(inst);
  }
  g_clear_error(&e);
  return out;
}

namespace {

struct InstalledRef {
  std::string ref, installation, deploy, metadata;
};

std::vector<InstalledRef> all_installed_refs() {
  std::vector<InstalledRef> out;
  for (const char *which : {"system", "user"}) {
    GError *e = nullptr;
    FlatpakInstallation *inst = open_installation(which, &e);
    g_clear_error(&e);
    if (!inst) continue;
    GPtrArray *refs = flatpak_installation_list_installed_refs(inst, nullptr, &e);
    g_clear_error(&e);
    for (guint i = 0; refs && i < refs->len; i++) {
      FlatpakInstalledRef *r = FLATPAK_INSTALLED_REF(g_ptr_array_index(refs, i));
      InstalledRef x;
      gchar *full = flatpak_ref_format_ref(FLATPAK_REF(r));
      x.ref = g2s(full);
      g_free(full);
      x.installation = which;
      x.deploy = g2s(flatpak_installed_ref_get_deploy_dir(r));
      GBytes *b = flatpak_installed_ref_load_metadata(r, nullptr, &e);
      g_clear_error(&e);
      if (b) {
        gsize n = 0;
        const char *d = (const char *)g_bytes_get_data(b, &n);
        x.metadata.assign(d, n);
        g_bytes_unref(b);
      }
      out.push_back(x);
    }
    if (refs) g_ptr_array_unref(refs);
    g_object_unref(inst);
  }
  return out;
}

void list_files(const std::string &root, const std::string &rel, RefInfo &info, size_t max) {
  std::vector<std::string> names = fld::list_dir(rel.empty() ? root : root + "/" + rel, true);
  std::sort(names.begin(), names.end());
  for (auto &n : names) {
    if (info.files.size() >= max) {
      info.files_truncated = true;
      return;
    }
    std::string r = rel.empty() ? n : rel + "/" + n;
    std::string full = root + "/" + r;
    struct stat st;
    bool dir = lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    info.files.push_back(dir ? r + "/" : r);
    if (dir) list_files(root, r, info, max);
  }
}

}  // namespace

void ref_info(const App &app, std::function<void(const RefInfo &)> done) {
  std::string ref = app.ref, installation = app.installation.empty() ? "system" : app.installation;
  run_bg([ref, installation, done] {
    RefInfo info;
    auto refs = all_installed_refs();
    const InstalledRef *self = nullptr;
    for (auto &r : refs)
      if (r.ref == ref && r.installation == installation) self = &r;
    if (!self)
      for (auto &r : refs)
        if (r.ref == ref) self = &r;
    if (!self) {
      info.error = "It is not installed.";
      fld::ui_call([done, info] { done(info); });
      return;
    }
    auto installed = [&](const std::string &x) {
      for (auto &r : refs)
        if (r.ref == x) return true;
      return false;
    };
    auto add_dep = [&](const std::string &x) {
      if (x.empty()) return;
      std::string label = installed(x) ? x : x + " (not installed)";
      if (std::find(info.depends.begin(), info.depends.end(), label) == info.depends.end()) info.depends.push_back(label);
    };
    add_dep(metadata_runtime(self->metadata));
    add_dep(metadata_extension_of(self->metadata));
    for (auto &r : refs) {
      if (r.ref == ref) continue;
      if (metadata_runtime(r.metadata) == ref || metadata_extension_of(r.metadata) == ref)
        if (std::find(info.used_by.begin(), info.used_by.end(), r.ref) == info.used_by.end()) info.used_by.push_back(r.ref);
    }
    std::sort(info.used_by.begin(), info.used_by.end());
    if (!self->deploy.empty()) list_files(self->deploy + "/files", "", info, 20000);
    fld::ui_call([done, info] { done(info); });
  });
}

std::string metadata_runtime(const std::string &keyfile) {
  fld::Ini ini;
  ini.parse(keyfile);
  std::string r = fld::trim(ini.get("Application", "runtime", ""));
  if (r.empty()) return "";
  return fld::starts_with(r, "runtime/") ? r : "runtime/" + r;
}

std::string metadata_extension_of(const std::string &keyfile) {
  fld::Ini ini;
  ini.parse(keyfile);
  return fld::trim(ini.get("ExtensionOf", "ref", ""));
}

std::string overrides_path(const std::string &app_id) {
  return fld::data_home() + "/flatpak/overrides/" + app_id;
}

// ---- helpers ---------------------------------------------------------------------------

std::string ref_id(const std::string &ref) {
  auto parts = fld::split(ref, '/', true);
  if (parts.size() >= 2 && (parts[0] == "app" || parts[0] == "runtime")) return parts[1];
  return parts.empty() ? "" : parts[0];
}

std::string strip_markup(const std::string &s) {
  // AppStream descriptions: <p>, <ul><li>, <ol>, <em>, <code>
  std::string out;
  size_t i = 0;
  int list_n = 0;
  bool ordered = false;
  while (i < s.size()) {
    if (s[i] == '<') {
      size_t e = s.find('>', i);
      if (e == std::string::npos) break;
      std::string tag = s.substr(i + 1, e - i - 1);
      if (tag == "p" || tag == "ul" || tag == "ol") {
        if (!out.empty() && out.back() != '\n') out += "\n";
        if (!out.empty()) out += "\n";
        ordered = tag == "ol";
        list_n = 0;
      } else if (tag == "li") {
        if (!out.empty() && out.back() != '\n') out += "\n";
        out += ordered ? "  " + std::to_string(++list_n) + ". " : "  • ";
      }
      i = e + 1;
    } else if (s[i] == '&') {
      size_t e = s.find(';', i);
      std::string ent = e == std::string::npos ? "" : s.substr(i + 1, e - i - 1);
      if (ent == "amp") out += '&';
      else if (ent == "lt") out += '<';
      else if (ent == "gt") out += '>';
      else if (ent == "quot") out += '"';
      else if (ent == "apos") out += '\'';
      else {
        out += '&';
        i++;
        continue;
      }
      i = e + 1;
    } else if (s[i] == '\n' || s[i] == '\r' || s[i] == '\t') {
      if (!out.empty() && out.back() != ' ' && out.back() != '\n') out += ' ';
      i++;
    } else if (s[i] == ' ' && !out.empty() && (out.back() == ' ' || out.back() == '\n')) {
      i++;
    } else {
      out += s[i++];
    }
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  size_t start = out.find_first_not_of("\n ");
  return start == std::string::npos ? "" : out.substr(start);
}

const std::vector<std::string> &browse_categories() {
  static const std::vector<std::string> c = {"AudioVideo", "Development", "Education", "Game",   "Graphics",
                                             "Network",    "Office",      "Science",   "System", "Utility"};
  return c;
}

std::string category_label(const std::string &c) {
  static const std::pair<const char *, const char *> names[] = {
      {"AudioVideo", "Audio & Video"}, {"Development", "Development"}, {"Education", "Education"},
      {"Game", "Games"},               {"Graphics", "Graphics & Photography"}, {"Network", "Internet"},
      {"Office", "Office"},            {"Science", "Science"},          {"System", "System"},
      {"Utility", "Utilities"},
  };
  for (auto &n : names)
    if (c == n.first) return n.second;
  return c;
}

} // namespace fp
