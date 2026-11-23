// fl-view: image viewer for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Anim_GIF_Image.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Spinner.H>
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
#include <map>
#include <vector>
#ifdef FLD_HAVE_JPEG
#include <cstdio>
#include <jpeglib.h>
#endif

#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/trash.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "view_util.h"

static const char *kImageExts[] = {"png", "jpg", "jpeg", "gif", "bmp", "svg", "svgz", "xpm",
                                   "xbm", "pnm", "pbm", "pgm", "ppm", "ico", "webp"};

static bool is_image(const std::string &path) {
  std::string e = fld::extension(path);
  for (auto x : kImageExts) if (e == x) return true;
  return fld::starts_with(fld::mime_from_name(path), "image/");
}

class Viewer;
class Strip;
static Viewer *g_view;
static Strip *g_strip;
static bool g_show_strip = true;
static std::string settings_path() { return fld::fld_config_dir() + "/view.ini"; }
static void relayout();

// ---- pixels to and from FLTK images ----------------------------------------------

static view::Pixels to_pixels(Fl_Image *img) {
  view::Pixels p;
  std::unique_ptr<Fl_Image> c(img->copy(img->data_w(), img->data_h()));
  Fl_RGB_Image *rgb = dynamic_cast<Fl_RGB_Image *>(c.get());
  if (!rgb || rgb->count() < 1 || rgb->d() < 1) return p;
  p.w = rgb->data_w();
  p.h = rgb->data_h();
  p.d = rgb->d();
  int ld = rgb->ld() ? rgb->ld() : p.w * p.d;
  p.data.resize((size_t)p.w * p.h * p.d);
  const unsigned char *src = (const unsigned char *)rgb->data()[0];
  for (int y = 0; y < p.h; y++) memcpy(p.data.data() + (size_t)y * p.w * p.d, src + (size_t)y * ld, (size_t)p.w * p.d);
  return p;
}

static Fl_RGB_Image *from_pixels(const view::Pixels &p) {
  if (p.w <= 0 || p.h <= 0) return nullptr;
  unsigned char *buf = new unsigned char[p.data.size()];
  memcpy(buf, p.data.data(), p.data.size());
  Fl_RGB_Image *img = new Fl_RGB_Image(buf, p.w, p.h, p.d);
  img->alloc_array = 1;
  return img;
}

