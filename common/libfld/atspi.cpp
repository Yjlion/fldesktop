// The AT-SPI2 bridge (fld/atspi.h). Two threads:
//  - the FLTK thread reads the widgets into a Snapshot (atspi_model.h) a few
//    times a second and carries out actions;
//  - a worker thread with its own GLib main context owns the D-Bus
//    connection to the accessibility bus, answers calls from the latest
//    snapshot and sends the events found by comparing snapshots.
// They talk through a mutex-guarded snapshot, a pipe (worker -> FLTK) and
// g_main_context_invoke (FLTK -> worker).
#include "fld/atspi.h"
#include "atspi_model.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Dial.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Light_Button.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Tree.H>
#include <FL/Fl_Valuator.H>
#include <FL/Fl_Window.H>

#include <gio/gio.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "fld/util.h"
#include "fld/widgets.h"

namespace fld {

using namespace atspi;

namespace {

const char *kRootPath = "/org/a11y/atspi/accessible/root";
const char *kBase = "/org/a11y/atspi/accessible";
const char *kNull = "/org/a11y/atspi/null";

// ---- shared state ------------------------------------------------------------------

std::mutex g_mu;
std::shared_ptr<const Snapshot> g_snap; // under g_mu
std::string g_app_name;
std::atomic<bool> g_running{false};

// worker side
GMainContext *g_ctx = nullptr;
GDBusConnection *g_bus = nullptr;
std::string g_unique;                         // our name on the bus
std::string g_parent_bus, g_parent_path;      // the registry's root (from Embed)
int g_app_id = 0;
GDBusNodeInfo *g_info = nullptr;

// FLTK side: work handed over by the worker
int g_pipe[2] = {-1, -1};
std::mutex g_qmu;
std::deque<std::function<void()>> g_queue;

void on_fltk(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> l(g_qmu);
    g_queue.push_back(std::move(fn));
  }
  char c = 1;
  if (write(g_pipe[1], &c, 1) < 0) {}
}

void queue_cb(int fd, void *) {
  char buf[64];
  while (read(fd, buf, sizeof buf) == (ssize_t)sizeof buf) {}
  std::deque<std::function<void()>> q;
  {
    std::lock_guard<std::mutex> l(g_qmu);
    q.swap(g_queue);
  }
  for (auto &f : q) f();
}

void on_worker(std::function<void()> fn) {
  auto *p = new std::function<void()>(std::move(fn));
  g_main_context_invoke_full(
      g_ctx, G_PRIORITY_DEFAULT,
      [](gpointer d) -> gboolean {
        (*(std::function<void()> *)d)();
        return G_SOURCE_REMOVE;
      },
      p, [](gpointer d) { delete (std::function<void()> *)d; });
}

std::shared_ptr<const Snapshot> snap() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_snap;
}

// ---- the FLTK side: reading the widgets ---------------------------------------------

std::map<std::pair<const void *, int>, int> g_ids; // (widget, list item or -1) -> node id
int g_next_id = 1;
std::map<int, Fl_Widget *> g_widget_of; // node -> widget (FLTK thread only)
std::map<int, int> g_item_of;           // node -> list item index (1-based) or tab child
std::map<Fl_Widget *, std::pair<unsigned, std::string>> g_described;
std::map<Fl_Widget *, std::function<std::string()>> g_text_of;

int id_for(const void *w, int item, std::map<std::pair<const void *, int>, int> &seen) {
  auto key = std::make_pair(w, item);
  auto it = g_ids.find(key);
  int id = it != g_ids.end() ? it->second : g_next_id++;
  seen[key] = id;
  return id;
}

// characters (not bytes) in a UTF-8 string, and conversions
int u8_len(const std::string &s) {
  int n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) n++;
  return n;
}
size_t u8_byte(const std::string &s, int chars) {
  size_t i = 0;
  int n = 0;
  while (i < s.size() && n < chars) {
    i++;
    while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    n++;
  }
  return i;
}
int u8_chars(const std::string &s, size_t bytes) { return u8_len(s.substr(0, std::min(bytes, s.size()))); }

struct Builder {
  Snapshot s;
  std::map<std::pair<const void *, int>, int> seen;
  std::map<int, Fl_Widget *> widget_of;
  std::map<int, int> item_of;
  Fl_Widget *focus = Fl::focus();

  void common(Node &n, Fl_Widget *w, int ox, int oy) {
    n.x = ox + w->x();
    n.y = oy + w->y();
    n.w = w->w();
    n.h = w->h();
    if (w->visible()) n.states |= bit(STATE_VISIBLE);
    if (w->visible_r()) n.states |= bit(STATE_SHOWING);
    if (w->active_r()) n.states |= bit(STATE_ENABLED) | bit(STATE_SENSITIVE);
    if (w->tooltip() && *w->tooltip()) n.states |= bit(STATE_HAS_TOOLTIP);
    if (w == focus) {
      n.states |= bit(STATE_FOCUSED);
      s.focus = n.id;
    }
    n.name = accessible_name(w->label() ? w->label() : "", w->tooltip() ? w->tooltip() : "");
    if (n.name.empty() && w->label() == nullptr && w->tooltip()) n.name = clean_label(w->tooltip());
  }

  // the items of a menu array from `first` up to its end (a null label),
  // submenus as menus with their own items; item_of holds the item's index
  void add_menu_items(Fl_Menu_ *mb, const Fl_Menu_Item *menu, int first, int size, Node &parent, Fl_Widget *w) {
    if (!menu) return;
    for (int i = first; i < size && menu[i].label();) {
      const Fl_Menu_Item &m = menu[i];
      int next = (int)(m.next() - menu);
      if (!m.visible()) {
        if (next <= i) break;
        i = next;
        continue;
      }
      Node c;
      c.id = id_for(w, 200000 + i, seen);
      c.parent = parent.id;
      c.name = clean_label(m.label());
      c.states = bit(STATE_VISIBLE) | bit(STATE_SHOWING);
      if (m.active()) c.states |= bit(STATE_ENABLED) | bit(STATE_SENSITIVE);
      c.x = parent.x;
      c.y = parent.y;
      c.w = parent.w;
      c.h = parent.h;
      if (m.submenu()) {
        c.role = ROLE_MENU;
        s.nodes[c.id] = c;
        if (!(m.flags & FL_SUBMENU_POINTER)) add_menu_items(mb, menu, i + 1, size, s.nodes[c.id], w);
        else add_menu_items(mb, (const Fl_Menu_Item *)m.user_data(), 0, 1 << 16, s.nodes[c.id], w);
      } else {
        bool radio = m.flags & FL_MENU_RADIO, toggle = m.flags & FL_MENU_TOGGLE;
        c.role = radio ? ROLE_RADIO_MENU_ITEM : toggle ? ROLE_CHECK_MENU_ITEM : ROLE_MENU_ITEM;
        c.action = "click";
        if (radio || toggle) {
          c.states |= bit(STATE_CHECKABLE);
          if (m.value()) c.states |= bit(STATE_CHECKED);
        }
        // the shortcut, as a screen reader says it ("Ctrl+S")
        if (m.shortcut()) {
          const char *sc = fl_shortcut_label(m.shortcut());
          if (sc && *sc) c.description = sc;
        }
        // activated by its place in the menu bar's array (items of a
        // submenu pointer outside it are only read)
        long idx = &m - mb->menu();
        if (idx >= 0 && idx < mb->size()) {
          widget_of[c.id] = w;
          item_of[c.id] = 200000 + (int)idx;
        } else
          c.action.clear();
        s.nodes[c.id] = c;
      }
      parent.children.push_back(c.id);
      if (next <= i) break;
      i = next;
    }
  }

