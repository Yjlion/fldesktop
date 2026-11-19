// fl-webcam: camera app for fldesktop (GStreamer): live preview, photos and
// video recordings with sound.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/Fl_Toggle_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cam_util.h"
#include "camera.h"
#include "fld/menus.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "frame_view.h"

static const int BAR = 64;

struct Settings {
  std::string camera, mode;
  bool video = false, mirror = true, sound = true;
  int timer = 0;
  std::string photo_format = "jpg";
} S;

static std::string settings_path() { return fld::fld_config_dir() + "/webcam.ini"; }

static void load_settings() {
  fld::Ini c;
  c.load(settings_path());
  S.camera = c.get("Webcam", "Camera");
  S.mode = c.get("Webcam", "Resolution");
  S.video = c.get("Webcam", "Mode", "photo") == "video";
  S.mirror = c.get_bool("Webcam", "Mirror", true);
  S.sound = c.get_bool("Webcam", "RecordSound", true);
  S.timer = c.get_int("Webcam", "Timer", 0);
  S.photo_format = fld::lower(c.get("Webcam", "PhotoFormat", "jpg")) == "png" ? "png" : "jpg";
}

static void save_settings() {
  fld::Ini c;
  c.load(settings_path());
  c.set("Webcam", "Camera", S.camera);
  c.set("Webcam", "Resolution", S.mode);
  c.set("Webcam", "Mode", S.video ? "video" : "photo");
  c.set_bool("Webcam", "Mirror", S.mirror);
  c.set_bool("Webcam", "RecordSound", S.sound);
  c.set_int("Webcam", "Timer", S.timer);
  c.set("Webcam", "PhotoFormat", S.photo_format);
  c.save(settings_path());
}

static std::string photo_dir() {
  std::string d = fld::user_dir("PICTURES");
  if (d.empty()) d = fld::home_dir() + "/Pictures";
  d += "/Webcam";
  fld::mkdir_p(d);
  return d;
}

static std::string video_dir() {
  std::string d = fld::user_dir("VIDEOS");
  if (d.empty()) d = fld::home_dir() + "/Videos";
  d += "/Webcam";
  fld::mkdir_p(d);
  return d;
}

// ---- state -----------------------------------------------------------------------------

static Fl_Double_Window *g_win;
static Camera *g_cam;
static fgst::FrameView *g_view;
static Fl_Group *g_bar;
static Fl_Choice *g_cam_choice, *g_mode_choice, *g_timer_choice;
static Fl_Button *g_shutter;
static Fl_Toggle_Button *g_photo_btn, *g_video_btn;
static fld::FlatButton *g_mirror_btn, *g_sound_btn, *g_last_btn, *g_menu_btn;
static std::string g_last_capture;
static int g_countdown = 0;
static bool g_force_sw = false;
static size_t g_cam_index = 0, g_mode_index = 0;

static void layout();
static void update_controls();

// ---- shutter button: a round button drawn by hand ------------------------------------------

class Shutter : public Fl_Button {
public:
  using Fl_Button::Fl_Button;
  void draw() override {
    const fld::Palette &p = fld::palette();
    int d = std::min(w(), h()) - 4;
    int cx = x() + (w() - d) / 2, cy = y() + (h() - d) / 2;
    fl_color(FL_WHITE);
    fl_pie(cx, cy, d, d, 0, 360);
    fl_color(color());
    fl_pie(cx + 3, cy + 3, d - 6, d - 6, 0, 360);
    bool rec = g_cam && g_cam->recording();
    Fl_Color inner = S.video ? fl_rgb_color(0xe0, 0x2a, 0x2a) : (value() ? fl_darker(p.accent) : p.accent);
    fl_color(inner);
    if (rec) {
      int s = d / 3;
      fl_rectf(x() + (w() - s) / 2, y() + (h() - s) / 2, s, s);
    } else {
      fl_pie(cx + 7, cy + 7, d - 14, d - 14, 0, 360);
    }
    if (g_countdown > 0) {
      fl_color(FL_WHITE);
      fl_font(FL_HELVETICA_BOLD, d / 2);
      std::string n = std::to_string(g_countdown);
      fl_draw(n.c_str(), x() + (w() - (int)fl_width(n.c_str())) / 2, y() + (h() + fl_height()) / 2 - fl_descent());
    }
  }
};

