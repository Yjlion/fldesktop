#include "mpv_view.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/gl.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <EGL/egl.h>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

// ---- Player ---------------------------------------------------------------------------

Player::Player() {
  // mpv requires the C numeric locale
  setlocale(LC_NUMERIC, "C");
  mpv_ = mpv_create();
  if (!mpv_) return;
  mpv_set_option_string(mpv_, "vo", "libmpv");
  mpv_set_option_string(mpv_, "hwdec", "auto-safe");
  mpv_set_option_string(mpv_, "keep-open", "yes");
  mpv_set_option_string(mpv_, "idle", "yes");
  mpv_set_option_string(mpv_, "input-default-bindings", "no");
  mpv_set_option_string(mpv_, "input-vo-keyboard", "no");
  mpv_set_option_string(mpv_, "osc", "no");
  mpv_set_option_string(mpv_, "osd-level", "1");
  if (getenv("FLD_VIDEO_DEBUG")) {
    mpv_set_option_string(mpv_, "terminal", "yes");
    mpv_set_option_string(mpv_, "msg-level", "all=v");
  } else {
    mpv_set_option_string(mpv_, "terminal", "no");
  }
  mpv_set_option_string(mpv_, "config", "no");
  // resuming a file should play it, not restore a paused state
  mpv_set_option_string(mpv_, "watch-later-options-remove", "pause");
  if (mpv_initialize(mpv_) < 0) {
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
    return;
  }
  for (const char *p : {"time-pos", "duration", "pause", "volume", "mute", "media-title", "speed", "eof-reached",
                        "playlist-pos", "playlist-count", "track-list", "vid", "seekable", "paused-for-cache"}) {
    mpv_format f = MPV_FORMAT_NONE; // just a change notification; values are read on demand
    mpv_observe_property(mpv_, 0, p, f);
  }
  mpv_set_wakeup_callback(mpv_, wakeup, this);
}

Player::~Player() {
  if (mpv_) {
    mpv_set_wakeup_callback(mpv_, nullptr, nullptr);
    mpv_terminate_destroy(mpv_);
  }
}

void Player::wakeup(void *d) { Fl::awake(drain, d); }

void Player::drain(void *d) {
  Player *p = (Player *)d;
  while (p->mpv_) {
    mpv_event *e = mpv_wait_event(p->mpv_, 0);
    if (e->event_id == MPV_EVENT_NONE) break;
    p->handle_event(e);
  }
}

void Player::handle_event(mpv_event *e) {
  switch (e->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE:
      if (on_property) on_property(((mpv_event_property *)e->data)->name);
      break;
    case MPV_EVENT_FILE_LOADED:
      if (on_file_loaded) on_file_loaded();
      break;
    case MPV_EVENT_END_FILE: {
      mpv_event_end_file *ef = (mpv_event_end_file *)e->data;
      if (ef->reason == MPV_END_FILE_REASON_ERROR && on_error) on_error(mpv_error_string(ef->error));
      break;
    }
    case MPV_EVENT_IDLE:
      if (on_idle) on_idle();
      break;
    case MPV_EVENT_SHUTDOWN:
      if (on_shutdown) on_shutdown();
      break;
    default: break;
  }
}

void Player::load(const std::string &file, bool append) {
  command({"loadfile", file, append ? "append-play" : "replace"});
}

void Player::command(const std::vector<std::string> &args) {
  if (!mpv_) return;
  std::vector<const char *> a;
  for (auto &s : args) a.push_back(s.c_str());
  a.push_back(nullptr);
  mpv_command_async(mpv_, 0, a.data());
}

void Player::set_flag(const char *prop, bool v) {
  if (!mpv_) return;
  int f = v;
  mpv_set_property_async(mpv_, 0, prop, MPV_FORMAT_FLAG, &f);
}

void Player::set_double(const char *prop, double v) {
  if (mpv_) mpv_set_property_async(mpv_, 0, prop, MPV_FORMAT_DOUBLE, &v);
}

void Player::set_string(const char *prop, const std::string &v) {
  if (mpv_) mpv_set_property_string(mpv_, prop, v.c_str());
}

bool Player::get_flag(const char *prop) const {
  int f = 0;
  if (mpv_) mpv_get_property(mpv_, prop, MPV_FORMAT_FLAG, &f);
  return f;
}

double Player::get_double(const char *prop, double def) const {
  double d = def;
  if (!mpv_ || mpv_get_property(mpv_, prop, MPV_FORMAT_DOUBLE, &d) < 0) return def;
  return d;
}

