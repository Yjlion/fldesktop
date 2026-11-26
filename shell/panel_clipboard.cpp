// fl-panel: the clipboard history (recorded by fl-cliphist). A click on the
// button lists what was copied; a click on an entry puts it on the
// clipboard again; entries can be pinned (kept) and removed.
#include "fld/i18n.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "cliphist_store.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/util.h"
#include "panel.h"

namespace {

fld::FlatButton *g_btn = nullptr;
std::vector<cliphist::Entry> g_entries;

bool history_on() {
  fld::Ini c;
  c.load(fld::fld_config_dir() + "/panel.ini");
  return c.get_bool("Clipboard", "History", true);
}

std::string age(time_t t) {
  long d = (long)(time(nullptr) - t);
  if (d < 60) return "just now";
  if (d < 3600) return std::to_string(d / 60) + " min ago";
  if (d < 86400) return std::to_string(d / 3600) + " h ago";
  return fld::format_time(t, "%d %b %H:%M");
}

class Row : public Fl_Widget {
public:
  cliphist::Entry e;
  std::unique_ptr<Fl_Image> thumb;
  static const int TEXT_H = 50, IMAGE_H = 84;

  Row(int X, int Y, int W, const cliphist::Entry &en)
      : Fl_Widget(X, Y, W, en.is_image() ? IMAGE_H : TEXT_H), e(en) {
    if (e.is_image()) {
      std::string mime, data;
      if (cliphist::Store().data(e.id, mime, data) && mime == "image/png") {
        Fl_PNG_Image png(nullptr, (const unsigned char *)data.data(), (int)data.size());
        if (png.w() > 0 && png.h() > 0) {
          int th = IMAGE_H - 16, tw = std::max(1, png.w() * th / png.h());
          if (tw > 120) {
            tw = 120;
            th = std::max(1, png.h() * tw / png.w());
          }
          thumb.reset(png.copy(tw, th));
        }
      }
    }
  }

  // the pin and remove buttons, from the right
  int pin_x() const { return x() + w() - 54; }
  int del_x() const { return x() + w() - 28; }

  void draw() override {
    const fld::Palette &p = fld::palette();
    bool hover = Fl::belowmouse() == this;
    fl_color(hover ? fl_color_average(p.bg2, p.accent, 0.85f) : p.bg2);
    fl_rectf(x(), y(), w(), h());
    fl_color(p.border);
    fl_xyline(x() + 8, y() + h() - 1, x() + w() - 8);
    int tx = x() + 10, tw = pin_x() - tx - 6;
    if (thumb) {
      thumb->draw(tx, y() + 8);
      tx += thumb->w() + 10;
      tw = pin_x() - tx - 6;
    }
    fl_color(p.fg);
    fl_font(FL_HELVETICA, 13);
    std::string text = fld::escape_at(e.preview);
    fl_push_clip(tx, y() + 4, tw, h() - 22);
    fl_draw(text.c_str(), tx, y() + 6, tw, h() - 26, FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE,
            nullptr, 0);
    fl_pop_clip();
    fl_font(FL_HELVETICA, 11);
    fl_color(p.fg_dim);
    std::string meta = age(e.time) + (e.pinned ? " · pinned" : "");
    fl_draw(meta.c_str(), tx, y() + h() - 20, tw, 16, FL_ALIGN_LEFT | FL_ALIGN_INSIDE, nullptr, 0);
    if (Fl_Image *pin = fld::symbolic_icon("view-pin", 16, e.pinned ? p.accent : p.fg_dim))
      pin->draw(pin_x(), y() + (h() - 16) / 2);
    if (Fl_Image *del = fld::symbolic_icon("edit-delete", 16, p.fg_dim)) del->draw(del_x(), y() + (h() - 16) / 2);
  }

  bool in(int bx) const {
    int my = y() + h() / 2;
    return Fl::event_x() >= bx - 4 && Fl::event_x() <= bx + 20 && Fl::event_y() >= my - 12 && Fl::event_y() <= my + 12;
  }

