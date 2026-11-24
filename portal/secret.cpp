// org.freedesktop.impl.portal.Secret: one secret per sandboxed application
// (Flatpak apps encrypt their own password storage with it). The secret is
// kept in the Secret Service (fl-secretd, or another one) under the app's id
// and made the first time. Runs on the GLib thread; all calls asynchronous
// (unlocking may wait for the person's password).
#include <gio/gunixfdlist.h>
#include <unistd.h>

#include <functional>
#include <memory>
#include <random>
#include <string>

#include "portal.h"

namespace portal {

namespace {

const char *kBus = "org.freedesktop.secrets";
const char *kRoot = "/org/freedesktop/secrets";

struct Job {
  GDBusMethodInvocation *inv = nullptr;
  std::string app_id, session;
  int fd = -1;
  ~Job() {
    if (fd >= 0) close(fd);
  }
};
using JobP = std::shared_ptr<Job>;

void finish(const JobP &j, unsigned response) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  g_dbus_method_invocation_return_value(j->inv, g_variant_new("(ua{sv})", response, &b));
  j->inv = nullptr;
}

// calls the Secret Service; done(reply or null) on the GLib thread
void call(const char *path, const char *iface, const char *method, GVariant *params, const char *type,
          std::function<void(GVariant *)> done) {
  auto *cb = new std::function<void(GVariant *)>(std::move(done));
  g_dbus_connection_call(connection(), kBus, path, iface, method, params, type ? G_VARIANT_TYPE(type) : nullptr,
                         G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                         [](GObject *src, GAsyncResult *res, gpointer d) {
                           auto *cb = (std::function<void(GVariant *)> *)d;
                           GError *e = nullptr;
                           GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
                           if (e) g_printerr("xdg-desktop-portal-fl: secret: %s\n", e->message);
                           g_clear_error(&e);
                           (*cb)(r);
                           if (r) g_variant_unref(r);
                           delete cb;
                         },
                         cb);
}

// runs a Secret Service prompt; done(result or null when dismissed)
void run_prompt(const std::string &prompt, std::function<void(GVariant *)> done) {
  if (prompt == "/") return done(nullptr);
  auto *cb = new std::function<void(GVariant *)>(std::move(done));
  auto *sub = new guint(0);
  struct Ctx {
    std::function<void(GVariant *)> *cb;
    guint *sub;
  };
  *sub = g_dbus_connection_signal_subscribe(
      connection(), kBus, "org.freedesktop.Secret.Prompt", "Completed", prompt.c_str(), nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE,
      [](GDBusConnection *c, const char *, const char *, const char *, const char *, GVariant *params, gpointer d) {
        Ctx *x = (Ctx *)d;
        gboolean dismissed = TRUE;
        GVariant *result = nullptr;
        g_variant_get(params, "(bv)", &dismissed, &result);
        g_dbus_connection_signal_unsubscribe(c, *x->sub);
        (*x->cb)(dismissed ? nullptr : result);
        if (result) g_variant_unref(result);
        delete x->cb;
        delete x->sub;
        delete x;
      },
      new Ctx{cb, sub}, nullptr);
  call(prompt.c_str(), "org.freedesktop.Secret.Prompt", "Prompt", g_variant_new("(s)", ""), nullptr, [](GVariant *) {});
}

GVariant *attributes(const std::string &app_id) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{ss}"));
  g_variant_builder_add(&b, "{ss}", "xdg:schema", "org.freedesktop.impl.portal.Secret");
  g_variant_builder_add(&b, "{ss}", "app_id", app_id.c_str());
  return g_variant_builder_end(&b);
}

void write_secret(const JobP &j, const std::string &value) {
  size_t off = 0;
  while (off < value.size()) {
    ssize_t n = write(j->fd, value.data() + off, value.size() - off);
    if (n <= 0) break;
    off += (size_t)n;
  }
  finish(j, off == value.size() ? 0 : 2);
}

void get_secret(const JobP &j, const std::string &item) {
  call(item.c_str(), "org.freedesktop.Secret.Item", "GetSecret", g_variant_new("(o)", j->session.c_str()),
       "((oayays))", [j](GVariant *r) {
         if (!r) return finish(j, 2);
         GVariant *value = nullptr;
         g_variant_get(r, "((o@ay@ay&s))", nullptr, nullptr, &value, nullptr);
         gsize n = 0;
         const char *d = (const char *)g_variant_get_fixed_array(value, &n, 1);
         write_secret(j, std::string(d ? d : "", n));
         g_variant_unref(value);
       });
}