// ---- captures ---------------------------------------------------------------------------

static void set_last_capture(const std::string &path) {
  g_last_capture = path;
  g_last_btn->tooltip(("Open " + fld::basename(path)).c_str());
  // a thumbnail of photos; a film icon for videos
  Fl_Image *thumb = nullptr;
  if (!fld::ends_with(path, ".webm")) {
    if (Fl_Shared_Image *img = Fl_Shared_Image::get(path.c_str())) {
      Fl_Image *t = img->copy(44, (int)lround(44.0 * img->h() / std::max(1, img->w())));
      img->release();
      thumb = t;
    }
  }
  static std::unique_ptr<Fl_Image> held;
  if (thumb) {
    held.reset(thumb);
    g_last_btn->icon_image(thumb);
  } else {
    g_last_btn->icon_name("video-x-generic", 32);
  }
  g_last_btn->activate();
  g_last_btn->redraw();
}

static void photo_now() {
  std::string path = cam::capture_path(photo_dir(), S.photo_format, time(nullptr));
  std::string err;
  if (g_cam->take_photo(path, err)) {
    set_last_capture(path);
    g_view->show_message("Photo saved: " + fld::basename(path), 2.5);
  } else {
    g_view->show_message("No photo: " + err, 4);
  }
}

static void record_tick(void *) {
  if (!g_cam->recording()) return;
  g_view->osd().corner = "● REC " + cam::format_duration(g_cam->recording_time());
  g_view->osd().corner_alert = true;
  g_view->redraw();
  Fl::repeat_timeout(0.5, record_tick);
}

static void start_recording() {
  std::string path = cam::capture_path(video_dir(), "webm", time(nullptr));
  std::string err;
  if (!g_cam->start_recording(path, S.sound, err)) {
    g_view->show_message("Cannot record: " + err, 5);
    return;
  }
  Fl::add_timeout(0.0, record_tick);
  update_controls();
}

static void stop_recording() {
  g_cam->stop_recording();
  g_view->show_message("Finishing the video…", 2);
}

static void countdown_tick(void *) {
  if (g_countdown <= 0) return;
  g_countdown--;
  g_shutter->redraw();
  if (g_countdown == 0) {
    g_view->osd().text.clear();
    if (S.video) start_recording();
    else photo_now();
    update_controls();
    return;
  }
  g_view->show_message(std::to_string(g_countdown), 1.1);
  Fl::repeat_timeout(1.0, countdown_tick);
}

static void shutter() {
  if (!g_cam->running()) return;
  if (g_countdown > 0) { // cancel the timer
    g_countdown = 0;
    Fl::remove_timeout(countdown_tick);
    g_view->show_message("Cancelled", 1);
    update_controls();
    return;
  }
  if (g_cam->recording()) {
    stop_recording();
    return;
  }
  if (S.timer > 0) {
    g_countdown = S.timer;
    g_view->show_message(std::to_string(g_countdown), 1.1);
    Fl::add_timeout(1.0, countdown_tick);
    update_controls();
    return;
  }
  if (S.video) start_recording();
  else photo_now();
}

// ---- camera selection ------------------------------------------------------------------

static void start_camera() {
  auto &cams = g_cam->cameras();
  if (cams.empty()) {
    g_view->osd().placeholder = "No camera was found. Connect a webcam and choose Refresh in the menu.";
    g_view->redraw();
    update_controls();
    return;
  }
  std::string err;
  if (!g_cam->start(g_cam_index, g_mode_index, err)) {
    g_view->osd().placeholder = err;
  } else {
    g_view->osd().placeholder.clear();
  }
  // software rendering: GStreamer scales the preview to the window
  if (g_view->software()) fgst::set_scale_to(g_cam->video_sink(), g_view->widget()->w(), g_view->widget()->h());
  g_view->redraw();
  update_controls();
}

