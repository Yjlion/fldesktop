// Backend for WPE WebKit built with the WPE Platform API: a WPEDisplay and
// WPEView of our own whose buffers are copied into memory for FLTK to draw.
// With a GPU the display offers its EGL display and DRM device, WebKit
// renders into DMA-BUF buffers and they are read back with GL; otherwise
// WebKit renders with the CPU into shared memory.
#include "backend.h"
#include "gl_readback.h"

#include <wpe/wpe-platform.h>

#include <cstring>
#include <vector>

namespace web {
class PlatformBackend;
}

// ---- GObject types ------------------------------------------------------------------

G_BEGIN_DECLS
#define FLD_TYPE_WPE_DISPLAY (fld_wpe_display_get_type())
G_DECLARE_FINAL_TYPE(FldWpeDisplay, fld_wpe_display, FLD, WPE_DISPLAY, WPEDisplay)
#define FLD_TYPE_WPE_VIEW (fld_wpe_view_get_type())
G_DECLARE_FINAL_TYPE(FldWpeView, fld_wpe_view, FLD, WPE_VIEW, WPEView)
#define FLD_TYPE_WPE_CLIPBOARD (fld_wpe_clipboard_get_type())
G_DECLARE_FINAL_TYPE(FldWpeClipboard, fld_wpe_clipboard, FLD, WPE_CLIPBOARD, WPEClipboard)
#define FLD_TYPE_WPE_TOPLEVEL (fld_wpe_toplevel_get_type())
G_DECLARE_FINAL_TYPE(FldWpeToplevel, fld_wpe_toplevel, FLD, WPE_TOPLEVEL, WPEToplevel)
G_END_DECLS

struct _FldWpeDisplay {
  WPEDisplay parent;
  WPEKeymap *keymap;
  WPEClipboard *clipboard;
  WPEDRMDevice *drm; // null without a GPU
  WPEBufferFormats *formats;
};
struct _FldWpeClipboard {
  WPEClipboard parent;
  gboolean setting; // we are putting the system clipboard in
};
struct _FldWpeView {
  WPEView parent;
  web::PlatformBackend *backend; // not owned; null once the backend is gone
  WPEBuffer *pending;            // shown, waiting for frame_shown()
};
struct _FldWpeToplevel {
  WPEToplevel parent;
};

G_DEFINE_FINAL_TYPE(FldWpeDisplay, fld_wpe_display, WPE_TYPE_DISPLAY)
G_DEFINE_FINAL_TYPE(FldWpeView, fld_wpe_view, WPE_TYPE_VIEW)
G_DEFINE_FINAL_TYPE(FldWpeToplevel, fld_wpe_toplevel, WPE_TYPE_TOPLEVEL)
G_DEFINE_FINAL_TYPE(FldWpeClipboard, fld_wpe_clipboard, WPE_TYPE_CLIPBOARD)

namespace web {

class PlatformBackend : public Backend {
public:
  ~PlatformBackend() override {
    if (view_) {
      view_->backend = nullptr;
      if (view_->pending) {
        wpe_view_buffer_released(WPE_VIEW(view_), view_->pending);
        g_clear_object(&view_->pending);
      }
    }
    g_clear_object(&toplevel_);
  }

  WebKitWebView *create_view(WebKitWebView *related, WebKitSettings *settings, WebKitNetworkSession *session,
                             WebKitUserContentManager *ucm) override;

