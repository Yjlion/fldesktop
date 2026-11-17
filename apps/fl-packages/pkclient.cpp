#include "pkclient.h"

#include <packagekit-glib2/packagekit.h>

#include <algorithm>
#include <cstring>

#include "fld/glib_bridge.h"
#include "fld/util.h"

namespace pk {

bool Package::installed() const { return info == PK_INFO_ENUM_INSTALLED || info == PK_INFO_ENUM_COLLECTION_INSTALLED; }

bool Package::is_update() const {
  switch (info) {
  case PK_INFO_ENUM_LOW:
  case PK_INFO_ENUM_ENHANCEMENT:
  case PK_INFO_ENUM_NORMAL:
  case PK_INFO_ENUM_BUGFIX:
  case PK_INFO_ENUM_IMPORTANT:
  case PK_INFO_ENUM_SECURITY:
    return true;
  default:
    return false;
  }
}

bool Error::untrusted() const {
  return code == PK_ERROR_ENUM_MISSING_GPG_SIGNATURE || code == PK_ERROR_ENUM_CANNOT_INSTALL_REPO_UNSIGNED ||
         code == PK_ERROR_ENUM_CANNOT_UPDATE_REPO_UNSIGNED || code == PK_ERROR_ENUM_BAD_GPG_SIGNATURE ||
         code == PK_ERROR_ENUM_GPG_FAILURE;
}

struct Job::Impl {
  GCancellable *cancel = g_cancellable_new();
  ~Impl() { g_object_unref(cancel); }
};

void Job::cancel() {
  if (impl) g_cancellable_cancel(impl->cancel);
}

namespace {

// one PkClient per transaction keeps calls independent
struct Call {
  PkClient *client = nullptr;
  std::shared_ptr<Job::Impl> job;
  ProgressFn progress;
  std::function<void(PkResults *, const Error &)> done; // GLib thread
  ~Call() {
    if (client) g_object_unref(client);
  }
};

Error error_from(GError *e) {
  Error r;
  if (!e) return r;
  if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    r.code = -1;
    r.text = "Canceled.";
    return r;
  }
  if (e->domain == PK_CLIENT_ERROR && e->code >= 0xff) r.code = e->code - 0xff;
  r.text = e->message;
  if (r.code == PK_ERROR_ENUM_TRANSACTION_CANCELLED || r.code == PK_ERROR_ENUM_PROCESS_KILL) r.code = -1;
  if (r.code == PK_ERROR_ENUM_NOT_AUTHORIZED) r.text = "You are not allowed to make this change.";
  else if (r.code == PK_ERROR_ENUM_NO_NETWORK) r.text = "There is no network connection.";
  else if (r.code == PK_ERROR_ENUM_LOCK_REQUIRED || r.code == PK_ERROR_ENUM_CANNOT_GET_LOCK)
    r.text = "Another program is changing the software. Try again when it has finished.\n\n" + r.text;
  else if (r.code == PK_ERROR_ENUM_NO_LICENSE_AGREEMENT)
    r.text = "A license agreement has to be accepted with the system's package tool first.\n\n" + r.text;
  else if (r.code == PK_ERROR_ENUM_MEDIA_CHANGE_REQUIRED)
    r.text = "Insert the installation medium and try again.\n\n" + r.text;
  return r;
}

void progress_cb(PkProgress *p, PkProgressType type, gpointer data) {
  Call *c = (Call *)data;
  if (!c->progress) return;
  Progress pr;
  gint pct = -1;
  PkStatusEnum st = PK_STATUS_ENUM_UNKNOWN;
  gboolean allow = TRUE;
  PkPackage *pkg = nullptr;
  g_object_get(p, "percentage", &pct, "status", &st, "allow-cancel", &allow, "package", &pkg, nullptr);
  pr.percent = pct >= 0 && pct <= 100 ? pct : -1;
  pr.status = status_text(st);
  pr.allow_cancel = allow;
  if (pkg) {
    pr.package = pk_package_get_name(pkg);
    g_object_unref(pkg);
  }
  auto fn = c->progress;
  fld::ui_call([fn, pr] { fn(pr); });
}

void finished_cb(GObject *src, GAsyncResult *res, gpointer data) {
  Call *c = (Call *)data;
  GError *e = nullptr;
  PkResults *r = pk_client_generic_finish(PK_CLIENT(src), res, &e);
  Error err = error_from(e);
  if (!e && r) {
    if (PkError *pe = pk_results_get_error_code(r)) {
      err.code = pk_error_get_code(pe);
      err.text = pk_error_get_details(pe);
      g_object_unref(pe);
    }
  }
  g_clear_error(&e);
  c->done(r, err);
  if (r) g_object_unref(r);
  delete c;
}

// starts a transaction on the GLib thread; `start` calls the pk_client_*_async
template <typename StartFn>
Job run(ProgressFn progress, std::function<void(PkResults *, const Error &)> done, StartFn start) {
  if (!fld::glib_running()) fld::glib_start();
  Job job;
  job.impl = std::make_shared<Job::Impl>();
  auto impl = job.impl;
  fld::glib_call([impl, progress, done, start] {
    Call *c = new Call;
    c->client = pk_client_new();
    pk_client_set_interactive(c->client, TRUE);
    c->job = impl;
    c->progress = progress;
    c->done = done;
    start(c);
  });
  return job;
}

std::vector<Package> packages_of(PkResults *r) {
  std::vector<Package> out;
  if (!r) return out;
  GPtrArray *arr = pk_results_get_package_array(r);
  for (guint i = 0; i < arr->len; i++) {
    PkPackage *p = PK_PACKAGE(g_ptr_array_index(arr, i));
    Package x = parse_id(pk_package_get_id(p));
    x.summary = pk_package_get_summary(p) ? pk_package_get_summary(p) : "";
    x.info = pk_package_get_info(p);
    out.push_back(x);
  }
  g_ptr_array_unref(arr);
  return out;
}

gchar **strv_of(const std::vector<std::string> &v) {
  gchar **s = g_new0(gchar *, v.size() + 1);
  for (size_t i = 0; i < v.size(); i++) s[i] = g_strdup(v[i].c_str());
  return s;
}

std::function<void(PkResults *, const Error &)> packages_done(
    std::function<void(const std::vector<Package> &, const Error &)> done) {
  return [done](PkResults *r, const Error &e) {
    auto list = packages_of(r);
    fld::ui_call([done, list, e] { done(list, e); });
  };
}

PkBitfield filter_for(int installed) {
  PkBitfield f = pk_bitfield_value(PK_FILTER_ENUM_NEWEST) | pk_bitfield_value(PK_FILTER_ENUM_ARCH);
  if (installed == 1) f |= pk_bitfield_value(PK_FILTER_ENUM_INSTALLED);
  else if (installed == 0) f |= pk_bitfield_value(PK_FILTER_ENUM_NOT_INSTALLED);
  return f;
}

PkBitfield tflags(bool simulate, bool only_trusted) {
  PkBitfield f = 0;
  if (simulate) f |= pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_SIMULATE);
  if (only_trusted) f |= pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED);
  return f;
}

} // namespace