#ifdef FLD_HAVE_JPEG
static bool write_jpeg(const std::string &path, const view::Pixels &p, int quality) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  jpeg_compress_struct c;
  jpeg_error_mgr err;
  c.err = jpeg_std_error(&err);
  jpeg_create_compress(&c);
  jpeg_stdio_dest(&c, f);
  c.image_width = (JDIMENSION)p.w;
  c.image_height = (JDIMENSION)p.h;
  bool gray = p.d <= 2;
  c.input_components = gray ? 1 : 3;
  c.in_color_space = gray ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_set_defaults(&c);
  jpeg_set_quality(&c, quality, TRUE);
  jpeg_start_compress(&c, TRUE);
  std::vector<unsigned char> row((size_t)p.w * c.input_components);
  while (c.next_scanline < c.image_height) {
    const unsigned char *s = p.data.data() + (size_t)c.next_scanline * p.w * p.d;
    // JPEG has no transparency: the colour channels only
    for (int x = 0; x < p.w; x++)
      for (int k = 0; k < c.input_components; k++) row[(size_t)x * c.input_components + k] = s[x * p.d + k];
    JSAMPROW r = row.data();
    jpeg_write_scanlines(&c, &r, 1);
  }
  jpeg_finish_compress(&c);
  jpeg_destroy_compress(&c);
  return fclose(f) == 0;
}
#endif
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
  bool modified = false;        // cropped, resized, turned: not saved yet
  bool cropping = false;        // C: a rectangle is being chosen
  bool crop_sel = false;
  int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;

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
    modified = false;
    cropping = crop_sel = false;
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
    // photos taken sideways: upright as the camera recorded it
    if (orig && !anim) {
      std::string e = fld::extension(f);
      if (e == "jpg" || e == "jpeg" || e == "tif" || e == "tiff") {
        int rot;
        bool fl;
        view::orientation_transform(view::read_exif(f).orientation, rot, fl);
        if (rot || fl) shown.reset(make_transformed(orig.get(), rot, fl));
      }
    }
    update_title();
    redraw();
    strip_changed();
  }
  void strip_changed();

  void update_title() {
    if (index < 0 || index >= (int)files.size()) {
      g_win->label("Image Viewer");
      g_status->label("");
      return;
    }
    std::string name = fld::basename(files[index]);
    g_win->copy_label(((modified ? "*" : "") + name + " – Image Viewer").c_str());
    char buf[256];
    struct stat st{};
    stat(files[index].c_str(), &st);
    Fl_Image *img = current();
    if (cropping) {
      int rx, ry, rw, rh;
      crop_rect(rx, ry, rw, rh);
      if (crop_sel && rw > 0)
        snprintf(buf, sizeof buf, "Crop to %d × %d: Enter crops, Escape cancels", rw, rh);
      else
        snprintf(buf, sizeof buf, "Drag over the part to keep; Escape cancels");
    } else if (img)
      snprintf(buf, sizeof buf, "%d / %d    %d × %d    %s    %.0f%%%s", index + 1, (int)files.size(),
               img->data_w(), img->data_h(), fld::human_size(st.st_size).c_str(), effective_zoom() * 100,
               modified ? "    changed (Ctrl+S saves)" : "");
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
    Fl_Image *n = make_transformed(current(), (rot_delta + 360) % 360, flip);
    if (n) shown.reset(n);
    modified = true;
    scaled.reset();
    clamp_pan();
    update_title();
    redraw();
  }

  // a new picture from the current one (crop, resize)
  void replace(const view::Pixels &p) {
    Fl_RGB_Image *n = from_pixels(p);
    if (!n) return;
    shown.reset(n);
    modified = true;
    fit = true;
    pan_x = pan_y = 0;
    scaled.reset();
    update_title();
    redraw();
  }

  // where the image is drawn and at what size
  void origin(double &ox, double &oy, double &z) const {
    Fl_Image *img = current();
    z = effective_zoom();
    int dw = std::max(1, (int)std::lround(img->data_w() * z));
    int dh = std::max(1, (int)std::lround(img->data_h() * z));
    ox = x() + (int)std::lround((w() - dw) / 2.0 + pan_x);
    oy = y() + (int)std::lround((h() - dh) / 2.0 + pan_y);
  }

  void crop_rect(int &rx, int &ry, int &rw, int &rh) const {
    rx = ry = rw = rh = 0;
    Fl_Image *img = current();
    if (!img) return;
    double ox, oy, z;
    origin(ox, oy, z);
    view::view_rect_to_image(cx0, cy0, cx1, cy1, ox, oy, z, img->data_w(), img->data_h(), rx, ry, rw, rh);
  }

  void start_crop() {
    if (!current() || anim) return;
    cropping = true;
    crop_sel = false;
    fl_cursor(FL_CURSOR_CROSS);
    update_title();
    redraw();
  }
  void end_crop(bool apply) {
    if (!cropping) return;
    int rx, ry, rw, rh;
    crop_rect(rx, ry, rw, rh);
    cropping = false;
    fl_cursor(FL_CURSOR_DEFAULT);
    if (apply && crop_sel && rw > 1 && rh > 1) replace(view::crop(to_pixels(current()), rx, ry, rw, rh));
    crop_sel = false;
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
    if (cropping && crop_sel) {
      // the part that goes is shaded with a stipple of dark lines
      int l = std::min(cx0, cx1), t = std::min(cy0, cy1), r = std::max(cx0, cx1), b = std::max(cy0, cy1);
      l = std::max(l, dx);
      t = std::max(t, dy);
      r = std::min(r, dx + dw);
      b = std::min(b, dy + dh);
      fl_color(FL_BLACK);
      for (int yy = dy; yy < dy + dh; yy += 2)
        if (yy < t || yy >= b) fl_xyline(dx, yy, dx + dw - 1);
        else {
          if (l > dx) fl_xyline(dx, yy, l - 1);
          if (r < dx + dw) fl_xyline(r, yy, dx + dw - 1);
        }
      fl_color(FL_WHITE);
      fl_rect(l, t, std::max(1, r - l), std::max(1, b - t));
      fl_color(fld::palette().accent);
      fl_line_style(FL_DASH, 1);
      fl_rect(l, t, std::max(1, r - l), std::max(1, b - t));
      fl_line_style(0);
    }
    fl_pop_clip();
  }

  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    clamp_pan();
    update_title();
  }

  bool confirm_discard();
  void go(int delta) {
    if (files.empty() || !confirm_discard()) return;
    index = (index + delta + (int)files.size()) % (int)files.size();
    load();
  }

  int handle(int e) override {
    switch (e) {
      case FL_FOCUS: case FL_UNFOCUS: return 1;
      case FL_PUSH:
        take_focus();
        if (Fl::event_button() == FL_RIGHT_MOUSE) { context_menu(); return 1; }
        if (cropping) {
          cx0 = cx1 = Fl::event_x();
          cy0 = cy1 = Fl::event_y();
          crop_sel = true;
          update_title();
          redraw();
          return 1;
        }
        if (Fl::event_clicks()) { toggle_fullscreen(); Fl::event_clicks(0); return 1; }
        drag_x = Fl::event_x();
        drag_y = Fl::event_y();
        fl_cursor(FL_CURSOR_MOVE);
        return 1;
      case FL_DRAG:
        if (cropping) {
          cx1 = Fl::event_x();
          cy1 = Fl::event_y();
          update_title();
          redraw();
          return 1;
        }
        pan_x += Fl::event_x() - drag_x;
        pan_y += Fl::event_y() - drag_y;
        drag_x = Fl::event_x();
        drag_y = Fl::event_y();
        clamp_pan();
        redraw();
        return 1;
      case FL_RELEASE:
        fl_cursor(cropping ? FL_CURSOR_CROSS : FL_CURSOR_DEFAULT);
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
    if (g_win->fullscreen_active()) g_win->fullscreen_off();
    else g_win->fullscreen();
    relayout();
    redraw();
  }

  void context_menu();
};

