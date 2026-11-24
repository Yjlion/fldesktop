// Backend for WPE WebKit with libwpe and WPEBackend-fdo (Debian 13's
// WebKit). With a GPU, WebKit renders with it and the view backend exports
// EGL images, which are read back into memory for FLTK to draw; otherwise it
// renders with the CPU and exports shared memory buffers.
#include "backend.h"
#include "gl_readback.h"

#include <wayland-server.h>
#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/wpe.h>

#include <cstring>

namespace web {
namespace {

bool g_initialized = false;
bool g_egl = false; // EGL images, else shared memory

void initialize() {
  if (g_initialized) return;
  g_initialized = true;
  wpe_loader_init("libWPEBackend-fdo-1.0.so.1");
  EGLDisplay dpy = gpu_display();
  if (dpy != EGL_NO_DISPLAY && wpe_fdo_initialize_for_egl_display(dpy)) {
    g_egl = true;
    return;
  }
  if (dpy != EGL_NO_DISPLAY) g_message("fl-web: WPEBackend-fdo cannot use the GPU, rendering with the CPU");
  if (!wpe_fdo_initialize_shm()) g_warning("fl-web: could not initialize WPEBackend-fdo");
}

class FdoBackend;

// The exportable lives as long as the WebKitWebViewBackend, which can outlive
// the FdoBackend: its callbacks go through this.
struct Proxy {
  FdoBackend *owner;
  wpe_view_backend_exportable_fdo *exportable = nullptr;
};

class FdoBackend : public Backend {
public:
  FdoBackend() {
    initialize();
    proxy_ = new Proxy{this};
    if (g_egl) {
      static const wpe_view_backend_exportable_fdo_egl_client client = {
          nullptr,
          [](void *data, wpe_fdo_egl_exported_image *image) { export_image((Proxy *)data, image); },
          [](void *data, wpe_fdo_shm_exported_buffer *buffer) { export_shm((Proxy *)data, buffer); },
          nullptr, nullptr};
      exportable_ = wpe_view_backend_exportable_fdo_egl_create(&client, proxy_, (uint32_t)width_, (uint32_t)height_);
    } else {
      static const wpe_view_backend_exportable_fdo_client client = {
          nullptr, nullptr, [](void *data, wpe_fdo_shm_exported_buffer *buffer) { export_shm((Proxy *)data, buffer); },
          nullptr, nullptr};
      exportable_ = wpe_view_backend_exportable_fdo_create(&client, proxy_, (uint32_t)width_, (uint32_t)height_);
    }
    proxy_->exportable = exportable_;
  }
  ~FdoBackend() override { proxy_->owner = nullptr; }

  WebKitWebView *create_view(WebKitWebView *related, WebKitSettings *settings, WebKitNetworkSession *session,
                             WebKitUserContentManager *ucm) override {
    wpe_view_backend *vb = wpe_view_backend_exportable_fdo_get_view_backend(exportable_);
    // the WebKitWebViewBackend owns the exportable
    WebKitWebViewBackend *wb = webkit_web_view_backend_new(
        vb,
        [](gpointer d) {
          Proxy *p = (Proxy *)d;
          wpe_view_backend_exportable_fdo_destroy(p->exportable);
          delete p;
        },
        proxy_);
    WebKitWebView *wv;
    if (related)
      wv = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", wb, "related-view", related, nullptr));
    else
      wv = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", wb, "settings", settings,
                                        "network-session", session, "user-content-manager", ucm, nullptr));
    backend_ = vb;
    wpe_view_backend_add_activity_state(backend_, wpe_view_activity_state_in_window);
    return wv;
  }