  int add(Fl_Widget *w, int parent, int ox, int oy) {
    Node n;
    n.id = id_for(w, -1, seen);
    n.parent = parent;
    widget_of[n.id] = w;
    common(n, w, ox, oy);
    bool descend = false;
    Fl_Window *win = w->as_window();
    int cx = ox, cy = oy; // where the children's coordinates start
    if (win) {
      // children of a window are placed relative to it
      cx = (parent == 0 ? win->x_root() : ox + win->x());
      cy = (parent == 0 ? win->y_root() : oy + win->y());
      n.x = cx;
      n.y = cy;
      if (parent == 0) {
        n.role = win->modal() || win->non_modal() ? ROLE_DIALOG : ROLE_FRAME;
        if (win->modal()) n.states |= bit(STATE_MODAL);
        if (win->resizable()) n.states |= bit(STATE_RESIZABLE);
        if (focus && focus->window() && (focus->window() == win || focus->top_window() == win)) {
          n.states |= bit(STATE_ACTIVE);
          s.active = n.id;
        }
        n.name = clean_label(win->label() ? win->label() : "");
      } else
        n.role = ROLE_PANEL;
      descend = true;
    } else if (auto *in = dynamic_cast<Fl_Input_ *>(w)) {
      int t = in->input_type();
      bool secret = t == FL_SECRET_INPUT;
      bool multi = t == FL_MULTILINE_INPUT;
      n.role = secret ? ROLE_PASSWORD_TEXT : multi ? ROLE_TEXT : ROLE_ENTRY;
      n.has_text = true;
      std::string v = in->value() ? in->value() : "";
      n.text = secret ? std::string((size_t)u8_len(v), '*') : v;
      n.caret = u8_chars(v, (size_t)in->insert_position());
      n.states |= bit(STATE_FOCUSABLE) | bit(multi ? STATE_MULTI_LINE : STATE_SINGLE_LINE);
      if (in->readonly()) n.states |= bit(STATE_READ_ONLY);
      else n.states |= bit(STATE_EDITABLE);
    } else if (auto *td = dynamic_cast<Fl_Text_Display *>(w)) {
      n.role = ROLE_TEXT;
      n.has_text = true;
      if (Fl_Text_Buffer *b = td->buffer()) {
        if (b->length() < (1 << 20)) {
          char *t = b->text();
          n.text = t;
          free(t);
        }
        n.caret = u8_chars(n.text, (size_t)td->insert_position());
      }
      n.states |= bit(STATE_FOCUSABLE) | bit(STATE_MULTI_LINE);
      if (dynamic_cast<Fl_Text_Editor *>(w)) n.states |= bit(STATE_EDITABLE);
      else n.states |= bit(STATE_READ_ONLY);
    } else if (auto *b = dynamic_cast<Fl_Button *>(w)) {
      bool check = dynamic_cast<Fl_Check_Button *>(w) != nullptr;
      bool round = dynamic_cast<Fl_Round_Button *>(w) != nullptr;
      bool light = dynamic_cast<Fl_Light_Button *>(w) != nullptr;
      int type = b->type();
      if (round || type == FL_RADIO_BUTTON) n.role = ROLE_RADIO_BUTTON;
      else if (check || light) n.role = ROLE_CHECK_BOX;
      else if (type == FL_TOGGLE_BUTTON) n.role = ROLE_TOGGLE_BUTTON;
      else n.role = ROLE_PUSH_BUTTON;
      if (n.role == ROLE_PUSH_BUTTON) n.action = "click";
      else {
        n.action = "toggle";
        n.states |= bit(STATE_CHECKABLE);
        if (b->value()) n.states |= bit(n.role == ROLE_TOGGLE_BUTTON ? STATE_PRESSED : STATE_CHECKED);
        if (n.role == ROLE_TOGGLE_BUTTON && b->value()) n.states |= bit(STATE_CHECKED);
      }
      if (b->shortcut() == FL_Enter || b->shortcut() == FL_KP_Enter) n.states |= bit(STATE_IS_DEFAULT);
      if (b->visible_focus()) n.states |= bit(STATE_FOCUSABLE);
    } else if (auto *mb = dynamic_cast<Fl_Menu_Bar *>(w)) {
      n.role = ROLE_MENU_BAR;
      // the menus and their items (File › Save): a screen reader can reach
      // and activate every item, as from the keyboard
      add_menu_items(mb, mb->menu(), 0, mb->size(), n, w);
    } else if (auto *ch = dynamic_cast<Fl_Menu_ *>(w)) {
      n.role = ROLE_COMBO_BOX;
      if (const Fl_Menu_Item *mv = ch->mvalue()) n.description = clean_label(mv->label() ? mv->label() : "");
      if (dynamic_cast<Fl_Menu_Button *>(w)) n.role = ROLE_PUSH_BUTTON, n.action = "click";
      n.states |= bit(STATE_FOCUSABLE);
    } else if (auto *sp = dynamic_cast<Fl_Spinner *>(w)) {
      n.role = ROLE_SPIN_BUTTON;
      n.has_value = true;
      n.vmin = sp->minimum();
      n.vmax = sp->maximum();
      n.vstep = sp->step();
      n.value = sp->value();
      n.states |= bit(STATE_FOCUSABLE);
    } else if (auto *pr = dynamic_cast<Fl_Progress *>(w)) {
      n.role = ROLE_PROGRESS_BAR;
      n.has_value = true;
      n.vmin = pr->minimum();
      n.vmax = pr->maximum();
      n.value = pr->value();
    } else if (auto *va = dynamic_cast<Fl_Valuator *>(w)) {
      n.role = dynamic_cast<Fl_Scrollbar *>(w) ? ROLE_SCROLL_BAR : dynamic_cast<Fl_Dial *>(w) ? ROLE_DIAL : ROLE_SLIDER;
      n.has_value = true;
      n.vmin = std::min(va->minimum(), va->maximum());
      n.vmax = std::max(va->minimum(), va->maximum());
      n.vstep = va->step();
      n.value = va->value();
      int t = va->type();
      bool hor = (t & FL_HOR_SLIDER) == FL_HOR_SLIDER || t == FL_HORIZONTAL;
      n.states |= bit(hor ? STATE_HORIZONTAL : STATE_VERTICAL) | bit(STATE_FOCUSABLE);
    } else if (auto *br = dynamic_cast<Fl_Browser *>(w)) {
      n.role = ROLE_LIST;
      n.states |= bit(STATE_FOCUSABLE);
      int count = std::min(br->size(), 500);
      for (int i = 1; i <= count; i++) {
        Node c;
        c.id = id_for(w, i, seen);
        c.parent = n.id;
        c.role = ROLE_LIST_ITEM;
        c.name = clean_browser_line(br->text(i) ? br->text(i) : "");
        c.states = bit(STATE_VISIBLE) | bit(STATE_ENABLED) | bit(STATE_SENSITIVE) | bit(STATE_SELECTABLE);
        if (br->displayed(i)) c.states |= bit(STATE_SHOWING);
        if (br->selected(i)) c.states |= bit(STATE_SELECTED);
        c.action = "select";
        c.x = n.x;
        c.y = n.y;
        c.w = n.w;
        c.h = 0;
        widget_of[c.id] = w;
        item_of[c.id] = i;
        n.children.push_back(c.id);
        s.nodes[c.id] = c;
      }
    } else if (auto *cb = dynamic_cast<Fl_Check_Browser *>(w)) {
      n.role = ROLE_LIST;
      n.states |= bit(STATE_FOCUSABLE);
      for (int i = 1; i <= std::min(cb->nitems(), 500); i++) {
        Node c;
        c.id = id_for(w, i, seen);
        c.parent = n.id;
        c.role = ROLE_CHECK_BOX;
        c.name = clean_label(cb->text(i) ? cb->text(i) : "");
        c.states = bit(STATE_VISIBLE) | bit(STATE_SHOWING) | bit(STATE_ENABLED) | bit(STATE_SENSITIVE) |
                   bit(STATE_CHECKABLE) | (cb->checked(i) ? bit(STATE_CHECKED) : 0);
        c.action = "toggle";
        c.x = n.x;
        c.y = n.y;
        c.w = n.w;
        widget_of[c.id] = w;
        item_of[c.id] = i;
        n.children.push_back(c.id);
        s.nodes[c.id] = c;
      }
    } else if (dynamic_cast<Fl_Tree *>(w)) {
      n.role = ROLE_TREE;
      n.states |= bit(STATE_FOCUSABLE);
    } else if (auto *tabs = dynamic_cast<Fl_Tabs *>(w)) {
      n.role = ROLE_PAGE_TAB_LIST;
      Fl_Widget *shown = tabs->value();
      for (int i = 0; i < tabs->children(); i++) {
        Fl_Widget *pg = tabs->child(i);
        Node c;
        c.id = id_for(w, 100000 + i, seen);
        c.parent = n.id;
        c.role = ROLE_PAGE_TAB;
        c.name = accessible_name(pg->label() ? pg->label() : "", pg->tooltip() ? pg->tooltip() : "");
        c.states = bit(STATE_VISIBLE) | bit(STATE_SHOWING) | bit(STATE_ENABLED) | bit(STATE_SENSITIVE) |
                   bit(STATE_SELECTABLE) | (pg == shown ? bit(STATE_SELECTED) : 0);
        c.action = "select";
        c.x = ox + pg->x();
        c.y = oy + pg->y();
        c.w = pg->w();
        c.h = pg->h();
        widget_of[c.id] = w;
        item_of[c.id] = i;
        s.nodes[c.id] = c;
        n.children.push_back(c.id);
        // the shown page's widgets under its tab
        if (pg == shown && pg->as_group()) {
          Fl_Group *g = pg->as_group();
          for (int k = 0; k < g->children(); k++)
            if (g->child(k)->visible()) {
              int cid = add(g->child(k), c.id, ox, oy);
              if (cid >= 0) s.nodes[c.id].children.push_back(cid);
            }
        }
      }
    } else if (dynamic_cast<Fl_Scroll *>(w)) {
      n.role = ROLE_SCROLL_PANE;
      descend = true;
    } else if (dynamic_cast<Toolbar *>(w)) {
      n.role = ROLE_TOOL_BAR;
      descend = true;
    } else if (w->as_group()) {
      n.role = ROLE_PANEL;
      descend = true;
    } else if (dynamic_cast<Fl_Box *>(w)) {
      if (n.name.empty()) {
        if (!w->image()) return -1; // decoration
        n.role = ROLE_IMAGE;
      } else
        n.role = ROLE_LABEL;
    } else {
      // a program's own widget
      n.role = ROLE_UNKNOWN;
      if (w->visible_focus()) n.states |= bit(STATE_FOCUSABLE);
    }
    auto d = g_described.find(w);
    if (d != g_described.end()) {
      if (d->second.first) n.role = d->second.first;
      if (!d->second.second.empty()) n.name = d->second.second;
    }
    auto t = g_text_of.find(w);
    if (t != g_text_of.end()) {
      n.has_text = true;
      n.text = t->second();
      n.states |= bit(STATE_READ_ONLY) | bit(STATE_MULTI_LINE);
    }
    if (descend)
      if (Fl_Group *g = w->as_group())
        for (int i = 0; i < g->children(); i++) {
          Fl_Widget *c = g->child(i);
          if (!c->visible()) continue;
          int cid = add(c, n.id, cx, cy);
          if (cid >= 0) n.children.push_back(cid);
        }
    s.nodes[n.id] = n;
    return n.id;
  }

