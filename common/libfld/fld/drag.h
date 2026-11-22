// Dragging out of a window (files, tabs) with a picture under the pointer.
//
// On Wayland FLTK's Fl::dnd() shows only a cursor and never learns that a
// drop on another window has ended, so a view stays in its "dragging" state.
// start_drag() makes the drag itself: it offers the text (and text/uri-list),
// shows a drag image (icons with a count badge, or a label "chip" for tabs),
// and reports the end of the drag. The data is served from a thread, so a
// drop onto a window of the same program (FLTK reads it while blocking the
// main loop) works. Elsewhere (X11) it falls back to Fl::dnd().
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Fl_Image;
class Fl_Window;

namespace fld {

struct DragImage {
  std::vector<Fl_Image *> icons; // the first 3 are stacked (front first)
  int count = 0;                 // the badge's number (none below 2)
  std::string label;             // no icons: a rounded chip with this text
  int size = 48;                 // icon size in logical pixels
};

enum class DragEnd { Dropped, Cancelled };

// Binds what a drag needs (seat, pointer, data device). Must have run before
// the button press that starts a drag; it is cheap to call again. Widgets
// that start drags schedule it from their constructor.
void drag_setup();
void drag_setup_later();  // the same from the event loop, once the display is open
bool drag_active();

// Starts dragging `text` from `origin`: call it from FL_DRAG with a button
// held. `uri_list`: the text is file URIs (offered as text/uri-list too).
// `done` runs in the FLTK thread when the drag is over. Returns false when no
// drag could start (done is not called then).
bool start_drag(Fl_Window *origin, const std::string &text, bool uri_list, const DragImage &img,
                std::function<void(DragEnd)> done);

// The drag image as ARGB32 premultiplied pixels (stride = w), at `scale`
// buffer pixels per logical pixel; `copy_badge` adds a "+" (Ctrl held).
struct DragPixels {
  int w = 0, h = 0;
  int hot_x = 0, hot_y = 0; // the pointer's place in the image (logical)
  std::vector<uint32_t> argb;
};
DragPixels render_drag_image(const DragImage &img, int scale, bool copy_badge);

} // namespace fld
