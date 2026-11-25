// Shows WebKit's GPU buffers (DMA-BUF) directly: each web view gets a
// Wayland subsurface of its window, and the buffers WebKit renders into are
// handed to the compositor as they are. Nothing is copied: before this,
// every frame was read back from the GPU, converted and drawn by FLTK, and
// then uploaded again by the compositor.
//
// Needs FLTK on Wayland and a compositor with zwp_linux_dmabuf_v1,
// wl_subcompositor and wp_viewporter (fl-wm with its GPU renderer). Without
// them, or with FL_WEB_READBACK=1, fl-web reads the frames back as before.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "damage.h"

class Fl_Window;
struct wl_buffer;
struct wl_callback;
struct wl_subsurface;
struct wl_surface;
struct wp_viewport;

namespace web {

// One DMA-BUF of a frame (up to 4 planes; the fds are the caller's)
struct Dmabuf {
  int width = 0, height = 0;
  uint32_t fourcc = 0;
  uint64_t modifier = 0;
  int planes = 0;
  int fd[4] = {-1, -1, -1, -1};
  uint32_t stride[4] = {}, offset[4] = {};
};

namespace wl_present {

// Direct presentation is possible (decided once, after FLTK opened its display)
bool available();
// The compositor takes this format with this modifier
bool supports(uint32_t fourcc, uint64_t modifier);
// A buffer identified by `key` is gone: its wl_buffer is destroyed
void forget(const void *key);

// The window drawn over the pages of `top` (an FLTK subwindow, or null):
// pages are kept below it
void set_overlay(Fl_Window *top, Fl_Window *overlay);
// A shown subwindow takes no pointer input: it goes to what is below
void pass_input(Fl_Window *w);
// an overlay of `top` was shown: the pages go below it again
void overlays_changed(Fl_Window *top);

class Presenter {
public:
  Presenter();
  ~Presenter(); // calls the callbacks still waiting
  Presenter(const Presenter &) = delete;
  Presenter &operator=(const Presenter &) = delete;

  // where the page is: x, y, w, h in `win`'s coordinates (the window must be shown)
  void place(Fl_Window *win, int x, int y, int w, int h);
  // shown or not (an inactive tab, a hidden widget); the last frame is kept
  void set_mapped(bool on);
  bool mapped() const { return mapped_; }

  // Shows a buffer. `key` identifies it while WebKit reuses it (its wl_buffer
  // is made once). `shown` is called once the frame is on screen (or at once,
  // a little later, while unmapped), `release` once the compositor no longer
  // reads it. Returns false when the compositor cannot take it (then neither
  // callback is called and the caller copies the frame instead).
  bool show(const void *key, const Dmabuf &buf, const std::vector<Rect> &damage, bool all,
            std::function<void()> shown, std::function<void()> release);

  // called by the module
  struct Entry {
    wl_buffer *buffer = nullptr;
    std::function<void()> release; // WebKit's buffer is ours until this is called
    Presenter *owner = nullptr;
    const void *key = nullptr;
    bool attached = false; // the compositor holds it
  };
  void buffer_released(Entry *e);
  void frame_done();
  void drop(const void *key);
  void restack_in(Fl_Window *top);

private:
  bool ensure_surface();
  void destroy_surface();
  void attach_current(bool full_damage, const std::vector<Rect> &damage);
  void restack();
  void give_back_others();
  static void shown_later(void *);

  Fl_Window *win_ = nullptr;
  wl_surface *parent_ = nullptr; // the window's surface the subsurface belongs to
  wl_surface *surface_ = nullptr;
  wl_subsurface *sub_ = nullptr;
  wp_viewport *viewport_ = nullptr;
  wl_callback *frame_cb_ = nullptr;
  int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
  int placed_x_ = -1, placed_y_ = -1, dest_w_ = -1, dest_h_ = -1;
  bool mapped_ = true;
  std::map<const void *, Entry *> entries_;
  Entry *current_ = nullptr;          // the frame to show
  std::function<void()> shown_;       // waiting for the frame callback (or the timer)
};

} // namespace wl_present
} // namespace web
