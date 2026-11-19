// fl-term terminal widget: libvterm emulation rendered with FLTK
#pragma once
#include <FL/Fl_Widget.H>
#include <deque>
#include <string>
#include <vector>
#include <sys/types.h>

extern "C" {
#include <vterm.h>
}

class Fl_Scrollbar;

struct TermColors {
  uint8_t fg[3], bg[3], cursor[3], sel_bg[3];
  uint8_t palette[16][3];
};

class TermWidget : public Fl_Widget {
public:
  TermWidget(int x, int y, int w, int h);
  ~TermWidget() override;

  // Starts argv (or the user's shell when empty) on a new pty.
  bool start(const std::vector<std::string> &argv, const std::string &cwd);
  void set_scrollbar(Fl_Scrollbar *sb);
  void set_font_size(int size);
  int font_size() const { return font_size_; }
  void set_colors(const TermColors &c);
  void set_scrollback_limit(size_t n) { sb_limit_ = n; }
  // Opacity of the default background (0.2 .. 1). Below 1 the window shows
  // what is behind it; text and coloured cells stay opaque. Needs a Wayland
  // compositor (FLTK draws with cairo into ARGB buffers there).
  void set_opacity(double o);
  double opacity() const { return opacity_; }
  // A title until the program sets one (SSH tabs show the host).
  void set_title(const std::string &t) { title_ = t; }
  void set_hold(bool h) { hold_ = h; }
  // keep the window open only when the command fails
  void set_hold_on_error(bool h) { hold_on_error_ = h; }
  // Preferred window size for cols x rows.
  void size_for(int cols, int rows, int &w, int &h);
  int cell_w() const { return cw_; }
  int cell_h() const { return ch_; }
  int rows() const { return rows_; }

  void copy_selection(int clipboard);
  void paste(int clipboard);
  void scroll_to(int offset); // lines up from the live screen
  bool child_alive() const { return pid_ > 0; }
  std::string current_dir() const; // working directory of the foreground process
  bool has_children() const;        // a program other than the shell runs in the foreground
  const std::string &title() const { return title_; }

  void (*on_title)(TermWidget *, const char *, void *) = nullptr;
  void (*on_exit)(TermWidget *, int status, void *) = nullptr;
  void (*on_menu)(TermWidget *, void *) = nullptr;
  void *cb_data = nullptr;

  void do_handle_flush(); // internal: retry pending pty writes
  int handle(int e) override;
  void draw() override;
  void resize(int x, int y, int w, int h) override;

private:
  VTerm *vt_ = nullptr;
  VTermScreen *screen_ = nullptr;
  VTermState *state_ = nullptr;
  int fd_ = -1;
  pid_t pid_ = -1;
  int rows_ = 24, cols_ = 80;
  int cw_ = 8, ch_ = 16, ascent_ = 12;
  int font_size_ = 14;
  int pad_ = 4;
  double opacity_ = 1.0;
  void fill_background(int x, int y, int w, int h);
  bool hold_ = false, hold_on_error_ = false;
  std::string title_;

  // scrollback: oldest first
  std::deque<std::vector<VTermScreenCell>> sb_;
  size_t sb_limit_ = 10000;
  int scroll_ = 0; // lines scrolled back
  Fl_Scrollbar *scrollbar_ = nullptr;

  // cursor
  VTermPos cursor_{0, 0};
  bool cursor_visible_ = true, cursor_blink_ = true, blink_on_ = true;
  int cursor_shape_ = VTERM_PROP_CURSORSHAPE_BLOCK;
  int mouse_mode_ = VTERM_PROP_MOUSE_NONE;
  bool altscreen_ = false, reverse_video_ = false, focus_report_ = false;

  // selection in absolute line coordinates: line 0 = oldest scrollback line
  bool selecting_ = false, has_sel_ = false;
  long sel_a_line_ = 0, sel_b_line_ = 0;
  int sel_a_col_ = 0, sel_b_col_ = 0;
  int click_count_ = 0;
  int last_mouse_row_ = -1, last_mouse_col_ = -1;

  TermColors colors_{};
  std::vector<char> out_pending_;

  void measure_font();
  void update_size();
  void update_scrollbar();
  void read_pty();
  void write_pty(const char *data, size_t len);
  void flush_output();
  bool get_cell(long abs_line, int col, VTermScreenCell &cell) const;
  long abs_line_of_row(int screen_row) const;
  void cell_at(int ex, int ey, int &row, int &col) const;
  std::string selection_text() const;
  bool in_selection(long line, int col) const;
  void select_word(long line, int col);
  void resolve_color(const VTermColor &c, bool is_fg, uint8_t rgb[3]) const;
  int key_to_vterm(int key, VTermKey &out) const;
  VTermModifier modifiers() const;
  void reset_blink();

  static void pty_cb(int fd, void *data);
  static void blink_cb(void *data);
  static void output_cb(const char *s, size_t len, void *user);
  static int damage_cb(VTermRect rect, void *user);
  static int movecursor_cb(VTermPos pos, VTermPos oldpos, int visible, void *user);
  static int settermprop_cb(VTermProp prop, VTermValue *val, void *user);
  static int bell_cb(void *user);
  static int sb_pushline_cb(int cols, const VTermScreenCell *cells, void *user);
  static int sb_popline_cb(int cols, VTermScreenCell *cells, void *user);
  static int sb_clear_cb(void *user);
};