  void resize(int w, int h) override {
    if (!view_ || w <= 0 || h <= 0) return;
    wpe_toplevel_resized(toplevel_, w, h);
    wpe_view_resized(WPE_VIEW(view_), w, h);
  }
  void set_focus(bool on) override {
    if (!view_) return;
    on ? wpe_view_focus_in(WPE_VIEW(view_)) : wpe_view_focus_out(WPE_VIEW(view_));
  }
  void set_visible(bool on) override {
    if (!view_) return;
    wpe_view_set_visible(WPE_VIEW(view_), on);
    on ? wpe_view_map(WPE_VIEW(view_)) : wpe_view_unmap(WPE_VIEW(view_));
  }
  static WPEModifiers wmods(unsigned m) {
    unsigned r = 0;
    if (m & MOD_CTRL) r |= WPE_MODIFIER_KEYBOARD_CONTROL;
    if (m & MOD_SHIFT) r |= WPE_MODIFIER_KEYBOARD_SHIFT;
    if (m & MOD_ALT) r |= WPE_MODIFIER_KEYBOARD_ALT;
    if (m & MOD_META) r |= WPE_MODIFIER_KEYBOARD_META;
    if (m & MOD_BUTTON1) r |= WPE_MODIFIER_POINTER_BUTTON1;
    if (m & MOD_BUTTON2) r |= WPE_MODIFIER_POINTER_BUTTON2;
    if (m & MOD_BUTTON3) r |= WPE_MODIFIER_POINTER_BUTTON3;
    return (WPEModifiers)r;
  }
  void send(WPEEvent *e) {
    wpe_view_event(WPE_VIEW(view_), e);
    wpe_event_unref(e);
  }
  void pointer_button(bool down, unsigned button, double x, double y, unsigned mods, unsigned, uint32_t time) override {
    if (!view_) return;
    WPEView *v = WPE_VIEW(view_);
    unsigned count = down ? wpe_view_compute_press_count(v, x, y, button, time) : 0;
    send(wpe_event_pointer_button_new(down ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP, v, WPE_INPUT_SOURCE_MOUSE,
                                      time, wmods(mods), button, x, y, count));
  }
  void pointer_motion(double x, double y, unsigned mods, uint32_t time) override {
    if (!view_) return;
    send(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, WPE_VIEW(view_), WPE_INPUT_SOURCE_MOUSE, time, wmods(mods),
                                    x, y, x - last_x_, y - last_y_));
    last_x_ = x;
    last_y_ = y;
  }
  void pointer_leave(double x, double y, unsigned mods, uint32_t time) override {
    if (!view_) return;
    send(wpe_event_pointer_move_new(WPE_EVENT_POINTER_LEAVE, WPE_VIEW(view_), WPE_INPUT_SOURCE_MOUSE, time, wmods(mods),
                                    x, y, 0, 0));
  }
  void scroll(double dx, double dy, double x, double y, unsigned mods, uint32_t time) override {
    if (!view_) return;
    // WPE: positive deltas scroll towards the start (like wl_pointer axis negated)
    send(wpe_event_scroll_new(WPE_VIEW(view_), WPE_INPUT_SOURCE_MOUSE, time, wmods(mods), -dx, -dy, TRUE, FALSE, x, y));
  }
  void key(bool down, unsigned keysym, unsigned keycode, unsigned mods, uint32_t time) override {
    if (!view_) return;
    send(wpe_event_keyboard_new(down ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP, WPE_VIEW(view_),
                                WPE_INPUT_SOURCE_KEYBOARD, time, wmods(mods), keycode, keysym));
  }
  bool shares_clipboard() const override { return true; }
  void set_clipboard_text(const std::string &text) override;
  void frame_shown() override {
    if (!view_ || !view_->pending) return;
    WPEBuffer *b = view_->pending;
    view_->pending = nullptr;
    wpe_view_buffer_rendered(WPE_VIEW(view_), b);
    wpe_view_buffer_released(WPE_VIEW(view_), b);
    g_object_unref(b);
  }

