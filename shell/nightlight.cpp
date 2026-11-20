// fl-nightlight: warmer screen colours at night (wlr-gamma-control)
//
// Reads [NightLight] in desktop.ini:
//   Enabled=false            on/off (the panel toggles it)
//   Temperature=4000         night colour temperature in kelvin
//   Schedule=sunset          sunset (sunset to sunrise), custom (From-To), always
//   From=20:00  To=07:00     the custom night
//   Latitude= Longitude=     optional; otherwise from the time zone
// and follows changes to the file. While it is off it holds no gamma
// control, so the screens have their normal colours.
#include <poll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <list>
#include <string>

#include <wayland-client.h>

#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "nightlight_util.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"

namespace {

using namespace nightlight;

struct Config {
  bool enabled = false;
  int temperature = 4000;
  std::string schedule = "sunset";
  int from = 20 * 60, to = 7 * 60;
  bool have_location = false;
  double lat = 0, lon = 0;
};

struct Output {
  wl_output *wl = nullptr;
  uint32_t name = 0;
  zwlr_gamma_control_v1 *control = nullptr;
  uint32_t size = 0;
  bool failed = false;
  int applied = 6500;
};

wl_display *g_display = nullptr;
zwlr_gamma_control_manager_v1 *g_manager = nullptr;
std::list<Output> g_outputs;
volatile sig_atomic_t g_quit = 0;

std::string local_zone() {
  if (const char *tz = getenv("TZ"); tz && *tz) return tz[0] == ':' ? tz + 1 : tz;
  char buf[PATH_MAX];
  ssize_t n = readlink("/etc/localtime", buf, sizeof buf - 1);
  if (n <= 0) return "";
  buf[n] = 0;
  std::string p = buf;
  size_t z = p.find("zoneinfo/");
  return z == std::string::npos ? "" : p.substr(z + 9);
}

Config load_config() {
  fld::Ini ini;
  ini.load(fld::desktop_config_path());
  Config c;
  const std::string S = "NightLight";
  c.enabled = ini.get_bool(S, "Enabled", false);
  c.temperature = std::max(1500, std::min(6500, ini.get_int(S, "Temperature", 4000)));
  c.schedule = ini.get(S, "Schedule", "sunset");
  int f = parse_hhmm(ini.get(S, "From", "20:00")), t = parse_hhmm(ini.get(S, "To", "07:00"));
  if (f >= 0) c.from = f;
  if (t >= 0) c.to = t;
  std::string la = ini.get(S, "Latitude", ""), lo = ini.get(S, "Longitude", "");
  if (!la.empty() && !lo.empty()) {
    c.lat = atof(la.c_str());
    c.lon = atof(lo.c_str());
    c.have_location = true;
  } else {
    std::string zone = local_zone();
    for (const char *tab : {"/usr/share/zoneinfo/zone1970.tab", "/usr/share/zoneinfo/zone.tab"})
      if (!zone.empty() && zone_coordinates(tab, zone, &c.lat, &c.lon)) {
        c.have_location = true;
        break;
      }
  }
  return c;
}

// the temperature the screens should have now
int target_temperature(const Config &c) {
  if (!c.enabled) return 6500;
  if (c.schedule == "always") return c.temperature;
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  int minute = lt.tm_hour * 60 + lt.tm_min;
  int start = c.from, end = c.to, fade = 30;
  if (c.schedule == "sunset" && c.have_location) {
    int rise, set;
    bool polar_day = false;
    if (!sun_times(lt.tm_yday + 1, c.lat, c.lon, (int)(lt.tm_gmtoff / 60), &rise, &set, &polar_day))
      return polar_day ? 6500 : c.temperature;
    start = set;
    end = rise;
    fade = 45;
  }
  return blend_temperature(c.temperature, night_amount(minute, start, end, fade));
}

void release(Output &o) {
  if (o.control) zwlr_gamma_control_v1_destroy(o.control);
  o.control = nullptr;
  o.size = 0;
  o.applied = 6500;
}

void gamma_size(void *data, zwlr_gamma_control_v1 *, uint32_t size) { ((Output *)data)->size = size; }
void gamma_failed(void *data, zwlr_gamma_control_v1 *) {
  Output *o = (Output *)data;
  // no gamma on this output, or another program holds it
  release(*o);
  o->failed = true;
}
const zwlr_gamma_control_v1_listener gamma_listener = {gamma_size, gamma_failed};

bool set_gamma(Output &o, int kelvin) {
  if (!o.control || !o.size) return false;
  std::vector<uint16_t> ramp = gamma_ramp(o.size, temperature_rgb(kelvin));
  size_t bytes = ramp.size() * sizeof(uint16_t);
  int fd = memfd_create("fl-nightlight", MFD_CLOEXEC);
  if (fd < 0) return false;
  bool ok = write(fd, ramp.data(), bytes) == (ssize_t)bytes && lseek(fd, 0, SEEK_SET) == 0;
  if (ok) zwlr_gamma_control_v1_set_gamma(o.control, fd);
  close(fd);
  if (ok) o.applied = kelvin;
  return ok;
}

// Moves every output to `kelvin`. A temperature of 6500 lets go of the gamma
// controls, which gives the screens back their own colours.
void apply(int kelvin) {
  for (Output &o : g_outputs) {
    if (o.failed) continue;
    if (kelvin >= 6500) {
      release(o);
      continue;
    }
    if (!o.control && g_manager) {
      o.control = zwlr_gamma_control_manager_v1_get_gamma_control(g_manager, o.wl);
      zwlr_gamma_control_v1_add_listener(o.control, &gamma_listener, &o);
      wl_display_roundtrip(g_display); // gamma_size or failed
    }
    if (o.applied != kelvin) set_gamma(o, kelvin);
  }
  wl_display_flush(g_display);
}

void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t) {
  if (!strcmp(iface, wl_output_interface.name)) {
    Output o;
    o.name = name;
    o.wl = (wl_output *)wl_registry_bind(reg, name, &wl_output_interface, 1);
    g_outputs.push_back(o);
  } else if (!strcmp(iface, zwlr_gamma_control_manager_v1_interface.name)) {
    g_manager = (zwlr_gamma_control_manager_v1 *)wl_registry_bind(reg, name, &zwlr_gamma_control_manager_v1_interface, 1);
  }
}
void reg_remove(void *, wl_registry *, uint32_t name) {
  for (auto it = g_outputs.begin(); it != g_outputs.end(); ++it)
    if (it->name == name) {
      release(*it);
      wl_output_destroy(it->wl);
      g_outputs.erase(it);
      return;
    }
}
const wl_registry_listener reg_listener = {reg_global, reg_remove};

