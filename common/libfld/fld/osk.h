// On-screen keyboard widget shared by fl-osk (Wayland virtual keyboard) and
// fl-greet (types into its own input fields).
#pragma once
#include <FL/Fl_Widget.H>
#include <cstdint>
#include <string>
#include <vector>

namespace fld {

enum OskSpecial {
  OSK_NONE = 0,
  OSK_BACKSPACE, OSK_ENTER, OSK_TAB, OSK_ESCAPE, OSK_LEFT, OSK_RIGHT, OSK_UP, OSK_DOWN,
  OSK_DELETE, OSK_HOME, OSK_END,
  // keys handled by the widget itself
  OSK_SHIFT, OSK_CAPS, OSK_CTRL, OSK_ALT, OSK_SYMBOLS, OSK_LETTERS, OSK_LAYOUT, OSK_HIDE, OSK_SPACE,
};

enum OskMods { OSK_MOD_SHIFT = 1, OSK_MOD_CTRL = 4, OSK_MOD_ALT = 8 };

// Where the keyboard sends its keys.
class OskSink {
public:
  virtual ~OskSink() = default;
  virtual void send_char(uint32_t codepoint, unsigned mods) = 0;
  virtual void send_special(OskSpecial key, unsigned mods) = 0;
};

// Sends the keys as FLTK keyboard events to the focused widget of this program.
class FltkKeySink : public OskSink {
public:
  void send_char(uint32_t codepoint, unsigned mods) override;
  void send_special(OskSpecial key, unsigned mods) override;
};

struct OskKey {
  OskSpecial special = OSK_NONE;
  uint32_t normal = 0, shifted = 0; // characters
  float width = 1;                  // in key units
  std::string label;                // for special keys
};

struct OskLayout {
  std::string id, name;
  std::vector<std::vector<OskKey>> rows;
};

// Parses a layout row description: tokens separated by spaces. A token is one
// character (shift = upper case), two characters (normal, shifted) or a special
// key in braces, optionally with a width: {bksp:1.5}.
std::vector<OskKey> osk_parse_row(const std::string &row);
// Built-in layouts: "us", "de", "fr", and "symbols".
const std::vector<OskLayout> &osk_layouts();
const OskLayout *osk_layout(const std::string &id);

class OskWidget : public Fl_Widget {
public:
  OskWidget(int x, int y, int w, int h);
  ~OskWidget() override;
  void sink(OskSink *s) { sink_ = s; }
  void layout(const std::string &id);
  const std::string &layout_id() const { return layout_; }
  // show an enlarged copy of a held character key above it (default on)
  void key_preview(bool on) { key_preview_ = on; }
  bool key_preview() const { return key_preview_; }
  // called when the hide key is pressed / the layout key cycles layouts
  void (*on_hide)(OskWidget *, void *) = nullptr;
  void (*on_layout_changed)(OskWidget *, void *) = nullptr;
  void *cb_data = nullptr;

  void draw() override;
  int handle(int e) override;

private:
  OskSink *sink_ = nullptr;
  std::string layout_ = "us";
  bool symbols_ = false, shift_ = false, caps_ = false, ctrl_ = false, alt_ = false;
  double last_shift_time_ = 0;
  int pressed_row_ = -1, pressed_col_ = -1;
  // a tapped key stays lit for a moment after release: a quick touch tap
  // delivers press and release together and would never show the press
  int flash_row_ = -1, flash_col_ = -1;
  bool key_preview_ = true;
  static void flash_cb(void *data);
  void press_feedback(int row, int col);
  void draw_preview(int row, int col);
  const OskLayout &current() const;
  bool key_rect(int row, int col, int &x, int &y, int &w, int &h) const;
  bool key_at(int ex, int ey, int &row, int &col) const;
  void activate(const OskKey &k);
  std::string key_label(const OskKey &k) const;
  static void repeat_cb(void *data);
};

} // namespace fld
