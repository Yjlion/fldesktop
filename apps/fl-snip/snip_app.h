// fl-snip: what its units share. main.cpp has the settings, the main
// window and the command line; capture.cpp grabs the screen and runs the
// selection overlay; glyphs.cpp draws the tool icons; output.cpp saves and
// copies; editor.cpp is the editor.
#pragma once
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Copy_Surface.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/mime.h"
#include "fld/notify.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "snip_util.h"
#ifdef FLD_HAVE_RECORD
#include "record.h"
#endif

using snip::Image;
using snip::Point;
using snip::Rect;

double now_s();  // monotonic seconds

// Pick and Ruler are tools (fl-snip --pick-color, --ruler), not ways to snip
enum class Mode { Rect, Freeform, Window, Full, Pick, Ruler };

struct Settings {
  bool copy = true;      // copy each snip to the clipboard
  bool autosave = true;  // save each snip to Pictures/Screenshots
  bool cursor = false;   // include the pointer
  bool notify = true;    // a notification after a quick snip
  Mode mode = Mode::Rect;
  int delay = 0;
  static std::string path() { return fld::fld_config_dir() + "/snip.ini"; }
  void load() {
    fld::Ini c;
    c.load(path());
    copy = c.get_bool("Snip", "CopyToClipboard", copy);
    autosave = c.get_bool("Snip", "AutoSave", autosave);
    cursor = c.get_bool("Snip", "IncludePointer", cursor);
    notify = c.get_bool("Snip", "Notify", notify);
    mode = (Mode)std::max(0, std::min(3, c.get_int("Snip", "Mode", 0)));
    delay = std::max(0, std::min(30, c.get_int("Snip", "Delay", 0)));
  }
  void save() const {
    fld::Ini c;
    c.load(path());
    c.set_bool("Snip", "CopyToClipboard", copy);
    c.set_bool("Snip", "AutoSave", autosave);
    c.set_bool("Snip", "IncludePointer", cursor);
    c.set_bool("Snip", "Notify", notify);
    c.set_int("Snip", "Mode", (int)mode);
    c.set_int("Snip", "Delay", delay);
    c.save(path());
  }
};
extern Settings S;

const char *mode_name(Mode m);
std::string screenshots_dir();

// ---- capture.cpp
struct Capture {
  Image img;
  int lx = 0, ly = 0, lw = 0, lh = 0; // the output layout (logical pixels)
  double sx = 1, sy = 1;              // layout to image pixels
};
bool grab_screen(Capture &cap, std::string &err);
std::vector<snip::Window> wm_windows();  // fl-wm's windows, in layout coordinates
std::unique_ptr<Fl_RGB_Image> rgb_image(const Image &img);
// captures after the delay and lets the user choose; empty when cancelled
Image take_snip(Mode mode, int delay, std::string &err);
int run_tool(Mode mode);  // the colour picker and the ruler

// ---- glyphs.cpp: small drawn glyphs for the tool and mode buttons
enum class Glyph { Rect, Freeform, Window, Full, Pen, Highlighter, Line, Arrow, Box, Ellipse, Text, Pixelate,
                   Crop, Eraser };
void draw_arrow_head(double x1, double y1, double x2, double y2, double len);
void draw_glyph(Glyph g, int x, int y, int s, Fl_Color c);

class GlyphButton : public fld::FlatButton {
public:
  Glyph glyph;
  GlyphButton(int x, int y, int w, int h, Glyph g, const char *tip) : fld::FlatButton(x, y, w, h), glyph(g) {
    tooltip(tip);
  }

protected:
  void draw() override {
    fld::FlatButton::draw();
    int s = std::min(w(), h()) - 12;
    draw_glyph(glyph, x() + (w() - s) / 2, y() + (h() - s) / 2, s,
               active_r() ? (active_look() ? fld::palette().accent : fld::palette().fg) : fld::palette().fg_dim);
  }
};

// ---- output.cpp
bool write_png(const Image &img, const std::string &path);
std::string auto_save(const Image &img);
void copy_image(const Image &img, bool lasting);

// ---- editor.cpp
class EditorWindow;
extern std::vector<EditorWindow *> g_editors;
EditorWindow *open_editor(Image img, const std::string &saved_path);

// ---- main.cpp
void new_snip(Mode mode, int delay);
void show_settings();