// a property of the PackageKit daemon (D-Bus activates it), "" on failure
static std::string daemon_property(const char *name, bool *ok) {
  GError *e = nullptr;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &e);
  std::string out;
  *ok = false;
  if (bus) {
    GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.PackageKit", "/org/freedesktop/PackageKit",
                                              "org.freedesktop.DBus.Properties", "Get",
                                              g_variant_new("(ss)", "org.freedesktop.PackageKit", name),
                                              G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 25000, nullptr, &e);
    if (r) {
      GVariant *v;
      g_variant_get(r, "(v)", &v);
      if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) out = g_variant_get_string(v, nullptr);
      g_variant_unref(v);
      g_variant_unref(r);
      *ok = true;
    }
    g_object_unref(bus);
  }
  g_clear_error(&e);
  return out;
}

bool available() {
  bool ok;
  daemon_property("BackendName", &ok);
  return ok;
}

std::string backend_name() {
  bool ok;
  return daemon_property("BackendDescription", &ok);
}

Job get_updates(std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p) {
  return run(p, packages_done(done), [](Call *c) {
    pk_client_get_updates_async(c->client, pk_bitfield_value(PK_FILTER_ENUM_NONE), c->job->cancel, progress_cb, c,
                                finished_cb, c);
  });
}

Job refresh_cache(bool force, std::function<void(const Error &)> done, ProgressFn p) {
  return run(p, [done](PkResults *, const Error &e) { fld::ui_call([done, e] { done(e); }); },
             [force](Call *c) {
               pk_client_refresh_cache_async(c->client, force, c->job->cancel, progress_cb, c, finished_cb, c);
             });
}

