// fl-cliphist: clipboard history (wlr-data-control)
//
//   fl-cliphist                 record the clipboard (started with the session)
//   fl-cliphist --list          the history, newest first: ID, P (pinned), type, text
//   fl-cliphist --copy ID       put an entry on the clipboard again
//   fl-cliphist --set TEXT      put a text on the clipboard
//   fl-cliphist --set-file FILE put a file's contents on it (a PNG as an image)
//   fl-cliphist --paste         print the clipboard's text
//   fl-cliphist --pin ID | --unpin ID | --delete ID | --clear
//
// [Clipboard] in panel.ini: History=false stops recording; Max= (default 50)
// is the number of entries kept besides the pinned ones. Programs that mark
// a copy as a password (x-kde-passwordManagerHint) are not recorded.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <wayland-client.h>

#include "cliphist_store.h"
#include "fld/ini.h"
#include "fld/util.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

namespace {

const size_t kMaxBytes = 32u << 20;

wl_display *g_display;
wl_seat *g_seat;
zwlr_data_control_manager_v1 *g_mgr;
zwlr_data_control_device_v1 *g_device;
bool g_finished;

struct Offer {
  zwlr_data_control_offer_v1 *offer;
  std::vector<std::string> mimes;
};
std::vector<Offer *> g_offers;
// --paste: the selection once it arrived
zwlr_data_control_offer_v1 *g_selection;
bool g_have_selection;
bool g_recording;

void reg_global(void *, wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
  if (!strcmp(iface, wl_seat_interface.name) && !g_seat)
    g_seat = (wl_seat *)wl_registry_bind(r, name, &wl_seat_interface, 1);
  else if (!strcmp(iface, zwlr_data_control_manager_v1_interface.name))
    g_mgr = (zwlr_data_control_manager_v1 *)wl_registry_bind(r, name, &zwlr_data_control_manager_v1_interface,
                                                           ver < 2 ? ver : 2);
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener reg_listener = {reg_global, reg_remove};

bool connect_wayland() {
  g_display = wl_display_connect(nullptr);
  if (!g_display) {
    fprintf(stderr, "fl-cliphist: no Wayland display\n");
    return false;
  }
  wl_registry_add_listener(wl_display_get_registry(g_display), &reg_listener, nullptr);
  wl_display_roundtrip(g_display);
  if (!g_mgr || !g_seat) {
    fprintf(stderr, "fl-cliphist: the compositor has no wlr-data-control\n");
    return false;
  }
  return true;
}

Offer *find_offer(zwlr_data_control_offer_v1 *o) {
  for (Offer *of : g_offers)
    if (of->offer == o) return of;
  return nullptr;
}

void drop_offer(zwlr_data_control_offer_v1 *o) {
  for (size_t i = 0; i < g_offers.size(); i++)
    if (g_offers[i]->offer == o) {
      zwlr_data_control_offer_v1_destroy(o);
      delete g_offers[i];
      g_offers.erase(g_offers.begin() + (long)i);
      return;
    }
}

void offer_mime(void *data, zwlr_data_control_offer_v1 *, const char *mime) { ((Offer *)data)->mimes.push_back(mime); }
const zwlr_data_control_offer_v1_listener offer_listener = {offer_mime};

// Reads what the selection's owner sends, up to kMaxBytes and 3 seconds.
bool receive(zwlr_data_control_offer_v1 *o, const std::string &mime, std::string &out) {
  int p[2];
  if (pipe2(p, O_CLOEXEC) < 0) return false;
  zwlr_data_control_offer_v1_receive(o, mime.c_str(), p[1]);
  close(p[1]);
  wl_display_flush(g_display);
  out.clear();
  char buf[65536];
  time_t start = time(nullptr);
  for (;;) {
    pollfd pfd = {p[0], POLLIN, 0};
    int r = poll(&pfd, 1, 500);
    if (r < 0 && errno != EINTR) break;
    if (r > 0) {
      ssize_t n = read(p[0], buf, sizeof buf);
      if (n <= 0) break;
      out.append(buf, (size_t)n);
      if (out.size() > kMaxBytes) {
        out.clear();
        break;
      }
    }
    if (time(nullptr) - start > 3) break;
  }
  close(p[0]);
  return !out.empty();
}

struct Settings {
  bool history = true;
  size_t max = 50;
};

Settings settings() {
  fld::Ini c;
  c.load(fld::fld_config_dir() + "/panel.ini");
  Settings s;
  s.history = c.get_bool("Clipboard", "History", true);
  int m = c.get_int("Clipboard", "Max", 50);
  s.max = (size_t)(m < 1 ? 1 : m > 1000 ? 1000 : m);
  return s;
}

void dev_data_offer(void *, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *o) {
  Offer *of = new Offer{o, {}};
  zwlr_data_control_offer_v1_add_listener(o, &offer_listener, of);
  g_offers.push_back(of);
}

void dev_selection(void *, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *o) {
  // offers other than this one are done with
  for (size_t i = g_offers.size(); i-- > 0;)
    if (g_offers[i]->offer != o) drop_offer(g_offers[i]->offer);
  g_selection = o;
  g_have_selection = true;
  if (!g_recording || !o) return;
  Offer *of = find_offer(o);
  if (!of) return;
  Settings s = settings();
  if (!s.history) return;
  std::string mime = cliphist::pick_mime(of->mimes), data;
  if (mime.empty() || !receive(o, mime, data)) return;
  // text under one name, whatever the program called it
  if (mime != "image/png") mime = "text/plain;charset=utf-8";
  cliphist::Store().add(mime, data, s.max);
}

void dev_finished(void *, zwlr_data_control_device_v1 *) { g_finished = true; }
void dev_primary(void *, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *o) {
  // the primary selection (middle click) is not recorded
  if (o && o != g_selection) drop_offer(o);
}
const zwlr_data_control_device_v1_listener dev_listener = {dev_data_offer, dev_selection, dev_finished, dev_primary};

int record() {
  if (!connect_wayland()) return 1;
  g_recording = true;
  g_device = zwlr_data_control_manager_v1_get_data_device(g_mgr, g_seat);
  zwlr_data_control_device_v1_add_listener(g_device, &dev_listener, nullptr);
  while (!g_finished && wl_display_dispatch(g_display) >= 0) {
  }
  return 0;
}

int paste() {
  if (!connect_wayland()) return 1;
  g_device = zwlr_data_control_manager_v1_get_data_device(g_mgr, g_seat);
  zwlr_data_control_device_v1_add_listener(g_device, &dev_listener, nullptr);
  for (int i = 0; i < 5 && !g_have_selection; i++) wl_display_roundtrip(g_display);
  if (!g_selection) return 1;
  Offer *of = find_offer(g_selection);
  std::string mime = of ? cliphist::pick_mime(of->mimes) : "", data;
  if (mime.empty() || mime == "image/png" || !receive(g_selection, mime, data)) return 1;
  fwrite(data.data(), 1, data.size(), stdout);
  return 0;
}

// ---- owning the selection (--copy, --set) ---------------------------------

std::string g_serve_mime, g_serve_data;
bool g_cancelled;

void src_send(void *, zwlr_data_control_source_v1 *, const char *, int32_t fd) {
  size_t done = 0;
  while (done < g_serve_data.size()) {
    ssize_t n = write(fd, g_serve_data.data() + done, g_serve_data.size() - done);
    if (n <= 0) break;
    done += (size_t)n;
  }
  close(fd);
}
void src_cancelled(void *, zwlr_data_control_source_v1 *) { g_cancelled = true; }
const zwlr_data_control_source_v1_listener src_listener = {src_send, src_cancelled};

// Puts data on the clipboard and keeps serving it in a child process until
// another program takes the clipboard.
int serve(const std::string &mime, const std::string &data) {
  g_serve_mime = mime;
  g_serve_data = data;
  int ready[2];
  if (pipe(ready) < 0) return 1;
  pid_t pid = fork();
  if (pid < 0) return 1;
  if (pid > 0) {
    close(ready[1]);
    char c = 0;
    ssize_t n = read(ready[0], &c, 1);
    close(ready[0]);
    return n == 1 && c == 1 ? 0 : 1;
  }
  close(ready[0]);
  setsid();
  char ok = 0;
  if (connect_wayland()) {
    g_device = zwlr_data_control_manager_v1_get_data_device(g_mgr, g_seat);
    zwlr_data_control_source_v1 *src = zwlr_data_control_manager_v1_create_data_source(g_mgr);
    zwlr_data_control_source_v1_add_listener(src, &src_listener, nullptr);
    if (mime == "image/png") {
      zwlr_data_control_source_v1_offer(src, "image/png");
    } else {
      for (const char *m : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"})
        zwlr_data_control_source_v1_offer(src, m);
    }
    zwlr_data_control_device_v1_set_selection(g_device, src);
    ok = wl_display_roundtrip(g_display) >= 0 ? 1 : 0;
  }
  if (write(ready[1], &ok, 1) < 0) {
  }
  close(ready[1]);
  if (!ok) _exit(1);
  int devnull = open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    dup2(devnull, 0);
    dup2(devnull, 1);
    close(devnull);
  }
  while (!g_cancelled && wl_display_dispatch(g_display) >= 0) {
  }
  _exit(0);
}

uint64_t parse_id(const char *s) { return strtoull(s, nullptr, 10); }

int usage(int code) {
  printf("Usage: fl-cliphist [--list | --copy ID | --set TEXT | --set-file FILE | --paste | --pin ID | --unpin ID | --delete ID | --clear]\n"
         "Without options: records the clipboard history ([Clipboard] in panel.ini).\n");
  return code;
}

} // namespace

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  if (argc < 2) return record();
  std::string a = argv[1];
  cliphist::Store store;
  if (a == "-h" || a == "--help") return usage(0);
  if (a == "--list") {
    for (const cliphist::Entry &e : store.list())
      printf("%llu\t%s\t%s\t%s\n", (unsigned long long)e.id, e.pinned ? "P" : "-", e.mime.c_str(), e.preview.c_str());
    return 0;
  }
  if (a == "--paste") return paste();
  if (a == "--clear") {
    store.clear(true);
    return 0;
  }
  if (argc < 3) return usage(2);
  if (a == "--set") return serve("text/plain;charset=utf-8", argv[2]);
  if (a == "--set-file") {
    std::string data;
    if (!fld::read_file(argv[2], data)) {
      fprintf(stderr, "fl-cliphist: cannot read %s\n", argv[2]);
      return 1;
    }
    int w, h;
    return serve(cliphist::png_size(data, &w, &h) ? "image/png" : "text/plain;charset=utf-8", data);
  }
  uint64_t id = parse_id(argv[2]);
  if (a == "--copy") {
    std::string mime, data;
    if (!store.data(id, mime, data)) {
      fprintf(stderr, "fl-cliphist: no entry %s\n", argv[2]);
      return 1;
    }
    return serve(mime, data);
  }
  if (a == "--pin" || a == "--unpin") return store.set_pinned(id, a == "--pin") ? 0 : 1;
  if (a == "--delete") return store.remove(id) ? 0 : 1;
  return usage(2);
}