  int handle(int ev) override {
    if (ev == FL_ENTER || ev == FL_LEAVE) {
      redraw();
      return 1;
    }
    if (ev == FL_PUSH) return 1;
    if (ev != FL_RELEASE) return 0;
    cliphist::Store store;
    if (in(pin_x())) {
      e.pinned = !e.pinned;
      store.set_pinned(e.id, e.pinned);
      redraw();
    } else if (in(del_x())) {
      store.remove(e.id);
      hide();
      // the entries below move up
      Fl_Group *gp = parent();
      for (int i = 0; i < gp->children(); i++) {
        Fl_Widget *c = gp->child(i);
        if (c->visible() && c->y() > y() && c != this) c->position(c->x(), c->y() - h());
      }
      gp->redraw();
    } else {
      fld::spawn({"fl-cliphist", "--copy", std::to_string(e.id)});
      window()->hide();
    }
    return 1;
  }
};

void clipboard_cb(Fl_Widget *w, void *) {
  if (panel_popup_toggle(w)) return; // open: a second click closes it
  g_entries = cliphist::Store().list();
  const int W = 380;
  int list_h = 0;
  for (auto &e : g_entries) list_h += e.is_image() ? Row::IMAGE_H : Row::TEXT_H;
  int H = 44 + std::max(60, std::min(460, list_h)) + 1;
  PanelPopup pop(W, H);
  const fld::Palette &p = fld::palette();
  Fl_Box *title = new Fl_Box(10, 8, 200, 28, _("Clipboard"));
  title->labelfont(FL_HELVETICA_BOLD);
  title->labelsize(14);
  title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  Fl_Button *clear = new Fl_Button(W - 82, 9, 72, 26, _("Clear"));
  clear->labelsize(12);
  clear->tooltip(_("Remove everything except the pinned entries"));
  clear->callback([](Fl_Widget *b, void *) {
    cliphist::Store().clear(true);
    b->window()->hide();
  });
  bool any_unpinned = false;
  for (auto &e : g_entries) any_unpinned |= !e.pinned;
  if (!any_unpinned) clear->deactivate();
  Fl_Scroll *sc = new Fl_Scroll(1, 44, W - 2, H - 45);
  sc->type(Fl_Scroll::VERTICAL);
  sc->box(FL_FLAT_BOX);
  sc->color(p.bg2);
  if (g_entries.empty()) {
    Fl_Box *none = new Fl_Box(1, 44, W - 2, 60, history_on() ? "Nothing copied yet" : "The clipboard history is off");
    none->labelcolor(p.fg_dim);
    none->labelsize(13);
  }
  int y = 44;
  for (auto &e : g_entries) {
    Row *r = new Row(1, y, W - 18, e);
    y += r->h();
  }
  sc->end();
  pop.end();
  pop.popup_at(w, false);
}

} // namespace

fld::FlatButton *clipboard_indicator_create() {
  g_btn = new fld::FlatButton(0, 0, 34, PANEL_H - 4);
  g_btn->color(panel_bg());
  g_btn->labelcolor(panel_fg());
  g_btn->hover_color(panel_hover());
  if (Fl_Image *i = fld::symbolic_icon("edit-paste", 18, panel_fg())) g_btn->icon_image(i);
  else g_btn->icon_name("edit-paste", 20);
  g_btn->tooltip(_("Clipboard history"));
  g_btn->callback(clipboard_cb);
  panel_context_menu(g_btn, [](fld::Menu &m) {
    bool on = history_on();
    m.toggle("Keep a Clipboard History", on, [on] {
      fld::Ini c;
      std::string path = fld::fld_config_dir() + "/panel.ini";
      c.load(path);
      c.set_bool("Clipboard", "History", !on);
      c.save(path);
    });
    m.item(_("Clear History"), [] { cliphist::Store().clear(true); }).divider();
    panel_menu_items(m);
  });
  fld::Ini c;
  c.load(fld::fld_config_dir() + "/panel.ini");
  if (fld::which("fl-cliphist").empty() || !c.get_bool("Clipboard", "Button", true)) g_btn->hide();
  return g_btn;
}