  void build() {
    Node app;
    app.id = 0;
    app.parent = -1;
    app.role = ROLE_APPLICATION;
    app.name = g_app_name;
    app.states = bit(STATE_VISIBLE) | bit(STATE_SHOWING) | bit(STATE_ENABLED) | bit(STATE_SENSITIVE);
    s.nodes[0] = app;
    std::vector<int> wins;
    for (Fl_Window *w = Fl::first_window(); w; w = Fl::next_window(w)) {
      if (!w->shown() || !w->visible() || w->menu_window() || w->tooltip_window()) continue;
      int id = add(w, 0, 0, 0);
      if (id >= 0) wins.push_back(id);
    }
    s.nodes[0].children = wins;
  }
};

void send_events(const std::vector<Event> &ev, std::shared_ptr<const Snapshot> now);

void rebuild(void *) {
  Builder b;
  b.build();
  g_ids.swap(b.seen); // ids of widgets that went are forgotten
  g_widget_of.swap(b.widget_of);
  g_item_of.swap(b.item_of);
  auto now = std::make_shared<const Snapshot>(std::move(b.s));
  std::shared_ptr<const Snapshot> before;
  {
    std::lock_guard<std::mutex> l(g_mu);
    before = g_snap;
    g_snap = now;
  }
  if (before) {
    auto ev = diff(*before, *now);
    if (!ev.empty()) on_worker([ev, now] { send_events(ev, now); });
  }
  Fl::repeat_timeout(0.25, rebuild);
}

