// Backend for WPE WebKit with libwpe and WPEBackend-fdo (Debian 13's
// WebKit): the view backend exports shared memory buffers that are copied
// into memory for FLTK to draw.
#include "backend.h"

#include <wayland-server.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/wpe.h>

#include <cstring>

namespace web {
namespace {

bool g_initialized = false;

void initialize() {
  if (g_initialized) return;
  g_initialized = true;
  wpe_loader_init("libWPEBackend-fdo-1.0.so.1");
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
    static const wpe_view_backend_exportable_fdo_client client = {
        nullptr, nullptr, [](void *data, wpe_fdo_shm_exported_buffer *buffer) { export_shm((Proxy *)data, buffer); },
        nullptr, nullptr};
    proxy_ = new Proxy{this};
    exportable_ = wpe_view_backend_exportable_fdo_create(&client, proxy_, (uint32_t)width_, (uint32_t)height_);
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
  static void export_shm(Proxy *p, wpe_fdo_shm_exported_buffer *buffer) {
    if (!p->owner) {
      wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(p->exportable, buffer);
      wpe_view_backend_exportable_fdo_dispatch_frame_complete(p->exportable);
      return;
    }
    p->owner->copy_shm(buffer);
  }
  void copy_shm(wpe_fdo_shm_exported_buffer *buffer) {
    wl_shm_buffer *shm = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
    auto f = std::make_shared<Frame>();
    if (shm) {
      wl_shm_buffer_begin_access(shm);
      f->width = wl_shm_buffer_get_width(shm);
      f->height = wl_shm_buffer_get_height(shm);
      f->stride = wl_shm_buffer_get_stride(shm);
      const uint8_t *p = (const uint8_t *)wl_shm_buffer_get_data(shm);
      f->pixels.assign(p, p + (size_t)f->stride * (size_t)f->height);
      wl_shm_buffer_end_access(shm);
    }
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(exportable_, buffer);
    pending_frame_ = true;
    if (on_frame && shm) on_frame(f);
  }

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
