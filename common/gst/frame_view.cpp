#include "frame_view.h"

#include <FL/Fl.H>
#include <FL/Fl_Gl_Window.H>
#include <FL/fl_draw.H>
#include <FL/gl.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "fld/icons.h"
#include "fld/theme.h"

namespace fgst {

bool init(std::string *err) {
  static int state = 0; // 0 untried, 1 ok, -1 failed
  static std::string msg;
  if (state == 0) {
    GError *e = nullptr;
    if (gst_init_check(nullptr, nullptr, &e)) {
      state = 1;
    } else {
      state = -1;
      msg = e ? e->message : "GStreamer could not be initialized";
      if (e) g_error_free(e);
    }
  }
  if (state < 0 && err) *err = msg;
  return state > 0;
}

bool have_element(const char *factory) {
  GstElementFactory *f = gst_element_factory_find(factory);
  if (!f) return false;
  gst_object_unref(f);
  return true;
}

double now() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---- frames ----------------------------------------------------------------------

void FrameSlot::put(GstSample *sample) {
  GstCaps *caps = gst_sample_get_caps(sample);
  GstBuffer *buf = gst_sample_get_buffer(sample);
  if (!caps || !buf) return;
  GstVideoInfo info;
  if (!gst_video_info_from_caps(&info, caps)) return;
  GstVideoFrame vf;
  if (!gst_video_frame_map(&vf, &info, buf, GST_MAP_READ)) return;
  auto f = std::make_shared<Frame>();
  f->w = GST_VIDEO_FRAME_WIDTH(&vf);
  f->h = GST_VIDEO_FRAME_HEIGHT(&vf);
  f->par = info.par_d ? (double)info.par_n / info.par_d : 1.0;
  if (f->par <= 0) f->par = 1.0;
  int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 0);
  const uint8_t *src = (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&vf, 0);
  f->rgba.resize((size_t)f->w * f->h * 4);
  for (int y = 0; y < f->h; y++) memcpy(&f->rgba[(size_t)y * f->w * 4], src + (size_t)y * stride, (size_t)f->w * 4);
  gst_video_frame_unmap(&vf);
  {
    std::lock_guard<std::mutex> l(mu_);
    f->serial = ++serial_;
    frame_ = f;
  }
  Fl::awake(awake_cb, this);
}

void FrameSlot::awake_cb(void *d) {
  FrameSlot *s = static_cast<FrameSlot *>(d);
  if (s->on_frame) s->on_frame();
}

std::shared_ptr<const Frame> FrameSlot::get() const {
  std::lock_guard<std::mutex> l(mu_);
  return frame_;
}

void FrameSlot::clear() {
  std::lock_guard<std::mutex> l(mu_);
  frame_.reset();
}

// ---- sink ---------------------------------------------------------------------------

static GstFlowReturn on_sample(GstAppSink *sink, gpointer data) {
  GstSample *s = gst_app_sink_pull_sample(sink);
  if (!s) return GST_FLOW_EOS;
  static_cast<FrameSlot *>(data)->put(s);
  gst_sample_unref(s);
  return GST_FLOW_OK;
}

static GstFlowReturn on_preroll(GstAppSink *sink, gpointer data) {
  GstSample *s = gst_app_sink_pull_preroll(sink);
  if (!s) return GST_FLOW_EOS;
  static_cast<FrameSlot *>(data)->put(s);
  gst_sample_unref(s);
  return GST_FLOW_OK;
}

