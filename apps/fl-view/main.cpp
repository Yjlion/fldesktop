// fl-view: image viewer for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Anim_GIF_Image.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <strings.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/trash.h"
#include "fld/util.h"
#include "fld/widgets.h"

static const char *kImageExts[] = {"png", "jpg", "jpeg", "gif", "bmp", "svg", "svgz", "xpm",
                                   "xbm", "pnm", "pbm", "pgm", "ppm", "ico", "webp"};

static bool is_image(const std::string &path) {
  std::string e = fld::extension(path);
  for (auto x : kImageExts) if (e == x) return true;
  return fld::starts_with(fld::mime_from_name(path), "image/");
}

class Viewer;
static Viewer *g_view;
static Fl_Double_Window *g_win;
static fld::Toolbar *g_bar;
static Fl_Box *g_status;

class Viewer : public Fl_Widget {
public:
  std::vector<std::string> files;
  int index = -1;
  std::unique_ptr<Fl_Image> orig;   // as loaded
  std::unique_ptr<Fl_Image> shown;  // rotated/flipped copy (or null = orig)
  std::unique_ptr<Fl_Image> scaled; // cache for the current zoom
  Fl_Anim_GIF_Image *anim = nullptr; // when the file is an animated GIF
  double zoom = 1.0;
  bool fit = true;
  int rotation = 0; // 0, 90, 180, 270
  bool flip_h = false;
  double pan_x = 0, pan_y = 0; // offset of the image centre from the view centre
  int drag_x = 0, drag_y = 0;
  bool slideshow = false;
  std::string error;