  // from the view
  // damage: what changed since the last buffer (n == 0: not known)
  gboolean render(WPEBuffer *buffer, const WPERectangle *rects, guint n, GError **error) {
    int w = wpe_buffer_get_width(buffer), h = wpe_buffer_get_height(buffer);
    // one Frame, updated where the page changed: copying all of a frame for
    // a blinking caret costs a slow machine a good part of a core
    bool same = frame_ && frame_->width == w && frame_->height == h && !frame_->pixels.empty();
    bool all = true;
    std::vector<Rect> dmg;
    if (same) {
      std::vector<Rect> in;
      for (guint i = 0; i < n; i++) in.push_back({rects[i].x, rects[i].y, rects[i].width, rects[i].height});
      dmg = clip_damage(in.data(), n, w, h, all);
    }
    if (!frame_) frame_ = std::make_shared<Frame>();
    Frame *f = frame_.get();
    if (WPE_IS_BUFFER_DMA_BUF(buffer) && gpu_display() != EGL_NO_DISPLAY) {
      // rendered by the GPU: copy it back with GL
      auto image = (EGLImageKHR)wpe_buffer_import_to_egl_image(buffer, error);
      if (!image) return FALSE;
      bool ok = all ? GLReadback::get().read(image, w, h, *f) : GLReadback::get().read_rects(image, dmg, *f);
      if (!ok) {
        frame_.reset();
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "fl-web: could not read the frame back");
        return FALSE;
      }
    } else if (WPE_IS_BUFFER_SHM(buffer)) {
      WPEBufferSHM *shm = WPE_BUFFER_SHM(buffer);
      GBytes *data = wpe_buffer_shm_get_data(shm);
      int stride = (int)wpe_buffer_shm_get_stride(shm);
      gsize len = 0;
      const uint8_t *p = (const uint8_t *)g_bytes_get_data(data, &len);
      if (!all && stride == f->stride && len == f->pixels.size()) {
        for (const Rect &r : dmg)
          for (int row = r.y; row < r.y + r.h; row++) {
            size_t off = (size_t)row * stride + (size_t)r.x * 4;
            memcpy(f->pixels.data() + off, p + off, (size_t)r.w * 4);
          }
      } else {
        all = true;
        f->width = w;
        f->height = h;
        f->stride = stride;
        f->pixels.assign(p, p + len);
      }
    } else {
      all = true;
      f->width = w;
      f->height = h;
      GBytes *data = wpe_buffer_import_to_pixels(buffer, error);
      if (!data) return FALSE;
      gsize len = 0;
      const uint8_t *p = (const uint8_t *)g_bytes_get_data(data, &len);
      f->stride = f->height ? (int)(len / (gsize)f->height) : 0;
      f->pixels.assign(p, p + len);
    }
    f->all = all;
    f->damage = all ? std::vector<Rect>{} : std::move(dmg);
    // one buffer at a time: WebKit renders the next after frame_shown()
    if (view_->pending) {
      wpe_view_buffer_rendered(WPE_VIEW(view_), view_->pending);
      wpe_view_buffer_released(WPE_VIEW(view_), view_->pending);
      g_object_unref(view_->pending);
    }
    view_->pending = WPE_BUFFER(g_object_ref(buffer));
    if (on_frame) on_frame(frame_);
    return TRUE;
  }
  void cursor(const char *name) {
    if (on_cursor) on_cursor(name ? name : "default");
  }

private:
  FldWpeView *view_ = nullptr;
  WPEToplevel *toplevel_ = nullptr;
  std::shared_ptr<Frame> frame_;
  double last_x_ = 0, last_y_ = 0;
};

namespace {
WPEDisplay *g_display = nullptr;

WPEDisplay *display() {
  if (!g_display) {
    g_display = WPE_DISPLAY(g_object_new(FLD_TYPE_WPE_DISPLAY, nullptr));
    GError *e = nullptr;
    if (!wpe_display_connect(g_display, &e)) {
      g_warning("fl-web: %s", e ? e->message : "display");
      g_clear_error(&e);
    }
  }
  return g_display;
}
} // namespace

WebKitWebView *PlatformBackend::create_view(WebKitWebView *related, WebKitSettings *settings,
                                            WebKitNetworkSession *session, WebKitUserContentManager *ucm) {
  WebKitWebView *wv;
  if (related)
    wv = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display(), "related-view", related, nullptr));
  else
    wv = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display(), "settings", settings,
                                      "network-session", session, "user-content-manager", ucm, "web-context",
                                      g_web_context ? g_web_context : webkit_web_context_get_default(), nullptr));
  WPEView *v = webkit_web_view_get_wpe_view(wv);
  if (v && FLD_IS_WPE_VIEW(v)) {
    view_ = FLD_WPE_VIEW(v);
    view_->backend = this;
    if (WPEToplevel *t = wpe_view_get_toplevel(v)) {
      toplevel_ = WPE_TOPLEVEL(g_object_ref(t));
    } else {
      toplevel_ = WPE_TOPLEVEL(g_object_new(FLD_TYPE_WPE_TOPLEVEL, "display", display(), "max-views", 1, nullptr));
      wpe_view_set_toplevel(v, toplevel_);
    }
  } else {
    g_warning("fl-web: WebKit did not use the fl-web display");
  }
  return wv;
}