  void resize(int w, int h) override {
    if (!backend_ || w <= 0 || h <= 0) return;
    width_ = w;
    height_ = h;
    wpe_view_backend_dispatch_set_size(backend_, (uint32_t)w, (uint32_t)h);
  }
  void set_focus(bool on) override {
    if (!backend_) return;
    if (on) wpe_view_backend_add_activity_state(backend_, wpe_view_activity_state_focused);
    else wpe_view_backend_remove_activity_state(backend_, wpe_view_activity_state_focused);
  }
  void set_visible(bool on) override {
    if (!backend_) return;
    if (on) wpe_view_backend_add_activity_state(backend_, wpe_view_activity_state_visible);
    else wpe_view_backend_remove_activity_state(backend_, wpe_view_activity_state_visible);
  }
  static uint32_t wmods(unsigned m) {
    uint32_t r = 0;
    if (m & MOD_CTRL) r |= wpe_input_keyboard_modifier_control;
    if (m & MOD_SHIFT) r |= wpe_input_keyboard_modifier_shift;
    if (m & MOD_ALT) r |= wpe_input_keyboard_modifier_alt;
    if (m & MOD_META) r |= wpe_input_keyboard_modifier_meta;
    if (m & MOD_BUTTON1) r |= wpe_input_pointer_modifier_button1;
    if (m & MOD_BUTTON2) r |= wpe_input_pointer_modifier_button3; // libwpe: 2 is the right button
    if (m & MOD_BUTTON3) r |= wpe_input_pointer_modifier_button2;
    return r;
  }
  void pointer_button(bool down, unsigned button, double x, double y, unsigned mods, unsigned, uint32_t time) override {
    if (!backend_) return;
    // libwpe numbers the buttons 1 left, 2 right, 3 middle
    unsigned b = button == 2 ? 3 : button == 3 ? 2 : button;
    wpe_input_pointer_event e = {wpe_input_pointer_event_type_button, time, (int)x, (int)y, b, down ? 1u : 0u,
                                 wmods(mods)};
    wpe_view_backend_dispatch_pointer_event(backend_, &e);
  }
  void pointer_motion(double x, double y, unsigned mods, uint32_t time) override {
    if (!backend_) return;
    wpe_input_pointer_event e = {wpe_input_pointer_event_type_motion, time, (int)x, (int)y, 0, 0, wmods(mods)};
    wpe_view_backend_dispatch_pointer_event(backend_, &e);
  }
  void pointer_leave(double, double, unsigned, uint32_t) override {}
  void scroll(double dx, double dy, double x, double y, unsigned mods, uint32_t time) override {
    if (!backend_) return;
    wpe_input_axis_2d_event e = {};
    e.base.type = (wpe_input_axis_event_type)(wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth);
    e.base.time = time;
    e.base.x = (int)x;
    e.base.y = (int)y;
    e.base.modifiers = wmods(mods);
    // WebKit's wheel deltas are positive towards the start of the page (the
    // opposite of ours): passing them as they are scrolled the wrong way,
    // like a touch screen
    e.x_axis = -dx;
    e.y_axis = -dy;
    wpe_view_backend_dispatch_axis_event(backend_, &e.base);
  }
  void key(bool down, unsigned keysym, unsigned keycode, unsigned mods, uint32_t time) override {
    if (!backend_) return;
    wpe_input_keyboard_event e = {time, keysym, keycode, down, wmods(mods)};
    wpe_view_backend_dispatch_keyboard_event(backend_, &e);
  }
  void frame_shown() override {
    if (!pending_frame_) return;
    pending_frame_ = false;
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(exportable_);
  }

private:
  static void release_shm(wpe_view_backend_exportable_fdo *e, wpe_fdo_shm_exported_buffer *buffer) {
    if (g_egl) wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(e, buffer);
    else wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(e, buffer);
  }
  static void export_shm(Proxy *p, wpe_fdo_shm_exported_buffer *buffer) {
    if (!p->owner) {
      release_shm(p->exportable, buffer);
      wpe_view_backend_exportable_fdo_dispatch_frame_complete(p->exportable);
      return;
    }
    p->owner->copy_shm(buffer);
  }
  static void export_image(Proxy *p, wpe_fdo_egl_exported_image *image) {
    if (!p->owner) {
      wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(p->exportable, image);
      wpe_view_backend_exportable_fdo_dispatch_frame_complete(p->exportable);
      return;
    }
    p->owner->copy_image(image);
  }
  // Two frames are kept and reused: the newest goes to the view, the next one
  // is read into the other. Comparing it with the newest gives the damage
  // (the libwpe exports have none), so the view converts and draws only what
  // changed, and frames that change nothing cost no drawing.
  std::shared_ptr<Frame> next_frame(int w, int h, int stride) {
    std::shared_ptr<Frame> &f = frames_[cur_ ^ 1];
    // the view still holds it (it keeps the newest only): a new one
    if (!f || f.use_count() > 1) f = std::make_shared<Frame>();
    f->width = w;
    f->height = h;
    f->stride = stride;
    f->pixels.resize((size_t)stride * (size_t)h);
    return f;
  }
  void deliver(const std::shared_ptr<Frame> &f) {
    const std::shared_ptr<Frame> &prev = frames_[cur_];
    f->damage.clear();
    f->all = true;
    if (prev && prev->width == f->width && prev->height == f->height && prev->stride == f->stride &&
        prev->pixels.size() == f->pixels.size()) {
      bool full = false;
      f->damage = diff_damage(prev->pixels.data(), f->pixels.data(), f->width, f->height, f->stride, full);
      f->all = full;
    }
    cur_ ^= 1;
    if (on_frame) on_frame(f);
  }
  // the image is read back at once and given back to WebKit; the next frame
  // comes after frame_shown()
  void copy_image(wpe_fdo_egl_exported_image *image) {
    int w = (int)wpe_fdo_egl_exported_image_get_width(image), h = (int)wpe_fdo_egl_exported_image_get_height(image);
    auto f = next_frame(w, h, w * 4);
    bool ok = GLReadback::get().read(wpe_fdo_egl_exported_image_get_egl_image(image), w, h, *f);
    wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(exportable_, image);
    pending_frame_ = true;
    if (!ok) {
      static bool warned = false;
      if (!warned) g_warning("fl-web: could not read a frame back from the GPU");
      warned = true;
      frame_shown(); // let WebKit go on
      return;
    }
    deliver(f);
  }
  void copy_shm(wpe_fdo_shm_exported_buffer *buffer) {
    wl_shm_buffer *shm = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
    std::shared_ptr<Frame> f;
    if (shm) {
      wl_shm_buffer_begin_access(shm);
      f = next_frame(wl_shm_buffer_get_width(shm), wl_shm_buffer_get_height(shm), wl_shm_buffer_get_stride(shm));
      memcpy(f->pixels.data(), wl_shm_buffer_get_data(shm), f->pixels.size());
      wl_shm_buffer_end_access(shm);
    }
    release_shm(exportable_, buffer);
    pending_frame_ = true;
    if (f) deliver(f);
  }

  std::shared_ptr<Frame> frames_[2];
  int cur_ = 0; // frames_[cur_] is the newest
  wpe_view_backend_exportable_fdo *exportable_ = nullptr;
  wpe_view_backend *backend_ = nullptr;
  int width_ = 800, height_ = 600;
  bool pending_frame_ = false;
  Proxy *proxy_ = nullptr;
};

} // namespace

std::function<void(const std::string &)> Backend::on_copy;
std::unique_ptr<Backend> Backend::create() { return std::make_unique<FdoBackend>(); }
const char *backend_name() { return "WPEBackend-fdo"; }

} // namespace web