Job search(const std::string &terms, bool details, int installed,
           std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p) {
  std::vector<std::string> words;
  for (auto &w : fld::split(terms, ' '))
    if (!fld::trim(w).empty()) words.push_back(fld::trim(w));
  return run(p, packages_done(done), [words, details, installed](Call *c) {
    gchar **v = strv_of(words);
    if (details)
      pk_client_search_details_async(c->client, filter_for(installed), v, c->job->cancel, progress_cb, c,
                                     finished_cb, c);
    else
      pk_client_search_names_async(c->client, filter_for(installed), v, c->job->cancel, progress_cb, c, finished_cb,
                                   c);
    g_strfreev(v);
  });
}

Job search_group(const std::string &group, int installed,
                 std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p) {
  return run(p, packages_done(done), [group, installed](Call *c) {
    gchar **v = strv_of({group});
    pk_client_search_groups_async(c->client, filter_for(installed), v, c->job->cancel, progress_cb, c, finished_cb,
                                  c);
    g_strfreev(v);
  });
}

Job installed_packages(std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p) {
  return run(p, packages_done(done), [](Call *c) {
    pk_client_get_packages_async(c->client, pk_bitfield_value(PK_FILTER_ENUM_INSTALLED), c->job->cancel, progress_cb,
                                 c, finished_cb, c);
  });
}

Job get_details(const std::vector<std::string> &ids,
                std::function<void(const std::vector<Details> &, const Error &)> done) {
  return run(nullptr,
             [done](PkResults *r, const Error &e) {
               std::vector<Details> out;
               if (r) {
                 GPtrArray *arr = pk_results_get_details_array(r);
                 for (guint i = 0; i < arr->len; i++) {
                   PkDetails *d = PK_DETAILS(g_ptr_array_index(arr, i));
                   Details x;
                   auto s = [](const gchar *p) { return std::string(p ? p : ""); };
                   x.id = s(pk_details_get_package_id(d));
                   x.summary = s(pk_details_get_summary(d));
                   x.description = s(pk_details_get_description(d));
                   x.license = s(pk_details_get_license(d));
                   x.url = s(pk_details_get_url(d));
                   x.group = pk_group_enum_to_string(pk_details_get_group(d));
                   x.size = pk_details_get_size(d);
                   x.download_size = pk_details_get_download_size(d);
                   out.push_back(x);
                 }
                 g_ptr_array_unref(arr);
               }
               fld::ui_call([done, out, e] { done(out, e); });
             },
             [ids](Call *c) {
               gchar **v = strv_of(ids);
               pk_client_get_details_async(c->client, v, c->job->cancel, progress_cb, c, finished_cb, c);
               g_strfreev(v);
             });
}

Job get_files(const std::vector<std::string> &ids,
              std::function<void(const std::vector<std::string> &, const Error &)> done) {
  return run(nullptr,
             [done](PkResults *r, const Error &e) {
               std::vector<std::string> out;
               if (r) {
                 GPtrArray *arr = pk_results_get_files_array(r);
                 for (guint i = 0; i < arr->len; i++) {
                   gchar **files = pk_files_get_files(PK_FILES(g_ptr_array_index(arr, i)));
                   for (gchar **f = files; f && *f; f++) out.push_back(*f);
                 }
                 g_ptr_array_unref(arr);
               }
               std::sort(out.begin(), out.end());
               out.erase(std::unique(out.begin(), out.end()), out.end());
               fld::ui_call([done, out, e] { done(out, e); });
             },
             [ids](Call *c) {
               gchar **v = strv_of(ids);
               pk_client_get_files_async(c->client, v, c->job->cancel, progress_cb, c, finished_cb, c);
               g_strfreev(v);
             });
}