long Player::get_int(const char *prop, long def) const {
  int64_t v = def;
  if (!mpv_ || mpv_get_property(mpv_, prop, MPV_FORMAT_INT64, &v) < 0) return def;
  return (long)v;
}

std::string Player::get_string(const char *prop) const {
  if (!mpv_) return "";
  char *s = mpv_get_property_string(mpv_, prop);
  std::string r = s ? s : "";
  mpv_free(s);
  return r;
}

static const mpv_node *map_get(const mpv_node &n, const char *key) {
  if (n.format != MPV_FORMAT_NODE_MAP) return nullptr;
  for (int i = 0; i < n.u.list->num; i++)
    if (!strcmp(n.u.list->keys[i], key)) return &n.u.list->values[i];
  return nullptr;
}

static std::string node_str(const mpv_node *n) {
  return n && n->format == MPV_FORMAT_STRING ? n->u.string : "";
}

std::vector<Player::Track> Player::tracks() const {
  std::vector<Track> v;
  mpv_node list;
  if (!mpv_ || mpv_get_property(mpv_, "track-list", MPV_FORMAT_NODE, &list) < 0) return v;
  if (list.format == MPV_FORMAT_NODE_ARRAY)
    for (int i = 0; i < list.u.list->num; i++) {
      const mpv_node &t = list.u.list->values[i];
      Track tr{0, node_str(map_get(t, "type")), node_str(map_get(t, "title")), node_str(map_get(t, "lang")), false};
      if (const mpv_node *id = map_get(t, "id"); id && id->format == MPV_FORMAT_INT64) tr.id = (long)id->u.int64;
      if (const mpv_node *s = map_get(t, "selected"); s && s->format == MPV_FORMAT_FLAG) tr.selected = s->u.flag;
      v.push_back(tr);
    }
  mpv_free_node_contents(&list);
  return v;
}

std::vector<Player::PlaylistItem> Player::playlist() const {
  std::vector<PlaylistItem> v;
  mpv_node list;
  if (!mpv_ || mpv_get_property(mpv_, "playlist", MPV_FORMAT_NODE, &list) < 0) return v;
  if (list.format == MPV_FORMAT_NODE_ARRAY)
    for (int i = 0; i < list.u.list->num; i++) {
      const mpv_node &t = list.u.list->values[i];
      PlaylistItem it{node_str(map_get(t, "filename")), node_str(map_get(t, "title")), false};
      if (const mpv_node *c = map_get(t, "current"); c && c->format == MPV_FORMAT_FLAG) it.current = c->u.flag;
      v.push_back(it);
    }
  mpv_free_node_contents(&list);
  return v;
}

// ---- surfaces ----------------------------------------------------------------------------

namespace {

bool debug() {
  static int d = -1;
  if (d < 0) d = getenv("FLD_VIDEO_DEBUG") ? 1 : 0;
  return d;
}

void *get_proc_address(void *, const char *name) {
  void *p = (void *)eglGetProcAddress(name);
  if (!p) p = dlsym(RTLD_DEFAULT, name);
  return p;
}

class GlSurface : public Fl_Gl_Window, public VideoSurface {
public:
  Player *player;
  mpv_render_context *ctx = nullptr;
  bool failed = false;
  GlSurface(Player *p, int x, int y, int w, int h) : Fl_Gl_Window(x, y, w, h), player(p) {
    mode(FL_RGB8 | FL_DOUBLE);
    end();
  }
  ~GlSurface() override { shutdown(); }
  Fl_Widget *widget() override { return this; }
  const char *kind() const override { return "OpenGL"; }

  static void update_cb(void *d) { Fl::awake(update_main, d); }
  static void update_main(void *d) {
    GlSurface *s = (GlSurface *)d;
    if (s->ctx && (mpv_render_context_update(s->ctx) & MPV_RENDER_UPDATE_FRAME)) s->redraw();
  }

  void create_ctx() {
    if (ctx) return;
    mpv_opengl_init_params gl{get_proc_address, nullptr};
    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL},
                                 {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    if (int err = mpv_render_context_create(&ctx, player->handle(), params); err < 0) {
      if (debug()) fprintf(stderr, "fl-video: mpv OpenGL renderer: %s\n", mpv_error_string(err));
      ctx = nullptr;
      fail();
      return;
    }
    mpv_render_context_set_update_callback(ctx, update_cb, this);
  }

  void fail() {
    if (failed) return;
    failed = true;
    if (on_failed) Fl::add_timeout(0, [](void *d) { ((GlSurface *)d)->on_failed(); }, this);
  }

