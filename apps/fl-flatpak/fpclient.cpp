#include "fpclient.h"

#include <gio/gio.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <flatpak.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
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

} // namespace

// --- AppStream ---------------------------------------------------------------------
//
// Flathub's catalog has thousands of components. libappstream builds an
// object tree of all of it (a few hundred MB at peak) before anything can be
// copied out; this reads the XML as a stream and keeps only what App holds.

namespace {

struct IconCand {
  std::string kind, name, url, file;
  int width = 0, scale = 1;
};

struct Comp {
  bool app = false;
  std::string ref;
  std::map<std::string, std::string> name, summary, dev_name, desc; // by xml:lang
  std::string license, homepage, version;
  bool have_version = false;
  std::vector<std::string> categories;
  std::vector<IconCand> icons;
};

std::string markup_escape(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

const char *attr(const char **names, const char **values, const char *key) {
  for (int i = 0; names[i]; i++)
    if (!strcmp(names[i], key)) return values[i];
  return nullptr;
}

struct AsParser {
  std::string remote, base;
  std::vector<std::string> langs; // preferred first; "" is untranslated
  std::map<std::string, App> *out;

  std::vector<std::string> stack;
  bool in_comp = false;
  size_t comp_depth = 0; // the component's children are at stack[comp_depth]
  Comp c;
  std::string text;        // text of the element being read
  std::string text_lang;   // its xml:lang
  std::string desc_lang;   // xml:lang of <description>
  std::set<std::string> list_open; // langs whose description has an open <ul>/<ol>
  std::string list_tag;
  IconCand icon;

  // how well an xml:lang fits (lower is better, -1: not at all): one the
  // locale names, else the same language for another country (de_CH for de)
  int rank(const std::string &tag) const {
    std::string lang = tag;
    std::replace(lang.begin(), lang.end(), '-', '_'); // "de-DE" is written too
    auto base = [](const std::string &l) { return l.substr(0, l.find_first_of("_@")); };
    int near = -1;
    for (size_t i = 0; i < langs.size(); i++) {
      if (langs[i] == lang) return (int)i * 2;
      if (near < 0 && !lang.empty() && !langs[i].empty() && base(langs[i]) == base(lang)) near = (int)i * 2 + 1;
    }
    return near;
  }
  bool wanted(const std::string &lang) const { return rank(lang) >= 0; }
  std::string best(const std::map<std::string, std::string> &m) const {
    int r = -1;
    std::string out;
    for (auto &kv : m) {
      int k = rank(kv.first);
      if (k >= 0 && !kv.second.empty() && (r < 0 || k < r)) {
        r = k;
        out = kv.second;
      }
    }
    return out;
  }
  const std::string &parent() const {
    static const std::string none;
    return stack.size() >= 2 ? stack[stack.size() - 2] : none;
  }
  // inside the component's own <description> (not a release's)
  bool in_desc() const { return in_comp && stack.size() > comp_depth && stack[comp_depth] == "description"; }

  void start(const char *el, const char **an, const char **av) {
    std::string e = el;
    stack.push_back(e);
    if (e == "component" && stack.size() <= 2) {
      in_comp = true;
      comp_depth = stack.size();
      c = Comp();
      const char *t = attr(an, av, "type");
      std::string type = t ? t : "";
      c.app = type == "desktop-application" || type == "desktop" || type == "console-application";
      return;
    }
    if (!in_comp) return;
    // inline markup in a paragraph: its text is part of the paragraph's
    if (in_desc() && (e == "em" || e == "code")) return;
    const char *l = attr(an, av, "xml:lang");
    text.clear();
    text_lang = l ? l : "";
    if (e == "description" && parent() == "component") {
      desc_lang = text_lang;
      list_open.clear();
    } else if (in_desc()) {
      std::string lang = l ? text_lang : desc_lang;
      text_lang = lang;
      if (e == "ul" || e == "ol") list_tag = e;
      else if (e == "li" && wanted(lang) && !list_open.count(lang)) {
        c.desc[lang] += "<" + (list_tag.empty() ? std::string("ul") : list_tag) + ">";
        list_open.insert(lang);
      } else if (e == "p" || e == "heading") {
        // a paragraph (or heading) ends the lists before it
        if (list_open.count(lang)) {
          c.desc[lang] += "</ul>";
          list_open.erase(lang);
        }
      }
    } else if (e == "bundle" && parent() == "component") {
      const char *t = attr(an, av, "type");
      text_lang = t && !strcmp(t, "flatpak") ? "flatpak" : "";
    } else if (e == "url" && parent() == "component") {
      const char *t = attr(an, av, "type");
      text_lang = t ? t : "";
    } else if (e == "release" && parent() == "releases") {
      // the newest comes first (catalogs are sorted; timestamps are not always right)
      const char *v = attr(an, av, "version");
      if (v && !c.have_version) {
        c.version = v;
        c.have_version = true;
      }
    } else if (e == "icon" && parent() == "component") {
      icon = IconCand();
      const char *t = attr(an, av, "type");
      icon.kind = t ? t : "";
      if (const char *w = attr(an, av, "width")) icon.width = atoi(w);
      if (const char *s = attr(an, av, "scale")) icon.scale = std::max(1, atoi(s));
    }
  }

  void chars(const char *t, gsize n) {
    if (in_comp) text.append(t, n);
  }

  void end(const char *el) {
    std::string e = el;
    if (in_desc() && (e == "em" || e == "code")) {
      stack.pop_back();
      return;
    }
    if (in_comp) {
      const std::string &p = parent();
      std::string v = fld::trim(text);
      if (e == "component" && stack.size() <= 2) {
        finish();
        in_comp = false;
      } else if (in_desc() && e != "description") {
        if ((e == "p" || e == "li") && wanted(text_lang))
          c.desc[text_lang] += "<" + e + ">" + markup_escape(v) + "</" + e + ">";
        else if (e == "heading" && wanted(text_lang))
          c.desc[text_lang] += "<p>" + markup_escape(v) + "</p>"; // a paragraph of its own
        else if (e == "ul" || e == "ol") {
          for (auto &l : list_open) c.desc[l] += "</ul>";
          list_open.clear();
          list_tag.clear();
        }
      } else if (p == "component") {
        if (e == "name" && wanted(text_lang)) c.name[text_lang] = v;
        else if (e == "summary" && wanted(text_lang)) c.summary[text_lang] = v;
        else if (e == "developer_name" && wanted(text_lang)) c.dev_name[text_lang] = v;
        else if (e == "project_license") c.license = v;
        else if (e == "url" && text_lang == "homepage") c.homepage = v;
        else if (e == "bundle" && text_lang == "flatpak" && c.ref.empty()) c.ref = v;
        else if (e == "icon") {
          if (icon.kind == "remote") icon.url = v;
          else if (icon.kind == "local") icon.file = v;
          else icon.name = v;
          c.icons.push_back(icon);
        }
      } else if (p == "developer" && e == "name" && wanted(text_lang)) {
        c.dev_name[text_lang] = v;
      } else if (p == "categories" && e == "category" && !v.empty()) {
        c.categories.push_back(v);
      }
      text.clear();
    }
    if (!stack.empty()) stack.pop_back();
  }

  void finish() {
    if (!c.app || c.ref.empty()) return;
    App a;
    a.ref = c.ref;
    a.id = ref_id(a.ref);
    if (a.id.empty() || out->count(a.id)) return;
    a.remote = remote;
    a.in_catalog = true;
    a.name = best(c.name);
    a.summary = best(c.summary);
    a.description = strip_markup(best(c.desc));
    a.developer = best(c.dev_name);
    a.license = c.license;
    a.homepage = c.homepage;
    a.categories = c.categories;
    a.version = c.version;
    // cached icons: icons/<size>x<size>[@<scale>]/<name> (or icons/flatpak/...);
    // the one closest to 128 px (sharp at 64 and on HiDPI), remote icons
    // and theme names as fallbacks
    int best_score = -1, best_remote = -1;
    for (auto &ic : c.icons) {
      int px = ic.width * ic.scale;
      int score = px >= 128 ? px - 128 : (128 - px) * 2;
      if (ic.kind == "stock") {
        if (a.icon_stock.empty()) a.icon_stock = ic.name;
      } else if (ic.kind == "remote") {
        if (best_remote < 0 || score < best_remote) {
          best_remote = score;
          a.icon_url = ic.url;
        }
      } else if (ic.kind == "cached" || ic.kind == "local") {
        if (best_score >= 0 && score >= best_score) continue;
        std::string dir = std::to_string(ic.width) + "x" + std::to_string(ic.width) +
                          (ic.scale > 1 ? "@" + std::to_string(ic.scale) : "");
        std::vector<std::string> paths = {ic.file};
        if (!ic.name.empty())
          for (const std::string &p : {base + "/icons/" + dir + "/" + ic.name, base + "/icons/flatpak/" + dir + "/" + ic.name})
            paths.push_back(p);
        for (const std::string &p : paths) {
          if (!p.empty() && p[0] == '/' && fld::file_exists(p)) {
            a.icon = p;
            best_score = score;
            break;
          }
        }
      }
    }
    if (a.name.empty()) a.name = a.id;
    (*out)[a.id] = a;
  }
};

void as_start(GMarkupParseContext *, const char *el, const char **an, const char **av, gpointer d, GError **) {
  static_cast<AsParser *>(d)->start(el, an, av);
}
void as_end(GMarkupParseContext *, const char *el, gpointer d, GError **) { static_cast<AsParser *>(d)->end(el); }
void as_text(GMarkupParseContext *, const char *t, gsize n, gpointer d, GError **) {
  static_cast<AsParser *>(d)->chars(t, n);
}
const GMarkupParser kAsParser = {as_start, as_end, as_text, nullptr, nullptr};

std::vector<std::string> locale_langs() {
  std::vector<std::string> out;
  for (const gchar *const *n = g_get_language_names(); n && *n; n++) {
    std::string l = *n;
    if (l == "C" || l == "POSIX") l = "";
    if (l.find('.') != std::string::npos) continue; // de_DE.UTF-8: de_DE comes too
    if (std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
  }
  if (std::find(out.begin(), out.end(), "") == out.end()) out.push_back("");
  return out;
}

} // namespace

static bool parse_appstream_stream(const std::string &remote, const std::string &base, GInputStream *in,
                            const std::vector<std::string> &langs, std::map<std::string, App> &out) {
  AsParser p;
  p.remote = remote;
  p.base = base;
  p.langs = langs;
  p.out = &out;
  GMarkupParseContext *ctx = g_markup_parse_context_new(&kAsParser, (GMarkupParseFlags)0, &p, nullptr);
  std::vector<char> buf(64 * 1024);
  bool ok = true;
  for (;;) {
    GError *e = nullptr;
    gssize n = g_input_stream_read(in, buf.data(), buf.size(), nullptr, &e);
    if (n < 0) {
      g_clear_error(&e);
      ok = false;
      break;
    }
    if (n == 0) break;
    if (!g_markup_parse_context_parse(ctx, buf.data(), n, &e)) {
      g_clear_error(&e);
      ok = false; // what was read until here is kept
      break;
    }
  }
  if (ok) {
    GError *e = nullptr;
    if (!g_markup_parse_context_end_parse(ctx, &e)) ok = false;
    g_clear_error(&e);
  }
  g_markup_parse_context_free(ctx);
  return ok;
}

bool parse_appstream_text(const std::string &remote, const std::string &base, const std::string &xml,
                          const std::vector<std::string> &langs, std::map<std::string, App> &out) {
  GInputStream *in = g_memory_input_stream_new_from_data(xml.data(), (gssize)xml.size(), nullptr);
  bool ok = parse_appstream_stream(remote, base, in, langs, out);
  g_object_unref(in);
  return ok;
}

static void parse_appstream(const std::string &remote, GFile *dir, std::map<std::string, App> &out) {
  bool gz = true;
  GFile *f = g_file_get_child(dir, "appstream.xml.gz");
  if (!g_file_query_exists(f, nullptr)) {
    g_object_unref(f);
    f = g_file_get_child(dir, "appstream.xml");
    gz = false;
  }
  GFileInputStream *fin = g_file_read(f, nullptr, nullptr);
  g_object_unref(f);
  if (!fin) return;
  GInputStream *in = G_INPUT_STREAM(fin);
  GZlibDecompressor *z = nullptr;
  if (gz) {
    z = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP);
    in = g_converter_input_stream_new(G_INPUT_STREAM(fin), G_CONVERTER(z));
  }
  gchar *dpath = g_file_get_path(dir);
  std::string base = dpath ? dpath : "";
  g_free(dpath);
  parse_appstream_stream(remote, base, in, locale_langs(), out);
  if (z) {
    g_object_unref(in);
    g_object_unref(z);
  }
  g_object_unref(fin);
}

namespace {

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

void load_catalog(bool refresh, std::function<void(Catalog &)> done, ProgressFn progress) {
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
        for (const char *sz : {"128x128", "scalable", "256x256", "64x64", "512x512", "48x48"}) {
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
      if (a.icon_stock.empty()) a.icon_stock = kv.second.icon_stock;
      if (a.icon_url.empty()) a.icon_url = kv.second.icon_url;
    }
    for (auto &kv : apps) cat.apps.push_back(kv.second);
    for (auto &r : runtimes) cat.apps.push_back(r);
    std::sort(cat.apps.begin(), cat.apps.end(),
              [](const App &a, const App &b) { return fld::lower(a.name) < fld::lower(b.name); });
    apps.clear();
    installed.clear();
#ifdef __GLIBC__
    // Reading Flathub's AppStream data takes a few hundred MB that glibc
    // would otherwise keep for this process after it is freed.
    malloc_trim(0);
#endif
    auto shared = std::make_shared<Catalog>(std::move(cat));
    fld::ui_call([done, shared] { done(*shared); });
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

// ---- sources ---------------------------------------------------------------

namespace {
// flatpak's command line: it talks to the system helper (polkit) itself
void run_flatpak(std::vector<std::string> cmd, Done done) {
  run_bg([cmd, done] {
    std::string out;
    int rc = fld::which(cmd[0]).empty() ? -1 : fld::run_capture(cmd, out);
    std::string err = rc == 0 ? "" : rc < 0 ? "The " + cmd[0] + " command is not installed." : fld::trim(out);
    if (rc > 0 && err.empty()) err = "flatpak failed (exit status " + std::to_string(rc) + ").";
    fld::ui_call([done, err] { done(err); });
  });
}
} // namespace

void list_remotes(std::function<void(std::vector<Remote> &, const std::string &)> done) {
  run_bg([done] {
    std::vector<Remote> out;
    std::string err;
    for (const char *which : {"system", "user"}) {
      GError *e = nullptr;
      FlatpakInstallation *inst = open_installation(which, &e);
      if (!inst) {
        g_clear_error(&e);
        continue;
      }
      std::map<std::string, int> counts;
      GPtrArray *refs = flatpak_installation_list_installed_refs(inst, nullptr, &e);
      g_clear_error(&e);
      for (guint i = 0; refs && i < refs->len; i++)
        counts[g2s(flatpak_installed_ref_get_origin(FLATPAK_INSTALLED_REF(g_ptr_array_index(refs, i))))]++;
      if (refs) g_ptr_array_unref(refs);
      GPtrArray *remotes = flatpak_installation_list_remotes(inst, nullptr, &e);
      if (!remotes && err.empty()) err = err_text(e);
      g_clear_error(&e);
      for (guint i = 0; remotes && i < remotes->len; i++) {
        FlatpakRemote *r = FLATPAK_REMOTE(g_ptr_array_index(remotes, i));
        Remote x;
        x.name = g2s(flatpak_remote_get_name(r));
        gchar *t = flatpak_remote_get_title(r), *u = flatpak_remote_get_url(r);
        x.title = g2s(t);
        x.url = g2s(u);
        g_free(t);
        g_free(u);
        x.installation = which;
        x.prio = flatpak_remote_get_prio(r);
        x.disabled = flatpak_remote_get_disabled(r);
        x.gpg_verify = flatpak_remote_get_gpg_verify(r);
        x.installed_refs = counts[x.name];
        // the remotes of installed bundles without a URL ("<app>-origin") are not sources to manage
        if (x.url.empty() && fld::ends_with(x.name, "-origin")) continue;
        out.push_back(x);
      }
      if (remotes) g_ptr_array_unref(remotes);
      g_object_unref(inst);
    }
    fld::ui_call([done, out, err]() mutable { done(out, err); });
  });
}

void add_remote(const std::string &installation, const std::string &name, const std::string &url,
                const std::string &title, const std::string &gpg_key, Done done) {
  run_flatpak(remote_add_cmd(installation, name, url, title, gpg_key), done);
}

void remove_remote(const Remote &r, bool force, Done done) { run_flatpak(remote_delete_cmd(r, force), done); }

void modify_remote(const Remote &r, Done done) { run_flatpak(remote_modify_cmd(r), done); }

Job remove_unused(const std::string &installation, ProgressFn progress, Done done) {
  return run_tx(installation, progress, done, [](FlatpakTransaction *t, FlatpakInstallation *inst, GError **e) {
    GPtrArray *unused = flatpak_installation_list_unused_refs(inst, nullptr, nullptr, e);
    if (!unused) return false;
    bool ok = true;
    for (guint i = 0; ok && i < unused->len; i++) {
      gchar *ref = flatpak_ref_format_ref(FLATPAK_REF(g_ptr_array_index(unused, i)));
      ok = flatpak_transaction_add_uninstall(t, ref, e);
      g_free(ref);
    }
    g_ptr_array_unref(unused);
    return ok;
  });
}

void repair(const std::string &installation, Done done) {
  std::vector<std::string> cmd = {"flatpak", "repair", installation == "user" ? "--user" : "--system"};
  // repairing the system installation needs root
  if (installation != "user") cmd.insert(cmd.begin(), "pkexec");
  run_flatpak(cmd, done);
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