// ---- actions ----------------------------------------------------------------------

// ---- the thumbnail strip ----------------------------------------------------------

// The pictures of the folder along the bottom; a click shows one
class Strip : public Fl_Widget {
public:
  static const int H = 76, T = 60; // height, thumbnail size
  std::map<std::string, std::unique_ptr<Fl_Image>> thumbs;
  std::map<std::string, bool> failed;
  int first = 0; // the first file shown
  Strip(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  int per_view() const { return std::max(1, (w() - 8) / (T + 8)); }
  void follow() {
    int n = per_view(), i = g_view->index;
    if (i < first || i >= first + n) first = std::max(0, i - n / 2);
    first = std::max(0, std::min(first, (int)g_view->files.size() - n));
    if (!Fl::has_idle(idle, this)) Fl::add_idle(idle, this);
    redraw();
  }
  // one thumbnail per idle call
  static void idle(void *d) {
    Strip *s = (Strip *)d;
    int n = s->per_view();
    for (int i = s->first; i < std::min((int)g_view->files.size(), s->first + n); i++) {
      const std::string &f = g_view->files[(size_t)i];
      if (s->thumbs.count(f) || s->failed.count(f)) continue;
      std::unique_ptr<Fl_Image> img(fld::load_image(f));
      if (!img || img->data_w() <= 0) {
        s->failed[f] = true;
        return;
      }
      // upright like the picture itself
      std::string e = fld::extension(f);
      if (e == "jpg" || e == "jpeg" || e == "tif" || e == "tiff") {
        int rot;
        bool fl;
        view::orientation_transform(view::read_exif(f).orientation, rot, fl);
        if (rot || fl)
          if (Fl_Image *t = Viewer::make_transformed(img.get(), rot, fl)) img.reset(t);
      }
      double z = std::min((double)T / img->data_w(), (double)T / img->data_h());
      int tw = std::max(1, (int)(img->data_w() * z)), th = std::max(1, (int)(img->data_h() * z));
      s->thumbs[f].reset(img->copy(tw, th));
      s->redraw();
      return;
    }
    Fl::remove_idle(idle, d);
  }
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_push_clip(x(), y(), w(), h());
    fl_color(p.dark ? fl_rgb_color(0x22, 0x23, 0x27) : fl_rgb_color(0x33, 0x34, 0x38));
    fl_rectf(x(), y(), w(), h());
    int n = per_view();
    for (int k = 0; k < n; k++) {
      int i = first + k;
      if (i >= (int)g_view->files.size()) break;
      int cx = x() + 8 + k * (T + 8), cy = y() + (H - T) / 2;
      if (i == g_view->index) {
        fl_color(p.accent);
        fl_rectf(cx - 3, cy - 3, T + 6, T + 6);
      }
      auto it = thumbs.find(g_view->files[(size_t)i]);
      if (it != thumbs.end()) {
        Fl_Image *im = it->second.get();
        im->draw(cx + (T - im->w()) / 2, cy + (T - im->h()) / 2);
      } else {
        fl_color(fl_rgb_color(0x44, 0x45, 0x4a));
        fl_rectf(cx, cy, T, T);
      }
    }
    fl_pop_clip();
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH: {
        int k = (Fl::event_x() - x() - 4) / (T + 8), i = first + k;
        if (i >= 0 && i < (int)g_view->files.size() && i != g_view->index && g_view->confirm_discard()) {
          g_view->index = i;
          g_view->modified = false; // already asked
          g_view->load();
        }
        return 1;
      }
      case FL_MOUSEWHEEL: {
        int d = Fl::event_dy() ? Fl::event_dy() : Fl::event_dx();
        first = std::max(0, std::min(first + d * 2, (int)g_view->files.size() - per_view()));
        if (!Fl::has_idle(idle, this)) Fl::add_idle(idle, this);
        redraw();
        return 1;
      }
    }
    return Fl_Widget::handle(e);
  }
  void resize(int X, int Y, int W, int Hh) override {
    Fl_Widget::resize(X, Y, W, Hh);
    if (!g_view->files.empty()) follow();
  }
};

