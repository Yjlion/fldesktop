#include "fprint.h"

#include <FL/Fl.H>
#include <gio/gio.h>

#include <atomic>
#include <thread>

namespace fprint {

namespace {

const char *kBus = "net.reactivated.Fprint";
const char *kManager = "net.reactivated.Fprint.Manager";
const char *kDevice = "net.reactivated.Fprint.Device";

GDBusConnection *system_bus() {
  GError *e = nullptr;
  GDBusConnection *c = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &e);
  g_clear_error(&e);
  return c;
}

GVariant *call(GDBusConnection *c, const std::string &path, const char *iface, const char *method, GVariant *args,
               const char *ret, std::string *err = nullptr, int timeout = 5000) {
  GError *e = nullptr;
  GVariant *r = g_dbus_connection_call_sync(c, kBus, path.c_str(), iface, method, args, ret ? G_VARIANT_TYPE(ret) : nullptr,
                                            G_DBUS_CALL_FLAGS_NONE, timeout, nullptr, &e);
  if (!r && e && err) {
    g_dbus_error_strip_remote_error(e);
    *err = e->message;
  }
  g_clear_error(&e);
  return r;
}

std::string default_device(GDBusConnection *c) {
  GVariant *r = call(c, "/net/reactivated/Fprint/Manager", kManager, "GetDefaultDevice", nullptr, "(o)");
  if (!r) return "";
  const char *p;
  g_variant_get(r, "(&o)", &p);
  std::string path = p;
  g_variant_unref(r);
  return path;
}

// runs fn in the FLTK thread
void ui(std::function<void()> fn) {
  Fl::awake([](void *d) {
    std::unique_ptr<std::function<void()>> f((std::function<void()> *)d);
    (*f)();
  }, new std::function<void()>(std::move(fn)));
}

int num_stages(GDBusConnection *c, const std::string &dev) {
  GVariant *r = call(c, dev, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", kDevice, "num-enroll-stages"),
                     "(v)");
  int n = 5;
  if (r) {
    GVariant *v;
    g_variant_get(r, "(v)", &v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32)) n = g_variant_get_int32(v);
    g_variant_unref(v);
    g_variant_unref(r);
  }
  return n > 0 ? n : 5;
}

} // namespace

bool available() {
  GDBusConnection *c = system_bus();
  if (!c) return false;
  bool ok = !default_device(c).empty();
  g_object_unref(c);
  return ok;
}

std::vector<std::string> enrolled(const std::string &user) {
  std::vector<std::string> out;
  GDBusConnection *c = system_bus();
  if (!c) return out;
  std::string dev = default_device(c);
  if (!dev.empty()) {
    if (GVariant *r = call(c, dev, kDevice, "ListEnrolledFingers", g_variant_new("(s)", user.c_str()), "(as)")) {
      GVariantIter *it;
      const char *f;
      g_variant_get(r, "(as)", &it);
      while (g_variant_iter_next(it, "&s", &f)) out.push_back(f);
      g_variant_iter_free(it);
      g_variant_unref(r);
    }
  }
  g_object_unref(c);
  return out;
}

const std::vector<std::string> &all_fingers() {
  static const std::vector<std::string> f = {
      "right-index-finger", "left-index-finger", "right-thumb",       "left-thumb",       "right-middle-finger",
      "left-middle-finger", "right-ring-finger", "left-ring-finger",  "right-little-finger", "left-little-finger"};
  return f;
}

std::string finger_label(const std::string &f) {
  std::string s = f;
  for (char &c : s)
    if (c == '-') c = ' ';
  if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
  return s;
}

std::string status_text(const std::string &r) {
  if (r == "verify-match") return "Recognised";
  if (r == "verify-no-match") return "Not recognised: try again or type the password";
  if (r == "verify-retry-scan" || r == "enroll-retry-scan") return "Try again";
  if (r == "verify-swipe-too-short" || r == "enroll-swipe-too-short") return "The swipe was too short: try again";
  if (r == "verify-finger-not-centered" || r == "enroll-finger-not-centered") return "Put the finger in the middle of the reader";
  if (r == "verify-remove-and-retry" || r == "enroll-remove-and-retry") return "Lift the finger and try again";
  if (r == "verify-disconnected" || r == "enroll-disconnected") return "The fingerprint reader went away";
  if (r == "enroll-stage-passed") return "Good. Again, a little differently";
  if (r == "enroll-completed") return "Done";
  if (r == "enroll-data-full") return "The reader has no room for more fingerprints";
  if (r == "enroll-duplicate") return "This finger is enrolled already";
  if (r == "enroll-failed") return "Enrolling failed";
  return "The fingerprint reader had a problem";
}