Job depends_on(const std::vector<std::string> &ids, bool recursive,
               std::function<void(const std::vector<Package> &, const Error &)> done) {
  return run(nullptr, packages_done(done), [ids, recursive](Call *c) {
    gchar **v = strv_of(ids);
    pk_client_depends_on_async(c->client, filter_for(-1), v, recursive, c->job->cancel, progress_cb, c, finished_cb, c);
    g_strfreev(v);
  });
}

Job required_by(const std::vector<std::string> &ids, bool recursive,
                std::function<void(const std::vector<Package> &, const Error &)> done) {
  return run(nullptr, packages_done(done), [ids, recursive](Call *c) {
    gchar **v = strv_of(ids);
    // what needs an installed package: only installed packages matter
    PkBitfield f = pk_bitfield_value(PK_FILTER_ENUM_INSTALLED);
    pk_client_required_by_async(c->client, f, v, recursive, c->job->cancel, progress_cb, c, finished_cb, c);
    g_strfreev(v);
  });
}

Job get_update_detail(const std::vector<std::string> &ids,
                      std::function<void(const std::vector<UpdateDetail> &, const Error &)> done) {
  return run(nullptr,
             [done](PkResults *r, const Error &e) {
               std::vector<UpdateDetail> out;
               if (r) {
                 GPtrArray *arr = pk_results_get_update_detail_array(r);
                 for (guint i = 0; i < arr->len; i++) {
                   PkUpdateDetail *d = PK_UPDATE_DETAIL(g_ptr_array_index(arr, i));
                   UpdateDetail x;
                   auto s = [](const gchar *p) { return std::string(p ? p : ""); };
                   auto sv = [](gchar **v) {
                     std::vector<std::string> o;
                     for (int k = 0; v && v[k]; k++) o.push_back(v[k]);
                     return o;
                   };
                   x.id = s(pk_update_detail_get_package_id(d));
                   x.update_text = s(pk_update_detail_get_update_text(d));
                   x.changelog = s(pk_update_detail_get_changelog(d));
                   x.issued = s(pk_update_detail_get_issued(d));
                   x.cves = sv(pk_update_detail_get_cve_urls(d));
                   x.bugzilla = sv(pk_update_detail_get_bugzilla_urls(d));
                   x.vendor_urls = sv(pk_update_detail_get_vendor_urls(d));
                   x.restart = pk_update_detail_get_restart(d);
                   out.push_back(x);
                 }
                 g_ptr_array_unref(arr);
               }
               fld::ui_call([done, out, e] { done(out, e); });
             },
             [ids](Call *c) {
               gchar **v = strv_of(ids);
               pk_client_get_update_detail_async(c->client, v, c->job->cancel, progress_cb, c, finished_cb, c);
               g_strfreev(v);
             });
}

Job install(const std::vector<std::string> &ids, bool simulate, bool only_trusted,
            std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p) {
  return run(p, packages_done(done), [ids, simulate, only_trusted](Call *c) {
    gchar **v = strv_of(ids);
    pk_client_install_packages_async(c->client, tflags(simulate, only_trusted), v, c->job->cancel, progress_cb, c,
                                     finished_cb, c);
    g_strfreev(v);
  });
}

Job remove(const std::vector<std::string> &ids, bool simulate, bool autoremove,
           std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p) {
  return run(p, packages_done(done), [ids, simulate, autoremove](Call *c) {
    gchar **v = strv_of(ids);
    pk_client_remove_packages_async(c->client, tflags(simulate, false), v, TRUE, autoremove, c->job->cancel,
                                    progress_cb, c, finished_cb, c);
    g_strfreev(v);
  });
}