// what a node's widget does when an assistive technology asks (FLTK thread)
bool do_action(int id) {
  auto it = g_widget_of.find(id);
  if (it == g_widget_of.end()) return false;
  Fl_Widget *w = it->second;
  auto item = g_item_of.find(id);
  if (auto *br = dynamic_cast<Fl_Browser *>(w)) {
    if (item == g_item_of.end()) return false;
    if (br->type() == FL_HOLD_BROWSER || br->type() == FL_SELECT_BROWSER) br->value(item->second);
    else br->select(item->second);
    br->do_callback();
    return true;
  }
  if (auto *cb = dynamic_cast<Fl_Check_Browser *>(w)) {
    if (item == g_item_of.end()) return false;
    cb->checked(item->second, !cb->checked(item->second));
    cb->do_callback();
    return true;
  }
  if (auto *tabs = dynamic_cast<Fl_Tabs *>(w)) {
    if (item == g_item_of.end() || item->second >= tabs->children()) return false;
    tabs->value(tabs->child(item->second));
    tabs->do_callback();
    return true;
  }
  if (auto *b = dynamic_cast<Fl_Button *>(w)) {
    if (!b->active_r()) return false;
    bool check = dynamic_cast<Fl_Light_Button *>(w) != nullptr;
    if (b->type() == FL_RADIO_BUTTON || dynamic_cast<Fl_Round_Button *>(w)) b->setonly();
    else if (check || b->type() == FL_TOGGLE_BUTTON) b->value(!b->value());
    b->do_callback();
    return true;
  }
  if (auto *bar = dynamic_cast<Fl_Menu_Bar *>(w)) {
    // an item of the menu bar: chosen as if from the menu
    if (item == g_item_of.end() || item->second < 200000) return false;
    int idx = item->second - 200000;
    if (!bar->menu() || idx >= bar->size()) return false;
    const Fl_Menu_Item *m = bar->menu() + idx;
    if (!m->label() || m->submenu() || !m->active() || !bar->active_r()) return false;
    bar->picked(m);
    return true;
  }
  if (auto *mb = dynamic_cast<Fl_Menu_Button *>(w)) {
    const Fl_Menu_Item *r = mb->popup();
    if (r) mb->picked(r);
    return true;
  }
  return false;
}

bool set_text(int id, const std::string &t) {
  auto it = g_widget_of.find(id);
  if (it == g_widget_of.end()) return false;
  if (auto *in = dynamic_cast<Fl_Input_ *>(it->second)) {
    if (in->readonly()) return false;
    in->value(t.c_str());
    in->set_changed();
    in->do_callback();
    return true;
  }
  if (auto *ed = dynamic_cast<Fl_Text_Editor *>(it->second)) {
    if (ed->buffer()) ed->buffer()->text(t.c_str());
    return true;
  }
  return false;
}

bool set_caret(int id, int chars) {
  auto it = g_widget_of.find(id);
  if (it == g_widget_of.end()) return false;
  if (auto *in = dynamic_cast<Fl_Input_ *>(it->second)) {
    std::string v = in->value() ? in->value() : "";
    in->insert_position((int)u8_byte(v, chars));
    return true;
  }
  if (auto *td = dynamic_cast<Fl_Text_Display *>(it->second)) {
    if (!td->buffer()) return false;
    char *t = td->buffer()->text();
    std::string v = t;
    free(t);
    td->insert_position((int)u8_byte(v, chars));
    td->show_insert_position();
    return true;
  }
  return false;
}

bool set_value(int id, double v) {
  auto it = g_widget_of.find(id);
  if (it == g_widget_of.end()) return false;
  Fl_Widget *w = it->second;
  if (auto *sp = dynamic_cast<Fl_Spinner *>(w)) {
    sp->value(std::max(sp->minimum(), std::min(sp->maximum(), v)));
    sp->do_callback();
    return true;
  }
  if (auto *va = dynamic_cast<Fl_Valuator *>(w)) {
    va->value(va->clamp(va->round(v)));
    va->do_callback();
    return true;
  }
  if (auto *pr = dynamic_cast<Fl_Progress *>(w)) {
    pr->value((float)v);
    return true;
  }
  return false;
}

bool grab_focus(int id) {
  auto it = g_widget_of.find(id);
  if (it == g_widget_of.end()) return false;
  Fl_Widget *w = it->second;
  if (Fl_Window *win = w->as_window()) {
    win->show();
    return true;
  }
  if (w->window()) w->window()->show();
  return w->take_focus() != 0;
}

// runs fn on the FLTK thread and waits for its answer (the worker thread
// only; the FLTK thread never waits for the worker)
bool fltk_sync(std::function<bool()> fn) {
  struct Wait {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, result = false;
  };
  auto w = std::make_shared<Wait>();
  on_fltk([w, fn] {
    bool r = fn();
    std::lock_guard<std::mutex> l(w->m);
    w->result = r;
    w->done = true;
    w->cv.notify_all();
  });
  std::unique_lock<std::mutex> l(w->m);
  if (!w->cv.wait_for(l, std::chrono::seconds(3), [&] { return w->done; })) return false;
  return w->result;
}

// ---- the worker side: D-Bus --------------------------------------------------------