  Viewer(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { box(FL_FLAT_BOX); }

  Fl_Image *current() const { return shown ? shown.get() : orig.get(); }

  void set_files_from(const std::string &path) {
    files.clear();
    std::string dir = fld::is_dir(path) ? path : fld::dirname(fld::abs_path(path));
    for (auto &n : fld::list_dir(dir)) {
      std::string p = fld::path_join(dir, n);
      if (!fld::is_dir(p) && is_image(p)) files.push_back(p);
    }
    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
      return strcasecmp(fld::basename(a).c_str(), fld::basename(b).c_str()) < 0;
    });
    index = 0;
    std::string ap = fld::abs_path(path);
    for (size_t i = 0; i < files.size(); i++) if (files[i] == ap) index = (int)i;
    if (!fld::is_dir(path) && (files.empty() || files[index] != ap)) {
      files.insert(files.begin(), ap); // file without an image extension
      index = 0;
    }
    load();
  }

  void load() {
    if (anim) { anim->stop(); }
    anim = nullptr;
    orig.reset();
    shown.reset();
    scaled.reset();
    error.clear();
    rotation = 0;
    flip_h = false;
    fit = true;
    pan_x = pan_y = 0;
    if (index < 0 || index >= (int)files.size()) { update_title(); redraw(); return; }
    const std::string &f = files[index];
    if (fld::extension(f) == "gif") {
      auto *g = new Fl_Anim_GIF_Image(f.c_str(), this, Fl_Anim_GIF_Image::DONT_RESIZE_CANVAS |
                                                          Fl_Anim_GIF_Image::DONT_SET_AS_IMAGE);
      if (g->ld() == 0 && g->w() > 0 && g->frames() > 1) {
        anim = g;
        orig.reset(g);
        anim->start();
      } else {
        delete g;
      }
    }
    if (!orig) orig.reset(fld::load_image(f));
    if (!orig) error = "Cannot open " + fld::basename(f);
    update_title();
    redraw();
  }

  void update_title() {
    if (index < 0 || index >= (int)files.size()) {
      g_win->label("Image Viewer");
      g_status->label("");
      return;
    }
    std::string name = fld::basename(files[index]);
    g_win->copy_label((name + " – Image Viewer").c_str());
    char buf[256];
    struct stat st{};
    stat(files[index].c_str(), &st);
    Fl_Image *img = current();
    if (img)
      snprintf(buf, sizeof buf, "%d / %d    %d × %d    %s    %.0f%%", index + 1, (int)files.size(),
               orig->data_w(), orig->data_h(), fld::human_size(st.st_size).c_str(), effective_zoom() * 100);
    else
      snprintf(buf, sizeof buf, "%d / %d", index + 1, (int)files.size());
    g_status->copy_label(buf);
  }

  double fit_zoom() const {
    Fl_Image *img = current();
    if (!img) return 1;
    double iw = img->data_w(), ih = img->data_h();
    double z = std::min((w() - 16) / iw, (h() - 16) / ih);
    return std::min(1.0, z); // don't enlarge small images when fitting
  }
  double effective_zoom() const { return fit ? fit_zoom() : zoom; }

  void set_zoom(double z, int cx = -1, int cy = -1) {
    z = std::max(0.02, std::min(z, 32.0));
    double old = effective_zoom();
    if (cx >= 0) {
      // keep the point under the cursor fixed
      double dx = cx - (x() + w() / 2.0) - pan_x, dy = cy - (y() + h() / 2.0) - pan_y;
      pan_x -= dx * (z / old - 1);
      pan_y -= dy * (z / old - 1);
    }
    zoom = z;
    fit = false;
    scaled.reset();
    clamp_pan();
    update_title();
    redraw();
  }

  void clamp_pan() {
    Fl_Image *img = current();
    if (!img) return;
    double z = effective_zoom();
    double iw = img->data_w() * z, ih = img->data_h() * z;
    double mx = std::max(0.0, (iw - w()) / 2), my = std::max(0.0, (ih - h()) / 2);
    pan_x = std::max(-mx, std::min(mx, pan_x));
    pan_y = std::max(-my, std::min(my, pan_y));
  }

  void transform(int rot_delta, bool flip) {
    if (!orig || anim) return;
    rotation = (rotation + rot_delta + 360) % 360;
    if (flip) flip_h = !flip_h;
    shown.reset(make_transformed(orig.get(), rotation, flip_h));
    scaled.reset();
    update_title();
    redraw();
  }

  // Rotates (multiples of 90°) and mirrors an image into a new RGB image.
  static Fl_Image *make_transformed(Fl_Image *src_img, int rot, bool flip) {
    if (rot == 0 && !flip) return nullptr;
    // rasterize at full resolution (SVGs etc. are converted by copy())
    std::unique_ptr<Fl_Image> base(src_img->copy(src_img->data_w(), src_img->data_h()));
    Fl_RGB_Image *rgb = dynamic_cast<Fl_RGB_Image *>(base.get());
    if (!rgb || rgb->count() < 1 || rgb->d() < 1) return nullptr;
    int W = rgb->data_w(), H = rgb->data_h(), D = rgb->d();
    int ld = rgb->ld() ? rgb->ld() : W * D;
    const unsigned char *src = (const unsigned char *)rgb->data()[0];
    int NW = (rot == 90 || rot == 270) ? H : W, NH = (rot == 90 || rot == 270) ? W : H;
    unsigned char *dst = new unsigned char[(size_t)NW * NH * D];
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        int sx = flip ? W - 1 - x : x;
        int nx, ny;
        switch (rot) {
          case 90: nx = H - 1 - y; ny = x; break;
          case 180: nx = W - 1 - x; ny = H - 1 - y; break;
          case 270: nx = y; ny = W - 1 - x; break;
          default: nx = x; ny = y; break;
        }
        memcpy(dst + ((size_t)ny * NW + nx) * D, src + (size_t)y * ld + sx * D, D);
      }
    }
    Fl_RGB_Image *out = new Fl_RGB_Image(dst, NW, NH, D);
    out->alloc_array = 1;
    return out;
  }

  void draw() override {
    const fld::Palette &p = fld::palette();
    Fl_Color bg = p.dark ? fl_rgb_color(0x18, 0x19, 0x1c) : fl_rgb_color(0x2a, 0x2b, 0x2f);
    fl_color(bg);
    fl_rectf(x(), y(), w(), h());
    Fl_Image *img = current();
    if (!img) {
      fl_font(FL_HELVETICA, 16);
      fl_color(fl_rgb_color(0xbb, 0xbb, 0xbb));
      const char *msg = !error.empty() ? error.c_str() : "Open an image with Ctrl+O or drop one here";
      fl_draw(fld::escape_at(msg).c_str(), x(), y(), w(), h(), FL_ALIGN_CENTER);
      return;
    }
    double z = effective_zoom();
    int dw = std::max(1, (int)std::lround(img->data_w() * z));
    int dh = std::max(1, (int)std::lround(img->data_h() * z));
    int dx = x() + (int)std::lround((w() - dw) / 2.0 + pan_x);
    int dy = y() + (int)std::lround((h() - dh) / 2.0 + pan_y);
    fl_push_clip(x(), y(), w(), h());
    // checkerboard behind transparent images
    if (img->d() == 4 || img->d() == 2) {
      int cs = 10;
      int x0 = std::max(dx, x()), y0 = std::max(dy, y());
      int x1 = std::min(dx + dw, x() + w()), y1 = std::min(dy + dh, y() + h());
      for (int yy = y0; yy < y1; yy += cs)
        for (int xx = x0; xx < x1; xx += cs) {
          fl_color(((xx - dx) / cs + (yy - dy) / cs) % 2 ? fl_rgb_color(0xcc, 0xcc, 0xcc) : fl_rgb_color(0xff, 0xff, 0xff));
          fl_rectf(xx, yy, std::min(cs, x1 - xx), std::min(cs, y1 - yy));
        }
    }
    if (anim) {
      // animated GIFs are drawn by FLTK scaled to the requested size
      anim->scale(dw, dh, 0, 1);
      ((Fl_Image *)anim)->draw(dx, dy);
    } else {
      // FLTK 1.4 draws scaled images directly (cairo does the filtering)
      img->scale(dw, dh, 0, 1);
      img->draw(dx, dy);
    }
    fl_pop_clip();
  }

  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    clamp_pan();
    update_title();
  }

  void go(int delta) {
    if (files.empty()) return;
    index = (index + delta + (int)files.size()) % (int)files.size();
    load();
  }

  int handle(int e) override {
    switch (e) {
      case FL_FOCUS: case FL_UNFOCUS: return 1;
      case FL_PUSH:
        take_focus();
        if (Fl::event_button() == FL_RIGHT_MOUSE) { context_menu(); return 1; }
        if (Fl::event_clicks()) { toggle_fullscreen(); Fl::event_clicks(0); return 1; }
        drag_x = Fl::event_x();
        drag_y = Fl::event_y();
        fl_cursor(FL_CURSOR_MOVE);
        return 1;
      case FL_DRAG:
        pan_x += Fl::event_x() - drag_x;
        pan_y += Fl::event_y() - drag_y;
        drag_x = Fl::event_x();
        drag_y = Fl::event_y();
        clamp_pan();
        redraw();
        return 1;
      case FL_RELEASE:
        fl_cursor(FL_CURSOR_DEFAULT);
        return 1;
      case FL_MOUSEWHEEL:
        {
          double f = Fl::event_dy() < 0 ? 1.15 : 1 / 1.15;
          set_zoom(effective_zoom() * f, Fl::event_x(), Fl::event_y());
        }
        return 1;
      case FL_DND_ENTER: case FL_DND_DRAG: case FL_DND_RELEASE: case FL_DND_LEAVE:
        return 1;
      case FL_PASTE: {
        std::string t(Fl::event_text(), Fl::event_length());
        std::string first = t.substr(0, t.find_first_of("\r\n"));
        if (fld::starts_with(first, "file://")) first = fld::uri_decode(first.substr(7));
        if (!first.empty() && fld::file_exists(first)) set_files_from(first);
        return 1;
      }
    }
    return Fl_Widget::handle(e);
  }

  void toggle_fullscreen() {
    if (g_win->fullscreen_active()) {
      g_win->fullscreen_off();
      g_bar->show();
      g_status->show();
      resize(0, g_bar->h(), g_win->w(), g_win->h() - g_bar->h() - g_status->h());
    } else {
      g_win->fullscreen();
      g_bar->hide();
      g_status->hide();
      resize(0, 0, g_win->w(), g_win->h());
    }
    redraw();
  }

  void context_menu();
};