GstElement *make_sink(FrameSlot *slot, const char *name) {
  GstElement *bin = gst_bin_new(name);
  GstElement *conv = gst_element_factory_make("videoconvertscale", "convert");
  GstElement *scale = nullptr;
  if (!conv) {
    conv = gst_element_factory_make("videoconvert", "convert");
    scale = gst_element_factory_make("videoscale", "scale");
  }
  GstElement *filter = gst_element_factory_make("capsfilter", "filter");
  GstElement *sink = gst_element_factory_make("appsink", "appsink");
  if (!conv || !filter || !sink) {
    if (conv) gst_object_unref(conv);
    if (scale) gst_object_unref(scale);
    if (filter) gst_object_unref(filter);
    if (sink) gst_object_unref(sink);
    gst_object_unref(bin);
    return nullptr;
  }
  GstCaps *caps = gst_caps_from_string("video/x-raw,format=RGBA");
  g_object_set(filter, "caps", caps, NULL);
  gst_caps_unref(caps);
  // only the newest frame matters; keep the clock (sync) for smooth playback
  g_object_set(sink, "max-buffers", 1, "drop", TRUE, "sync", TRUE, "qos", TRUE, NULL);
  GstAppSinkCallbacks cb;
  memset(&cb, 0, sizeof cb);
  cb.new_sample = on_sample;
  cb.new_preroll = on_preroll;
  gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, slot, nullptr);
  if (scale) {
    gst_bin_add_many(GST_BIN(bin), conv, scale, filter, sink, NULL);
    gst_element_link_many(conv, scale, filter, sink, NULL);
  } else {
    gst_bin_add_many(GST_BIN(bin), conv, filter, sink, NULL);
    gst_element_link_many(conv, filter, sink, NULL);
  }
  GstPad *pad = gst_element_get_static_pad(conv, "sink");
  gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
  gst_object_unref(pad);
  return bin;
}

void set_scale_to(GstElement *sinkbin, int W, int H) {
  if (!sinkbin) return;
  GstElement *filter = gst_bin_get_by_name(GST_BIN(sinkbin), "filter");
  if (!filter) return;
  GstCaps *caps;
  if (W > 0 && H > 0) {
    // the converter keeps the display aspect ratio inside W x H when only an
    // upper bound is given
    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", "width", GST_TYPE_INT_RANGE, 1, W,
                               "height", GST_TYPE_INT_RANGE, 1, H, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
                               NULL);
  } else {
    caps = gst_caps_from_string("video/x-raw,format=RGBA");
  }
  g_object_set(filter, "caps", caps, NULL);
  gst_caps_unref(caps);
  gst_object_unref(filter);
}

// ---- views ------------------------------------------------------------------------------

void FrameView::fit(int fw, int fh, double par, int W, int H, int &x, int &y, int &w, int &h) {
  if (fw <= 0 || fh <= 0 || W <= 0 || H <= 0) { x = y = 0; w = W; h = H; return; }
  double aspect = fw * par / fh;
  if (W / (double)H > aspect) {
    h = H;
    w = (int)lround(H * aspect);
  } else {
    w = W;
    h = (int)lround(W / aspect);
  }
  x = (W - w) / 2;
  y = (H - h) / 2;
}

void FrameView::osd_timeout(void *d) { static_cast<FrameView *>(d)->widget()->redraw(); }

void FrameView::show_message(const std::string &text, double secs) {
  osd_.text = text;
  osd_.until = now() + secs;
  Fl::remove_timeout(osd_timeout, this);
  Fl::add_timeout(secs + 0.05, osd_timeout, this);
  widget()->redraw();
}

static void shadow_text(const std::string &t, int x, int y, Fl_Color c) {
  fl_color(FL_BLACK);
  fl_draw(t.c_str(), x + 1, y + 1);
  fl_color(c);
  fl_draw(t.c_str(), x, y);
}

void FrameView::draw_osd(int X, int Y, int W, int H) {
  fl_font(FL_HELVETICA_BOLD, std::max(14, std::min(28, H / 22)));
  int lh = fl_height();
  if (!osd_.text.empty() && now() < osd_.until) {
    // several lines, each centred
    size_t start = 0;
    int ly = Y + 20 + lh;
    while (start <= osd_.text.size()) {
      size_t e = osd_.text.find('\n', start);
      std::string line = osd_.text.substr(start, e == std::string::npos ? std::string::npos : e - start);
      int tw = (int)fl_width(line.c_str());
      fl_color(fl_rgb_color(0x10, 0x10, 0x10));
      fl_rectf(X + (W - tw) / 2 - 8, ly - lh + 2, tw + 16, lh + 4);
      shadow_text(line, X + (W - tw) / 2, ly, FL_WHITE);
      ly += lh + 6;
      if (e == std::string::npos) break;
      start = e + 1;
    }
  }
  if (!osd_.corner.empty()) {
    fl_font(FL_HELVETICA_BOLD, std::max(13, std::min(22, H / 26)));
    shadow_text(osd_.corner, X + 14, Y + 14 + fl_height(), osd_.corner_alert ? fl_rgb_color(0xff, 0x45, 0x45) : FL_WHITE);
  }
}