const char *kXml = R"XML(<node>
<interface name="org.a11y.atspi.Accessible">
 <property name="Name" type="s" access="read"/>
 <property name="Description" type="s" access="read"/>
 <property name="Parent" type="(so)" access="read"/>
 <property name="ChildCount" type="i" access="read"/>
 <property name="Locale" type="s" access="read"/>
 <property name="AccessibleId" type="s" access="read"/>
 <property name="HelpText" type="s" access="read"/>
 <method name="GetChildAtIndex"><arg direction="in" type="i"/><arg direction="out" type="(so)"/></method>
 <method name="GetChildren"><arg direction="out" type="a(so)"/></method>
 <method name="GetIndexInParent"><arg direction="out" type="i"/></method>
 <method name="GetRelationSet"><arg direction="out" type="a(ua(so))"/></method>
 <method name="GetRole"><arg direction="out" type="u"/></method>
 <method name="GetRoleName"><arg direction="out" type="s"/></method>
 <method name="GetLocalizedRoleName"><arg direction="out" type="s"/></method>
 <method name="GetState"><arg direction="out" type="au"/></method>
 <method name="GetAttributes"><arg direction="out" type="a{ss}"/></method>
 <method name="GetApplication"><arg direction="out" type="(so)"/></method>
 <method name="GetInterfaces"><arg direction="out" type="as"/></method>
</interface>
<interface name="org.a11y.atspi.Application">
 <property name="ToolkitName" type="s" access="read"/>
 <property name="Version" type="s" access="read"/>
 <property name="ToolkitVersion" type="s" access="read"/>
 <property name="AtspiVersion" type="s" access="read"/>
 <property name="InterfaceVersion" type="u" access="read"/>
 <property name="Id" type="i" access="readwrite"/>
 <method name="GetLocale"><arg direction="in" type="u"/><arg direction="out" type="s"/></method>
 <method name="GetApplicationBusAddress"><arg direction="out" type="s"/></method>
</interface>
<interface name="org.a11y.atspi.Component">
 <method name="Contains"><arg direction="in" type="i"/><arg direction="in" type="i"/><arg direction="in" type="u"/><arg direction="out" type="b"/></method>
 <method name="GetAccessibleAtPoint"><arg direction="in" type="i"/><arg direction="in" type="i"/><arg direction="in" type="u"/><arg direction="out" type="(so)"/></method>
 <method name="GetExtents"><arg direction="in" type="u"/><arg direction="out" type="(iiii)"/></method>
 <method name="GetPosition"><arg direction="in" type="u"/><arg direction="out" type="i"/><arg direction="out" type="i"/></method>
 <method name="GetSize"><arg direction="out" type="i"/><arg direction="out" type="i"/></method>
 <method name="GetLayer"><arg direction="out" type="u"/></method>
 <method name="GetMDIZOrder"><arg direction="out" type="n"/></method>
 <method name="GrabFocus"><arg direction="out" type="b"/></method>
 <method name="GetAlpha"><arg direction="out" type="d"/></method>
 <method name="ScrollTo"><arg direction="in" type="u"/><arg direction="out" type="b"/></method>
</interface>
<interface name="org.a11y.atspi.Action">
 <property name="NActions" type="i" access="read"/>
 <method name="GetDescription"><arg direction="in" type="i"/><arg direction="out" type="s"/></method>
 <method name="GetName"><arg direction="in" type="i"/><arg direction="out" type="s"/></method>
 <method name="GetLocalizedName"><arg direction="in" type="i"/><arg direction="out" type="s"/></method>
 <method name="GetKeyBinding"><arg direction="in" type="i"/><arg direction="out" type="s"/></method>
 <method name="GetActions"><arg direction="out" type="a(sss)"/></method>
 <method name="DoAction"><arg direction="in" type="i"/><arg direction="out" type="b"/></method>
</interface>
<interface name="org.a11y.atspi.Text">
 <property name="CharacterCount" type="i" access="read"/>
 <property name="CaretOffset" type="i" access="read"/>
 <method name="GetText"><arg direction="in" type="i"/><arg direction="in" type="i"/><arg direction="out" type="s"/></method>
 <method name="SetCaretOffset"><arg direction="in" type="i"/><arg direction="out" type="b"/></method>
 <method name="GetTextAtOffset"><arg direction="in" type="i"/><arg direction="in" type="u"/><arg direction="out" type="s"/><arg direction="out" type="i"/><arg direction="out" type="i"/></method>
 <method name="GetCharacterAtOffset"><arg direction="in" type="i"/><arg direction="out" type="i"/></method>
 <method name="GetNSelections"><arg direction="out" type="i"/></method>
 <method name="GetSelection"><arg direction="in" type="i"/><arg direction="out" type="i"/><arg direction="out" type="i"/></method>
 <method name="GetDefaultAttributes"><arg direction="out" type="a{ss}"/></method>
 <method name="GetAttributes"><arg direction="in" type="i"/><arg direction="out" type="a{ss}"/><arg direction="out" type="i"/><arg direction="out" type="i"/></method>
</interface>
<interface name="org.a11y.atspi.EditableText">
 <method name="SetTextContents"><arg direction="in" type="s"/><arg direction="out" type="b"/></method>
 <method name="InsertText"><arg direction="in" type="i"/><arg direction="in" type="s"/><arg direction="in" type="i"/><arg direction="out" type="b"/></method>
 <method name="DeleteText"><arg direction="in" type="i"/><arg direction="in" type="i"/><arg direction="out" type="b"/></method>
</interface>
<interface name="org.a11y.atspi.Value">
 <property name="MinimumValue" type="d" access="read"/>
 <property name="MaximumValue" type="d" access="read"/>
 <property name="MinimumIncrement" type="d" access="read"/>
 <property name="CurrentValue" type="d" access="readwrite"/>
 <property name="Text" type="s" access="read"/>
</interface>
</node>)XML";

std::string path_of(int id) { return id == 0 ? kRootPath : std::string(kBase) + "/" + std::to_string(id); }

int id_of(const char *path) {
  if (!path) return -1;
  if (!strcmp(path, kRootPath)) return 0;
  size_t n = strlen(kBase);
  if (strncmp(path, kBase, n) != 0 || path[n] != '/') return -1;
  char *end = nullptr;
  long v = strtol(path + n + 1, &end, 10);
  return end && !*end && v > 0 ? (int)v : -1;
}

GVariant *ref(int id) {
  if (id < 0) return g_variant_new("(so)", g_unique.c_str(), kNull);
  return g_variant_new("(so)", g_unique.c_str(), path_of(id).c_str());
}

GVariant *parent_ref(const Node &n) {
  if (n.id == 0) {
    if (g_parent_bus.empty()) return g_variant_new("(so)", "", kNull);
    return g_variant_new("(so)", g_parent_bus.c_str(), g_parent_path.c_str());
  }
  return ref(n.parent);
}

