#include "fld/osk.h"
#include "fld/theme.h"
#include "fld/util.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <ctime>

namespace fld {

// ---- layouts ----------------------------------------------------------------

std::vector<OskKey> osk_parse_row(const std::string &row) {
  static const struct { const char *name; OskSpecial key; const char *label; } specials[] = {
      {"bksp", OSK_BACKSPACE, "⌫"}, {"enter", OSK_ENTER, "⏎"}, {"tab", OSK_TAB, "Tab"},
      {"esc", OSK_ESCAPE, "Esc"}, {"left", OSK_LEFT, "←"}, {"right", OSK_RIGHT, "→"},
      {"up", OSK_UP, "↑"}, {"down", OSK_DOWN, "↓"}, {"del", OSK_DELETE, "Del"},
      {"home", OSK_HOME, "Home"}, {"end", OSK_END, "End"}, {"shift", OSK_SHIFT, "⇧"},
      {"caps", OSK_CAPS, "Caps"}, {"ctrl", OSK_CTRL, "Ctrl"}, {"alt", OSK_ALT, "Alt"},
      {"sym", OSK_SYMBOLS, "?123"}, {"abc", OSK_LETTERS, "ABC"}, {"lang", OSK_LAYOUT, "🌐"},
      {"hide", OSK_HIDE, "⌨"}, {"space", OSK_SPACE, ""},
  };
  std::vector<OskKey> keys;
  for (auto &tok : split(row, ' ')) {
    OskKey k;
    if (tok.size() > 2 && tok.front() == '{' && tok.back() == '}') {
      std::string inner = tok.substr(1, tok.size() - 2);
      size_t colon = inner.find(':');
      if (colon != std::string::npos) {
        k.width = (float)atof(inner.c_str() + colon + 1);
        inner = inner.substr(0, colon);
      }
      for (auto &sp : specials)
        if (inner == sp.name) {
          k.special = sp.key;
          k.label = sp.label;
        }
      if (k.special == OSK_NONE) continue; // unknown token
      keys.push_back(k);
      continue;
    }
    const char *p = tok.c_str(), *end = p + tok.size();
    int len;
    unsigned a = fl_utf8decode(p, end, &len);
    p += len;
    k.normal = a;
    if (p < end) k.shifted = fl_utf8decode(p, end, &len);
    else k.shifted = (uint32_t)towupper((wint_t)a);
    keys.push_back(k);
  }
  return keys;
}

static OskLayout make_layout(const char *id, const char *name, std::vector<const char *> rows) {
  OskLayout l;
  l.id = id;
  l.name = name;
  for (auto r : rows) l.rows.push_back(osk_parse_row(r));
  return l;
}

const std::vector<OskLayout> &osk_layouts() {
  static const std::vector<OskLayout> layouts = {
      make_layout("us", "English (US)",
                  {"{esc} `~ 1! 2@ 3# 4$ 5% 6^ 7& 8* 9( 0) -_ =+ {bksp:1.5}",
                   "{tab:1.5} q w e r t y u i o p [{ ]} \\|",
                   "{caps:1.75} a s d f g h j k l ;: '\" {enter:2.25}",
                   "{shift:2.25} z x c v b n m ,< .> /? {shift:2.75}",
                   "{ctrl:1.25} {sym:1.5} {lang:1.25} {space:6} {left} {down} {up} {right} {hide:1.25}"}),
      make_layout("de", "Deutsch",
                  {"{esc} ^° 1! 2\" 3§ 4$ 5% 6& 7/ 8( 9) 0= ß? ´` {bksp:1.5}",
                   "{tab:1.5} q w e r t z u i o p ü +*",
                   "{caps:1.75} a s d f g h j k l ö ä #' {enter:1.5}",
                   "{shift:1.25} <> y x c v b n m ,; .: -_ {shift:2.5}",
                   "{ctrl:1.25} {sym:1.5} {lang:1.25} {space:6} {left} {down} {up} {right} {hide:1.25}"}),
      make_layout("fr", "Français",
                  {"{esc} ²³ &1 é2 \"3 '4 (5 -6 è7 _8 ç9 à0 )° =+ {bksp:1.5}",
                   "{tab:1.5} a z e r t y u i o p ^¨ $£",
                   "{caps:1.75} q s d f g h j k l m ù% *µ {enter:1.5}",
                   "{shift:1.25} <> w x c v b n ,? ;. :/ !§ {shift:2.5}",
                   "{ctrl:1.25} {sym:1.5} {lang:1.25} {space:6} {left} {down} {up} {right} {hide:1.25}"}),
      make_layout("symbols", "Symbols",
                  {"{esc} 1 2 3 4 5 6 7 8 9 0 {bksp:1.5}",
                   "{tab:1.5} @ # $ % & * ( ) - + = {del}",
                   "€ £ ¥ ° § ¶ • … « » ¡ ¿ {enter:1.5}",
                   "~ ` | \\ / < > [ ] { } ^ _ {home} {end}",
                   "{ctrl:1.25} {abc:1.5} {lang:1.25} {space:6} {left} {down} {up} {right} {hide:1.25}"}),
  };
  return layouts;
}

const OskLayout *osk_layout(const std::string &id) {
  for (auto &l : osk_layouts())
    if (l.id == id) return &l;
  return nullptr;
}

// ---- FLTK sink --------------------------------------------------------------------

static void dispatch_key(int keysym, const std::string &text, unsigned mods) {
  static char buf[16];
  snprintf(buf, sizeof buf, "%s", text.c_str());
  int state = 0;
  if (mods & OSK_MOD_SHIFT) state |= FL_SHIFT;
  if (mods & OSK_MOD_CTRL) state |= FL_CTRL;
  if (mods & OSK_MOD_ALT) state |= FL_ALT;
  Fl::e_keysym = keysym;
  Fl::e_original_keysym = keysym;
  Fl::e_text = buf;
  Fl::e_length = (int)strlen(buf);
  Fl::e_state = state;
  Fl_Window *win = Fl::focus() ? Fl::focus()->top_window() : Fl::first_window();
  if (win) Fl::handle(FL_KEYBOARD, win);
}

void FltkKeySink::send_char(uint32_t cp, unsigned mods) {
  char buf[8];
  int n = fl_utf8encode(cp, buf);
  buf[n] = 0;
  int keysym = cp < 128 ? (int)tolower((int)cp) : (int)cp;
  // FLTK passes control characters as text for Ctrl+letter
  if ((mods & OSK_MOD_CTRL) && cp < 128 && isalpha((int)cp)) {
    buf[0] = (char)(tolower((int)cp) - 'a' + 1);
    buf[1] = 0;
  }
  dispatch_key(keysym, buf, mods);
}

void FltkKeySink::send_special(OskSpecial key, unsigned mods) {
  int ks = 0;
  const char *text = "";
  switch (key) {
    case OSK_BACKSPACE: ks = FL_BackSpace; text = "\b"; break;
    case OSK_ENTER: ks = FL_Enter; text = "\r"; break;
    case OSK_TAB: ks = FL_Tab; text = "\t"; break;
    case OSK_ESCAPE: ks = FL_Escape; text = "\033"; break;
    case OSK_LEFT: ks = FL_Left; break;
    case OSK_RIGHT: ks = FL_Right; break;
    case OSK_UP: ks = FL_Up; break;
    case OSK_DOWN: ks = FL_Down; break;
    case OSK_DELETE: ks = FL_Delete; text = "\177"; break;
    case OSK_HOME: ks = FL_Home; break;
    case OSK_END: ks = FL_End; break;
    default: return;
  }
  dispatch_key(ks, text, mods);
}

// ---- widget -----------------------------------------------------------------------

OskWidget::OskWidget(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
  box(FL_FLAT_BOX);
  clear_visible_focus();
}

OskWidget::~OskWidget() {
  Fl::remove_timeout(flash_cb, this);
  Fl::remove_timeout(repeat_cb, this);
}

static const double FLASH_TIME = 0.12;

void OskWidget::flash_cb(void *data) {
  OskWidget *o = static_cast<OskWidget *>(data);
  o->flash_row_ = o->flash_col_ = -1;
  o->redraw();
}

// show the press right away: redraw and flush now instead of waiting for the
// event loop, which may already hold the release of a quick tap
void OskWidget::press_feedback(int row, int col) {
  Fl::remove_timeout(flash_cb, this);
  flash_row_ = row;
  flash_col_ = col;
  redraw();
  Fl::flush();
}

const OskLayout &OskWidget::current() const {
  const OskLayout *l = osk_layout(symbols_ ? "symbols" : layout_);
  return l ? *l : osk_layouts()[0];
}

void OskWidget::layout(const std::string &id) {
  if (osk_layout(id) && id != "symbols") layout_ = id;
  symbols_ = false;
  redraw();
}

static const int PAD = 6, GAP = 5;

bool OskWidget::key_rect(int row, int col, int &kx, int &ky, int &kw, int &kh) const {
  const OskLayout &l = current();
  if (row < 0 || row >= (int)l.rows.size()) return false;
  const auto &keys = l.rows[row];
  if (col < 0 || col >= (int)keys.size()) return false;
  int nrows = (int)l.rows.size();
  kh = (h() - 2 * PAD - (nrows - 1) * GAP) / nrows;
  ky = y() + PAD + row * (kh + GAP);
  float units = 0;
  for (auto &k : keys) units += k.width;
  double unit_w = (w() - 2 * PAD - (keys.size() - 1) * GAP) / units;
  double xx = x() + PAD;
  for (int c = 0; c < col; c++) xx += keys[c].width * unit_w + GAP;
  kx = (int)lround(xx);
  kw = (int)lround(keys[col].width * unit_w);
  return true;
}

bool OskWidget::key_at(int ex, int ey, int &row, int &col) const {
  const OskLayout &l = current();
  for (int r = 0; r < (int)l.rows.size(); r++)
    for (int c = 0; c < (int)l.rows[r].size(); c++) {
      int kx, ky, kw, kh;
      key_rect(r, c, kx, ky, kw, kh);
      // the gaps belong to the key on their left/top so there are no dead spots
      if (ex >= kx && ex < kx + kw + GAP && ey >= ky && ey < ky + kh + GAP) {
        row = r;
        col = c;
        return true;
      }
    }
  return false;
}

std::string OskWidget::key_label(const OskKey &k) const {
  if (k.special == OSK_LAYOUT) {
    std::string up = layout_;
    for (auto &c : up) c = (char)toupper((unsigned char)c);
    return up;
  }
  if (k.special == OSK_SPACE) {
    const OskLayout *l = osk_layout(layout_);
    return l ? l->name : "";
  }
  if (k.special != OSK_NONE) return k.label;
  bool upper = shift_ != caps_;
  uint32_t cp = upper ? k.shifted : k.normal;
  // letters show their case; other keys show the shifted symbol small (drawn separately)
  char buf[8];
  int n = fl_utf8encode(cp, buf);
  return std::string(buf, n);
}

void OskWidget::draw() {
  const Palette &p = palette();
  Fl_Color bg = p.dark ? fl_rgb_color(0x16, 0x17, 0x1a) : fl_rgb_color(0xd3, 0xd6, 0xdb);
  Fl_Color key_bg = p.dark ? fl_rgb_color(0x3a, 0x3c, 0x42) : FL_WHITE;
  Fl_Color special_bg = p.dark ? fl_rgb_color(0x2a, 0x2c, 0x31) : fl_rgb_color(0xb7, 0xbc, 0xc4);
  Fl_Color fg = p.dark ? fl_rgb_color(0xee, 0xee, 0xee) : fl_rgb_color(0x22, 0x24, 0x28);
  fl_color(bg);
  fl_rectf(x(), y(), w(), h());
  const OskLayout &l = current();
  for (int r = 0; r < (int)l.rows.size(); r++) {
    for (int c = 0; c < (int)l.rows[r].size(); c++) {
      const OskKey &k = l.rows[r][c];
      int kx, ky, kw, kh;
      key_rect(r, c, kx, ky, kw, kh);
      bool pressed = (r == pressed_row_ && c == pressed_col_) || (r == flash_row_ && c == flash_col_);
      bool latched = (k.special == OSK_SHIFT && shift_) || (k.special == OSK_CAPS && caps_) ||
                     (k.special == OSK_CTRL && ctrl_) || (k.special == OSK_ALT && alt_);
      Fl_Color kb = k.special != OSK_NONE && k.special != OSK_SPACE ? special_bg : key_bg;
      if (latched) kb = p.accent;
      if (pressed) kb = latched ? fl_color_average(p.accent, FL_BLACK, 0.75f) : p.accent;
      fl_color(kb);
      // a pressed key sinks in a little
      int in = pressed ? 2 : 0;
      fl_rounded_rectf(kx + in, ky + in, kw - 2 * in, kh - 2 * in, 6);
      std::string lab = key_label(k);
      int fs = std::max(10, kh * 2 / 5);
      if (k.special != OSK_NONE && lab.size() > 3) fs = std::max(10, kh / 3);
      if (k.special == OSK_SPACE) fs = std::max(9, kh / 4);
      fl_font(FL_HELVETICA, fs);
      fl_color(latched || pressed ? p.accent_fg : (k.special == OSK_SPACE ? p.fg_dim : fg));
      fl_draw(lab.c_str(), (int)lab.size(), kx + (kw - (int)fl_width(lab.c_str())) / 2, ky + (kh + fs) / 2 - 2);
      // the shifted symbol of number / punctuation keys in the corner
      if (k.special == OSK_NONE && k.shifted && k.shifted != (uint32_t)towupper((wint_t)k.normal) &&
          !(shift_ != caps_)) {
        char buf[8];
        int n = fl_utf8encode(k.shifted, buf);
        fl_font(FL_HELVETICA, std::max(8, kh / 4));
        fl_color(pressed ? p.accent_fg : p.fg_dim);
        fl_draw(buf, n, kx + kw - (int)fl_width(buf, n) - 5, ky + std::max(8, kh / 4) + 2);
      }
    }
  }
  int pr = pressed_row_ >= 0 ? pressed_row_ : flash_row_;
  int pc = pressed_row_ >= 0 ? pressed_col_ : flash_col_;
  if (key_preview_ && pr >= 0) draw_preview(pr, pc);
}

// an enlarged copy of a character key above the finger, kept inside the widget
void OskWidget::draw_preview(int row, int col) {
  int kx, ky, kw, kh;
  if (!key_rect(row, col, kx, ky, kw, kh)) return;
  const OskKey &k = current().rows[row][col];
  if (k.special != OSK_NONE) return;
  const Palette &p = palette();
  int bw = std::max(kw * 3 / 2, kh), bh = kh * 3 / 2;
  int bx = kx + (kw - bw) / 2, by = ky - bh + kh / 3;
  bx = std::max(x() + 2, std::min(bx, x() + w() - bw - 2));
  by = std::max(y() + 2, by);
  fl_color(fl_color_average(FL_BLACK, p.accent, 0.35f));
  fl_rounded_rectf(bx - 1, by - 1, bw + 2, bh + 2, 8);
  fl_color(p.accent);
  fl_rounded_rectf(bx, by, bw, bh, 8);
  std::string lab = key_label(k);
  int fs = std::max(14, bh * 11 / 20);
  fl_font(FL_HELVETICA_BOLD, fs);
  fl_color(p.accent_fg);
  fl_draw(lab.c_str(), (int)lab.size(), bx + (bw - (int)fl_width(lab.c_str())) / 2, by + (bh + fs) / 2 - 3);
}

void OskWidget::activate(const OskKey &k) {
  unsigned mods = (ctrl_ ? OSK_MOD_CTRL : 0) | (alt_ ? OSK_MOD_ALT : 0);
  bool clear_mods = true;
  switch (k.special) {
    case OSK_SHIFT: {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      double now = ts.tv_sec + ts.tv_nsec / 1e9;
      if (shift_ && now - last_shift_time_ < 0.4) { caps_ = !caps_; shift_ = false; }
      else shift_ = !shift_;
      last_shift_time_ = now;
      clear_mods = false;
      break;
    }
    case OSK_CAPS: caps_ = !caps_; clear_mods = false; break;
    case OSK_CTRL: ctrl_ = !ctrl_; clear_mods = false; break;
    case OSK_ALT: alt_ = !alt_; clear_mods = false; break;
    case OSK_SYMBOLS: symbols_ = true; clear_mods = false; break;
    case OSK_LETTERS: symbols_ = false; clear_mods = false; break;
    case OSK_LAYOUT: {
      // cycle through the letter layouts
      const auto &ls = osk_layouts();
      size_t i = 0;
      for (; i < ls.size(); i++) if (ls[i].id == layout_) break;
      do { i = (i + 1) % ls.size(); } while (ls[i].id == "symbols");
      layout_ = ls[i].id;
      symbols_ = false;
      if (on_layout_changed) on_layout_changed(this, cb_data);
      clear_mods = false;
      break;
    }
    case OSK_HIDE:
      if (on_hide) on_hide(this, cb_data);
      return;
    case OSK_SPACE:
      if (sink_) sink_->send_char(' ', mods);
      break;
    case OSK_NONE: {
      uint32_t cp = (shift_ != caps_) ? k.shifted : k.normal;
      if (sink_) sink_->send_char(cp, mods);
      break;
    }
    default:
      if (sink_) sink_->send_special(k.special, mods | (shift_ ? OSK_MOD_SHIFT : 0));
      break;
  }
  if (clear_mods) shift_ = ctrl_ = alt_ = false;
  redraw();
}

void OskWidget::repeat_cb(void *data) {
  OskWidget *o = static_cast<OskWidget *>(data);
  const OskLayout &l = o->current();
  if (o->pressed_row_ < 0 || o->pressed_row_ >= (int)l.rows.size()) return;
  const OskKey &k = l.rows[o->pressed_row_][o->pressed_col_];
  o->activate(k);
  Fl::repeat_timeout(0.06, repeat_cb, data);
}

int OskWidget::handle(int e) {
  switch (e) {
    case FL_FOCUS:
      return 0; // never take the keyboard focus away from the input being typed into
    case FL_PUSH: {
      int r, c;
      if (!key_at(Fl::event_x(), Fl::event_y(), r, c)) return 1;
      pressed_row_ = r;
      pressed_col_ = c;
      const OskKey &k = current().rows[r][c];
      press_feedback(r, c);
      // keys that repeat while held
      if (k.special == OSK_BACKSPACE || k.special == OSK_LEFT || k.special == OSK_RIGHT ||
          k.special == OSK_UP || k.special == OSK_DOWN || k.special == OSK_DELETE) {
        activate(k);
        Fl::add_timeout(0.5, repeat_cb, this);
        pressed_row_ = r;
        pressed_col_ = c;
      }
      return 1;
    }
    case FL_DRAG: {
      int r, c;
      if (key_at(Fl::event_x(), Fl::event_y(), r, c) && (r != pressed_row_ || c != pressed_col_)) {
        Fl::remove_timeout(repeat_cb, this);
        pressed_row_ = r;
        pressed_col_ = c;
        press_feedback(r, c);
      }
      return 1;
    }
    case FL_RELEASE: {
      Fl::remove_timeout(repeat_cb, this);
      int r = pressed_row_, c = pressed_col_;
      pressed_row_ = pressed_col_ = -1;
      if (r >= 0) {
        const OskKey &k = current().rows[r][c];
        bool repeats = k.special == OSK_BACKSPACE || k.special == OSK_LEFT || k.special == OSK_RIGHT ||
                       k.special == OSK_UP || k.special == OSK_DOWN || k.special == OSK_DELETE;
        const OskLayout *before = &current();
        if (!repeats) activate(k); // repeating keys already fired on press
        // keep the released key lit briefly (not when the key switched the
        // layout: the keys moved)
        if (&current() == before) {
          flash_row_ = r;
          flash_col_ = c;
          Fl::remove_timeout(flash_cb, this);
          Fl::add_timeout(FLASH_TIME, flash_cb, this);
        } else {
          flash_row_ = flash_col_ = -1;
        }
      }
      redraw();
      return 1;
    }
  }
  return Fl_Widget::handle(e);
}

} // namespace fld