void PlatformBackend::set_clipboard_text(const std::string &text) {
  WPEClipboard *cb = wpe_display_get_clipboard(display());
  if (!cb) return;
  WPEClipboardContent *c = wpe_clipboard_content_new();
  wpe_clipboard_content_set_text(c, text.c_str());
  FLD_WPE_CLIPBOARD(cb)->setting = TRUE;
  wpe_clipboard_set_content(cb, c);
  FLD_WPE_CLIPBOARD(cb)->setting = FALSE;
  wpe_clipboard_content_unref(c);
}

std::function<void(const std::string &)> Backend::on_copy;
std::unique_ptr<Backend> Backend::create() { return std::make_unique<PlatformBackend>(); }
const char *backend_name() { return "WPE Platform"; }

} // namespace web

// ---- GObject implementations ------------------------------------------------------

static gboolean display_connect(WPEDisplay *, GError **) { return TRUE; }
static WPEView *display_create_view(WPEDisplay *d) {
  return WPE_VIEW(g_object_new(FLD_TYPE_WPE_VIEW, "display", d, nullptr));
}
static gpointer display_get_egl_display(WPEDisplay *, GError **error) {
  EGLDisplay dpy = web::gpu_display();
  if (dpy != EGL_NO_DISPLAY) return dpy;
  // no GPU: WebKit renders with the CPU into shared memory buffers
  g_set_error_literal(error, WPE_DISPLAY_ERROR, WPE_DISPLAY_ERROR_NOT_SUPPORTED, "fl-web renders without EGL");
  return nullptr;
}
static WPEDRMDevice *display_get_drm_device(WPEDisplay *d) {
  FldWpeDisplay *self = FLD_WPE_DISPLAY(d);
  if (!self->drm) {
    web::DrmNodes n = web::gpu_drm_nodes();
    if (n.render.empty()) return nullptr;
    self->drm = wpe_drm_device_new(n.primary.empty() ? nullptr : n.primary.c_str(), n.render.c_str());
  }
  return self->drm;
}
// The formats the readback can import (as GL_TEXTURE_2D): ARGB8888 and
// XRGB8888 with the modifiers the EGL display supports.
static WPEBufferFormats *display_get_preferred_buffer_formats(WPEDisplay *d) {
  FldWpeDisplay *self = FLD_WPE_DISPLAY(d);
  if (!self->formats) {
    EGLDisplay dpy = web::gpu_display();
    WPEDRMDevice *drm = display_get_drm_device(d);
    if (dpy == EGL_NO_DISPLAY || !drm) return nullptr;
    auto query_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    const guint64 kInvalid = 0x00ffffffffffffffULL; // DRM_FORMAT_MOD_INVALID: the driver's choice
    WPEBufferFormatsBuilder *b = wpe_buffer_formats_builder_new(drm);
    wpe_buffer_formats_builder_append_group(b, drm, WPE_BUFFER_FORMAT_USAGE_RENDERING);
    for (guint32 fourcc : {0x34325241u /* AR24 */, 0x34325258u /* XR24 */}) {
      std::vector<EGLuint64KHR> mods;
      std::vector<EGLBoolean> external;
      EGLint n = 0;
      if (query_modifiers && query_modifiers(dpy, (EGLint)fourcc, 0, nullptr, nullptr, &n) && n > 0) {
        mods.resize(n);
        external.resize(n);
        if (!query_modifiers(dpy, (EGLint)fourcc, n, mods.data(), external.data(), &n)) n = 0;
      }
      bool any = false;
      for (EGLint i = 0; i < n; i++)
        if (!external[i]) {
          wpe_buffer_formats_builder_append_format(b, fourcc, mods[i]);
          any = true;
        }
      if (!any) wpe_buffer_formats_builder_append_format(b, fourcc, kInvalid);
    }
    self->formats = wpe_buffer_formats_builder_end(b);
  }
  return self->formats; // transfer none
}
static WPEKeymap *display_get_keymap(WPEDisplay *d) {
  FldWpeDisplay *self = FLD_WPE_DISPLAY(d);
  if (!self->keymap) self->keymap = wpe_keymap_xkb_new();
  return self->keymap;
}
static WPEToplevel *display_create_toplevel(WPEDisplay *d, guint max_views) {
  return WPE_TOPLEVEL(g_object_new(FLD_TYPE_WPE_TOPLEVEL, "display", d, "max-views", max_views, nullptr));
}
static WPEClipboard *display_get_clipboard(WPEDisplay *d) {
  FldWpeDisplay *self = FLD_WPE_DISPLAY(d);
  if (!self->clipboard) self->clipboard = WPE_CLIPBOARD(g_object_new(FLD_TYPE_WPE_CLIPBOARD, "display", d, nullptr));
  return self->clipboard;
}
static void display_dispose(GObject *o) {
  g_clear_object(&FLD_WPE_DISPLAY(o)->keymap);
  g_clear_object(&FLD_WPE_DISPLAY(o)->clipboard);
  g_clear_object(&FLD_WPE_DISPLAY(o)->formats);
  if (FLD_WPE_DISPLAY(o)->drm) {
    wpe_drm_device_unref(FLD_WPE_DISPLAY(o)->drm);
    FLD_WPE_DISPLAY(o)->drm = nullptr;
  }
  G_OBJECT_CLASS(fld_wpe_display_parent_class)->dispose(o);
}
static void fld_wpe_display_class_init(FldWpeDisplayClass *k) {
  WPEDisplayClass *dc = WPE_DISPLAY_CLASS(k);
  dc->connect = display_connect;
  dc->create_view = display_create_view;
  dc->get_egl_display = display_get_egl_display;
  dc->get_drm_device = display_get_drm_device;
  dc->get_preferred_buffer_formats = display_get_preferred_buffer_formats;
  dc->get_keymap = display_get_keymap;
  dc->get_clipboard = display_get_clipboard;
  dc->create_toplevel = display_create_toplevel;
  G_OBJECT_CLASS(k)->dispose = display_dispose;
}
static void fld_wpe_display_init(FldWpeDisplay *) {}

