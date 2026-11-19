// fl-panel: the notification bell (history and do not disturb of fl-notifyd)
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#include "fld/icons.h"
#include "fld/notify.h"
#include "fld/util.h"
#include "panel.h"

namespace {

// The bell with a dot when there are unseen notifications.
class BellButton : public fld::FlatButton {
public:
  using fld::FlatButton::FlatButton;
  int unseen = 0;
  bool dnd = false;
protected:
  void draw() override {
    fld::FlatButton::draw();
    if (unseen > 0 && !dnd) {
      int r = 7;
      int cx = x() + w() - 10, cy = y() + 9;
      fl_color(fld::palette().accent);
      fl_pie(cx - r / 2 - 1, cy - r / 2 - 1, r + 2, r + 2, 0, 360);
    }
  }
};

BellButton *g_bell = nullptr;
std::vector<fld::HistoryEntry> g_list;
bool g_dnd = false, g_available = false;
uint32_t g_seen_id = 0; // newest id the user has seen in the popup
bool g_first = true;

void update_button() {
  int unseen = 0;
  for (auto &e : g_list)
    if (e.id > g_seen_id) unseen++;
  g_bell->unseen = unseen;
  g_bell->dnd = g_dnd;
  Fl_Color fg = panel_fg();
  Fl_Image *img = g_dnd ? fld::symbolic_icon("notifications-disabled", 18, fg)
                        : fld::symbolic_icon("preferences-system-notifications", 18, fg);
  g_bell->icon_image(img);
  if (!img) g_bell->copy_label(g_dnd ? "🔕" : "🔔");
  std::string tip = g_dnd ? "Do not disturb" : unseen ? std::to_string(unseen) + " new notifications" : "Notifications";
  g_bell->copy_tooltip(tip.c_str());
  bool vis = g_available;
  if (vis != (bool)g_bell->visible()) {
    vis ? g_bell->show() : g_bell->hide();
    relayout();
  }
  g_bell->redraw();
}

void refresh() {
  fld::NotifyHistory::get().fetch([](const std::vector<fld::HistoryEntry> &list, bool dnd, bool ok) {
    g_list = list;
    g_dnd = dnd;
    g_available = ok;
    if (g_first) {
      // notifications from before the panel started are not "new"
      for (auto &e : g_list) g_seen_id = std::max(g_seen_id, e.id);
      g_first = false;
    }
    update_button();
  });
}

std::string time_ago(int64_t t) {
  time_t now = time(nullptr);
  long d = (long)(now - t);
  if (d < 60) return "now";
  if (d < 3600) return std::to_string(d / 60) + " min ago";
  struct tm a, b;
  time_t tt = (time_t)t;
  localtime_r(&tt, &a);
  localtime_r(&now, &b);
  if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) return fld::format_time(tt, "%H:%M");
  return fld::format_time(tt, "%d %b %H:%M");
}

// one history entry in the popup
class EntryBox : public Fl_Widget {
public:
  fld::HistoryEntry e;
  EntryBox(int X, int Y, int W, int H, const fld::HistoryEntry &en) : Fl_Widget(X, Y, W, H), e(en) {}
  static int height_for(const fld::HistoryEntry &e, int W) {
    int tw = W - 40 - 30, h = 8;
    fl_font(FL_HELVETICA_BOLD, 13);
    int w1 = tw, h1 = 0;
    fl_measure(e.summary.c_str(), w1, h1, 0);
    h += h1;
    if (!e.body.empty()) {
      fl_font(FL_HELVETICA, 12);
      int w2 = tw, h2 = 0;
      fl_measure(e.body.c_str(), w2, h2, 0);
      h += 2 + std::min(h2, 4 * 16);
    }
    h += 16 + 8; // time line
    return std::max(h, 48);
  }
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_color(p.bg2);
    fl_rectf(x(), y(), w(), h());
    fl_color(p.border);
    fl_xyline(x() + 8, y() + h() - 1, x() + w() - 8);
    Fl_Image *ic = nullptr;
    if (!e.icon.empty() && e.icon[0] == '/') ic = nullptr; // paths: not cached here
    else if (!e.icon.empty()) ic = fld::icon(e.icon, 24);
    if (!ic) ic = fld::icon_first({"dialog-information", "preferences-system-notifications"}, 24);
    if (ic) ic->draw(x() + 8, y() + 8);
    int tx = x() + 40, tw = w() - 40 - 30, ty = y() + 8;
    fl_color(p.fg);
    fl_font(FL_HELVETICA_BOLD, 13);
    int w1 = tw, h1 = 0;
    fl_measure(e.summary.c_str(), w1, h1, 0);
    fl_draw(e.summary.c_str(), tx, ty, tw, h1, FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE, nullptr, 0);
    ty += h1 + 2;
    if (!e.body.empty()) {
      fl_font(FL_HELVETICA, 12);
      int w2 = tw, h2 = 0;
      fl_measure(e.body.c_str(), w2, h2, 0);
      h2 = std::min(h2, 4 * 16);
      fl_push_clip(tx, ty, tw, h2);
      fl_color(p.fg_dim);
      fl_draw(e.body.c_str(), tx, ty, tw, h2, FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE, nullptr, 0);
      fl_pop_clip();
      ty += h2;
    }
    fl_font(FL_HELVETICA, 11);
    fl_color(p.fg_dim);
    std::string meta = e.app.empty() ? time_ago(e.time) : e.app + " · " + time_ago(e.time);
    fl_draw(meta.c_str(), tx, ty + 2, tw, 16, FL_ALIGN_LEFT | FL_ALIGN_INSIDE, nullptr, 0);
    // remove cross
    int cx = x() + w() - 22, cy = y() + 10;
    fl_color(p.fg_dim);
    fl_line(cx, cy, cx + 8, cy + 8);
    fl_line(cx + 8, cy, cx, cy + 8);
  }
  int handle(int ev) override {
    if (ev == FL_PUSH) return 1;
    if (ev == FL_RELEASE) {
      int cx = x() + w() - 22, cy = y() + 10;
      if (Fl::event_x() >= cx - 6 && Fl::event_x() <= cx + 14 && Fl::event_y() >= cy - 6 && Fl::event_y() <= cy + 14) {
        fld::NotifyHistory::get().remove(e.id);
        g_list.erase(std::remove_if(g_list.begin(), g_list.end(), [this](auto &x) { return x.id == e.id; }), g_list.end());
        hide();
        // move the entries below up
        Fl_Group *gp = parent();
        int my = y(), mh = h();
        for (int i = 0; i < gp->children(); i++) {
          Fl_Widget *c = gp->child(i);
          if (c->visible() && c->y() > my && c != this) c->position(c->x(), c->y() - mh);
        }
        gp->redraw();
        update_button();
      }
      return 1;
    }
    return 0;
  }
};