// no secret yet: 64 random bytes in the default keyring
void create_secret(const JobP &j) {
  call(kRoot, "org.freedesktop.Secret.Service", "ReadAlias", g_variant_new("(s)", "default"), "(o)", [j](GVariant *r) {
    const char *coll = "/";
    if (r) g_variant_get(r, "(&o)", &coll);
    std::string c = coll;
    if (c == "/") return finish(j, 2); // no keyring to keep it in
    std::string secret(64, '\0');
    std::random_device rd;
    for (auto &ch : secret) ch = (char)(rd() & 0xff);
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    std::string label = "Secret of " + app_name(j->app_id);
    g_variant_builder_add(&props, "{sv}", "org.freedesktop.Secret.Item.Label", g_variant_new_string(label.c_str()));
    g_variant_builder_add(&props, "{sv}", "org.freedesktop.Secret.Item.Attributes", attributes(j->app_id));
    GVariant *value = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, secret.data(), secret.size(), 1);
    GVariant *params = g_variant_new("(a{sv}(o@ay@ays)b)", &props, j->session.c_str(),
                                      g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, "", 0, 1), value,
                                      "application/octet-stream", TRUE);
    call(c.c_str(), "org.freedesktop.Secret.Collection", "CreateItem", params, "(oo)", [j, secret](GVariant *r) {
      if (!r) return finish(j, 2);
      const char *item, *prompt;
      g_variant_get(r, "(&o&o)", &item, &prompt);
      if (std::string(prompt) == "/") return write_secret(j, secret);
      run_prompt(prompt, [j, secret](GVariant *res) { res ? write_secret(j, secret) : finish(j, 1); });
    });
  });
}

void lookup(const JobP &j) {
  call(kRoot, "org.freedesktop.Secret.Service", "SearchItems", g_variant_new("(@a{ss})", attributes(j->app_id)),
       "(aoao)", [j](GVariant *r) {
         if (!r) return finish(j, 2);
         GVariant *unlocked = g_variant_get_child_value(r, 0), *locked = g_variant_get_child_value(r, 1);
         std::string u = g_variant_n_children(unlocked) ? g_variant_get_string(g_variant_get_child_value(unlocked, 0), nullptr) : "";
         std::string l = g_variant_n_children(locked) ? g_variant_get_string(g_variant_get_child_value(locked, 0), nullptr) : "";
         g_variant_unref(unlocked);
         g_variant_unref(locked);
         if (!u.empty()) return get_secret(j, u);
         if (l.empty()) return create_secret(j);
         // locked: unlock it (the keyring's password dialog)
         const char *objs[] = {l.c_str(), nullptr};
         call(kRoot, "org.freedesktop.Secret.Service", "Unlock", g_variant_new("(^ao)", objs), "(aoo)",
              [j, l](GVariant *r) {
                if (!r) return finish(j, 2);
                const char *prompt;
                g_variant_get(r, "(@ao&o)", nullptr, &prompt);
                if (std::string(prompt) == "/") return get_secret(j, l);
                run_prompt(prompt, [j, l](GVariant *res) { res ? get_secret(j, l) : finish(j, 1); });
              });
       });
}

} // namespace

void secret_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  if (method != "RetrieveSecret") {
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method.c_str());
    return;
  }
  auto j = std::make_shared<Job>();
  j->inv = inv;
  const char *app = nullptr;
  gint32 fd_index = -1;
  g_variant_get(params, "(&o&sh@a{sv})", nullptr, &app, &fd_index, nullptr);
  j->app_id = app ? app : "";
  GUnixFDList *fds = g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(inv));
  GError *e = nullptr;
  j->fd = fds ? g_unix_fd_list_get(fds, fd_index, &e) : -1;
  g_clear_error(&e);
  if (j->fd < 0 || j->app_id.empty()) return finish(j, 2);
  call(kRoot, "org.freedesktop.Secret.Service", "OpenSession", g_variant_new("(sv)", "plain", g_variant_new_string("")),
       "(vo)", [j](GVariant *r) {
         if (!r) return finish(j, 2);
         const char *session;
         g_variant_get(r, "(v&o)", nullptr, &session);
         j->session = session;
         lookup(j);
       });
}

} // namespace portal