static gboolean view_render_buffer(WPEView *v, WPEBuffer *buffer, const WPERectangle *damage, guint n_damage,
                                   GError **error) {
  FldWpeView *self = FLD_WPE_VIEW(v);
  if (!self->backend) {
    wpe_view_buffer_rendered(v, buffer);
    wpe_view_buffer_released(v, buffer);
    return TRUE;
  }
  return self->backend->render(buffer, damage, n_damage, error);
}
static void view_set_cursor_from_name(WPEView *v, const char *name) {
  FldWpeView *self = FLD_WPE_VIEW(v);
  if (self->backend) self->backend->cursor(name);
}
static gboolean view_can_be_mapped(WPEView *) { return TRUE; }
static void view_dispose(GObject *o) {
  g_clear_object(&FLD_WPE_VIEW(o)->pending);
  G_OBJECT_CLASS(fld_wpe_view_parent_class)->dispose(o);
}
static void fld_wpe_view_class_init(FldWpeViewClass *k) {
  WPEViewClass *vc = WPE_VIEW_CLASS(k);
  vc->render_buffer = view_render_buffer;
  vc->set_cursor_from_name = view_set_cursor_from_name;
  vc->can_be_mapped = view_can_be_mapped;
  G_OBJECT_CLASS(k)->dispose = view_dispose;
}
static void fld_wpe_view_init(FldWpeView *) {}

static gboolean toplevel_resize(WPEToplevel *t, int w, int h) {
  wpe_toplevel_resized(t, w, h);
  return TRUE;
}
static void fld_wpe_toplevel_class_init(FldWpeToplevelClass *k) { WPE_TOPLEVEL_CLASS(k)->resize = toplevel_resize; }
static void fld_wpe_toplevel_init(FldWpeToplevel *) {}

// the page copied: hand the text to the system clipboard
static void clipboard_changed(WPEClipboard *cb, GPtrArray *formats, gboolean is_local, WPEClipboardContent *content) {
  WPE_CLIPBOARD_CLASS(fld_wpe_clipboard_parent_class)->changed(cb, formats, is_local, content);
  if (FLD_WPE_CLIPBOARD(cb)->setting || !content || !web::Backend::on_copy) return;
  if (const char *t = wpe_clipboard_content_get_text(content)) web::Backend::on_copy(t);
}
static void fld_wpe_clipboard_class_init(FldWpeClipboardClass *k) { WPE_CLIPBOARD_CLASS(k)->changed = clipboard_changed; }
static void fld_wpe_clipboard_init(FldWpeClipboard *) {}
