// GStreamer video in FLTK: an appsink-based video sink and the widgets that
// show its frames (OpenGL texture, or software drawing as a fallback).
#pragma once
#include <FL/Fl_Widget.H>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

typedef struct _GstElement GstElement;
typedef struct _GstSample GstSample;

namespace fgst {

// Initializes GStreamer once; false (with a message) when it cannot start.
bool init(std::string *err = nullptr);
// Whether an element factory exists (a plugin is installed).
bool have_element(const char *factory);

// One RGBA frame, rows packed (stride = 4 * w).
struct Frame {
  int w = 0, h = 0;
  double par = 1.0; // pixel aspect ratio
  std::vector<uint8_t> rgba;
  uint64_t serial = 0;
};

// The newest frame of a sink, shared between GStreamer's streaming thread and
// the FLTK thread.
class FrameSlot {
public:
  void put(GstSample *sample); // streaming thread
  // a picture of its own (a music file's cover), until the next frame
  void set(std::shared_ptr<Frame> frame);
  std::shared_ptr<const Frame> get() const;
  void clear();
  // called in the FLTK thread after a new frame arrived
  std::function<void()> on_frame;

private:
  mutable std::mutex mu_;
  std::shared_ptr<const Frame> frame_;
  uint64_t serial_ = 0;
  static void awake_cb(void *);
};

// A sink element (a bin: convert/scale -> RGBA -> appsink) that delivers
// frames to `slot`. The bin keeps a pointer to the slot: the slot must
// outlive the pipeline. Optionally scales in the pipeline (software
// rendering): set_scale_to().
GstElement *make_sink(FrameSlot *slot, const char *name = "fld-video-sink");
// Asks the sink to deliver frames at most W x H (keeping the aspect ratio);
// 0 x 0 for the source size.
void set_scale_to(GstElement *sink, int W, int H);

// Text drawn over the video (playback messages, a recording indicator).
struct Osd {
  std::string text;          // centred near the top, disappears at `until`
  double until = 0;          // monotonic seconds
  std::string corner;        // top-left, stays (e.g. "● REC 00:12")
  bool corner_alert = false; // draw the corner text in red
  std::string placeholder;   // shown centred when there is no frame
  std::string placeholder_icon = "audio-x-generic"; // above it
  bool mirror = false;       // mirror the picture horizontally
};

double now();

// The video area. create() picks OpenGL when FLTK can do it.
class FrameView {
public:
  virtual ~FrameView(); // removes the OSD timer
  virtual Fl_Widget *widget() = 0;
  virtual const char *kind() const = 0;
  virtual bool software() const = 0;
  // call once the window is shown; calls on_failed if OpenGL does not work
  virtual void start() {}
  void set_slot(FrameSlot *s) { slot_ = s; }
  Osd &osd() { return osd_; }
  void show_message(const std::string &text, double secs = 2.0);
  void redraw() { widget()->redraw(); }
  // the picture's place inside the widget (letterboxed)
  static void fit(int fw, int fh, double par, int W, int H, int &x, int &y, int &w, int &h);

  std::function<int(int)> on_event;  // mouse / keyboard events
  std::function<void()> on_failed;   // OpenGL did not start
  std::function<void(int, int)> on_resize; // new widget size (software: rescale)

  static FrameView *create(int x, int y, int w, int h, bool force_sw);

protected:
  FrameSlot *slot_ = nullptr;
  Osd osd_;
  // draws the OSD with FLTK (both renderers)
  void draw_osd(int X, int Y, int W, int H);
  static void osd_timeout(void *);
};

} // namespace fgst
