// mpv playback core and its FLTK rendering surfaces
#pragma once
#include <FL/Fl_Gl_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Widget.H>
#include <functional>
#include <string>
#include <vector>

struct mpv_handle;
struct mpv_render_context;
struct mpv_event;

// Owns the mpv instance. Events are delivered in the FLTK thread.
class Player {
public:
  Player();
  ~Player();
  bool ok() const { return mpv_ != nullptr; }
  mpv_handle *handle() const { return mpv_; }

  void load(const std::string &file, bool append = false);
  void command(const std::vector<std::string> &args);
  void set_flag(const char *prop, bool v);
  void set_double(const char *prop, double v);
  void set_string(const char *prop, const std::string &v);
  bool get_flag(const char *prop) const;
  double get_double(const char *prop, double def = 0) const;
  long get_int(const char *prop, long def = 0) const;
  std::string get_string(const char *prop) const;

  struct Track {
    long id;
    std::string type, title, lang;
    bool selected;
  };
  std::vector<Track> tracks() const;
  struct PlaylistItem {
    std::string filename, title;
    bool current;
  };
  std::vector<PlaylistItem> playlist() const;

  // Called in the FLTK thread
  std::function<void(const std::string &prop)> on_property; // an observed property changed
  std::function<void()> on_file_loaded, on_idle, on_shutdown;
  std::function<void(const std::string &msg)> on_error;

private:
  mpv_handle *mpv_ = nullptr;
  static void wakeup(void *);
  static void drain(void *);
  void handle_event(mpv_event *e);
};

// The video area: an OpenGL window when possible, otherwise software
// rendering into an image. Both redraw when mpv has a new frame.
class VideoSurface {
public:
  virtual ~VideoSurface() = default;
  virtual Fl_Widget *widget() = 0;
  virtual const char *kind() const = 0;
  // Releases the render context (before the Player goes away)
  virtual void shutdown() = 0;
  // Call once the window is shown: sets up rendering, or calls on_failed.
  virtual void start() {}
  static VideoSurface *create(Player *p, int x, int y, int w, int h, bool force_sw);
  std::function<int(int)> on_event; // mouse/keyboard events from the surface
  std::function<void()> on_failed;  // the OpenGL renderer could not start
};