// ---- verify -------------------------------------------------------------------------

struct Verify::Impl {
  std::string user;
  std::function<void(Event, const std::string &)> cb;
  std::atomic<bool> stopping{false};
  GMainContext *ctx = nullptr;
  GMainLoop *loop = nullptr;
  std::thread th;
};

Verify::Verify(const std::string &user, std::function<void(Event, const std::string &)> on_event)
    : d(std::make_shared<Impl>()) {
  d->user = user;
  d->cb = std::move(on_event);
  std::shared_ptr<Impl> s = d;
  d->ctx = g_main_context_new();
  d->loop = g_main_loop_new(d->ctx, FALSE);
  d->th = std::thread([s] {
    g_main_context_push_thread_default(s->ctx);
    auto event = [s](Event e, const std::string &t) {
      if (s->stopping) return;
      ui([s, e, t] {
        if (!s->stopping) s->cb(e, t);
      });
    };
    GDBusConnection *c = system_bus();
    std::string dev = c ? default_device(c) : "";
    std::string err;
    GVariant *r = dev.empty() ? nullptr : call(c, dev, kDevice, "Claim", g_variant_new("(s)", s->user.c_str()), nullptr, &err);
    if (!r) {
      event(UNAVAILABLE, err);
      if (c) g_object_unref(c);
      g_main_context_pop_thread_default(s->ctx);
      return;
    }
    g_variant_unref(r);
    struct Ctx {
      std::shared_ptr<Impl> s;
      GDBusConnection *c;
      std::string dev;
      int tries = 0;
    } ctx{s, c, dev};
    guint sub = g_dbus_connection_signal_subscribe(
        c, kBus, kDevice, "VerifyStatus", dev.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *p, gpointer data) {
          Ctx *x = (Ctx *)data;
          const char *result;
          gboolean done;
          g_variant_get(p, "(&sb)", &result, &done);
          std::string res = result;
          std::shared_ptr<Impl> s = x->s;
          auto event = [s](Event e, const std::string &t) {
            if (s->stopping) return;
            ui([s, e, t] {
              if (!s->stopping) s->cb(e, t);
            });
          };
          if (res == "verify-match") {
            event(MATCH, status_text(res));
            g_main_loop_quit(s->loop);
            return;
          }
          if (!done) {
            event(RETRY, status_text(res));
            return;
          }
          // a mismatch ends this scan: start another (a few times)
          event(res == "verify-no-match" ? NO_MATCH : RETRY, status_text(res));
          GVariant *r = call(x->c, x->dev, kDevice, "VerifyStop", nullptr, nullptr);
          if (r) g_variant_unref(r);
          if (++x->tries >= 5 || s->stopping) {
            g_main_loop_quit(s->loop);
            return;
          }
          r = call(x->c, x->dev, kDevice, "VerifyStart", g_variant_new("(s)", "any"), nullptr);
          if (r) g_variant_unref(r);
        },
        &ctx, nullptr);
    r = call(c, dev, kDevice, "VerifyStart", g_variant_new("(s)", "any"), nullptr, &err);
    if (r) {
      g_variant_unref(r);
      event(READY, "Or touch the fingerprint reader");
      if (!s->stopping) g_main_loop_run(s->loop);
      GVariant *st = call(c, dev, kDevice, "VerifyStop", nullptr, nullptr);
      if (st) g_variant_unref(st);
    } else {
      // no enrolled fingers, or the reader is busy
      event(UNAVAILABLE, err);
    }
    g_dbus_connection_signal_unsubscribe(c, sub);
    GVariant *rel = call(c, dev, kDevice, "Release", nullptr, nullptr);
    if (rel) g_variant_unref(rel);
    g_object_unref(c);
    g_main_context_pop_thread_default(s->ctx);
  });
}

void Verify::stop() {
  if (!d || d->stopping.exchange(true)) return;
  g_main_loop_quit(d->loop);
  g_main_context_wakeup(d->ctx);
}

Verify::~Verify() {
  stop();
  if (d->th.joinable()) d->th.detach(); // it ends by itself (and keeps d alive)
}

// ---- enroll -------------------------------------------------------------------------

struct Enroll::Impl {
  std::string user, finger;
  std::function<void(int, int, const std::string &, bool, bool)> cb;
  std::atomic<bool> stopping{false};
  GMainContext *ctx = nullptr;
  GMainLoop *loop = nullptr;
  std::thread th;
  int stage = 0, stages = 5;
};