void bell_cb(Fl_Widget *w, void *) {
  const int W = 360;
  int list_h = 0;
  for (auto &e : g_list) list_h += EntryBox::height_for(e, W - 18);
  int H = 44 + std::max(60, std::min(440, list_h)) + 1;
  PanelPopup pop(W, H);
  const fld::Palette &p = fld::palette();
  Fl_Box *title = new Fl_Box(10, 8, 140, 28, "Notifications");
  title->labelfont(FL_HELVETICA_BOLD);
  title->labelsize(14);
  title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  Fl_Check_Button *dnd = new Fl_Check_Button(140, 8, 130, 28, "Do not disturb");
  dnd->labelsize(12);
  dnd->value(g_dnd);
  dnd->callback([](Fl_Widget *b, void *) {
    g_dnd = ((Fl_Check_Button *)b)->value();
    fld::NotifyHistory::get().set_dnd(g_dnd);
    update_button();
  });
  Fl_Button *clear = new Fl_Button(W - 82, 9, 72, 26, "Clear all");
  clear->labelsize(12);
  clear->callback([](Fl_Widget *b, void *) {
    fld::NotifyHistory::get().clear();
    g_list.clear();
    update_button();
    b->window()->hide();
  });
  if (g_list.empty()) clear->deactivate();
  Fl_Scroll *sc = new Fl_Scroll(1, 44, W - 2, H - 45);
  sc->type(Fl_Scroll::VERTICAL);
  sc->box(FL_FLAT_BOX);
  sc->color(p.bg2);
  if (g_list.empty()) {
    Fl_Box *none = new Fl_Box(1, 44, W - 2, 60, "No notifications");
    none->labelcolor(p.fg_dim);
    none->labelsize(13);
  }
  int y = 44;
  for (auto &e : g_list) {
    int h = EntryBox::height_for(e, W - 18);
    new EntryBox(1, y, W - 18, h, e);
    y += h;
  }
  sc->end();
  pop.end();
  for (auto &e : g_list) g_seen_id = std::max(g_seen_id, e.id);
  update_button();
  pop.popup_at(w, false);
}

} // namespace

fld::FlatButton *notify_indicator_create() {
  g_bell = new BellButton(0, 0, 34, PANEL_H - 4);
  g_bell->color(panel_bg());
  g_bell->labelcolor(panel_fg());
  g_bell->hover_color(panel_hover());
  g_bell->callback(bell_cb);
  panel_context_menu(g_bell, [](fld::Menu &m) {
    m.toggle("Do Not Disturb", g_dnd, [] {
      g_dnd = !g_dnd;
      fld::NotifyHistory::get().set_dnd(g_dnd);
      update_button();
    });
    m.item("Clear All", [] {
      fld::NotifyHistory::get().clear();
      g_list.clear();
      update_button();
    }, g_list.empty() ? FL_MENU_INACTIVE : 0).divider();
    m.item("Notification Settings…", [] { fld::spawn({"fl-settings", "notifications"}); });
  });
  g_bell->hide();
  fld::NotifyHistory::get().on_changed = refresh;
  fld::NotifyHistory::get().start();
  refresh();
  return g_bell;
}