// ---- actions ----------------------------------------------------------------------

static void open_dialog() {
  std::string dir = g_view->index >= 0 && !g_view->files.empty() ? fld::dirname(g_view->files[g_view->index])
                                                                   : fld::user_dir("PICTURES");
  Fl_File_Chooser fc(dir.c_str(), "Images (*.{png,jpg,jpeg,gif,bmp,svg,xpm,pnm,ppm,pgm,pbm,webp})",
                     Fl_File_Chooser::SINGLE, "Open Image");
  fc.preview(1);
  fc.show();
  while (fc.shown()) Fl::wait();
  if (fc.value()) g_view->set_files_from(fc.value());
}

static void delete_current() {
  if (g_view->index < 0 || g_view->files.empty()) return;
  std::string f = g_view->files[g_view->index];
  if (fl_choice("Move \"%s\" to the trash?", "Cancel", "Move to Trash", nullptr, fld::basename(f).c_str()) != 1) return;
  std::string err;
  if (!fld::trash(f, &err)) { fl_alert("%s", err.c_str()); return; }
  g_view->files.erase(g_view->files.begin() + g_view->index);
  if (g_view->index >= (int)g_view->files.size()) g_view->index = (int)g_view->files.size() - 1;
  g_view->load();
}

static void slideshow_tick(void *) {
  if (!g_view->slideshow) return;
  g_view->go(1);
  Fl::repeat_timeout(4.0, slideshow_tick);
}