int usage(int code) {
  printf("Usage: fl-nightlight [--print]\n"
         "Tints the screens warmer at night ([NightLight] in desktop.ini).\n"
         "  --print   print the colour temperature for now and exit\n");
  return code;
}

} // namespace

int main(int argc, char **argv) {
  bool print = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--print") print = true;
    else if (a == "-h" || a == "--help") return usage(0);
    else return usage(2);
  }
  Config cfg = load_config();
  if (print) {
    printf("%d\n", target_temperature(cfg));
    return 0;
  }
  g_display = wl_display_connect(nullptr);
  if (!g_display) {
    fprintf(stderr, "fl-nightlight: no Wayland display\n");
    return 1;
  }
  wl_registry *reg = wl_display_get_registry(g_display);
  wl_registry_add_listener(reg, &reg_listener, nullptr);
  wl_display_roundtrip(g_display);
  if (!g_manager) {
    fprintf(stderr, "fl-nightlight: the compositor has no wlr-gamma-control\n");
    return 1;
  }
  signal(SIGTERM, [](int) { g_quit = 1; });
  signal(SIGINT, [](int) { g_quit = 1; });

  int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (ino >= 0) inotify_add_watch(ino, fld::fld_config_dir().c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);

  int current = 6500;
  size_t known_outputs = g_outputs.size();
  while (!g_quit) {
    int target = target_temperature(cfg);
    bool animating = false;
    if (target != current || g_outputs.size() != known_outputs) {
      // fade over about two seconds
      int step = std::max(50, std::abs(target - current) / 20);
      if (std::abs(target - current) <= step) current = target;
      else current += target > current ? step : -step;
      animating = current != target;
      known_outputs = g_outputs.size();
      apply(current);
    }
    pollfd p[2] = {{wl_display_get_fd(g_display), POLLIN, 0}, {ino, POLLIN, 0}};
    wl_display_flush(g_display);
    int r = poll(p, ino >= 0 ? 2 : 1, animating ? 100 : 30000);
    if (r < 0 && errno != EINTR) break;
    if (r > 0 && (p[0].revents & POLLIN) && wl_display_dispatch(g_display) < 0) break;
    if (r > 0 && (p[0].revents & (POLLHUP | POLLERR))) break;
    if (ino >= 0 && r > 0 && (p[1].revents & POLLIN)) {
      char buf[4096];
      while (read(ino, buf, sizeof buf) > 0) {
      }
      cfg = load_config();
    }
    // new outputs get the current temperature
    for (Output &o : g_outputs)
      if (!o.failed && !o.control && current < 6500) {
        apply(current);
        break;
      }
  }
  for (Output &o : g_outputs) release(o);
  wl_display_flush(g_display);
  wl_display_disconnect(g_display);
  return 0;
}