// -- software ---------------------------------------------------------------------------

class SwView : public Fl_Widget, public FrameView {
public:
  SwView(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { box(FL_FLAT_BOX); color(FL_BLACK); }
  Fl_Widget *widget() override { return this; }
  const char *kind() const override { return "software"; }
  bool software() const override { return true; }
  int handle(int e) override {
    if (on_event) {
      int r = on_event(e);
      if (r) return r;
    }
    if (e == FL_PUSH || e == FL_ENTER || e == FL_MOVE) return 1;
    return Fl_Widget::handle(e);
  }
  void resize(int X, int Y, int W, int H) override {
    bool changed = W != w() || H != h();
    Fl_Widget::resize(X, Y, W, H);
    if (changed && on_resize) on_resize(W, H);
  }
  void draw() override {
    fl_push_clip(x(), y(), w(), h());
    fl_color(FL_BLACK);
    fl_rectf(x(), y(), w(), h());
    auto f = slot_ ? slot_->get() : nullptr;
    if (f && f->w > 0) {
      int fx, fy, fw, fh;
      fit(f->w, f->h, f->par, w(), h(), fx, fy, fw, fh);
      if (fw == f->w && fh == f->h && !osd_.mirror) {
        // the pipeline already scaled the frame
        fl_draw_image(f->rgba.data(), x() + fx, y() + fy, f->w, f->h, 4, f->w * 4);
      } else {
        // scale (and mirror) by nearest neighbour into a scratch buffer
        scratch_.resize((size_t)fw * fh * 4);
        for (int yy = 0; yy < fh; yy++) {
          int sy = std::min(f->h - 1, (int)((long)yy * f->h / fh));
          const uint8_t *srow = &f->rgba[(size_t)sy * f->w * 4];
          uint8_t *drow = &scratch_[(size_t)yy * fw * 4];
          for (int xx = 0; xx < fw; xx++) {
            int sx = std::min(f->w - 1, (int)((long)xx * f->w / fw));
            if (osd_.mirror) sx = f->w - 1 - sx;
            memcpy(drow + xx * 4, srow + sx * 4, 4);
          }
        }
        fl_draw_image(scratch_.data(), x() + fx, y() + fy, fw, fh, 4, fw * 4);
      }
    } else if (!osd_.placeholder.empty()) {
      draw_placeholder();
    }
    draw_osd(x(), y(), w(), h());
    fl_pop_clip();
  }

private:
  std::vector<uint8_t> scratch_;
  void draw_placeholder() {
    Fl_Image *ic = fld::icon("audio-x-generic", 96);
    int cy = y() + h() / 2 - 40;
    if (ic) ic->draw(x() + (w() - ic->w()) / 2, cy - ic->h() / 2);
    fl_font(FL_HELVETICA, 16);
    fl_color(fl_rgb_color(0xcc, 0xcc, 0xcc));
    fl_draw(osd_.placeholder.c_str(), x() + 10, cy + 60, w() - 20, 60, FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE);
  }
};

// -- OpenGL -----------------------------------------------------------------------------

class GlView : public Fl_Gl_Window, public FrameView {
public:
  GlView(int x, int y, int w, int h) : Fl_Gl_Window(x, y, w, h) { mode(FL_RGB8 | FL_DOUBLE); }
  ~GlView() override { Fl::remove_timeout(osd_timeout, this); }
  Fl_Widget *widget() override { return this; }
  const char *kind() const override { return "OpenGL"; }
  bool software() const override { return false; }
  void start() override {
    Fl::flush();
    if (failed_ || !context_valid()) {
      // draw() decides; check again once it ran
      Fl::add_timeout(0.5, check_cb, this);
    }
  }
  int handle(int e) override {
    if (on_event) {
      int r = on_event(e);
      if (r) return r;
    }
    if (e == FL_PUSH || e == FL_ENTER || e == FL_MOVE) return 1;
    return Fl_Gl_Window::handle(e);
  }
  void draw() override {
    if (!checked_) {
      checked_ = true;
      const char *v = (const char *)glGetString(GL_VERSION);
      if (!v) {
        fail();
        return;
      }
    }
    if (failed_) return;
    glViewport(0, 0, pixel_w(), pixel_h());
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w(), h(), 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    auto f = slot_ ? slot_->get() : nullptr;
    if (f && f->w > 0) {
      if (!tex_) glGenTextures(1, &tex_);
      glBindTexture(GL_TEXTURE_2D, tex_);
      if (f->serial != uploaded_) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (f->w != tw_ || f->h != th_) {
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f->w, f->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, f->rgba.data());
          tw_ = f->w;
          th_ = f->h;
        } else {
          glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->w, f->h, GL_RGBA, GL_UNSIGNED_BYTE, f->rgba.data());
        }
        uploaded_ = f->serial;
      }
      int fx, fy, fw, fh;
      fit(f->w, f->h, f->par, w(), h(), fx, fy, fw, fh);
      float u0 = osd_.mirror ? 1.f : 0.f, u1 = osd_.mirror ? 0.f : 1.f;
      glEnable(GL_TEXTURE_2D);
      glColor4f(1, 1, 1, 1);
      glBegin(GL_QUADS);
      glTexCoord2f(u0, 0); glVertex2i(fx, fy);
      glTexCoord2f(u1, 0); glVertex2i(fx + fw, fy);
      glTexCoord2f(u1, 1); glVertex2i(fx + fw, fy + fh);
      glTexCoord2f(u0, 1); glVertex2i(fx, fy + fh);
      glEnd();
      glDisable(GL_TEXTURE_2D);
    }
    draw_gl_osd(f == nullptr);
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Gl_Window::resize(X, Y, W, H);
    if (on_resize) on_resize(W, H);
  }