void Viewer::strip_changed() {
  if (!g_strip) return;
  // forget thumbnails of other folders
  if (g_strip->thumbs.size() > 400) g_strip->thumbs.clear();
  g_strip->follow();
}

static void relayout() {
  const int TB = g_bar->h(), ST = g_status->h();
  bool full = g_win->fullscreen_active();
  int y = full ? 0 : TB, H = g_win->h(), W = g_win->w();
  if (full) { g_bar->hide(); g_status->hide(); }
  else { g_bar->show(); g_status->show(); }
  bool strip = g_show_strip && !full && g_view->files.size() > 1;
  int bottom = full ? H : H - ST;
  if (strip) {
    g_strip->resize(0, bottom - Strip::H, W, Strip::H);
    g_strip->show();
    bottom -= Strip::H;
  } else g_strip->hide();
  g_view->resize(0, y, W, bottom - y);
  g_win->redraw();
}

class ViewWindow : public Fl_Double_Window {
public:
  using Fl_Double_Window::Fl_Double_Window;
  void resize(int X, int Y, int W, int H) override {
    Fl_Double_Window::resize(X, Y, W, H);
    if (g_strip) relayout();
  }
};

// ---- saving ---------------------------------------------------------------------

static bool save_to(const std::string &path) {
  Fl_Image *img = g_view->current();
  if (!img) return false;
  view::Pixels p = to_pixels(img);
  std::string e = fld::extension(path);
  bool ok;
  if (e == "jpg" || e == "jpeg") {
#ifdef FLD_HAVE_JPEG
    ok = write_jpeg(path, p, 92);
#else
    fl_alert("This build of fl-view saves PNG only.");
    return false;
#endif
  } else if (e == "png") {
    ok = fl_write_png(path.c_str(), p.data.data(), p.w, p.h, p.d) == 0;
  } else {
    fl_alert("Pictures are saved as PNG or JPEG: give the name a .png or .jpg ending.");
    return false;
  }
  if (!ok) {
    fl_alert("Could not save %s", fld::escape_at(path).c_str());
    return false;
  }
  g_view->modified = false;
  // a new file in the folder joins the list
  std::string ap = fld::abs_path(path);
  if (std::find(g_view->files.begin(), g_view->files.end(), ap) == g_view->files.end() &&
      !g_view->files.empty() && fld::dirname(ap) == fld::dirname(g_view->files[0])) {
    g_view->files.push_back(ap);
    std::sort(g_view->files.begin(), g_view->files.end(), [](const std::string &a, const std::string &b) {
      return strcasecmp(fld::basename(a).c_str(), fld::basename(b).c_str()) < 0;
    });
  }
  if (g_strip) g_strip->thumbs.erase(ap);
  g_view->update_title();
  g_strip->follow();
  return true;
}