static void fill_modes() {
  g_mode_choice->clear();
  auto &cams = g_cam->cameras();
  if (g_cam_index >= cams.size()) return;
  auto &modes = cams[g_cam_index].modes;
  g_mode_index = cam::default_mode(modes);
  for (size_t i = 0; i < modes.size(); i++) {
    std::string l = fld::escape_at(cam::mode_label(modes[i]));
    // '/' would make a submenu
    g_mode_choice->add(l.c_str());
    if (cam::mode_label(modes[i]) == S.mode) g_mode_index = i;
  }
  g_mode_choice->value((int)g_mode_index);
}

static void refresh_cameras() {
  if (g_cam->recording()) return;
  g_cam->stop();
  g_cam->probe(getenv("FLD_WEBCAM_SOURCE") != nullptr);
  g_cam_choice->clear();
  auto &cams = g_cam->cameras();
  g_cam_index = 0;
  for (size_t i = 0; i < cams.size(); i++) {
    std::string l = fld::escape_at(cams[i].name);
    std::string esc;
    for (char c : l) {
      if (c == '/' || c == '&' || c == '_' || c == '\\') esc += '\\';
      esc += c;
    }
    g_cam_choice->add(esc.c_str());
    if (cams[i].id == S.camera) g_cam_index = i;
  }
  if (!cams.empty()) g_cam_choice->value((int)g_cam_index);
  fill_modes();
  start_camera();
}

// ---- UI ---------------------------------------------------------------------------------

static void update_controls() {
  bool rec = g_cam->recording();
  bool has = !g_cam->cameras().empty();
  if (rec || !has) { g_cam_choice->deactivate(); g_mode_choice->deactivate(); }
  else { g_cam_choice->activate(); g_mode_choice->activate(); }
  if (rec) { g_photo_btn->deactivate(); g_video_btn->deactivate(); }
  else { g_photo_btn->activate(); g_video_btn->activate(); }
  g_photo_btn->value(!S.video);
  g_video_btn->value(S.video);
  if (g_cam->running()) g_shutter->activate();
  else g_shutter->deactivate();
  g_shutter->tooltip(rec ? "Stop recording (Space)" : S.video ? "Start recording (Space)" : "Take a photo (Space)");
  g_sound_btn->icon_name(S.sound ? "audio-input-microphone" : "microphone-disabled-symbolic", 20);
  if (!fld::icon(S.sound ? "audio-input-microphone" : "microphone-disabled-symbolic", 20))
    g_sound_btn->label(S.sound ? "Mic" : "No mic");
  g_sound_btn->tooltip(S.sound ? "Record sound: on" : "Record sound: off");
  if (S.video) g_sound_btn->show();
  else g_sound_btn->hide();
  layout();
  if (!rec) {
    g_view->osd().corner.clear();
    g_view->redraw();
  }
  g_shutter->redraw();
  g_bar->redraw();
}

static void menu_cb(Fl_Widget *w, void *) {
  enum { M_REFRESH = 1, M_PHOTOS, M_VIDEOS, M_JPG, M_PNG, M_ABOUT };
  Fl_Menu_Item items[] = {
      {"Refresh Cameras", 0, nullptr, (void *)M_REFRESH, FL_MENU_DIVIDER},
      {"Open Photos Folder", 0, nullptr, (void *)M_PHOTOS},
      {"Open Videos Folder", 0, nullptr, (void *)M_VIDEOS, FL_MENU_DIVIDER},
      {"Photos as JPEG", 0, nullptr, (void *)M_JPG, FL_MENU_RADIO | (S.photo_format == "jpg" ? FL_MENU_VALUE : 0)},
      {"Photos as PNG", 0, nullptr, (void *)M_PNG,
       FL_MENU_RADIO | FL_MENU_DIVIDER | (S.photo_format == "png" ? FL_MENU_VALUE : 0)},
      {"About", 0, nullptr, (void *)M_ABOUT},
      {nullptr}};
  const Fl_Menu_Item *r = items[0].popup(w->x(), w->y() - 6 * 24);
  if (!r) return;
  switch ((long)r->user_data()) {
    case M_REFRESH: refresh_cameras(); break;
    case M_PHOTOS: fld::open_path(photo_dir()); break;
    case M_VIDEOS: fld::open_path(video_dir()); break;
    case M_JPG: S.photo_format = "jpg"; save_settings(); break;
    case M_PNG: S.photo_format = "png"; save_settings(); break;
    case M_ABOUT:
      fld::about_dialog("Webcam", "fl-webcam",
                        ("Takes photos and records videos with a camera. Built with FLTK and GStreamer.\nRendering: " +
                         std::string(g_view->kind()))
                            .c_str());
      break;
  }
}