std::vector<const char *> interfaces(const Node &n) {
  std::vector<const char *> v{"org.a11y.atspi.Accessible"};
  if (n.id == 0) v.push_back("org.a11y.atspi.Application");
  else v.push_back("org.a11y.atspi.Component");
  if (!n.action.empty()) v.push_back("org.a11y.atspi.Action");
  if (n.has_text) {
    v.push_back("org.a11y.atspi.Text");
    if (n.states & bit(STATE_EDITABLE)) v.push_back("org.a11y.atspi.EditableText");
  }
  if (n.has_value) v.push_back("org.a11y.atspi.Value");
  return v;
}

bool has_interface(const Node &n, const char *iface) {
  for (const char *i : interfaces(n))
    if (!strcmp(i, iface)) return true;
  return false;
}

std::string text_range(const std::string &t, int a, int b) {
  int len = u8_len(t);
  if (b < 0 || b > len) b = len;
  a = std::max(0, std::min(a, len));
  if (b < a) b = a;
  size_t ba = u8_byte(t, a), bb = u8_byte(t, b);
  return t.substr(ba, bb - ba);
}

// the word, line or character around an offset (AtspiTextBoundaryType)
void boundary(const std::string &t, int off, unsigned type, int &a, int &b) {
  int len = u8_len(t);
  off = std::max(0, std::min(off, len));
  if (type == 0) { // char
    a = off;
    b = std::min(off + 1, len);
    return;
  }
  // work on bytes for ASCII separators
  size_t bo = u8_byte(t, off);
  auto is_sep = [&](size_t i) {
    char c = t[i];
    return type >= 5 ? c == '\n' : (type >= 3 ? (c == '.' || c == '!' || c == '?') : (c == ' ' || c == '\n' || c == '\t'));
  };
  size_t s = bo, e = bo;
  while (s > 0 && !is_sep(s - 1)) s--;
  while (e < t.size() && !is_sep(e)) e++;
  if (e < t.size()) e++; // the separator goes with the word / line before it
  a = u8_chars(t, s);
  b = u8_chars(t, e);
}

