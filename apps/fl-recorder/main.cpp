// fl-recorder: records sound from the microphone (GStreamer; PipeWire or
// PulseAudio through autoaudiosrc) into $XDG_MUSIC_DIR/Recordings, with a
// level meter, and lists the recordings to play, rename or delete.
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <gst/gst.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/trash.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fld/window_size.h"
#include "rec_util.h"

namespace {

GstElement *g_pipe = nullptr;
std::string g_file;     // being recorded
double g_started = 0;   // monotonic seconds
double g_level = 0;     // 0..1
bool g_stopping = false;
Fl_Button *g_rec;
Fl_Box *g_time;
Fl_Choice *g_format;
Fl_Hold_Browser *g_list;
std::vector<std::string> g_files; // the list's files (full paths), newest first

double now() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string dir() {
  std::string m = fld::user_dir("MUSIC");
  if (m.empty()) m = fld::home_dir() + "/Music";
  return rec::recordings_dir(m);
}

std::string config_path() { return fld::fld_config_dir() + "/recorder.ini"; }

class Meter : public Fl_Widget {
public:
  Meter(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_color(p.bg2);
    fl_rectf(x(), y(), w(), h());
    int fill = (int)(w() * g_level);
    fl_color(g_level > 0.95 ? fl_rgb_color(0xe0, 0x40, 0x40) : p.accent);
    fl_rectf(x(), y(), fill, h());
    fl_color(p.border);
    fl_rect(x(), y(), w(), h());
  }
};
Meter *g_meter;

void fill_list() {
  g_files.clear();
  std::vector<std::pair<time_t, std::string>> found;
  for (auto &n : fld::list_dir(dir())) {
    std::string p = dir() + "/" + n;
    struct stat st;
    if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) found.push_back({st.st_mtime, p});
  }
  std::sort(found.begin(), found.end(), [](auto &a, auto &b) { return a.first > b.first; });
  g_list->clear();
  for (auto &f : found) {
    if (f.second == g_file && g_pipe) continue; // the one being written
    g_files.push_back(f.second);
    struct stat st;
    stat(f.second.c_str(), &st);
    std::string l = fld::escape_at(fld::basename(f.second)) + "\t" + fld::human_size((uint64_t)st.st_size);
    g_list->add(l.c_str());
  }
  if (g_files.empty()) g_list->add(fld::escape_at(_("No recordings yet")).c_str());
}

void update_ui() {
  bool on = g_pipe != nullptr;
  g_rec->label(on ? _("Stop") : _("Record"));
  g_rec->color(on ? fl_rgb_color(0xd0, 0x30, 0x30) : fld::palette().accent);
  on ? g_format->deactivate() : g_format->activate();
  g_time->copy_label(on ? rec::duration(now() - g_started).c_str() : "0:00");
  g_meter->redraw();
  g_rec->redraw();
}

void stop_pipeline() {
  if (!g_pipe) return;
  gst_element_set_state(g_pipe, GST_STATE_NULL);
  gst_object_unref(g_pipe);
  g_pipe = nullptr;
  g_stopping = false;
  g_level = 0;
  update_ui();
  fill_list();
}

void poll(void *) {
  if (g_pipe) {
    GstBus *bus = gst_element_get_bus(g_pipe);
    while (GstMessage *m = gst_bus_pop(bus)) {
      switch (GST_MESSAGE_TYPE(m)) {
        case GST_MESSAGE_ELEMENT: {
          const GstStructure *s = gst_message_get_structure(m);
          if (s && gst_structure_has_name(s, "level")) {
            const GValue *peaks = gst_structure_get_value(s, "peak");
            double db = -100;
            if (peaks && G_VALUE_HOLDS(peaks, G_TYPE_VALUE_ARRAY)) {
              GValueArray *a = (GValueArray *)g_value_get_boxed(peaks);
              for (guint i = 0; a && i < a->n_values; i++) db = std::max(db, g_value_get_double(&a->values[i]));
            }
            g_level = rec::meter(db);
          }
          break;
        }
        case GST_MESSAGE_EOS:
          gst_message_unref(m);
          gst_object_unref(bus);
          stop_pipeline();
          Fl::repeat_timeout(0.1, poll);
          return;
        case GST_MESSAGE_ERROR: {
          GError *err = nullptr;
          gst_message_parse_error(m, &err, nullptr);
          std::string msg = err ? err->message : "?";
          if (err) g_error_free(err);
          gst_message_unref(m);
          gst_object_unref(bus);
          stop_pipeline();
          fl_alert("%s", fld::format(_("Recording failed: %s"), {msg}).c_str());
          Fl::repeat_timeout(0.1, poll);
          return;
        }
        default: break;
      }
      gst_message_unref(m);
    }
    gst_object_unref(bus);
    update_ui();
  }
  Fl::repeat_timeout(0.1, poll);
}

