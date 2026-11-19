// The rendering backend of a web view: creates the WebKitWebView with a
// view backend that renders into memory, delivers frames, and forwards
// input. Two implementations, picked at build time:
//   backend_platform.cpp  WPE WebKit >= 2.46 built with the WPE Platform API
//   backend_fdo.cpp       WPE WebKit with libwpe + WPEBackend-fdo (SHM export)
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wpe/webkit.h>

namespace web {

// premultiplied BGRA (little endian ARGB8888) rows
struct Frame {
  int width = 0, height = 0, stride = 0;
  std::vector<uint8_t> pixels;
};

enum Mods { MOD_CTRL = 1, MOD_SHIFT = 2, MOD_ALT = 4, MOD_META = 8, MOD_BUTTON1 = 16, MOD_BUTTON2 = 32, MOD_BUTTON3 = 64 };

class Backend {
public:
  virtual ~Backend() = default;
  // A backend for one web view; create_view() makes the view (a view opened
  // by a page, window.open, is created related to its opener).
  static std::unique_ptr<Backend> create();
  virtual WebKitWebView *create_view(WebKitWebView *related, WebKitSettings *settings,
                                     WebKitNetworkSession *session, WebKitUserContentManager *ucm) = 0;

  virtual void resize(int width, int height) = 0;
  virtual void set_focus(bool on) = 0;
  virtual void set_visible(bool on) = 0;
  // x, y in view coordinates; button 1 left, 2 middle, 3 right
  virtual void pointer_button(bool down, unsigned button, double x, double y, unsigned mods, unsigned clicks,
                              uint32_t time) = 0;
  virtual void pointer_motion(double x, double y, unsigned mods, uint32_t time) = 0;
  virtual void pointer_leave(double x, double y, unsigned mods, uint32_t time) = 0;
  // deltas in pixels, positive towards the end of the page (like a wheel turned down)
  virtual void scroll(double dx, double dy, double x, double y, unsigned mods, uint32_t time) = 0;
  // keysym: an XKB keysym; keycode: the evdev code + 8 (0 when unknown)
  virtual void key(bool down, unsigned keysym, unsigned keycode, unsigned mods, uint32_t time) = 0;
  // the last frame has been shown: WebKit may render the next one
  virtual void frame_shown() = 0;
  // the system clipboard's text, for a paste in the page (only the WPE
  // Platform backend shares the clipboard; libwpe keeps its own)
  virtual void set_clipboard_text(const std::string &) {}
  virtual bool shares_clipboard() const { return false; }

  // newest frame (called in the main thread)
  std::function<void(std::shared_ptr<const Frame>)> on_frame;
  std::function<void(const std::string &cursor)> on_cursor; // CSS cursor names
  static std::function<void(const std::string &text)> on_copy; // the page copied text
};

// Name of the backend for --version ("WPE Platform" or "WPEBackend-fdo")
const char *backend_name();

} // namespace web