void method_call(GDBusConnection *, const char *, const char *path, const char *iface, const char *method,
                 GVariant *params, GDBusMethodInvocation *inv, gpointer) {
  auto s = snap();
  int id = id_of(path);
  const Node *n = s ? s->get(id) : nullptr;
  if (!n) {
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownObject", "no such object");
    return;
  }
  std::string I = iface, M = method;
  if (I == "org.a11y.atspi.Accessible") {
    if (M == "GetChildAtIndex") {
      int i;
      g_variant_get(params, "(i)", &i);
      int c = i >= 0 && i < (int)n->children.size() ? n->children[(size_t)i] : -1;
      g_dbus_method_invocation_return_value(inv, g_variant_new("(@(so))", ref(c)));
    } else if (M == "GetChildren") {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("a(so)"));
      for (int c : n->children) g_variant_builder_add_value(&b, ref(c));
      g_dbus_method_invocation_return_value(inv, g_variant_new("(a(so))", &b));
    } else if (M == "GetIndexInParent") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(i)", n->id == 0 ? -1 : s->index_in_parent(n->id)));
    } else if (M == "GetRelationSet") {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("a(ua(so))"));
      g_dbus_method_invocation_return_value(inv, g_variant_new("(a(ua(so)))", &b));
    } else if (M == "GetRole") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", n->role));
    } else if (M == "GetRoleName" || M == "GetLocalizedRoleName") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", role_name(n->role)));
    } else if (M == "GetState") {
      auto w = state_words(n->states);
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("au"));
      g_variant_builder_add(&b, "u", w.first);
      g_variant_builder_add(&b, "u", w.second);
      g_dbus_method_invocation_return_value(inv, g_variant_new("(au)", &b));
    } else if (M == "GetAttributes") {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("a{ss}"));
      g_variant_builder_add(&b, "{ss}", "toolkit", "FLTK");
      g_dbus_method_invocation_return_value(inv, g_variant_new("(a{ss})", &b));
    } else if (M == "GetApplication") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(@(so))", ref(0)));
    } else if (M == "GetInterfaces") {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
      for (const char *i : interfaces(*n)) g_variant_builder_add(&b, "s", i);
      g_dbus_method_invocation_return_value(inv, g_variant_new("(as)", &b));
    } else
      g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
    return;
  }
  if (!has_interface(*n, iface)) {
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownInterface", iface);
    return;
  }
  if (I == "org.a11y.atspi.Application") {
    if (M == "GetLocale") {
      const char *l = getenv("LANG");
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", l ? l : "C"));
    } else
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", ""));
    return;
  }
  if (I == "org.a11y.atspi.Component") {
    if (M == "Contains") {
      int x, y;
      guint32 t;
      g_variant_get(params, "(iiu)", &x, &y, &t);
      bool in = x >= n->x && y >= n->y && x < n->x + n->w && y < n->y + n->h;
      g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", in));
    } else if (M == "GetAccessibleAtPoint") {
      int x, y;
      guint32 t;
      g_variant_get(params, "(iiu)", &x, &y, &t);
      int hit = s->at_point(x, y);
      g_dbus_method_invocation_return_value(inv, g_variant_new("(@(so))", ref(hit)));
    } else if (M == "GetExtents") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("((iiii))", n->x, n->y, n->w, n->h));
    } else if (M == "GetPosition") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(ii)", n->x, n->y));
    } else if (M == "GetSize") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(ii)", n->w, n->h));
    } else if (M == "GetLayer") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", n->role == ROLE_FRAME || n->role == ROLE_DIALOG ? 7u : 3u));
    } else if (M == "GetMDIZOrder") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(n)", (gint16)0));
    } else if (M == "GrabFocus") {
      int i = n->id;
      g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", fltk_sync([i] { return grab_focus(i); })));
    } else if (M == "GetAlpha") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(d)", 1.0));
    } else
      g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
    return;
  }
  if (I == "org.a11y.atspi.Action") {
    int i = 0;
    if (M != "GetActions") g_variant_get(params, "(i)", &i);
    if (M == "GetActions") {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("a(sss)"));
      g_variant_builder_add(&b, "(sss)", n->action.c_str(), n->action.c_str(), "");
      g_dbus_method_invocation_return_value(inv, g_variant_new("(a(sss))", &b));
    } else if (M == "DoAction") {
      int nid = n->id;
      // answered at once: the action may open a dialog that waits
      on_fltk([nid] { do_action(nid); });
      g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", i == 0));
    } else if (M == "GetKeyBinding") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", ""));
    } else
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", i == 0 ? n->action.c_str() : ""));
    return;
  }
  if (I == "org.a11y.atspi.Text") {
    const std::string &t = n->text;
    if (M == "GetText") {
      int a, b;
      g_variant_get(params, "(ii)", &a, &b);
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", text_range(t, a, b).c_str()));
    } else if (M == "SetCaretOffset") {
      int o, nid = n->id;
      g_variant_get(params, "(i)", &o);
      g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", fltk_sync([nid, o] { return set_caret(nid, o); })));
    } else if (M == "GetTextAtOffset") {
      int o, a, b;
      guint32 type;
      g_variant_get(params, "(iu)", &o, &type);
      boundary(t, o, type, a, b);
      g_dbus_method_invocation_return_value(inv, g_variant_new("(sii)", text_range(t, a, b).c_str(), a, b));
    } else if (M == "GetCharacterAtOffset") {
      int o;
      g_variant_get(params, "(i)", &o);
      std::string c = text_range(t, o, o + 1);
      gunichar u = c.empty() ? 0 : g_utf8_get_char_validated(c.c_str(), (gssize)c.size());
      g_dbus_method_invocation_return_value(inv, g_variant_new("(i)", (int)(u == (gunichar)-1 || u == (gunichar)-2 ? 0 : u)));
    } else if (M == "GetNSelections") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(i)", 0));
    } else if (M == "GetSelection") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(ii)", 0, 0));
    } else if (M == "GetDefaultAttributes") {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("a{ss}"));
      g_dbus_method_invocation_return_value(inv, g_variant_new("(a{ss})", &b));
    } else if (M == "GetAttributes") {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("a{ss}"));
      g_dbus_method_invocation_return_value(inv, g_variant_new("(a{ss}ii)", &b, 0, u8_len(t)));
    } else
      g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
    return;
  }
  if (I == "org.a11y.atspi.EditableText") {
    std::string t = n->text, out;
    int nid = n->id;
    if (M == "SetTextContents") {
      const char *v;
      g_variant_get(params, "(&s)", &v);
      out = v;
    } else if (M == "InsertText") {
      int pos, len;
      const char *v;
      g_variant_get(params, "(i&si)", &pos, &v, &len);
      std::string ins = text_range(v, 0, len);
      size_t b = u8_byte(t, pos);
      out = t.substr(0, b) + ins + t.substr(b);
    } else if (M == "DeleteText") {
      int a, b;
      g_variant_get(params, "(ii)", &a, &b);
      out = text_range(t, 0, a) + text_range(t, b, -1);
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", fltk_sync([nid, out] { return set_text(nid, out); })));
    return;
  }
  g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

GVariant *get_property(GDBusConnection *, const char *, const char *path, const char *iface, const char *prop,
                       GError **error, gpointer) {
  auto s = snap();
  const Node *n = s ? s->get(id_of(path)) : nullptr;
  if (!n) {
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT, "no such object");
    return nullptr;
  }
  std::string I = iface, P = prop;
  if (I == "org.a11y.atspi.Accessible") {
    if (P == "Name") return g_variant_new_string(n->name.c_str());
    if (P == "Description") return g_variant_new_string(n->description.c_str());
    if (P == "Parent") return parent_ref(*n);
    if (P == "ChildCount") return g_variant_new_int32((int)n->children.size());
    if (P == "Locale") return g_variant_new_string(getenv("LANG") ? getenv("LANG") : "C");
    if (P == "AccessibleId") return g_variant_new_string(n->id == 0 ? "" : std::to_string(n->id).c_str());
    if (P == "HelpText") return g_variant_new_string("");
  } else if (I == "org.a11y.atspi.Application") {
    if (P == "ToolkitName") return g_variant_new_string("FLTK");
    if (P == "Version" || P == "ToolkitVersion") {
      char v[32];
      snprintf(v, sizeof v, "%d.%d.%d", FL_MAJOR_VERSION, FL_MINOR_VERSION, FL_PATCH_VERSION);
      return g_variant_new_string(v);
    }
    if (P == "AtspiVersion") return g_variant_new_string("2.1");
    if (P == "InterfaceVersion") return g_variant_new_uint32(1);
    if (P == "Id") return g_variant_new_int32(g_app_id);
  } else if (I == "org.a11y.atspi.Action") {
    if (P == "NActions") return g_variant_new_int32(n->action.empty() ? 0 : 1);
  } else if (I == "org.a11y.atspi.Text") {
    if (P == "CharacterCount") return g_variant_new_int32(u8_len(n->text));
    if (P == "CaretOffset") return g_variant_new_int32(n->caret);
  } else if (I == "org.a11y.atspi.Value") {
    if (P == "MinimumValue") return g_variant_new_double(n->vmin);
    if (P == "MaximumValue") return g_variant_new_double(n->vmax);
    if (P == "MinimumIncrement") return g_variant_new_double(n->vstep);
    if (P == "CurrentValue") return g_variant_new_double(n->value);
    if (P == "Text") return g_variant_new_string("");
  }
  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "%s", prop);
  return nullptr;
}

gboolean set_property(GDBusConnection *, const char *, const char *path, const char *iface, const char *prop,
                      GVariant *value, GError **error, gpointer) {
  std::string I = iface, P = prop;
  if (I == "org.a11y.atspi.Application" && P == "Id") {
    g_app_id = g_variant_get_int32(value);
    return TRUE;
  }
  if (I == "org.a11y.atspi.Value" && P == "CurrentValue") {
    int id = id_of(path);
    double v = g_variant_get_double(value);
    if (fltk_sync([id, v] { return set_value(id, v); })) return TRUE;
  }
  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY, "%s", prop);
  return FALSE;
}

const GDBusInterfaceVTable kVTable = {method_call, get_property, set_property, {nullptr}};

char **enumerate(GDBusConnection *, const char *, const char *, gpointer) {
  // nodes are dispatched without being listed (DISPATCH_TO_UNENUMERATED_NODES)
  char **out = g_new0(char *, 2);
  out[0] = g_strdup("root");
  return out;
}

GDBusInterfaceInfo **introspect(GDBusConnection *, const char *, const char *, const char *node, gpointer) {
  auto s = snap();
  std::string p = node ? std::string(kBase) + "/" + node : kBase;
  const Node *n = s ? s->get(id_of(p.c_str())) : nullptr;
  if (!n) return nullptr;
  auto ifs = interfaces(*n);
  GDBusInterfaceInfo **out = g_new0(GDBusInterfaceInfo *, ifs.size() + 1);
  for (size_t i = 0; i < ifs.size(); i++)
    out[i] = g_dbus_interface_info_ref(g_dbus_node_info_lookup_interface(g_info, ifs[i]));
  return out;
}