private:
  GLuint tex_ = 0;
  int tw_ = 0, th_ = 0;
  uint64_t uploaded_ = 0;
  bool checked_ = false, failed_ = false;

  void fail() {
    failed_ = true;
    if (getenv("FLD_VIDEO_DEBUG")) fprintf(stderr, "fgst: OpenGL unavailable, switching to software\n");
    Fl::add_timeout(0, fail_cb, this);
  }
  static void fail_cb(void *d) {
    GlView *v = static_cast<GlView *>(d);
    if (v->on_failed) v->on_failed();
  }
  static void check_cb(void *d) {
    GlView *v = static_cast<GlView *>(d);
    if (!v->checked_ && !v->failed_ && !v->shown()) return;
    if (!v->checked_) v->Fl_Gl_Window::redraw();
  }
  // OSD with gl_draw (FLTK's text in OpenGL)
  void draw_gl_osd(bool no_frame) {
    int fs = std::max(14, std::min(28, h() / 22));
    gl_font(FL_HELVETICA_BOLD, fs);
    auto text_at = [&](const std::string &t, int x, int y, float r, float g, float b) {
      glColor3f(0, 0, 0);
      gl_draw(t.c_str(), x + 1, h() - y - 1);
      glColor3f(r, g, b);
      gl_draw(t.c_str(), x, h() - y);
    };
    // gl_draw uses a bottom-left origin
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w(), 0, h(), -1, 1);
    if (no_frame && !osd_.placeholder.empty()) {
      int tw = (int)gl_width(osd_.placeholder.c_str());
      text_at(osd_.placeholder, (w() - tw) / 2, h() / 2, 0.8f, 0.8f, 0.8f);
    }
    if (!osd_.text.empty() && now() < osd_.until) {
      size_t start = 0;
      int ly = 20 + fs;
      while (start <= osd_.text.size()) {
        size_t e = osd_.text.find('\n', start);
        std::string line = osd_.text.substr(start, e == std::string::npos ? std::string::npos : e - start);
        int tw = (int)gl_width(line.c_str());
        text_at(line, (w() - tw) / 2, ly, 1, 1, 1);
        ly += fs + 8;
        if (e == std::string::npos) break;
        start = e + 1;
      }
    }
    if (!osd_.corner.empty()) {
      if (osd_.corner_alert) text_at(osd_.corner, 14, 14 + fs, 1.f, 0.27f, 0.27f);
      else text_at(osd_.corner, 14, 14 + fs, 1, 1, 1);
    }
  }
};

FrameView *FrameView::create(int x, int y, int w, int h, bool force_sw) {
  if (!force_sw && Fl_Gl_Window::can_do(FL_RGB8 | FL_DOUBLE)) return new GlView(x, y, w, h);
  return new SwView(x, y, w, h);
}

} // namespace fgst
