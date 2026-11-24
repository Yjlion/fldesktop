// fl-scan: scanning with SANE (through scanimage). Pick a scanner, preview,
// choose an area, scan one page or the whole feeder, and save the pages as
// PNG pictures or one PDF.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <algorithm>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Progress.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <cairo-pdf.h>
#include <cairo.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "scan_util.h"
#ifdef FLD_HAVE_MDNS
#include "mdns.h"
#endif

namespace {

struct Page {
  std::string path; // a PNG in the work folder
  int dpi = 150;
  int w = 0, h = 0;
};

Fl_Double_Window *g_win;
Fl_Choice *g_device, *g_mode, *g_res, *g_source;
Fl_Button *g_preview_btn, *g_scan_btn, *g_refresh;
Fl_Progress *g_progress;
Fl_Hold_Browser *g_pages_list;
Fl_Box *g_status;
class PreviewBox;
PreviewBox *g_preview;
std::vector<scan::Device> g_devices;
scan::Options g_opts;
std::vector<Page> g_pages;
std::string g_work; // $XDG_RUNTIME_DIR/fl-scan-<pid>
bool g_busy = false;
int g_counter = 0;

void status(const std::string &s) {
  static std::string keep;
  keep = s;
  g_status->label(keep.c_str());
  g_status->redraw();
}

// The preview scan; a rectangle dragged on it is the area to scan.
class PreviewBox : public Fl_Box {
public:
  std::unique_ptr<Fl_PNG_Image> img;
  double ax = 0, ay = 0, bx = 0, by = 0; // the area, 0..1 of the page
  bool has_area = false;
  PreviewBox(int x, int y, int w, int h) : Fl_Box(x, y, w, h) { box(FL_FLAT_BOX); }
  void load(const std::string &path) {
    img.reset(new Fl_PNG_Image(path.c_str()));
    if (img->fail()) img.reset();
    has_area = false;
    redraw();
  }
  void place(int &X, int &Y, int &W, int &H) {
    double pw = g_opts.max_w > 0 ? g_opts.max_w : 216, ph = g_opts.max_h > 0 ? g_opts.max_h : 297;
    double z = std::min((w() - 16) / pw, (h() - 16) / ph);
    W = (int)(pw * z);
    H = (int)(ph * z);
    X = x() + (w() - W) / 2;
    Y = y() + (h() - H) / 2;
  }
  void draw() override {
    fl_color(fl_darker(fld::palette().bg));
    fl_rectf(x(), y(), w(), h());
    int X, Y, W, H;
    place(X, Y, W, H);
    fl_color(FL_WHITE);
    fl_rectf(X, Y, W, H);
    if (img) {
      img->scale(W, H, 0, 1);
      img->draw(X, Y);
    } else {
      fl_color(FL_DARK3);
      fl_font(FL_HELVETICA, 13);
      fl_draw("Preview shows the page here.\nDrag on it to scan only a part.", X, Y, W, H, FL_ALIGN_CENTER | FL_ALIGN_WRAP);
    }
    if (has_area) {
      int x1 = X + (int)(std::min(ax, bx) * W), y1 = Y + (int)(std::min(ay, by) * H);
      int x2 = X + (int)(std::max(ax, bx) * W), y2 = Y + (int)(std::max(ay, by) * H);
      fl_color(fld::palette().accent);
      fl_line_style(FL_SOLID, 2);
      fl_rect(x1, y1, x2 - x1, y2 - y1);
      fl_line_style(0);
    }
  }
  int handle(int e) override {
    int X, Y, W, H;
    place(X, Y, W, H);
    double fx = std::max(0.0, std::min(1.0, (Fl::event_x() - X) / (double)W));
    double fy = std::max(0.0, std::min(1.0, (Fl::event_y() - Y) / (double)H));
    if (e == FL_PUSH) {
      ax = bx = fx;
      ay = by = fy;
      has_area = false;
      redraw();
      return 1;
    }
    if (e == FL_DRAG) {
      bx = fx;
      by = fy;
      has_area = std::fabs(bx - ax) > 0.02 && std::fabs(by - ay) > 0.02;
      redraw();
      return 1;
    }
    if (e == FL_RELEASE) return 1;
    return Fl_Box::handle(e);
  }
};

void set_busy(bool b) {
  g_busy = b;
  for (Fl_Widget *w : {(Fl_Widget *)g_preview_btn, (Fl_Widget *)g_scan_btn, (Fl_Widget *)g_refresh, (Fl_Widget *)g_device})
    b || g_devices.empty() ? w->deactivate() : w->activate();
  g_refresh->activate();
  if (b) g_refresh->deactivate();
  b ? g_progress->show() : g_progress->hide();
}

std::string current_device() {
  int i = g_device->value();
  return i >= 0 && i < (int)g_devices.size() ? g_devices[(size_t)i].name : "";
}

void load_options() {
  g_mode->clear();
  g_res->clear();
  g_source->clear();
  std::string dev = current_device();
  if (dev.empty()) return;
  std::string out;
  fld::run_capture({"scanimage", "-d", dev, "-A"}, out);
  g_opts = scan::parse_options(out);
  int pick = 0;
  for (size_t i = 0; i < g_opts.modes.size(); i++) {
    g_mode->add(g_opts.modes[i].c_str());
    if (g_opts.modes[i] == "Color") pick = (int)i;
  }
  if (!g_opts.modes.empty()) g_mode->value(pick);
  pick = 0;
  if (g_opts.resolutions.empty()) g_opts.resolutions = {75, 150, 300, 600};
  for (size_t i = 0; i < g_opts.resolutions.size(); i++) {
    g_res->add((std::to_string(g_opts.resolutions[i]) + " dpi").c_str());
    if (g_opts.resolutions[i] <= 300) pick = (int)i;
  }
  g_res->value(pick);
  for (size_t i = 0; i < g_opts.sources.size(); i++) g_source->add(fld::escape_at(g_opts.sources[i]).c_str());
  if (!g_opts.sources.empty()) g_source->value(0);
  g_opts.sources.size() > 1 ? g_source->activate() : g_source->deactivate();
  g_preview->img.reset();
  g_preview->redraw();
}

#ifdef FLD_HAVE_MDNS
void network_hint(void *);
std::vector<std::string> g_network; // network scanners' names (mDNS)
#endif

void find_devices() {
  if (fld::which("scanimage").empty()) {
    status("scanimage is not installed: install SANE (sane / sane-utils) to scan.");
    set_busy(false);
    return;
  }
  status("Looking for scanners…");
  set_busy(true);
  g_progress->value(0);
  std::thread([] {
    std::string out;
    fld::run_capture({"scanimage", "-L"}, out);
    std::string *o = new std::string(out);
    Fl::awake([](void *d) {
      std::string *o = (std::string *)d;
      g_devices = scan::parse_devices(*o);
      delete o;
      g_device->clear();
      for (auto &dv : g_devices) g_device->add(fld::escape_at(dv.label + "  (" + dv.name + ")").c_str());
      if (!g_devices.empty()) g_device->value(0);
      set_busy(false);
      status(g_devices.empty() ? "No scanner found. Is it switched on and connected?"
                               : std::to_string(g_devices.size()) + " scanner(s) found.");
      load_options();
#ifdef FLD_HAVE_MDNS
      if (!g_network.empty()) network_hint(nullptr);
#endif
    }, o);
  }).detach();
}

// ---- scanners that announce themselves on the network (eSCL / AirScan) ----
#ifdef FLD_HAVE_MDNS
mdns::Browser *g_browser = nullptr;

bool sane_has(const std::string &name) {
  for (auto &d : g_devices)
    if (d.label.find(name) != std::string::npos || d.name.find(name) != std::string::npos) return true;
  return false;
}

bool have_airscan() {
  return fld::file_exists("/etc/sane.d/airscan.conf") || fld::file_exists("/etc/sane.d/dll.d/airscan") ||
         fld::file_exists("/usr/lib/sane/libsane-airscan.so.1") ||
         fld::file_exists("/usr/lib/x86_64-linux-gnu/sane/libsane-airscan.so.1");
}

// what SANE does not list yet: look again (sane-airscan), or say what is missing
void network_hint(void *) {
  std::vector<std::string> missing;
  for (auto &n : g_network)
    if (!sane_has(n)) missing.push_back(n);
  if (missing.empty()) return;
  if (have_airscan()) {
    static int tries = 0;
    if (tries++ < 2) find_devices();
    return;
  }
  status("Found on the network: " + fld::join(missing, ", ") +
         ". Install sane-airscan to scan with it (driverless scanning).");
}

void start_discovery() {
  g_browser = new mdns::Browser({"_uscan._tcp", "_uscans._tcp"}, [](const mdns::Service &s) {
    if (std::find(g_network.begin(), g_network.end(), s.name) == g_network.end()) g_network.push_back(s.name);
    Fl::remove_timeout(network_hint);
    Fl::add_timeout(1.5, network_hint);
  }, [](const mdns::Service &s) {
    g_network.erase(std::remove(g_network.begin(), g_network.end(), s.name), g_network.end());
  });
}
#endif

// runs scanimage in a thread, reading its progress; done(ok, message) in the FLTK thread
void run_scan(const scan::Request &req, std::function<void(bool, const std::string &)> done) {
  set_busy(true);
  g_progress->value(0);
  std::vector<std::string> args = scan::scanimage_args(req);
  auto *cb = new std::function<void(bool, const std::string &)>(done);
  std::thread([args, cb] {
    int err[2];
    if (pipe2(err, O_CLOEXEC) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
      dup2(err[1], 2);
      int null = open("/dev/null", O_WRONLY);
      if (null >= 0) dup2(null, 1);
      std::vector<char *> av;
      for (auto &s : args) av.push_back((char *)s.c_str());
      av.push_back(nullptr);
      execvp(av[0], av.data());
      _exit(127);
    }
    close(err[1]);
    std::string all, line;
    char buf[512];
    ssize_t n;
    while ((n = read(err[0], buf, sizeof buf)) > 0) {
      for (ssize_t i = 0; i < n; i++) {
        char c = buf[i];
        all += c;
        if (c == '\n' || c == '\r') {
          if (fld::starts_with(line, "Progress: ")) {
            float *pct = new float((float)atof(line.c_str() + 10));
            Fl::awake([](void *d) {
              g_progress->value(*(float *)d);
              delete (float *)d;
            }, pct);
          }
          line.clear();
        } else {
          line += c;
        }
      }
    }
    close(err[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    bool ok = WIFEXITED(st) && (WEXITSTATUS(st) == 0 || WEXITSTATUS(st) == 7); // 7: the feeder ran empty
    struct R {
      std::function<void(bool, const std::string &)> *cb;
      bool ok;
      std::string msg;
    };
    Fl::awake([](void *d) {
      R *r = (R *)d;
      set_busy(false);
      (*r->cb)(r->ok, r->msg);
      delete r->cb;
      delete r;
    }, new R{cb, ok, scan::error_text(all)});
  }).detach();
}

scan::Request base_request() {
  scan::Request r;
  r.device = current_device();
  if (g_mode->value() >= 0) r.mode = g_opts.modes[(size_t)g_mode->value()];
  if (g_source->value() >= 0 && g_source->value() < (int)g_opts.sources.size())
    r.source = g_opts.sources[(size_t)g_source->value()];
  r.resolution = g_opts.resolutions[(size_t)std::max(0, g_res->value())];
  return r;
}

void preview() {
  scan::Request r = base_request();
  r.resolution = g_opts.resolutions.empty() ? 75 : g_opts.resolutions.front();
  for (int x : g_opts.resolutions)
    if (x >= 50 && x <= 100) r.resolution = x;
  r.output = g_work + "/preview.png";
  status("Previewing…");
  run_scan(r, [r](bool ok, const std::string &msg) {
    if (!ok) {
      status("Preview failed: " + msg);
      return;
    }
    g_preview->load(r.output);
    status("Drag on the preview to scan only a part, or scan the whole page.");
  });
}

void fill_pages() {
  g_pages_list->clear();
  for (size_t i = 0; i < g_pages.size(); i++) {
    char b[96];
    snprintf(b, sizeof b, "Page %d   %d × %d px, %d dpi", (int)i + 1, g_pages[i].w, g_pages[i].h, g_pages[i].dpi);
    g_pages_list->add(b);
  }
  if (g_pages.empty()) g_pages_list->add("@iScanned pages are listed here.");
}

void add_page(const std::string &path, int dpi) {
  Fl_PNG_Image png(path.c_str());
  Page p;
  p.path = path;
  p.dpi = dpi;
  p.w = png.fail() ? 0 : png.w();
  p.h = png.fail() ? 0 : png.h();
  if (p.w > 0) g_pages.push_back(p);
}

void do_scan() {
  scan::Request r = base_request();
  if (g_preview->has_area) {
    double pw = g_opts.max_w > 0 ? g_opts.max_w : 216, ph = g_opts.max_h > 0 ? g_opts.max_h : 297;
    r.x = std::min(g_preview->ax, g_preview->bx) * pw;
    r.y = std::min(g_preview->ay, g_preview->by) * ph;
    r.w = std::fabs(g_preview->bx - g_preview->ax) * pw;
    r.h = std::fabs(g_preview->by - g_preview->ay) * ph;
  }
  bool feeder = fld::lower(r.source).find("adf") != std::string::npos ||
                fld::lower(r.source).find("feeder") != std::string::npos;
  int n = ++g_counter;
  r.batch = feeder;
  r.output = feeder ? g_work + "/scan-" + std::to_string(n) + "-%d.png" : g_work + "/scan-" + std::to_string(n) + ".png";
  status("Scanning…");
  run_scan(r, [r, n, feeder](bool ok, const std::string &msg) {
    size_t before = g_pages.size();
    if (feeder) {
      for (int i = 1; i < 500; i++) {
        std::string p = g_work + "/scan-" + std::to_string(n) + "-" + std::to_string(i) + ".png";
        if (!fld::file_exists(p)) break;
        add_page(p, r.resolution);
      }
    } else if (ok) {
      add_page(r.output, r.resolution);
    }
    fill_pages();
    size_t added = g_pages.size() - before;
    if (added) status("Scanned " + std::to_string(added) + " page(s). Save them when you are done.");
    else status("Scanning failed: " + msg);
  });
}

bool save_pdf(const std::string &path) {
  cairo_surface_t *pdf = cairo_pdf_surface_create(path.c_str(), 595, 842);
  if (cairo_surface_status(pdf) != CAIRO_STATUS_SUCCESS) return false;
  cairo_t *cr = cairo_create(pdf);
  for (const Page &p : g_pages) {
    cairo_surface_t *img = cairo_image_surface_create_from_png(p.path.c_str());
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(img);
      continue;
    }
    double w = scan::px_to_pt(p.w, p.dpi), h = scan::px_to_pt(p.h, p.dpi);
    cairo_pdf_surface_set_size(pdf, w, h);
    cairo_save(cr);
    cairo_scale(cr, w / p.w, h / p.h);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_show_page(cr);
    cairo_surface_destroy(img);
  }
  cairo_destroy(cr);
  cairo_surface_finish(pdf);
  bool ok = cairo_surface_status(pdf) == CAIRO_STATUS_SUCCESS;
  cairo_surface_destroy(pdf);
  return ok;
}

void save() {
  if (g_pages.empty()) return;
  Fl_Native_File_Chooser fc;
  fc.title("Save the Scan");
  fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  fc.filter("PDF document (all pages)\t*.pdf\nPNG pictures (one per page)\t*.png");
  std::string dir = fld::user_dir("DOCUMENTS");
  if (!dir.empty()) fc.directory(dir.c_str());
  fc.preset_file(("Scan " + fld::format_time(time(nullptr), "%Y-%m-%d %H-%M") + ".pdf").c_str());
  fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
  if (fc.show() != 0) return;
  std::string p = fc.filename();
  std::string ext = fld::extension(p);
  if (ext != "pdf" && ext != "png") p += fc.filter_value() == 1 ? ".png" : ".pdf", ext = fld::extension(p);
  if (ext == "pdf") {
    if (!save_pdf(p)) fl_alert("Cannot write %s", p.c_str());
    else status("Saved " + fld::tilde(p));
    return;
  }
  // PNG: name.png, name-2.png, ...
  std::string stem = p.substr(0, p.size() - 4);
  for (size_t i = 0; i < g_pages.size(); i++) {
    std::string out = i == 0 ? p : stem + "-" + std::to_string(i + 1) + ".png";
    std::string data;
    if (!fld::read_file(g_pages[i].path, data) || !fld::write_file(out, data)) {
      fl_alert("Cannot write %s", out.c_str());
      return;
    }
  }
  status("Saved " + std::to_string(g_pages.size()) + " picture(s) next to " + fld::tilde(p));
}

void cleanup() {
  std::string out;
  if (!g_work.empty()) fld::run_capture({"rm", "-rf", g_work}, out);
}

} // namespace

int main(int argc, char **argv) {
  fld::init_app("fl-scan", argc, argv);
  Fl::lock();
#ifdef FLD_HAVE_MDNS
  start_discovery();
#endif
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      printf("Usage: fl-scan\nScans with SANE (scanimage) and saves PNG pictures or a PDF.\n");
      return 0;
    }
  g_work = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-scan-" + std::to_string(getpid());
  fld::mkdir_p(g_work, 0700);
  atexit(cleanup);
  const int W = 960, H = 680, SW = 300;
  g_win = new Fl_Double_Window(W, H, "Scan");
  int y = 10;
  g_device = new Fl_Choice(90, y, W - SW - 190, 28, "Scanner:");
  g_device->callback([](Fl_Widget *, void *) { load_options(); });
  g_refresh = new Fl_Button(W - SW - 94, y, 86, 28, "Search");
  g_refresh->tooltip("Look for scanners again");
  g_refresh->callback([](Fl_Widget *, void *) { find_devices(); });
  y += 36;
  g_mode = new Fl_Choice(90, y, 120, 28, "Colours:");
  g_res = new Fl_Choice(300, y, 110, 28, "Quality:");
  g_source = new Fl_Choice(480, y, W - SW - 488, 28, "From:");
  y += 38;
  g_preview = new PreviewBox(8, y, W - SW - 16, H - y - 80);
  g_preview_btn = new Fl_Button(8, H - 68, 110, 34, "Preview");
  g_preview_btn->callback([](Fl_Widget *, void *) { preview(); });
  g_scan_btn = new Fl_Button(124, H - 68, 110, 34, "Scan");
  g_scan_btn->callback([](Fl_Widget *, void *) { do_scan(); });
  g_progress = new Fl_Progress(244, H - 62, W - SW - 260, 22);
  g_progress->minimum(0);
  g_progress->maximum(100);
  g_progress->selection_color(fld::palette().accent);
  g_progress->hide();
  Fl_Box *ph = new Fl_Box(W - SW + 8, 10, SW - 16, 28, "Pages");
  ph->labelfont(FL_HELVETICA_BOLD);
  ph->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  g_pages_list = new Fl_Hold_Browser(W - SW + 8, 42, SW - 16, H - 42 - 120);
  int bx = W - SW + 8;
  auto pbtn = [&](const char *l, int w, Fl_Callback *cb) {
    Fl_Button *b = new Fl_Button(bx, H - 110, w, 30, l);
    b->callback(cb);
    bx += w + 4;
  };
  pbtn("↑", 36, [](Fl_Widget *, void *) {
    int i = g_pages_list->value() - 1;
    if (i > 0 && i < (int)g_pages.size()) {
      std::swap(g_pages[(size_t)i], g_pages[(size_t)i - 1]);
      fill_pages();
      g_pages_list->select(i);
    }
  });
  pbtn("↓", 36, [](Fl_Widget *, void *) {
    int i = g_pages_list->value() - 1;
    if (i >= 0 && i + 1 < (int)g_pages.size()) {
      std::swap(g_pages[(size_t)i], g_pages[(size_t)i + 1]);
      fill_pages();
      g_pages_list->select(i + 2);
    }
  });
  pbtn("Remove", 80, [](Fl_Widget *, void *) {
    int i = g_pages_list->value() - 1;
    if (i >= 0 && i < (int)g_pages.size()) {
      g_pages.erase(g_pages.begin() + i);
      fill_pages();
    }
  });
  pbtn("Show", 64, [](Fl_Widget *, void *) {
    int i = g_pages_list->value() - 1;
    if (i >= 0 && i < (int)g_pages.size()) g_preview->load(g_pages[(size_t)i].path);
  });
  Fl_Button *sv = new Fl_Button(W - SW + 8, H - 70, SW - 16, 36, "Save…");
  sv->labelfont(FL_HELVETICA_BOLD);
  sv->callback([](Fl_Widget *, void *) { save(); });
  g_status = new Fl_Box(8, H - 28, W - 16, 24);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  g_status->labelcolor(fld::palette().fg_dim);
  g_win->end();
  g_win->resizable(g_preview);
  g_win->size_range(760, 520);
  g_win->callback([](Fl_Widget *w, void *) {
    if (!g_pages.empty() && fl_choice("Close without saving the scanned pages?", "Cancel", "Close", nullptr) != 1) return;
    w->hide();
  });
  fld::set_window_icon(g_win, "fl-scan");
  fill_pages();
  g_win->show();
  find_devices();
  int r = Fl::run();
  cleanup();
  return r;
}
