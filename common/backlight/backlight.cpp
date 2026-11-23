#include "backlight.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef FLD_HAVE_GIO
#include <gio/gio.h>
#endif

#include "fld/util.h"

namespace bl {

namespace {

int read_int(const std::string &path, int def) {
  std::string s;
  if (!fld::read_file(path, s, 64)) return def;
  s = fld::trim(s);
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return def;
  return atoi(s.c_str());
}

std::string dir_of(const Device &d) { return sysfs_root() + "/sys/class/backlight/" + d.name; }

int type_rank(const std::string &t) { return t == "firmware" ? 0 : t == "platform" ? 1 : t == "raw" ? 2 : 3; }

#ifdef FLD_HAVE_GIO
GDBusConnection *bus(const char *env) {
  const char *b = getenv(env);
  GError *e = nullptr;
  GDBusConnection *c = g_bus_get_sync(b && !strcmp(b, "session") ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM, nullptr, &e);
  g_clear_error(&e);
  return c;
}

// logind: Session.SetBrightness("backlight", name, value) on our session
bool logind_set(const Device &d, int raw, std::string &err) {
  GDBusConnection *c = bus("FLD_LOGIND_BUS");
  if (!c) return false;
  GError *e = nullptr;
  GVariant *r = g_dbus_connection_call_sync(c, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                                            "org.freedesktop.login1.Session", "SetBrightness",
                                            g_variant_new("(ssu)", "backlight", d.name.c_str(), (guint32)raw), nullptr,
                                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, nullptr, &e);
  g_object_unref(c);
  if (r) {
    g_variant_unref(r);
    return true;
  }
  if (e) {
    g_dbus_error_strip_remote_error(e);
    err = e->message;
    g_error_free(e);
  }
  return false;
}

GVariant *upower_kbd(const char *method, GVariant *args) {
  GDBusConnection *c = bus("FLD_UPOWER_BUS");
  if (!c) return nullptr;
  GError *e = nullptr;
  GVariant *r = g_dbus_connection_call_sync(c, "org.freedesktop.UPower", "/org/freedesktop/UPower/KbdBacklight",
                                            "org.freedesktop.UPower.KbdBacklight", method, args, nullptr,
                                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, nullptr, &e);
  g_clear_error(&e);
  g_object_unref(c);
  return r;
}
#endif

} // namespace

std::string sysfs_root() {
  const char *r = getenv("FLD_SYSFS_ROOT");
  return r ? r : "";
}

std::vector<Device> list() {
  std::vector<Device> out;
  std::string base = sysfs_root() + "/sys/class/backlight";
  for (const std::string &n : fld::list_dir(base)) {
    Device d;
    d.name = n;
    std::string t;
    fld::read_file(base + "/" + n + "/type", t, 64);
    d.type = fld::trim(t);
    d.max = read_int(base + "/" + n + "/max_brightness", 0);
    if (d.max <= 0) continue;
    refresh(d);
    out.push_back(d);
  }
  std::sort(out.begin(), out.end(), [](const Device &a, const Device &b) { return a.name < b.name; });
  return out;
}

const Device *pick(const std::vector<Device> &devs) {
  const Device *best = nullptr;
  for (const Device &d : devs) {
    if (!best || type_rank(d.type) < type_rank(best->type) ||
        (type_rank(d.type) == type_rank(best->type) && d.max > best->max))
      best = &d;
  }
  return best;
}

bool refresh(Device &d) {
  // actual_brightness is what the hardware does; brightness what was asked
  int v = read_int(dir_of(d) + "/actual_brightness", -1);
  if (v < 0 || v > d.max) v = read_int(dir_of(d) + "/brightness", -1);
  if (v < 0) return false;
  d.brightness = std::min(v, d.max);
  return true;
}

int percent(int raw, int max) {
  if (max <= 0) return 0;
  return (int)std::lround(100.0 * std::max(0, std::min(raw, max)) / max);
}

int raw_for_percent(int pct, int max) {
  if (max <= 0) return 0;
  pct = std::max(1, std::min(100, pct));
  int raw = (int)std::lround(max * pct / 100.0);
  return std::max(1, std::min(max, raw));
}

int step(int raw, int max, int dir, int step_pct) {
  if (max <= 0) return 0;
  int pct = percent(raw, max);
  // snap to the step grid, so 37 % goes to 40 % or 35 %
  int next;
  if (dir > 0) next = (pct / step_pct + 1) * step_pct;
  else next = ((pct + step_pct - 1) / step_pct - 1) * step_pct;
  int r = raw_for_percent(next, max);
  // tiny ranges (max 7): always move at least one raw step
  if (dir > 0 && r <= raw && raw < max) r = raw + 1;
  int floor = raw_for_percent(1, max);
  if (dir < 0 && r >= raw && raw > floor) r = raw - 1;
  return std::max(floor, std::min(max, r));
}

bool set(const Device &d, int raw, std::string &err) {
  raw = std::max(0, std::min(d.max, raw));
  if (d.name.empty() || d.name.find('/') != std::string::npos || d.name[0] == '.') {
    err = "not a backlight device";
    return false;
  }
#ifdef FLD_HAVE_GIO
  std::string lerr;
  if (logind_set(d, raw, lerr)) return true;
#endif
  std::string path = dir_of(d) + "/brightness";
  if (access(path.c_str(), W_OK) == 0) {
    FILE *f = fopen(path.c_str(), "w");
    if (f) {
      bool ok = fprintf(f, "%d\n", raw) > 0;
      ok = fclose(f) == 0 && ok;
      if (ok) return true;
    }
  }
  // the polkit helper (sysfs only: it never writes elsewhere)
  if (sysfs_root().empty()) {
    std::string helper = std::string(FLD_LIBEXECDIR) + "/fl-backlight-helper";
    if (access(helper.c_str(), X_OK) == 0 && !fld::which("pkexec").empty()) {
      std::string out;
      if (fld::run_capture({"pkexec", helper, d.name, std::to_string(raw)}, out) == 0) return true;
      err = fld::trim(out);
      if (err.empty()) err = "fl-backlight-helper failed";
      return false;
    }
  }
  err = "The brightness cannot be changed (no logind session and no permission to write " + path + ").";
  return false;
}

bool screen_get(int &pct) {
  std::vector<Device> devs = list();
  const Device *d = pick(devs);
  if (!d) return false;
  pct = percent(d->brightness, d->max);
  return true;
}

bool screen_set(int pct, std::string &err) {
  std::vector<Device> devs = list();
  const Device *d = pick(devs);
  if (!d) {
    err = "There is no screen backlight.";
    return false;
  }
  return set(*d, raw_for_percent(pct, d->max), err);
}

bool screen_step(int dir, int &pct, std::string &err) {
  std::vector<Device> devs = list();
  const Device *d = pick(devs);
  if (!d) {
    err = "There is no screen backlight.";
    return false;
  }
  int raw = step(d->brightness, d->max, dir);
  if (!set(*d, raw, err)) return false;
  pct = percent(raw, d->max);
  return true;
}

bool kbd_get(int &value, int &max) {
#ifdef FLD_HAVE_GIO
  GVariant *m = upower_kbd("GetMaxBrightness", nullptr);
  if (!m) return false;
  g_variant_get(m, "(i)", &max);
  g_variant_unref(m);
  GVariant *v = upower_kbd("GetBrightness", nullptr);
  if (!v) return false;
  g_variant_get(v, "(i)", &value);
  g_variant_unref(v);
  return max > 0;
#else
  (void)value;
  (void)max;
  return false;
#endif
}

bool kbd_set(int value) {
#ifdef FLD_HAVE_GIO
  GVariant *r = upower_kbd("SetBrightness", g_variant_new("(i)", value));
  if (!r) return false;
  g_variant_unref(r);
  return true;
#else
  (void)value;
  return false;
#endif
}

bool kbd_step(int dir, bool wrap, int &pct) {
  int v = 0, max = 0;
  if (!kbd_get(v, max)) return false;
  // keyboards have few levels (2 or 3): one level per press
  int n = v + (dir > 0 ? 1 : -1);
  if (n > max) n = wrap ? 0 : max;
  if (n < 0) n = 0;
  if (!kbd_set(n)) return false;
  pct = percent(n, max);
  return true;
}

} // namespace bl