const GDBusInterfaceVTable *dispatch(GDBusConnection *, const char *, const char *, const char *, const char *,
                                     gpointer *out_user_data, gpointer) {
  *out_user_data = nullptr;
  return &kVTable;
}

const GDBusSubtreeVTable kSubtree = {enumerate, introspect, dispatch, {nullptr}};

void emit(int id, const char *iface, const char *member, const char *detail, int d1, int d2, GVariant *any) {
  if (!g_bus) return;
  GVariantBuilder props;
  g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
  g_dbus_connection_emit_signal(g_bus, nullptr, path_of(id).c_str(), iface, member,
                                g_variant_new("(siiva{sv})", detail, d1, d2, any ? any : g_variant_new_int32(0), &props),
                                nullptr);
}

void send_events(const std::vector<Event> &ev, std::shared_ptr<const Snapshot> now) {
  for (auto &e : ev) {
    const Node *n = now->get(e.node);
    if (e.kind == "state") emit(e.node, "org.a11y.atspi.Event.Object", "StateChanged", e.detail.c_str(), e.value, 0, nullptr);
    else if (e.kind == "focus") emit(e.node, "org.a11y.atspi.Event.Focus", "Focus", "", 0, 0, nullptr);
    else if (e.kind == "window-activate" && n)
      emit(e.node, "org.a11y.atspi.Event.Window", "Activate", "", 0, 0, g_variant_new_string(n->name.c_str()));
    else if (e.kind == "name" && n)
      emit(e.node, "org.a11y.atspi.Event.Object", "PropertyChange", "accessible-name", 0, 0,
           g_variant_new_string(n->name.c_str()));
    else if (e.kind == "value" && n)
      emit(e.node, "org.a11y.atspi.Event.Object", "PropertyChange", "accessible-value", 0, 0, g_variant_new_double(n->value));
    else if (e.kind == "text" && n)
      emit(e.node, "org.a11y.atspi.Event.Object", "TextChanged", "insert", 0, u8_len(n->text),
           g_variant_new_string(n->text.c_str()));
  }
}

// the accessibility bus: AT_SPI_BUS_ADDRESS, else org.a11y.Bus on the session bus
std::string bus_address(bool &enabled) {
  const char *forced = getenv("FLD_ATSPI");
  enabled = forced && !strcmp(forced, "1");
  const char *env = getenv("AT_SPI_BUS_ADDRESS");
  std::string addr = env ? env : "";
  GError *e = nullptr;
  GDBusConnection *session = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &e);
  if (!session) {
    if (e) g_error_free(e);
    return addr;
  }
  auto prop = [&](const char *name) {
    GVariant *r = g_dbus_connection_call_sync(session, "org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties",
                                              "Get", g_variant_new("(ss)", "org.a11y.Status", name), G_VARIANT_TYPE("(v)"),
                                              G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, nullptr);
    bool on = false;
    if (r) {
      GVariant *v;
      g_variant_get(r, "(v)", &v);
      on = g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
      g_variant_unref(v);
      g_variant_unref(r);
    }
    return on;
  };
  if (!enabled && !(forced && !strcmp(forced, "0"))) enabled = prop("IsEnabled") || prop("ScreenReaderEnabled");
  if (enabled && addr.empty()) {
    GVariant *r = g_dbus_connection_call_sync(session, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress",
                                              nullptr, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr);
    if (r) {
      const char *a;
      g_variant_get(r, "(&s)", &a);
      addr = a;
      g_variant_unref(r);
    }
  }
  g_object_unref(session);
  return addr;
}

void worker() {
  g_ctx = g_main_context_new();
  g_main_context_push_thread_default(g_ctx);
  bool enabled = false;
  std::string addr = bus_address(enabled);
  if (!enabled || addr.empty()) {
    g_main_context_pop_thread_default(g_ctx);
    g_running = false;
    return;
  }
  GError *e = nullptr;
  g_bus = g_dbus_connection_new_for_address_sync(
      addr.c_str(), (GDBusConnectionFlags)(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &e);
  if (!g_bus) {
    if (e) g_error_free(e);
    g_main_context_pop_thread_default(g_ctx);
    g_running = false;
    return;
  }
  g_unique = g_dbus_connection_get_unique_name(g_bus);
  g_info = g_dbus_node_info_new_for_xml(kXml, nullptr);
  g_dbus_connection_register_subtree(g_bus, kBase, &kSubtree, G_DBUS_SUBTREE_FLAGS_DISPATCH_TO_UNENUMERATED_NODES, nullptr,
                                     nullptr, nullptr);
  // the FLTK thread starts reading the widgets
  on_fltk([] { Fl::add_timeout(0.05, rebuild); });
  // the registry learns about the application
  g_dbus_connection_call(g_bus, "org.a11y.atspi.Registry", kRootPath, "org.a11y.atspi.Socket", "Embed",
                         g_variant_new("((so))", g_unique.c_str(), kRootPath), G_VARIANT_TYPE("((so))"),
                         G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
                         [](GObject *o, GAsyncResult *r, gpointer) {
                           GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(o), r, nullptr);
                           if (!v) return;
                           const char *bus, *path;
                           g_variant_get(v, "((&s&o))", &bus, &path);
                           g_parent_bus = bus;
                           g_parent_path = path;
                           g_variant_unref(v);
                         },
                         nullptr);
  GMainLoop *loop = g_main_loop_new(g_ctx, FALSE);
  g_main_loop_run(loop); // for the life of the program
}

} // namespace

void atspi_start(const std::string &app_name) {
  if (g_running.exchange(true)) return;
  const char *forced = getenv("FLD_ATSPI");
  if (forced && !strcmp(forced, "0")) {
    g_running = false;
    return;
  }
  g_app_name = app_name;
  if (pipe(g_pipe) != 0) {
    g_running = false;
    return;
  }
  Fl::add_fd(g_pipe[0], FL_READ, queue_cb);
  std::thread(worker).detach();
}

bool atspi_running() { return g_running && g_bus; }

void atspi_describe(Fl_Widget *w, unsigned role, const std::string &name) { g_described[w] = {role, name}; }
void atspi_describe_text(Fl_Widget *w, std::function<std::string()> text) { g_text_of[w] = std::move(text); }
void atspi_forget(Fl_Widget *w) {
  g_described.erase(w);
  g_text_of.erase(w);
}

} // namespace fld