Job update(const std::vector<std::string> &ids, bool simulate, bool only_trusted,
           std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p) {
  return run(p, packages_done(done), [ids, simulate, only_trusted](Call *c) {
    gchar **v = strv_of(ids);
    pk_client_update_packages_async(c->client, tflags(simulate, only_trusted), v, c->job->cancel, progress_cb, c,
                                    finished_cb, c);
    g_strfreev(v);
  });
}

Job install_files(const std::vector<std::string> &paths, bool simulate, bool only_trusted,
                  std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p) {
  return run(p, packages_done(done), [paths, simulate, only_trusted](Call *c) {
    gchar **v = strv_of(paths);
    pk_client_install_files_async(c->client, tflags(simulate, only_trusted), v, c->job->cancel, progress_cb, c,
                                  finished_cb, c);
    g_strfreev(v);
  });
}

Job get_repos(std::function<void(const std::vector<Repo> &, const Error &)> done) {
  return run(nullptr,
             [done](PkResults *r, const Error &e) {
               std::vector<Repo> out;
               if (r) {
                 GPtrArray *arr = pk_results_get_repo_detail_array(r);
                 for (guint i = 0; i < arr->len; i++) {
                   PkRepoDetail *d = PK_REPO_DETAIL(g_ptr_array_index(arr, i));
                   Repo x;
                   x.id = pk_repo_detail_get_id(d) ? pk_repo_detail_get_id(d) : "";
                   x.description = pk_repo_detail_get_description(d) ? pk_repo_detail_get_description(d) : "";
                   x.enabled = pk_repo_detail_get_enabled(d);
                   out.push_back(x);
                 }
                 g_ptr_array_unref(arr);
               }
               std::sort(out.begin(), out.end(), [](const Repo &a, const Repo &b) { return a.id < b.id; });
               fld::ui_call([done, out, e] { done(out, e); });
             },
             [](Call *c) {
               pk_client_get_repo_list_async(c->client, pk_bitfield_value(PK_FILTER_ENUM_NONE), c->job->cancel,
                                             progress_cb, c, finished_cb, c);
             });
}

Job set_repo_enabled(const std::string &id, bool on, std::function<void(const Error &)> done) {
  return run(nullptr, [done](PkResults *, const Error &e) { fld::ui_call([done, e] { done(e); }); },
             [id, on](Call *c) {
               pk_client_repo_enable_async(c->client, id.c_str(), on, c->job->cancel, progress_cb, c, finished_cb,
                                           c);
             });
}

Job get_history(unsigned max, std::function<void(const std::vector<PastTransaction> &, const Error &)> done) {
  return run(nullptr,
             [done](PkResults *r, const Error &e) {
               std::vector<PastTransaction> out;
               if (r) {
                 GPtrArray *arr = pk_results_get_transaction_array(r);
                 for (guint i = 0; i < arr->len; i++) {
                   PkTransactionPast *t = PK_TRANSACTION_PAST(g_ptr_array_index(arr, i));
                   PastTransaction x;
                   auto s = [](const gchar *p) { return std::string(p ? p : ""); };
                   x.tid = s(pk_transaction_past_get_id(t));
                   x.when = s(pk_transaction_past_get_timespec(t));
                   x.role = pk_role_enum_to_string(pk_transaction_past_get_role(t));
                   x.data = s(pk_transaction_past_get_data(t));
                   x.cmdline = s(pk_transaction_past_get_cmdline(t));
                   x.succeeded = pk_transaction_past_get_succeeded(t);
                   x.duration_ms = pk_transaction_past_get_duration(t);
                   out.push_back(x);
                 }
                 g_ptr_array_unref(arr);
               }
               std::reverse(out.begin(), out.end()); // newest first
               fld::ui_call([done, out, e] { done(out, e); });
             },
             [max](Call *c) {
               G_GNUC_BEGIN_IGNORE_DEPRECATIONS
               pk_client_get_old_transactions_async(c->client, max, c->job->cancel, progress_cb, c, finished_cb, c);
               G_GNUC_END_IGNORE_DEPRECATIONS
             });
}