  void start() override {
    if (ctx || failed) return;
    Fl::flush(); // may draw, and draw() creates the renderer
    if (ctx || failed) return;
    if (!shown() || !context_valid_or_make()) {
      if (debug()) fprintf(stderr, "fl-video: no OpenGL context (shown %d)\n", shown());
      fail();
      return;
    }
    const GLubyte *ver = glGetString(GL_VERSION);
    if (debug()) fprintf(stderr, "fl-video: GL context %p, version %s\n", (void *)context(), ver ? (const char *)ver : "(none)");
    if (!ver || !*ver) {
      fail();
      return;
    }
    create_ctx();
  }

  bool context_valid_or_make() {
    make_current();
    return context() != nullptr;
  }

  void draw() override {
    if (!ctx && !failed && player->ok()) create_ctx();
    if (!ctx) {
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      return;
    }
    mpv_opengl_fbo fbo{0, pixel_w(), pixel_h(), 0};
    int flip = 1;
    mpv_render_param params[] = {{MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
                                 {MPV_RENDER_PARAM_FLIP_Y, &flip},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context_render(ctx, params);
  }

  void shutdown() override {
    if (!ctx) return;
    if (shown()) make_current();
    mpv_render_context_set_update_callback(ctx, nullptr, nullptr);
    mpv_render_context_free(ctx);
    ctx = nullptr;
  }

  int handle(int e) override {
    if (on_event) {
      int r = on_event(e);
      if (r) return r;
    }
    return Fl_Gl_Window::handle(e);
  }
};

class SwSurface : public Fl_Widget, public VideoSurface {
public:
  Player *player;
  mpv_render_context *ctx = nullptr;
  std::vector<unsigned char> buf, rgb;
  SwSurface(Player *p, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), player(p) {
    if (!p->ok()) return;
    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    if (int err = mpv_render_context_create(&ctx, p->handle(), params); err < 0) {
      if (debug()) fprintf(stderr, "fl-video: mpv software renderer: %s\n", mpv_error_string(err));
      ctx = nullptr;
    } else {
      if (debug()) fprintf(stderr, "fl-video: using the software renderer\n");
      mpv_render_context_set_update_callback(ctx, update_cb, this);
    }
  }
  ~SwSurface() override { shutdown(); }
  Fl_Widget *widget() override { return this; }
  const char *kind() const override { return "software"; }

  static void update_cb(void *d) { Fl::awake(update_main, d); }
  static void update_main(void *d) {
    SwSurface *s = (SwSurface *)d;
    if (s->ctx && (mpv_render_context_update(s->ctx) & MPV_RENDER_UPDATE_FRAME)) s->redraw();
  }

  void draw() override {
    int W = w(), H = h();
    if (!ctx || W <= 0 || H <= 0) {
      fl_color(FL_BLACK);
      fl_rectf(x(), y(), W, H);
      return;
    }
    int size[2] = {W, H};
    size_t stride = (size_t)W * 4;
    buf.resize(stride * (size_t)H);
    mpv_render_param params[] = {{MPV_RENDER_PARAM_SW_SIZE, size},
                                 {MPV_RENDER_PARAM_SW_FORMAT, (void *)"rgb0"},
                                 {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                                 {MPV_RENDER_PARAM_SW_POINTER, buf.data()},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    if (mpv_render_context_render(ctx, params) < 0) {
      fl_color(FL_BLACK);
      fl_rectf(x(), y(), W, H);
      return;
    }
    // rgb0 -> rgb (fl_draw_image treats a 4th byte as alpha)
    rgb.resize((size_t)W * H * 3);
    const unsigned char *s = buf.data();
    unsigned char *d = rgb.data();
    for (size_t i = 0, n = (size_t)W * H; i < n; i++, s += 4, d += 3) {
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
    }
    fl_draw_image(rgb.data(), x(), y(), W, H, 3);
  }

  void shutdown() override {
    if (!ctx) return;
    mpv_render_context_set_update_callback(ctx, nullptr, nullptr);
    mpv_render_context_free(ctx);
    ctx = nullptr;
  }

  int handle(int e) override {
    if (on_event) {
      int r = on_event(e);
      if (r) return r;
    }
    return e == FL_PUSH || e == FL_ENTER || e == FL_MOVE || e == FL_FOCUS ? 1 : Fl_Widget::handle(e);
  }
};

} // namespace

VideoSurface *VideoSurface::create(Player *p, int x, int y, int w, int h, bool force_sw) {
  if (!force_sw && Fl_Gl_Window::can_do(FL_RGB8 | FL_DOUBLE)) return new GlSurface(p, x, y, w, h);
  return new SwSurface(p, x, y, w, h);
}