static void layout() {
  int W = g_win->w(), H = g_win->h();
  g_view->widget()->resize(0, 0, W, H - BAR);
  g_bar->resize(0, H - BAR, W, BAR);
  int y = H - BAR + (BAR - 32) / 2;
  int x = 10;
  g_photo_btn->resize(x, y, 70, 32); x += 72;
  g_video_btn->resize(x, y, 70, 32); x += 82;
  g_timer_choice->resize(x, y, 86, 32);
  g_shutter->resize((W - 56) / 2, H - BAR + 4, 56, 56);
  int r = W - 10;
  g_menu_btn->resize(r - 36, y - 2, 36, 36); r -= 40;
  g_last_btn->resize(r - 48, H - BAR + 8, 48, 48); r -= 52;
  g_mirror_btn->resize(r - 36, y - 2, 36, 36); r -= 38;
  if (S.video) {
    g_sound_btn->resize(r - 36, y - 2, 36, 36);
    r -= 38;
  }
  r -= 6;
  // camera and resolution share what is left right of the shutter
  int left = (W + 56) / 2 + 12, avail = r - left;
  if (avail >= 200) {
    int mw = std::min(150, avail / 2);
    g_cam_choice->resize(left, y, avail - mw - 6, 32);
    g_mode_choice->resize(r - mw, y, mw, 32);
    g_cam_choice->show();
    g_mode_choice->show();
  } else if (avail >= 110) {
    g_cam_choice->hide();
    g_mode_choice->resize(left, y, avail, 32);
    g_mode_choice->show();
  } else {
    g_cam_choice->hide();
    g_mode_choice->hide();
  }
  g_win->redraw();
}

class WebcamWindow : public Fl_Double_Window {
public:
  using Fl_Double_Window::Fl_Double_Window;
  void resize(int X, int Y, int W, int H) override {
    Fl_Double_Window::resize(X, Y, W, H);
    if (g_bar) layout();
  }
};

static void toggle_fullscreen() {
  if (g_win->fullscreen_active()) g_win->fullscreen_off();
  else g_win->fullscreen();
}

// right-click on the picture
static void view_menu() {
  bool rec = g_cam && g_cam->recording();
  fld::Menu m;
  m.item(rec ? "Stop Recording" : S.video ? "Start Recording" : "Take Photo", [] { shutter(); },
         g_cam && g_cam->running() ? 0 : FL_MENU_INACTIVE, ' ');
  m.item("Open Last Capture", [] { fld::open_path(g_last_capture); }, g_last_capture.empty() ? FL_MENU_INACTIVE : 0)
      .divider();
  m.toggle("Mirror", S.mirror, [] {
    S.mirror = !S.mirror;
    g_view->osd().mirror = S.mirror;
    g_view->widget()->redraw();
    save_settings();
  });
  m.toggle("Full Screen", g_win->fullscreen_active(), [] { toggle_fullscreen(); }).divider();
  m.item("Refresh Cameras", [] { refresh_cameras(); });
  m.item("Open Photos Folder", [] { fld::open_path(photo_dir()); });
  m.item("Open Videos Folder", [] { fld::open_path(video_dir()); });
  m.popup();
}

static int view_event(int e) {
  if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
    view_menu();
    return 1;
  }
  if (e == FL_PUSH && Fl::event_clicks()) {
    // double click: full screen
    if (g_win->fullscreen_active()) g_win->fullscreen_off();
    else g_win->fullscreen();
    return 1;
  }
  return 0;
}

static int keys(int e) {
  if (e != FL_SHORTCUT) return 0;
  if (Fl::focus() && Fl::focus()->top_window() != g_win) return 0;
  switch (Fl::event_key()) {
    case ' ': case FL_Enter: shutter(); return 1;
    case FL_Escape:
      if (g_countdown > 0) { shutter(); return 1; }
      if (g_win->fullscreen_active()) { g_win->fullscreen_off(); return 1; }
      return 1; // never close on Escape
    case FL_F + 11: case 'f':
      if (g_win->fullscreen_active()) g_win->fullscreen_off();
      else g_win->fullscreen();
      return 1;
  }
  return 0;
}

