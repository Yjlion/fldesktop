#include "fld/i18n.h"
#include "fld/menus.h"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Window.H>

#include <cstdlib>
#include <cstring>
#include <list>
#include <unordered_set>

namespace fld {

// ---- Menu ---------------------------------------------------------------------------

Menu &Menu::item(const std::string &label, long id, int flags, int shortcut) {
  Entry e;
  e.label = label;
  e.id = id;
  e.flags = flags;
  e.shortcut = shortcut;
  items_.push_back(e);
  return *this;
}

Menu &Menu::item(const std::string &label, std::function<void()> action, int flags, int shortcut) {
  Entry e;
  e.label = label;
  e.flags = flags;
  e.shortcut = shortcut;
  e.action = (int)actions_.size();
  e.id = -1 - e.action; // still non-zero, so popup() reports a choice
  actions_.push_back(std::move(action));
  items_.push_back(e);
  return *this;
}

Menu &Menu::toggle(const std::string &label, bool on, std::function<void()> action, int flags) {
  return item(label, std::move(action), flags | FL_MENU_TOGGLE | (on ? FL_MENU_VALUE : 0));
}

Menu &Menu::radio(const std::string &label, bool on, std::function<void()> action, int flags) {
  return item(label, std::move(action), flags | FL_MENU_RADIO | (on ? FL_MENU_VALUE : 0));
}

Menu &Menu::button(Fl_Widget *b, const std::string &label) {
  if (!b || !b->visible_r()) return *this;
  std::string l = label;
  if (l.empty() && b->label()) {
    // drop FLTK's '&' shortcut marks and '@' symbols from the button label
    for (const char *c = b->label(); *c; c++) {
      if (*c == '&') {
        if (c[1] != '&') continue; // a shortcut mark
        c++;                       // "&&" is an '&'
      }
      l += *c;
    }
    l = text(l);
  }
  int flags = b->active_r() ? 0 : FL_MENU_INACTIVE;
  auto act = [b] { b->do_callback(); };
  if (auto *cb = dynamic_cast<Fl_Button *>(b)) {
    if (cb->type() == FL_TOGGLE_BUTTON || dynamic_cast<Fl_Check_Button *>(b)) {
      bool on = cb->value();
      return toggle(l, on, [cb, on] {
        cb->value(!on);
        cb->do_callback();
      }, flags);
    }
  }
  return item(l, act, flags);
}

Menu &Menu::divider() {
  for (auto it = items_.rbegin(); it != items_.rend(); ++it)
    if (!it->end) {
      it->flags |= FL_MENU_DIVIDER;
      break;
    }
  return *this;
}

Menu &Menu::submenu(const std::string &label, int flags) {
  Entry e;
  e.label = label;
  e.flags = flags | FL_SUBMENU;
  items_.push_back(e);
  return *this;
}

Menu &Menu::end_submenu() {
  Entry e;
  e.end = true;
  items_.push_back(e);
  return *this;
}

const Fl_Menu_Item *Menu::items() {
  built_.clear();
  labels_.clear();
  for (auto &e : items_) {
    if (e.end) {
      built_.push_back({nullptr});
      continue;
    }
    labels_.push_back(e.label);
    Fl_Menu_Item m = {labels_.back().c_str(), e.shortcut, nullptr, (void *)e.id, e.flags};
    built_.push_back(m);
  }
  built_.push_back({nullptr});
  return built_.data();
}

void Menu::run(long id) {
  if (id >= 0) return;
  size_t a = (size_t)(-1 - id);
  if (a < actions_.size() && actions_[a]) actions_[a]();
}

long Menu::popup(int x, int y, const char *title) {
  if (items_.empty()) return 0;
  const Fl_Menu_Item *r = items()->popup(x, y, title);
  if (!r) return 0;
  long id = (long)r->user_data();
  run(id);
  return id;
}

long Menu::popup() { return popup(Fl::event_x(), Fl::event_y()); }

std::string Menu::text(const std::string &s) {
  std::string r;
  for (char c : s) {
    if (c == '&' || c == '@') r += c; // "&&" and "@@" are the characters
    r += c;
  }
  return r;
}

// ---- the text field menu ---------------------------------------------------------------

static std::unordered_set<Fl_Widget *> &opted_out() {
  static std::unordered_set<Fl_Widget *> s;
  return s;
}

void no_edit_menu(Fl_Widget *w) { opted_out().insert(w); }

static bool input_menu(Fl_Input_ *in) {
  bool ro = in->readonly() || !in->active_r();
  bool secret = (in->input_type() == FL_SECRET_INPUT);
  bool sel = in->insert_position() != in->mark();
  Menu m;
  if (!ro) {
    m.item(_("Undo"), [in] { in->undo(); }, in->can_undo() ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'z');
    m.item(_("Redo"), [in] { in->redo(); }, in->can_redo() ? 0 : FL_MENU_INACTIVE, FL_CTRL + FL_SHIFT + 'z');
    m.divider();
    m.item(_("Cut"), [in] {
      in->copy(1);
      in->cut();
      in->do_callback(FL_REASON_CHANGED);
    }, sel && !secret ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'x');
  }
  m.item(_("Copy"), [in] { in->copy(1); }, sel && !secret ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'c');
  if (!ro) {
    m.item(_("Paste"), [in] { Fl::paste(*in, 1); }, 0, FL_CTRL + 'v');
    m.item(_("Delete"), [in] {
      in->cut();
      in->do_callback(FL_REASON_CHANGED);
    }, sel ? 0 : FL_MENU_INACTIVE);
  }
  m.divider();
  m.item(_("Select All"), [in] { in->insert_position(0, in->size()); }, in->size() ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'a');
  m.popup();
  return true;
}

static bool text_menu(Fl_Text_Display *td) {
  Fl_Text_Buffer *b = td->buffer();
  if (!b) return false;
  Fl_Text_Editor *ed = dynamic_cast<Fl_Text_Editor *>(td);
  bool sel = b->selected();
  Menu m;
  if (ed) {
    m.item(_("Undo"), [ed] { Fl_Text_Editor::kf_undo(0, ed); }, b->can_undo() ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'z');
    m.item(_("Redo"), [ed] { Fl_Text_Editor::kf_redo(0, ed); }, b->can_redo() ? 0 : FL_MENU_INACTIVE,
           FL_CTRL + FL_SHIFT + 'z');
    m.divider();
    m.item(_("Cut"), [ed] { Fl_Text_Editor::kf_cut(0, ed); }, sel ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'x');
    m.item(_("Copy"), [ed] { Fl_Text_Editor::kf_copy(0, ed); }, sel ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'c');
    m.item(_("Paste"), [ed] { Fl_Text_Editor::kf_paste(0, ed); }, 0, FL_CTRL + 'v');
    m.item(_("Delete"), [ed] { Fl_Text_Editor::kf_delete(0, ed); }, sel ? 0 : FL_MENU_INACTIVE);
    m.divider();
    m.item(_("Select All"), [ed] { Fl_Text_Editor::kf_select_all(0, ed); }, 0, FL_CTRL + 'a');
  } else {
    m.item(_("Copy"), [b] {
      char *t = b->selection_text();
      if (t && *t) Fl::copy(t, (int)strlen(t), 1);
      free(t);
    }, sel ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'c');
    m.item(_("Select All"), [td, b] {
      b->select(0, b->length());
      td->redraw();
    }, b->length() ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'a');
  }
  m.popup();
  return true;
}

bool show_edit_menu(Fl_Widget *w) {
  if (auto *in = dynamic_cast<Fl_Input_ *>(w)) return input_menu(in);
  if (auto *td = dynamic_cast<Fl_Text_Display *>(w)) return text_menu(td);
  return false;
}

// the innermost visible widget at x, y (coordinates of window w)
static Fl_Widget *widget_at(Fl_Group *g, int x, int y) {
  for (int i = g->children() - 1; i >= 0; i--) {
    Fl_Widget *c = g->child(i);
    if (!c->visible()) continue;
    int cx = x, cy = y;
    if (c->as_window()) {
      cx -= c->x();
      cy -= c->y();
      if (cx < 0 || cy < 0 || cx >= c->w() || cy >= c->h()) continue;
      Fl_Widget *in = widget_at(c->as_group(), cx, cy);
      return in ? in : c;
    }
    if (x < c->x() || y < c->y() || x >= c->x() + c->w() || y >= c->y() + c->h()) continue;
    if (Fl_Group *cg = c->as_group()) {
      // an input inside a group (a spinner, a combo) wins over the group
      if (!dynamic_cast<Fl_Text_Display *>(c)) {
        Fl_Widget *in = widget_at(cg, x, y);
        return in ? in : c;
      }
    }
    return c;
  }
  return nullptr;
}

// Registered widgets are watched (Fl::watch_widget_pointer): a deleted
// widget's entry has a null pointer and is dropped.
struct ContextMenu {
  Fl_Widget *w;
  std::function<void(Menu &)> fill;
  bool select_first;
};

static std::list<ContextMenu> &context_menus() {
  static std::list<ContextMenu> m;
  return m;
}

static ContextMenu *find_menu(Fl_Widget *w) {
  auto &l = context_menus();
  for (auto it = l.begin(); it != l.end();) {
    if (!it->w) {
      it = l.erase(it);
      continue;
    }
    if (it->w == w) return &*it;
    ++it;
  }
  return nullptr;
}

void set_context_menu(Fl_Widget *w, std::function<void(Menu &)> fill, bool select_first) {
  if (!w) return;
  if (ContextMenu *c = find_menu(w)) {
    c->fill = std::move(fill);
    c->select_first = select_first;
    return;
  }
  context_menus().push_back({w, std::move(fill), select_first});
  Fl::watch_widget_pointer(context_menus().back().w);
}

void remove_context_menu(Fl_Widget *w) {
  auto &l = context_menus();
  for (auto it = l.begin(); it != l.end(); ++it)
    if (it->w == w) {
      Fl::release_widget_pointer(it->w);
      l.erase(it);
      return;
    }
}

static Fl_Event_Dispatch g_prev_dispatch = nullptr;

static int dispatch(int e, Fl_Window *win) {
  if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE && win && !Fl::grab() && !context_menus().empty()) {
    // the nearest widget (or parent) with a context menu
    Fl_Widget *w = widget_at(win, Fl::event_x(), Fl::event_y());
    for (Fl_Widget *p = w ? w : win; p; p = p->parent()) {
      ContextMenu *found = find_menu(p);
      if (!found) continue;
      // text fields inside it keep the text menu
      if (w != p && (dynamic_cast<Fl_Input_ *>(w) || dynamic_cast<Fl_Text_Display *>(w))) break;
      if (!p->active_r()) break;
      ContextMenu cm = *found; // a copy: fill may change the registry
      if (cm.select_first) {
        // an ordinary click first (as the left button): lists and tables select the row
        int ex = Fl::e_keysym;
        int state = Fl::e_state;
        Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
        Fl::e_state = (state & ~FL_BUTTONS) | FL_BUTTON1;
        Fl::e_clicks = 0;
        Fl::handle_(FL_PUSH, win);
        Fl::e_state = state & ~FL_BUTTONS;
        Fl::handle_(FL_RELEASE, win);
        Fl::e_keysym = ex;
        Fl::e_state = state;
        Fl::pushed(nullptr);
      }
      Menu m;
      cm.fill(m);
      m.popup();
      return 1;
    }
  }
  if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE && win && !Fl::grab()) {
    Fl_Widget *w = widget_at(win, Fl::event_x(), Fl::event_y());
    // the text area of an Fl_Text_Display is the widget itself; its
    // scrollbars are children
    if (w && w->parent() && dynamic_cast<Fl_Text_Display *>(w->parent()) && !dynamic_cast<Fl_Input_ *>(w))
      w = nullptr;
    if (w && !opted_out().count(w) && w->active_r() &&
        (dynamic_cast<Fl_Input_ *>(w) || dynamic_cast<Fl_Text_Display *>(w))) {
      if (Fl::focus() != w && w->visible_focus()) w->take_focus();
      show_edit_menu(w);
      return 1;
    }
  }
  return g_prev_dispatch ? g_prev_dispatch(e, win) : Fl::handle_(e, win);
}

void install_edit_menu() {
  static bool done = false;
  if (done) return;
  done = true;
  g_prev_dispatch = Fl::event_dispatch();
  Fl::event_dispatch(dispatch);
}

} // namespace fld