static bool save_as() {
  if (g_view->index < 0 || !g_view->current()) return false;
  std::string cur = g_view->files[(size_t)g_view->index];
  std::string e = fld::extension(cur);
  std::string def = fld::dirname(cur) + "/" + fld::basename(cur).substr(0, fld::basename(cur).rfind('.')) + " (edited)." +
                    (e == "jpg" || e == "jpeg" ? e : std::string("png"));
  const char *f = fl_file_chooser("Save Picture As", "Pictures (*.{png,jpg,jpeg})", def.c_str(), 0);
  if (!f) return false;
  if (fld::file_exists(f) && fl_choice("\"%s\" exists. Replace it?", "Cancel", "Replace", nullptr,
                                       fld::escape_at(fld::basename(f)).c_str()) != 1)
    return false;
  return save_to(f);
}

static bool save() {
  if (g_view->index < 0 || !g_view->current()) return false;
  std::string cur = g_view->files[(size_t)g_view->index];
  std::string e = fld::extension(cur);
  if (e != "png" && e != "jpg" && e != "jpeg") return save_as();
  int c = fl_choice("Replace \"%s\" with the changed picture?%s", "Cancel", "Replace", "Save As…",
                    fld::escape_at(fld::basename(cur)).c_str(),
                    e == "png" ? "" : "\nThe camera information (EXIF) is not kept.");
  if (c == 0) return false;
  if (c == 2) return save_as();
  return save_to(cur);
}

bool Viewer::confirm_discard() {
  if (!modified) return true;
  int c = fl_choice("Save the changes to \"%s\"?", "Cancel", "Save", "Discard",
                    index >= 0 ? fld::escape_at(fld::basename(files[(size_t)index])).c_str() : "");
  if (c == 0) return false;
  if (c == 1) return save_as();
  modified = false;
  return true;
}

// ---- resize and information --------------------------------------------------------