static void toggle_slideshow() {
  g_view->slideshow = !g_view->slideshow;
  if (g_view->slideshow) Fl::add_timeout(4.0, slideshow_tick);
  else Fl::remove_timeout(slideshow_tick);
}

static void set_wallpaper() {
  if (g_view->index < 0) return;
  fld::Ini cfg;
  cfg.load(fld::desktop_config_path());
  cfg.set("Desktop", "Wallpaper", g_view->files[g_view->index]);
  cfg.save(fld::desktop_config_path());
}

static void action(long id) {
  switch (id) {
    case 1: open_dialog(); break;
    case 2: g_view->go(-1); break;
    case 3: g_view->go(1); break;
    case 4: g_view->set_zoom(g_view->effective_zoom() * 1.25); break;
    case 5: g_view->set_zoom(g_view->effective_zoom() / 1.25); break;
    case 6: g_view->set_zoom(1.0); g_view->pan_x = g_view->pan_y = 0; break;
    case 7: g_view->fit = true; g_view->pan_x = g_view->pan_y = 0; g_view->update_title(); g_view->redraw(); break;
    case 8: g_view->transform(-90, false); break;
    case 9: g_view->transform(90, false); break;
    case 10: g_view->transform(0, true); break;
    case 11: g_view->toggle_fullscreen(); break;
    case 12: toggle_slideshow(); break;
    case 13: delete_current(); break;
    case 14: set_wallpaper(); break;
    case 15:
      if (g_view->index >= 0) fld::spawn({"fl-files", fld::dirname(g_view->files[g_view->index])});
      break;
    case 16: fld::about_dialog("Image Viewer", "fl-view", "A simple image viewer built with FLTK."); break;
    case 17: g_win->hide(); break;
    case 18: if (!g_view->files.empty()) { g_view->index = 0; g_view->load(); } break;
    case 19: if (!g_view->files.empty()) { g_view->index = (int)g_view->files.size() - 1; g_view->load(); } break;
  }
}

static void action_cb(Fl_Widget *, void *v) { action((long)v); }

void Viewer::context_menu() {
  Fl_Menu_Item m[] = {
      {"Open…", FL_CTRL + 'o', nullptr, (void *)1, FL_MENU_DIVIDER},
      {"Previous", FL_Left, nullptr, (void *)2},
      {"Next", FL_Right, nullptr, (void *)3, FL_MENU_DIVIDER},
      {"Zoom In", '+', nullptr, (void *)4},
      {"Zoom Out", '-', nullptr, (void *)5},
      {"Original Size", '1', nullptr, (void *)6},
      {"Best Fit", 'f', nullptr, (void *)7, FL_MENU_DIVIDER},
      {"Rotate Left", 'l', nullptr, (void *)8},
      {"Rotate Right", 'r', nullptr, (void *)9},
      {"Flip Horizontally", 'h', nullptr, (void *)10, FL_MENU_DIVIDER},
      {"Fullscreen", FL_F + 11, nullptr, (void *)11},
      {"Slideshow", FL_F + 5, nullptr, (void *)12, FL_MENU_TOGGLE | (slideshow ? FL_MENU_VALUE : 0)},
      {"Set as Wallpaper", 0, nullptr, (void *)14},
      {"Show in Folder", 0, nullptr, (void *)15, FL_MENU_DIVIDER},
      {"Move to Trash", FL_Delete, nullptr, (void *)13},
      {nullptr}};
  const Fl_Menu_Item *r = m->popup(Fl::event_x(), Fl::event_y());
  if (r) action((long)r->user_data());
}

