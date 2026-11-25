// fl-panel: the on-screen display for volume, brightness, the microphone,
// Caps Lock and the keyboard layout. A borderless window titled
// "fldesktop.osd": fl-wm keeps it above everything at the bottom of the
// screen and never gives it the keyboard. It hides itself after a moment.
#include <FL/Fl_Double_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <string>

#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/text.h"
#include "panel.h"

namespace {

const int W = 300, H = 64; // wide enough for "Built-in Audio Analog Stereo"
const double kShowSeconds = 1.6;

class OsdWindow : public Fl_Double_Window {
public:
  std::string icon, text;
  int level = -1; // 0..100 (up to 150 for volume), -1: none
  OsdWindow() : Fl_Double_Window(W, H, "fldesktop.osd") {
    border(0);
    end();
  }
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_color(p.bg2);
    fl_rectf(0, 0, w(), h());
    fl_color(p.border);
    fl_rect(0, 0, w(), h());
    int x = 14;
    if (Fl_Image *img = fld::symbolic_icon(icon, 32, p.fg)) {
      img->draw(x, (h() - 32) / 2);
    } else if (Fl_Image *img2 = fld::icon(icon, 32)) {
      img2->draw(x, (h() - 32) / 2);
    }
    x += 32 + 14;
    int right = w() - 14;
    fl_color(p.fg);
    if (level >= 0) {
      // the bar, with the value next to it
      fl_font(FL_HELVETICA, fld::ui_font_size());
      char buf[16];
      snprintf(buf, sizeof buf, "%d%%", level);
      int tw = (int)fl_width("150%");
      int bw = right - x - tw - 10, by = h() / 2 - 3;
      fl_color(fl_color_average(p.fg_dim, p.bg2, 0.35f));
      fl_rectf(x, by, bw, 6);
      fl_color(p.accent);
      fl_rectf(x, by, std::min(bw, bw * std::min(level, 100) / 100), 6);
      fl_color(p.fg);
      fl_draw(buf, right - tw, 0, tw, h(), FL_ALIGN_RIGHT);
      if (!text.empty()) {
        fl_font(FL_HELVETICA, fld::ui_font_size() - 2);
        fl_color(p.fg_dim);
        // the whole width up to the edge (the value is on the bar's line);
        // a long device name ends with "…" instead of being cut off
        std::string t = fld::ellipsize(text, right - x);
        fl_draw(fld::escape_at(t).c_str(), x, 4, right - x, by - 6, FL_ALIGN_LEFT | FL_ALIGN_BOTTOM | FL_ALIGN_CLIP);
      }
    } else {
      fl_font(FL_HELVETICA_BOLD, fld::ui_font_size() + 1);
      fl_draw(text.c_str(), x, 0, right - x, h(), FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    }
  }
  int handle(int e) override {
    // a click hides it at once
    if (e == FL_PUSH) {
      hide();
      return 1;
    }
    return Fl_Double_Window::handle(e);
  }
};

OsdWindow *g_win = nullptr;

void hide_cb(void *) {
  if (g_win) g_win->hide();
}

} // namespace

void osd_show(const std::string &icon, int level, const std::string &text) {
  if (!g_win) {
    Fl_Group *cur = Fl_Group::current();
    Fl_Group::current(nullptr);
    g_win = new OsdWindow();
    Fl_Group::current(cur);
  }
  g_win->icon = icon;
  g_win->level = level;
  g_win->text = text;
  g_win->redraw();
  if (!g_win->shown()) g_win->show();
  Fl::remove_timeout(hide_cb);
  Fl::add_timeout(kShowSeconds, hide_cb);
}