static void resize_dialog() {
  Fl_Image *img = g_view->current();
  if (!img || g_view->anim) return;
  int ow = img->data_w(), oh = img->data_h();
  Fl_Double_Window d(320, 190, "Resize Picture");
  Fl_Int_Input w(120, 16, 110, 28, "Width:");
  Fl_Int_Input h(120, 52, 110, 28, "Height:");
  w.value(std::to_string(ow).c_str());
  h.value(std::to_string(oh).c_str());
  Fl_Box px(236, 16, 60, 64, "pixels");
  px.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  Fl_Check_Button keep(120, 88, 190, 26, "Keep the proportions");
  keep.value(1);
  struct Ctx {
    Fl_Int_Input *w, *h;
    Fl_Check_Button *keep;
    int ow, oh;
  } c{&w, &h, &keep, ow, oh};
  // typing one side changes the other
  w.callback([](Fl_Widget *, void *v) {
    Ctx &c = *(Ctx *)v;
    int n = atoi(c.w->value());
    if (c.keep->value() && n > 0) c.h->value(std::to_string(view::keep_aspect(c.ow, c.oh, n, true)).c_str());
  }, &c);
  h.callback([](Fl_Widget *, void *v) {
    Ctx &c = *(Ctx *)v;
    int n = atoi(c.h->value());
    if (c.keep->value() && n > 0) c.w->value(std::to_string(view::keep_aspect(c.ow, c.oh, n, false)).c_str());
  }, &c);
  w.when(FL_WHEN_CHANGED);
  h.when(FL_WHEN_CHANGED);
  Fl_Button cancel(104, 146, 100, 30, "Cancel");
  Fl_Return_Button ok(210, 146, 100, 30, "Resize");
  int result = 0;
  cancel.callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  ok.callback([](Fl_Widget *b, void *r) { *(int *)r = 1; b->window()->hide(); }, &result);
  d.end();
  d.set_modal();
  d.show();
  while (d.shown()) Fl::wait();
  if (result != 1) return;
  int nw = atoi(w.value()), nh = atoi(h.value());
  if (nw <= 0 || nh <= 0 || (nw == ow && nh == oh)) return;
  if ((long long)nw * nh > 400LL * 1000 * 1000) {
    fl_alert("That is too large.");
    return;
  }
  g_view->replace(view::resize(to_pixels(img), nw, nh));
}

static void info_window() {
  if (g_view->index < 0 || g_view->files.empty()) return;
  const std::string &f = g_view->files[(size_t)g_view->index];
  std::vector<std::pair<std::string, std::string>> rows;
  rows.push_back({"Name", fld::basename(f)});
  rows.push_back({"Folder", fld::tilde(fld::dirname(f))});
  std::string mime = fld::mime_type(f);
  rows.push_back({"Type", fld::mime_description(mime)});
  struct stat st{};
  if (stat(f.c_str(), &st) == 0) {
    rows.push_back({"File size", fld::human_size(st.st_size)});
    rows.push_back({"Modified", fld::format_time(st.st_mtime, "%Y-%m-%d %H:%M")});
  }
  if (Fl_Image *img = g_view->orig.get())
    rows.push_back({"Dimensions", std::to_string(img->data_w()) + " × " + std::to_string(img->data_h()) + " pixels"});
  auto ex = view::exif_rows(view::read_exif(f));
  rows.insert(rows.end(), ex.begin(), ex.end());
  int H = 24 + (int)rows.size() * 26 + 56;
  Fl_Double_Window d(460, H, "Image Information");
  int y = 14;
  std::vector<std::unique_ptr<Fl_Box>> boxes;
  for (auto &r : rows) {
    boxes.emplace_back(new Fl_Box(12, y, 120, 24));
    boxes.back()->copy_label((r.first + ":").c_str());
    boxes.back()->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
    boxes.back()->labelcolor(fld::palette().fg_dim);
    boxes.emplace_back(new Fl_Box(140, y, 308, 24));
    boxes.back()->copy_label(fld::escape_at(r.second).c_str());
    boxes.back()->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    y += 26;
  }
  if (!ex.empty() && view::read_exif(f).has_gps) {
    // the place on a map in the browser
    auto e = view::read_exif(f);
    static std::string url;
    char b[160];
    snprintf(b, sizeof b, "https://www.openstreetmap.org/?mlat=%.6f&mlon=%.6f#map=15/%.6f/%.6f", e.lat, e.lon, e.lat, e.lon);
    url = b;
    auto *map = new Fl_Button(12, H - 42, 130, 30, "Show on Map");
    map->callback([](Fl_Widget *, void *) { fld::spawn({"xdg-open", url}); });
  }
  Fl_Return_Button ok(348, H - 42, 100, 30, "Close");
  ok.callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  d.end();
  d.set_modal();
  d.show();
  while (d.shown()) Fl::wait();
}