Enroll::Enroll(const std::string &user, const std::string &finger,
               std::function<void(int, int, const std::string &, bool, bool)> on_progress)
    : d(std::make_shared<Impl>()) {
  d->user = user;
  d->finger = finger;
  d->cb = std::move(on_progress);
  std::shared_ptr<Impl> s = d;
  d->ctx = g_main_context_new();
  d->loop = g_main_loop_new(d->ctx, FALSE);
  d->th = std::thread([s] {
    g_main_context_push_thread_default(s->ctx);
    auto progress = [s](const std::string &t, bool done, bool ok) {
      int stage = s->stage, stages = s->stages;
      ui([s, stage, stages, t, done, ok] {
        if (!s->stopping) s->cb(stage, stages, t, done, ok);
      });
    };
    GDBusConnection *c = system_bus();
    std::string dev = c ? default_device(c) : "";
    std::string err;
    GVariant *r = dev.empty() ? nullptr : call(c, dev, kDevice, "Claim", g_variant_new("(s)", s->user.c_str()), nullptr, &err);
    if (!r) {
      progress(err.empty() ? "No fingerprint reader" : err, true, false);
      if (c) g_object_unref(c);
      g_main_context_pop_thread_default(s->ctx);
      return;
    }
    g_variant_unref(r);
    s->stages = num_stages(c, dev);
    guint sub = g_dbus_connection_signal_subscribe(
        c, kBus, kDevice, "EnrollStatus", dev.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *p, gpointer data) {
          std::shared_ptr<Impl> s = *(std::shared_ptr<Impl> *)data;
          const char *result;
          gboolean done;
          g_variant_get(p, "(&sb)", &result, &done);
          std::string res = result;
          if (res == "enroll-stage-passed") s->stage++;
          if (res == "enroll-completed") s->stage = s->stages;
          bool ok = res == "enroll-completed";
          int stage = s->stage, stages = s->stages;
          std::string t = status_text(res);
          ui([s, stage, stages, t, done, ok] {
            if (!s->stopping) s->cb(stage, stages, t, done, ok);
          });
          if (done) g_main_loop_quit(s->loop);
        },
        new std::shared_ptr<Impl>(s), [](gpointer p) { delete (std::shared_ptr<Impl> *)p; });
    r = call(c, dev, kDevice, "EnrollStart", g_variant_new("(s)", s->finger.c_str()), nullptr, &err);
    if (r) {
      g_variant_unref(r);
      progress("Put the finger on the reader", false, false);
      if (!s->stopping) g_main_loop_run(s->loop);
      GVariant *st = call(c, dev, kDevice, "EnrollStop", nullptr, nullptr);
      if (st) g_variant_unref(st);
    } else {
      progress(err, true, false);
    }
    g_dbus_connection_signal_unsubscribe(c, sub);
    GVariant *rel = call(c, dev, kDevice, "Release", nullptr, nullptr);
    if (rel) g_variant_unref(rel);
    g_object_unref(c);
    g_main_context_pop_thread_default(s->ctx);
  });
}

void Enroll::stop() {
  if (!d || d->stopping.exchange(true)) return;
  g_main_loop_quit(d->loop);
  g_main_context_wakeup(d->ctx);
}

Enroll::~Enroll() {
  stop();
  if (d->th.joinable()) d->th.join();
}

bool delete_all(const std::string &user, std::string &err) {
  GDBusConnection *c = system_bus();
  if (!c) {
    err = "No system bus";
    return false;
  }
  std::string dev = default_device(c);
  bool ok = false;
  if (dev.empty()) {
    err = "No fingerprint reader";
  } else if (GVariant *r = call(c, dev, kDevice, "Claim", g_variant_new("(s)", user.c_str()), nullptr, &err)) {
    g_variant_unref(r);
    // fprintd 1.92+: DeleteEnrolledFingers2 on the claimed device
    GVariant *d = call(c, dev, kDevice, "DeleteEnrolledFingers2", nullptr, nullptr, &err, 25000);
    if (!d) d = call(c, dev, kDevice, "DeleteEnrolledFingers", g_variant_new("(s)", user.c_str()), nullptr, &err, 25000);
    if (d) {
      g_variant_unref(d);
      ok = true;
    }
    GVariant *rel = call(c, dev, kDevice, "Release", nullptr, nullptr);
    if (rel) g_variant_unref(rel);
  }
  g_object_unref(c);
  return ok;
}

} // namespace fprint