void start() {
  const auto &fs = rec::formats();
  int fi = std::max(0, g_format->value());
  const rec::Format &f = fs[std::min(fi, (int)fs.size() - 1)];
  fld::mkdir_p(dir());
  g_file = dir() + "/" + rec::file_name(time(nullptr), f);
  GError *err = nullptr;
  g_pipe = gst_parse_launch(rec::pipeline(f, g_file).c_str(), &err);
  if (!g_pipe || err) {
    std::string msg = err ? err->message : "?";
    if (err) g_error_free(err);
    if (g_pipe) gst_object_unref(g_pipe);
    g_pipe = nullptr;
    fl_alert("%s", fld::format(_("Cannot record in this format: %s"), {msg}).c_str());
    return;
  }
  if (gst_element_set_state(g_pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    stop_pipeline();
    fl_alert("%s", _("The microphone could not be opened."));
    return;
  }
  g_started = now();
  fld::Ini c;
  c.load(config_path());
  c.set("Recorder", "Format", f.id);
  c.save(config_path());
  update_ui();
}

void stop() {
  if (!g_pipe || g_stopping) return;
  // the encoder and muxer finish the file on end of stream
  g_stopping = true;
  gst_element_send_event(g_pipe, gst_event_new_eos());
}

std::string selected() {
  int i = g_list->value() - 1;
  return i >= 0 && i < (int)g_files.size() ? g_files[i] : "";
}

void rename_selected() {
  std::string p = selected();
  if (p.empty()) return;
  const char *typed = fl_input("%s", fld::basename(p).c_str(), _("New name:"));
  if (!typed) return;
  std::string n = rec::renamed(fld::basename(p), typed);
  if (n.empty()) return;
  std::string to = fld::dirname(p) + "/" + n;
  if (fld::file_exists(to)) {
    fl_alert("%s", fld::format(_("%s exists already."), {n}).c_str());
    return;
  }
  if (rename(p.c_str(), to.c_str()) != 0) fl_alert("%s", strerror(errno));
  fill_list();
}

void trash_selected() {
  std::string p = selected(), err;
  if (p.empty()) return;
  if (!fld::trash(p, &err)) fl_alert("%s", err.c_str());
  fill_list();
}

} // namespace

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  fld::init_app("fl-recorder", argc, argv);
  const int W = 460, H = 420;
  const fld::Palette &p = fld::palette();
  Fl_Double_Window *win = new Fl_Double_Window(W, H, _("Sound Recorder"));
  g_time = new Fl_Box(16, 14, W - 32, 50, "0:00");
  g_time->labelsize(36);
  g_time->labelfont(FL_HELVETICA_BOLD);
  g_meter = new Meter(16, 70, W - 32, 14);
  g_rec = new Fl_Button((W - 140) / 2, 96, 140, 40, _("Record"));
  g_rec->labelcolor(FL_WHITE);
  g_rec->color(p.accent);
  g_rec->shortcut(' ');
  g_rec->tooltip(_("Start or stop recording (Space)"));
  g_rec->callback([](Fl_Widget *, void *) { g_pipe ? stop() : start(); });
  g_format = new Fl_Choice(110, 148, W - 126, 28, _("Format:"));
  fld::Ini cfg;
  cfg.load(config_path());
  std::string fmt = cfg.get("Recorder", "Format", "opus");
  for (size_t i = 0; i < rec::formats().size(); i++) {
    const rec::Format &f = rec::formats()[i];
    g_format->add(_(f.label), 0, nullptr, nullptr, 0);
    if (fmt == f.id) g_format->value((int)i);
  }
  if (g_format->value() < 0) g_format->value(0);
  Fl_Box *lbl = new Fl_Box(16, 186, W - 32, 22, _("Recordings"));
  lbl->labelfont(FL_HELVETICA_BOLD);
  lbl->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  g_list = new Fl_Hold_Browser(16, 210, W - 32, H - 260);
  static int widths[] = {W - 132, 0};
  g_list->column_widths(widths);
  g_list->column_char('\t');
  g_list->callback([](Fl_Widget *, void *) {
    if (Fl::event_clicks() && !selected().empty()) fld::open_path(selected());
  });
  fld::set_context_menu(g_list, [](fld::Menu &m) {
    bool any = !selected().empty();
    m.item(_("Play"), [] { fld::open_path(selected()); }, any ? 0 : FL_MENU_INACTIVE);
    m.item(_("Rename…"), [] { rename_selected(); }, any ? 0 : FL_MENU_INACTIVE);
    m.item(_("Move to Trash"), [] { trash_selected(); }, any ? 0 : FL_MENU_INACTIVE).divider();
    m.item(_("Open the Folder"), [] { fld::open_path(dir()); });
  });
  Fl_Button *folder = new Fl_Button(16, H - 42, 140, 30, _("Open Folder"));
  folder->callback([](Fl_Widget *, void *) {
    fld::mkdir_p(dir());
    fld::open_path(dir());
  });
  win->end();
  win->resizable(g_list);
  win->size_range(360, 340);
  fld::remember_window_size(win, "fl-recorder");
  win->callback([](Fl_Widget *w, void *) {
    // finish the file before closing
    if (g_pipe) {
      stop();
      for (int i = 0; i < 50 && g_pipe; i++) Fl::wait(0.1);
      stop_pipeline();
    }
    w->hide();
  });
  win->show(argc, argv);
  fill_list();
  Fl::add_timeout(0.1, poll);
  return Fl::run();
}