void time_since_refresh(std::function<void(unsigned)> done) {
  if (!fld::glib_running()) fld::glib_start();
  fld::glib_call([done] {
    PkControl *c = pk_control_new();
    pk_control_get_time_since_action_async(
        c, PK_ROLE_ENUM_REFRESH_CACHE, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
          auto *fn = (std::function<void(unsigned)> *)data;
          GError *e = nullptr;
          guint secs = pk_control_get_time_since_action_finish(PK_CONTROL(src), res, &e);
          if (e) secs = G_MAXUINT;
          g_clear_error(&e);
          g_object_unref(src);
          auto f = *fn;
          delete fn;
          fld::ui_call([f, secs] { f(secs); });
        },
        new std::function<void(unsigned)>(done));
  });
}

// ---- helpers ------------------------------------------------------------------

std::string status_text(int s) {
  switch (s) {
  case PK_STATUS_ENUM_WAIT:
  case PK_STATUS_ENUM_WAITING_FOR_LOCK: return "Waiting for other software changes to finish";
  case PK_STATUS_ENUM_WAITING_FOR_AUTH: return "Waiting for authentication";
  case PK_STATUS_ENUM_SETUP: return "Starting";
  case PK_STATUS_ENUM_RUNNING: return "Running";
  case PK_STATUS_ENUM_QUERY: return "Querying";
  case PK_STATUS_ENUM_INFO: return "Getting information";
  case PK_STATUS_ENUM_REMOVE: return "Removing packages";
  case PK_STATUS_ENUM_REFRESH_CACHE: return "Refreshing the software list";
  case PK_STATUS_ENUM_DOWNLOAD: return "Downloading packages";
  case PK_STATUS_ENUM_DOWNLOAD_REPOSITORY:
  case PK_STATUS_ENUM_DOWNLOAD_PACKAGELIST:
  case PK_STATUS_ENUM_DOWNLOAD_FILELIST:
  case PK_STATUS_ENUM_DOWNLOAD_CHANGELOG:
  case PK_STATUS_ENUM_DOWNLOAD_GROUP:
  case PK_STATUS_ENUM_DOWNLOAD_UPDATEINFO: return "Downloading the software list";
  case PK_STATUS_ENUM_INSTALL: return "Installing packages";
  case PK_STATUS_ENUM_UPDATE: return "Updating packages";
  case PK_STATUS_ENUM_CLEANUP: return "Cleaning up";
  case PK_STATUS_ENUM_OBSOLETE: return "Obsoleting packages";
  case PK_STATUS_ENUM_DEP_RESOLVE: return "Resolving dependencies";
  case PK_STATUS_ENUM_SIG_CHECK: return "Checking signatures";
  case PK_STATUS_ENUM_TEST_COMMIT: return "Testing changes";
  case PK_STATUS_ENUM_COMMIT: return "Committing changes";
  case PK_STATUS_ENUM_REQUEST: return "Requesting data";
  case PK_STATUS_ENUM_FINISHED: return "Finished";
  case PK_STATUS_ENUM_CANCEL: return "Canceling";
  case PK_STATUS_ENUM_LOADING_CACHE: return "Loading cache";
  case PK_STATUS_ENUM_SCAN_APPLICATIONS: return "Scanning applications";
  case PK_STATUS_ENUM_GENERATE_PACKAGE_LIST: return "Generating package lists";
  case PK_STATUS_ENUM_COPY_FILES: return "Copying files";
  case PK_STATUS_ENUM_RUN_HOOK: return "Running hooks";
  case PK_STATUS_ENUM_CHECK_LIBRARIES: return "Checking libraries in use";
  case PK_STATUS_ENUM_CHECK_EXECUTABLE_FILES: return "Checking applications in use";
  }
  return "Working";
}