static int key_handler(int e) {
  if (e != FL_SHORTCUT) return 0;
  int k = Fl::event_key();
  bool ctrl = Fl::event_state() & FL_CTRL;
  const char *t = Fl::event_text();
  if (ctrl && k == 'o') { action(1); return 1; }
  if (ctrl && k == 'q') { action(17); return 1; }
  if (ctrl && k == 'w') { action(17); return 1; }
  if (ctrl) return 0;
  switch (k) {
    case FL_Left: case FL_Page_Up: case FL_BackSpace: action(2); return 1;
    case FL_Right: case FL_Page_Down: case ' ': action(3); return 1;
    case FL_Home: action(18); return 1;
    case FL_End: action(19); return 1;
    case FL_F + 11: action(11); return 1;
    case FL_F + 5: action(12); return 1;
    case FL_Delete: action(13); return 1;
    case FL_Escape:
      if (g_win->fullscreen_active()) action(11);
      else if (g_view->slideshow) toggle_slideshow();
      else g_win->hide();
      return 1;
  }
  if (t && *t) {
    switch (t[0]) {
      case '+': case '=': action(4); return 1;
      case '-': action(5); return 1;
      case '1': action(6); return 1;
      case 'f': action(7); return 1;
      case 'l': action(8); return 1;
      case 'r': action(9); return 1;
      case 'h': action(10); return 1;
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  fld::init_app("fl-view", argc, argv);
  fl_register_images();
  std::string start;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") { printf("Usage: fl-view [image|folder]\n"); return 0; }
    if (start.empty() && a[0] != '-') start = fld::starts_with(a, "file://") ? fld::uri_decode(a.substr(7)) : a;
  }
  const int TB = 40, ST = 24;
  g_win = new Fl_Double_Window(900, 640, "Image Viewer");
  g_bar = new fld::Toolbar(0, 0, 900, TB);
  g_bar->color(fld::palette().bg);
  g_bar->add_button("document-open", "Open (Ctrl+O)", action_cb, (void *)1);
  g_bar->add_separator();
  g_bar->add_button("go-previous", "Previous (←)", action_cb, (void *)2);
  g_bar->add_button("go-next", "Next (→)", action_cb, (void *)3);
  g_bar->add_separator();
  g_bar->add_button("zoom-in", "Zoom in (+)", action_cb, (void *)4);
  g_bar->add_button("zoom-out", "Zoom out (−)", action_cb, (void *)5);
  g_bar->add_button("zoom-original", "Original size (1)", action_cb, (void *)6);
  g_bar->add_button("zoom-fit-best", "Best fit (F)", action_cb, (void *)7);
  g_bar->add_separator();
  g_bar->add_button("object-rotate-left", "Rotate left (L)", action_cb, (void *)8);
  g_bar->add_button("object-rotate-right", "Rotate right (R)", action_cb, (void *)9);
  g_bar->add_button("object-flip-horizontal", "Flip (H)", action_cb, (void *)10);
  g_bar->add_separator();
  g_bar->add_button("media-playback-start", "Slideshow (F5)", action_cb, (void *)12);
  g_bar->add_button("view-fullscreen", "Fullscreen (F11)", action_cb, (void *)11);
  g_bar->add_button("user-trash", "Move to trash (Del)", action_cb, (void *)13);
  g_view = new Viewer(0, TB, 900, 640 - TB - ST);
  g_status = new Fl_Box(0, 640 - ST, 900, ST);
  g_status->box(FL_FLAT_BOX);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  g_status->labelsize(12);
  g_status->labelcolor(fld::palette().fg_dim);
  g_win->resizable(g_view);
  g_win->end();
  g_win->size_range(400, 300);
  fld::set_window_icon(g_win, "fl-view");
  Fl::add_handler(key_handler);
  if (!start.empty()) g_view->set_files_from(start);
  else g_view->update_title();
  g_win->show(argc, argv);
  g_view->take_focus();
  return Fl::run();
}