static void make_view(int W, int H) {
  g_win->begin();
  g_view = fgst::FrameView::create(0, 0, W, H - BAR, g_force_sw);
  g_win->end();
  g_view->set_slot(&g_cam->frames());
  g_view->osd().mirror = S.mirror;
  g_view->on_event = view_event;
  g_view->on_resize = [](int w, int h) {
    if (g_view && g_view->software()) fgst::set_scale_to(g_cam->video_sink(), w, h);
  };
  g_view->on_failed = [] {
    fgst::FrameView *old = g_view;
    std::string ph = old->osd().placeholder;
    g_win->remove(old->widget());
    Fl::delete_widget(old->widget());
    g_force_sw = true;
    make_view(g_win->w(), g_win->h());
    g_view->osd().placeholder = ph;
    g_win->insert(*g_view->widget(), 0);
    g_view->widget()->show();
    g_view->start();
    if (g_cam->running()) fgst::set_scale_to(g_cam->video_sink(), g_view->widget()->w(), g_view->widget()->h());
    layout();
  };
}

int main(int argc, char **argv) {
  fld::init_app("fl-webcam", argc, argv);
  Fl::lock(); // frames arrive through Fl::awake
  fl_register_images();
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-webcam [--software] [--test]\n"
             "  --software  draw the preview without OpenGL (also FLD_VIDEO_SW=1)\n"
             "  --test      use a test pattern instead of a camera (FLD_WEBCAM_SOURCE=test)\n");
      return 0;
    }
    if (a == "--software") g_force_sw = true;
    if (a == "--test") setenv("FLD_WEBCAM_SOURCE", "test", 1);
  }
  const char *sw = getenv("FLD_VIDEO_SW");
  if (sw && *sw && strcmp(sw, "0")) g_force_sw = true;
  std::string err;
  if (!fgst::init(&err)) {
    fl_alert("GStreamer could not be started:\n%s", err.c_str());
    return 1;
  }
  load_settings();
  g_cam = new Camera();

  const fld::Palette &pal = fld::palette();
  const int W = 960, H = 640;
  g_win = new WebcamWindow(W, H, "Webcam");
  g_win->color(FL_BLACK);
  make_view(W, H);
  g_win->begin();
  g_bar = new Fl_Group(0, H - BAR, W, BAR);
  g_bar->box(FL_FLAT_BOX);
  Fl_Color barbg = pal.dark ? pal.bg : fl_rgb_color(0x23, 0x25, 0x2a);
  g_bar->color(barbg);
  auto mode_button = [&](const char *label) {
    Fl_Toggle_Button *b = new Fl_Toggle_Button(0, 0, 70, 32, label);
    b->box(FL_FLAT_BOX);
    b->down_box(FL_FLAT_BOX);
    b->color(barbg);
    b->selection_color(pal.accent);
    b->labelcolor(FL_WHITE);
    b->clear_visible_focus();
    return b;
  };
  g_photo_btn = mode_button("Photo");
  g_photo_btn->callback([](Fl_Widget *, void *) { S.video = false; save_settings(); update_controls(); });
  g_video_btn = mode_button("Video");
  g_video_btn->callback([](Fl_Widget *, void *) { S.video = true; save_settings(); update_controls(); });
  g_timer_choice = new Fl_Choice(0, 0, 86, 32);
  g_timer_choice->add("No timer");
  g_timer_choice->add("3 s");
  g_timer_choice->add("10 s");
  g_timer_choice->value(S.timer >= 10 ? 2 : S.timer >= 3 ? 1 : 0);
  g_timer_choice->tooltip("Self-timer");
  g_timer_choice->clear_visible_focus();
  g_timer_choice->callback([](Fl_Widget *w, void *) {
    int v = ((Fl_Choice *)w)->value();
    S.timer = v == 2 ? 10 : v == 1 ? 3 : 0;
    save_settings();
  });
  g_shutter = new Shutter(0, 0, 56, 56);
  g_shutter->box(FL_NO_BOX);
  g_shutter->color(barbg);
  g_shutter->clear_visible_focus();
  g_shutter->callback([](Fl_Widget *, void *) { shutter(); });
  g_cam_choice = new Fl_Choice(0, 0, 160, 32);
  g_cam_choice->tooltip("Camera");
  g_cam_choice->clear_visible_focus();
  g_cam_choice->callback([](Fl_Widget *w, void *) {
    g_cam_index = (size_t)std::max(0, ((Fl_Choice *)w)->value());
    if (g_cam_index < g_cam->cameras().size()) S.camera = g_cam->cameras()[g_cam_index].id;
    save_settings();
    fill_modes();
    start_camera();
  });
  g_mode_choice = new Fl_Choice(0, 0, 140, 32);
  g_mode_choice->tooltip("Resolution");
  g_mode_choice->clear_visible_focus();
  g_mode_choice->callback([](Fl_Widget *w, void *) {
    g_mode_index = (size_t)std::max(0, ((Fl_Choice *)w)->value());
    auto &cams = g_cam->cameras();
    if (g_cam_index < cams.size() && g_mode_index < cams[g_cam_index].modes.size())
      S.mode = cam::mode_label(cams[g_cam_index].modes[g_mode_index]);
    save_settings();
    start_camera();
  });
  auto flat = [&](const char *icon, const char *fallback, const char *tip) {
    fld::FlatButton *b = new fld::FlatButton(0, 0, 36, 36);
    b->color(barbg);
    b->labelcolor(FL_WHITE);
    if (fld::icon(icon, 20)) b->icon_name(icon, 20);
    else b->label(fallback);
    b->tooltip(tip);
    b->clear_visible_focus();
    return b;
  };
  g_sound_btn = flat("audio-input-microphone", "Mic", "Record sound");
  g_sound_btn->callback([](Fl_Widget *, void *) { S.sound = !S.sound; save_settings(); update_controls(); });
  g_mirror_btn = flat("object-flip-horizontal", "Mirror", "Mirror the preview (photos and videos are not mirrored)");
  g_mirror_btn->callback([](Fl_Widget *, void *) {
    S.mirror = !S.mirror;
    g_view->osd().mirror = S.mirror;
    g_view->redraw();
    save_settings();
  });
  g_last_btn = flat("image-x-generic", "Last", "The last photo or video");
  g_last_btn->deactivate();
  g_last_btn->callback([](Fl_Widget *, void *) {
    if (!g_last_capture.empty()) fld::open_path(g_last_capture);
  });
  g_menu_btn = flat("open-menu", "☰", "Menu");
  if (!fld::icon("open-menu", 20) && fld::icon("preferences-system", 20)) g_menu_btn->icon_name("preferences-system", 20);
  g_menu_btn->callback(menu_cb);
  g_bar->end();
  g_win->end();
  g_win->resizable(g_win);
  g_win->size_range(520, 360);
  fld::set_window_icon(g_win, "fl-webcam");
  g_win->callback([](Fl_Widget *w, void *) {
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
    if (g_cam->recording()) {
      // finish the file before quitting
      g_cam->on_recorded = [](const std::string &, const std::string &) { g_win->hide(); };
      g_cam->stop_recording();
      return;
    }
    w->hide();
  });
  Fl::add_handler(keys);

  g_cam->frames().on_frame = [] { g_view->redraw(); };
  g_cam->on_error = [](const std::string &msg) {
    g_cam->stop();
    g_view->osd().placeholder = "Camera error: " + msg;
    g_view->redraw();
    update_controls();
  };
  g_cam->on_recorded = [](const std::string &path, const std::string &err) {
    Fl::remove_timeout(record_tick);
    if (err.empty()) {
      set_last_capture(path);
      g_view->show_message("Video saved: " + fld::basename(path), 3);
    } else {
      g_view->show_message("Recording stopped: " + err, 5);
      if (fld::file_exists(path)) set_last_capture(path);
    }
    update_controls();
  };

  layout();
  g_win->show();
  g_view->start();
  refresh_cameras();
  int r = Fl::run();
  g_cam->stop();
  delete g_cam;
  return r;
}