std::string info_text(int i) {
  switch (i) {
  case PK_INFO_ENUM_INSTALLED: return "Installed";
  case PK_INFO_ENUM_AVAILABLE: return "Available";
  case PK_INFO_ENUM_LOW: return "Minor update";
  case PK_INFO_ENUM_ENHANCEMENT: return "Enhancement";
  case PK_INFO_ENUM_NORMAL: return "Update";
  case PK_INFO_ENUM_BUGFIX: return "Bug fix";
  case PK_INFO_ENUM_IMPORTANT: return "Important update";
  case PK_INFO_ENUM_SECURITY: return "Security update";
  case PK_INFO_ENUM_BLOCKED: return "Held back";
  case PK_INFO_ENUM_DOWNLOADING: return "Downloading";
  case PK_INFO_ENUM_UPDATING: return "Updating";
  case PK_INFO_ENUM_INSTALLING: return "Installing";
  case PK_INFO_ENUM_REMOVING: return "Removing";
  case PK_INFO_ENUM_REINSTALLING: return "Reinstalling";
  case PK_INFO_ENUM_DOWNGRADING: return "Downgrading";
  case PK_INFO_ENUM_OBSOLETING: return "Obsoleting";
  case PK_INFO_ENUM_UNTRUSTED: return "Untrusted";
  }
  return "";
}

std::string role_text(const std::string &role) {
  if (role == "install-packages" || role == "install-files") return "Installed";
  if (role == "remove-packages") return "Removed";
  if (role == "update-packages") return "Updated";
  if (role == "refresh-cache") return "Refreshed the software list";
  if (role == "repo-enable") return "Changed a repository";
  if (role == "upgrade-system") return "Upgraded the system";
  return role;
}

std::vector<std::string> groups() {
  return {"accessories", "admin-tools", "communication", "desktop-other", "education", "fonts", "games",
          "graphics", "internet", "multimedia", "office", "other", "programming", "publishing", "science",
          "security", "system"};
}

std::string group_text(const std::string &g) {
  static const std::pair<const char *, const char *> names[] = {
      {"accessories", "Accessories"}, {"admin-tools", "Administration"}, {"communication", "Communication"},
      {"desktop-other", "Desktop"},   {"education", "Education"},         {"fonts", "Fonts"},
      {"games", "Games"},             {"graphics", "Graphics"},           {"internet", "Internet"},
      {"multimedia", "Multimedia"},   {"office", "Office"},               {"other", "Other"},
      {"programming", "Development"}, {"publishing", "Publishing"},       {"science", "Science"},
      {"security", "Security"},       {"system", "System"},
  };
  for (auto &n : names)
    if (g == n.first) return n.second;
  return g;
}

Package parse_id(const std::string &id) {
  Package p;
  p.id = id;
  auto parts = fld::split(id, ';', true);
  if (parts.size() > 0) p.name = parts[0];
  if (parts.size() > 1) p.version = parts[1];
  if (parts.size() > 2) p.arch = parts[2];
  if (parts.size() > 3) p.repo = parts[3];
  // "installed:extra" (alpm) / "manual:..." / "auto:main" (aptcc): the repository part
  size_t c = p.repo.find(':');
  if (c != std::string::npos) p.repo = p.repo.substr(c + 1);
  return p;
}

std::vector<std::string> filter_files(const std::vector<std::string> &files, const std::string &text) {
  if (text.empty()) return files;
  std::string t = fld::lower(text);
  std::vector<std::string> out;
  for (auto &f : files)
    if (fld::lower(f).find(t) != std::string::npos) out.push_back(f);
  return out;
}

std::vector<Package> unique_by_name(const std::vector<Package> &list) {
  std::vector<Package> out;
  for (auto &p : list) {
    auto it = std::find_if(out.begin(), out.end(), [&](const Package &q) { return q.name == p.name; });
    if (it == out.end()) out.push_back(p);
    else if (p.installed() && !it->installed()) *it = p;
  }
  std::sort(out.begin(), out.end(), [](const Package &a, const Package &b) { return a.name < b.name; });
  return out;
}

std::vector<std::string> history_lines(const std::string &data) {
  std::vector<std::string> out;
  for (auto &line : fld::split(data, '\n')) {
    auto f = fld::split(line, '\t', true);
    if (f.size() < 2) continue;
    Package p = parse_id(f[1]);
    std::string what = f[0];
    if (!what.empty()) what[0] = (char)toupper((unsigned char)what[0]);
    out.push_back(what + " " + p.name + " " + p.version);
  }
  return out;
}

} // namespace pk