static void toggle_strip() {
  g_show_strip = !g_show_strip;
  fld::Ini ini;
  ini.load(settings_path());
  ini.set_bool("View", "Thumbnails", g_show_strip);
  ini.save(settings_path());
  relayout();
}

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
  g_view->modified = false; // it goes anyway
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
    case 17: if (g_view->confirm_discard()) g_win->hide(); break;
    case 18: if (!g_view->files.empty() && g_view->confirm_discard()) { g_view->index = 0; g_view->load(); } break;
    case 19:
      if (!g_view->files.empty() && g_view->confirm_discard()) {
        g_view->index = (int)g_view->files.size() - 1;
        g_view->load();
      }
      break;
    case 20: g_view->start_crop(); break;
    case 21: resize_dialog(); break;
    case 22: save(); break;
    case 23: save_as(); break;
    case 24: info_window(); break;
    case 25: toggle_strip(); break;
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
      {"Flip Horizontally", 'h', nullptr, (void *)10},
      {"Crop…", 'c', nullptr, (void *)20},
      {"Resize…", FL_CTRL + 'r', nullptr, (void *)21, FL_MENU_DIVIDER},
      {"Save", FL_CTRL + 's', nullptr, (void *)22, modified ? 0 : FL_MENU_INACTIVE},
      {"Save As…", FL_CTRL + FL_SHIFT + 's', nullptr, (void *)23, FL_MENU_DIVIDER},
      {"Image Information", 'i', nullptr, (void *)24},
      {"Thumbnails", 't', nullptr, (void *)25, FL_MENU_TOGGLE | (g_show_strip ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER},
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
  bool shift = Fl::event_state() & FL_SHIFT;
  if (g_view->cropping) {
    if (k == FL_Escape) { g_view->end_crop(false); return 1; }
    if (k == FL_Enter || k == FL_KP_Enter) { g_view->end_crop(true); return 1; }
  }
  if (ctrl && k == 's') { action(shift ? 23 : 22); return 1; }
  if (ctrl && k == 'r') { action(21); return 1; }
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
      case 'c': action(20); return 1;
      case 'i': action(24); return 1;
      case 't': action(25); return 1;
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
  {
    fld::Ini ini;
    ini.load(settings_path());
    g_show_strip = ini.get_bool("View", "Thumbnails", true);
  }
  g_win = new ViewWindow(900, 640, "Image Viewer");
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
  g_bar->add_button("transform-crop", "Crop (C)", action_cb, (void *)20);
  g_bar->add_button("transform-scale", "Resize (Ctrl+R)", action_cb, (void *)21);
  g_bar->add_button("document-save-as", "Save as (Ctrl+Shift+S)", action_cb, (void *)23);
  g_bar->add_separator();
  g_bar->add_button("document-properties", "Image information (I)", action_cb, (void *)24);
  g_bar->add_separator();
  g_bar->add_button("media-playback-start", "Slideshow (F5)", action_cb, (void *)12);
  g_bar->add_button("view-fullscreen", "Fullscreen (F11)", action_cb, (void *)11);
  g_bar->add_button("user-trash", "Move to trash (Del)", action_cb, (void *)13);
  g_view = new Viewer(0, TB, 900, 640 - TB - ST);
  g_strip = new Strip(0, 640 - ST - Strip::H, 900, Strip::H);
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
  g_win->callback([](Fl_Widget *w, void *) {
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return; // handled by the key handler
    if (g_view->confirm_discard()) w->hide();
  });
  if (!start.empty()) g_view->set_files_from(start);
  else g_view->update_title();
  relayout();
  g_win->show(); // not show(argc, argv): it resets the theme colors
  g_view->take_focus();
  return Fl::run();
}
