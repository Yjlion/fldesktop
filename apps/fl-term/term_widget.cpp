#include "term_widget.h"

#include <FL/Fl.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fld/theme.h"

// ---- construction ------------------------------------------------------------

TermWidget::TermWidget(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
  box(FL_FLAT_BOX);
  font_size_ = fld::mono_size();
  vt_ = vterm_new(rows_, cols_);
  vterm_set_utf8(vt_, 1);
  vterm_output_set_callback(vt_, output_cb, this);
  screen_ = vterm_obtain_screen(vt_);
  state_ = vterm_obtain_state(vt_);
  static VTermScreenCallbacks cbs;
  memset(&cbs, 0, sizeof cbs);
  cbs.damage = damage_cb;
  cbs.movecursor = movecursor_cb;
  cbs.settermprop = settermprop_cb;
  cbs.bell = bell_cb;
  cbs.sb_pushline = sb_pushline_cb;
  cbs.sb_popline = sb_popline_cb;
  cbs.sb_clear = sb_clear_cb;
  vterm_screen_set_callbacks(screen_, &cbs, this);
  vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_SCROLL);
  vterm_screen_enable_altscreen(screen_, 1);
  vterm_screen_enable_reflow(screen_, true);
  vterm_state_set_bold_highbright(state_, 1);
  vterm_screen_reset(screen_, 1);
  measure_font();
  Fl::add_timeout(0.53, blink_cb, this);
}

TermWidget::~TermWidget() {
  Fl::remove_timeout(blink_cb, this);
  if (fd_ >= 0) {
    Fl::remove_fd(fd_);
    close(fd_);
  }
  if (pid_ > 0) {
    kill(-pid_, SIGHUP);
    kill(pid_, SIGHUP);
    waitpid(pid_, nullptr, WNOHANG);
  }
  vterm_free(vt_);
}

void TermWidget::measure_font() {
  fl_font(fld::mono_font(), font_size_);
  double w = fl_width("M", 1);
  cw_ = (int)std::ceil(w - 0.05);
  if (cw_ < 1) cw_ = 1;
  ch_ = fl_height();
  ascent_ = fl_height() - fl_descent();
}

void TermWidget::set_font_size(int size) {
  if (size < 6 || size > 72) return;
  font_size_ = size;
  measure_font();
  update_size();
  redraw();
}

void TermWidget::size_for(int cols, int rows, int &w, int &h) {
  w = cols * cw_ + 2 * pad_;
  h = rows * ch_ + 2 * pad_;
}

void TermWidget::set_colors(const TermColors &c) {
  colors_ = c;
  VTermColor fg, bg;
  vterm_color_rgb(&fg, c.fg[0], c.fg[1], c.fg[2]);
  vterm_color_rgb(&bg, c.bg[0], c.bg[1], c.bg[2]);
  vterm_screen_set_default_colors(screen_, &fg, &bg);
  for (int i = 0; i < 16; i++) {
    VTermColor col;
    vterm_color_rgb(&col, c.palette[i][0], c.palette[i][1], c.palette[i][2]);
    vterm_state_set_palette_color(state_, i, &col);
  }
  color(fl_rgb_color(c.bg[0], c.bg[1], c.bg[2]));
  redraw();
}

void TermWidget::set_scrollbar(Fl_Scrollbar *sb) {
  scrollbar_ = sb;
  update_scrollbar();
}

// ---- child process -------------------------------------------------------------

bool TermWidget::start(const std::vector<std::string> &argv_in, const std::string &cwd) {
  std::vector<std::string> argv = argv_in;
  if (argv.empty()) {
    const char *sh = getenv("SHELL");
    std::string shell = (sh && *sh) ? sh : "";
    if (shell.empty()) {
      struct passwd *pw = getpwuid(getuid());
      shell = (pw && pw->pw_shell && *pw->pw_shell) ? pw->pw_shell : "/bin/sh";
    }
    argv.push_back(shell);
  }
  struct winsize ws{};
  ws.ws_row = rows_;
  ws.ws_col = cols_;
  ws.ws_xpixel = cols_ * cw_;
  ws.ws_ypixel = rows_ * ch_;
  pid_t pid = forkpty(&fd_, nullptr, nullptr, &ws);
  if (pid < 0) return false;
  if (pid == 0) {
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("TERM_PROGRAM", "fl-term", 1);
    unsetenv("COLUMNS");
    unsetenv("LINES");
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    for (int s = 1; s < NSIG; s++) sigaction(s, &sa, nullptr);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) { /* stay where we are */ }
    std::vector<char *> args;
    for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    fprintf(stderr, "fl-term: cannot execute %s: %s\n", args[0], strerror(errno));
    _exit(127);
  }
  pid_ = pid;
  fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
  fcntl(fd_, F_SETFD, FD_CLOEXEC);
  Fl::add_fd(fd_, FL_READ, pty_cb, this);
  update_size();
  return true;
}

std::string TermWidget::current_dir() const {
  if (fd_ < 0 || pid_ <= 0) return "";
  pid_t fg = tcgetpgrp(fd_);
  for (pid_t p : {fg, pid_}) {
    if (p <= 0) continue;
    char link[64], buf[PATH_MAX];
    snprintf(link, sizeof link, "/proc/%d/cwd", (int)p);
    ssize_t n = readlink(link, buf, sizeof buf - 1);
    if (n > 0) {
      buf[n] = 0;
      return buf;
    }
  }
  return "";
}

void TermWidget::pty_cb(int fd, void *data) { static_cast<TermWidget *>(data)->read_pty(); }

void TermWidget::read_pty() {
  char buf[65536];
  bool got = false;
  for (int i = 0; i < 16; i++) { // bounded so the UI stays responsive
    ssize_t n = read(fd_, buf, sizeof buf);
    if (n > 0) {
      vterm_input_write(vt_, buf, n);
      got = true;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
    // EOF / EIO: the child is gone
    Fl::remove_fd(fd_);
    close(fd_);
    fd_ = -1;
    int status = 0;
    if (pid_ > 0) waitpid(pid_, &status, 0);
    pid_ = -1;
    if (got) vterm_screen_flush_damage(screen_);
    if (!hold_ && on_exit) on_exit(this, status, cb_data);
    else {
      const char *msg = "\r\n\x1b[2m[process exited]\x1b[0m";
      vterm_input_write(vt_, msg, strlen(msg));
      redraw();
    }
    return;
  }
  if (got) {
    vterm_screen_flush_damage(screen_);
    if (scroll_ == 0) update_scrollbar();
    redraw();
  }
}

void TermWidget::write_pty(const char *data, size_t len) {
  if (fd_ < 0) return;
  out_pending_.insert(out_pending_.end(), data, data + len);
  flush_output();
}

static void flush_timer(void *d);

void TermWidget::flush_output() {
  while (!out_pending_.empty() && fd_ >= 0) {
    ssize_t n = write(fd_, out_pending_.data(), out_pending_.size());
    if (n > 0) {
      out_pending_.erase(out_pending_.begin(), out_pending_.begin() + n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  if (!out_pending_.empty() && fd_ >= 0 && !Fl::has_timeout(flush_timer, this))
    Fl::add_timeout(0.01, flush_timer, this);
}

static void flush_timer(void *d) {
  TermWidget *t = static_cast<TermWidget *>(d);
  t->do_handle_flush();
}

void TermWidget::do_handle_flush() { flush_output(); }

// ---- libvterm callbacks ------------------------------------------------------------

void TermWidget::output_cb(const char *s, size_t len, void *user) {
  static_cast<TermWidget *>(user)->write_pty(s, len);
}

int TermWidget::damage_cb(VTermRect, void *user) {
  static_cast<TermWidget *>(user)->redraw();
  return 1;
}

int TermWidget::movecursor_cb(VTermPos pos, VTermPos, int visible, void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  t->cursor_ = pos;
  t->cursor_visible_ = visible;
  t->reset_blink();
  t->redraw();
  return 1;
}

int TermWidget::settermprop_cb(VTermProp prop, VTermValue *val, void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: t->cursor_visible_ = val->boolean; break;
    case VTERM_PROP_CURSORBLINK: t->cursor_blink_ = val->boolean; break;
    case VTERM_PROP_ALTSCREEN:
      t->altscreen_ = val->boolean;
      t->scroll_ = 0;
      t->update_scrollbar();
      break;
    case VTERM_PROP_TITLE:
      // libvterm 0.3 may deliver the title in fragments
      if (val->string.initial) t->title_.clear();
      t->title_.append(val->string.str, val->string.len);
      if (val->string.final && t->on_title) t->on_title(t, t->title_.c_str(), t->cb_data);
      break;
    case VTERM_PROP_REVERSE: t->reverse_video_ = val->boolean; break;
    case VTERM_PROP_CURSORSHAPE: t->cursor_shape_ = val->number; break;
    case VTERM_PROP_MOUSE: t->mouse_mode_ = val->number; break;
    case VTERM_PROP_FOCUSREPORT: t->focus_report_ = val->boolean; break;
    default: return 0;
  }
  t->redraw();
  return 1;
}

int TermWidget::bell_cb(void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  if (!t->window() || !t->window()->visible() || Fl::focus() != t) fl_beep(FL_BEEP_DEFAULT);
  return 1;
}

int TermWidget::sb_pushline_cb(int cols, const VTermScreenCell *cells, void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  std::vector<VTermScreenCell> line(cells, cells + cols);
  // trim trailing blank cells to save memory
  while (!line.empty() && line.back().chars[0] == 0 && VTERM_COLOR_IS_DEFAULT_BG(&line.back().bg))
    line.pop_back();
  for (auto &c : line) {
    if (VTERM_COLOR_IS_INDEXED(&c.fg)) vterm_screen_convert_color_to_rgb(t->screen_, &c.fg);
    if (VTERM_COLOR_IS_INDEXED(&c.bg)) vterm_screen_convert_color_to_rgb(t->screen_, &c.bg);
  }
  t->sb_.push_back(std::move(line));
  if (t->scroll_ > 0) t->scroll_++; // keep the viewed text in place
  if (t->sb_.size() > t->sb_limit_) {
    t->sb_.pop_front();
    if (t->has_sel_) { t->sel_a_line_--; t->sel_b_line_--; }
  }
  if (t->scroll_ > (int)t->sb_.size()) t->scroll_ = (int)t->sb_.size();
  return 1;
}

int TermWidget::sb_popline_cb(int cols, VTermScreenCell *cells, void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  if (t->sb_.empty()) return 0;
  auto &line = t->sb_.back();
  for (int i = 0; i < cols; i++) {
    if (i < (int)line.size()) cells[i] = line[i];
    else {
      memset(&cells[i], 0, sizeof cells[i]);
      cells[i].width = 1;
      vterm_state_get_default_colors(t->state_, &cells[i].fg, &cells[i].bg);
    }
  }
  t->sb_.pop_back();
  if (t->scroll_ > (int)t->sb_.size()) t->scroll_ = (int)t->sb_.size();
  return 1;
}

int TermWidget::sb_clear_cb(void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  t->sb_.clear();
  t->scroll_ = 0;
  t->has_sel_ = false;
  t->update_scrollbar();
  return 1;
}

// ---- geometry -------------------------------------------------------------------

void TermWidget::resize(int X, int Y, int W, int H) {
  Fl_Widget::resize(X, Y, W, H);
  update_size();
}

void TermWidget::update_size() {
  int cols = std::max(2, (w() - 2 * pad_) / cw_);
  int rows = std::max(1, (h() - 2 * pad_) / ch_);
  if (cols != cols_ || rows != rows_) {
    cols_ = cols;
    rows_ = rows;
    vterm_set_size(vt_, rows_, cols_);
    vterm_screen_flush_damage(screen_);
  }
  if (fd_ >= 0) {
    struct winsize ws{};
    ws.ws_row = rows_;
    ws.ws_col = cols_;
    ws.ws_xpixel = cols_ * cw_;
    ws.ws_ypixel = rows_ * ch_;
    ioctl(fd_, TIOCSWINSZ, &ws);
  }
  update_scrollbar();
}

void TermWidget::update_scrollbar() {
  if (!scrollbar_) return;
  int total = (int)sb_.size() + rows_;
  scrollbar_->value((int)sb_.size() - scroll_, rows_, 0, total);
  scrollbar_->linesize(3);
  if (altscreen_ || sb_.empty()) scrollbar_->deactivate();
  else scrollbar_->activate();
}

void TermWidget::scroll_to(int offset) {
  offset = std::max(0, std::min(offset, (int)sb_.size()));
  if (offset == scroll_) return;
  scroll_ = offset;
  update_scrollbar();
  redraw();
}

long TermWidget::abs_line_of_row(int r) const { return (long)sb_.size() - scroll_ + r; }

bool TermWidget::get_cell(long line, int col, VTermScreenCell &cell) const {
  long nsb = (long)sb_.size();
  if (line < nsb) {
    const auto &l = sb_[line];
    if (col < (int)l.size()) {
      cell = l[col];
      return true;
    }
    memset(&cell, 0, sizeof cell);
    cell.width = 1;
    vterm_state_get_default_colors(state_, &cell.fg, &cell.bg);
    return true;
  }
  VTermPos pos{(int)(line - nsb), col};
  if (pos.row >= rows_ || col >= cols_) return false;
  return vterm_screen_get_cell(screen_, pos, &cell) != 0;
}

void TermWidget::cell_at(int ex, int ey, int &row, int &col) const {
  col = (ex - x() - pad_) / cw_;
  row = (ey - y() - pad_) / ch_;
  col = std::max(0, std::min(col, cols_ - 1));
  row = std::max(0, std::min(row, rows_ - 1));
}

// ---- drawing ----------------------------------------------------------------------

void TermWidget::resolve_color(const VTermColor &c_in, bool is_fg, uint8_t rgb[3]) const {
  if (is_fg ? VTERM_COLOR_IS_DEFAULT_FG(&c_in) : VTERM_COLOR_IS_DEFAULT_BG(&c_in)) {
    memcpy(rgb, is_fg ? colors_.fg : colors_.bg, 3);
    return;
  }
  VTermColor c = c_in;
  if (VTERM_COLOR_IS_INDEXED(&c)) vterm_screen_convert_color_to_rgb(screen_, &c);
  rgb[0] = c.rgb.red;
  rgb[1] = c.rgb.green;
  rgb[2] = c.rgb.blue;
}

bool TermWidget::in_selection(long line, int col) const {
  if (!has_sel_) return false;
  long al = sel_a_line_, bl = sel_b_line_;
  int ac = sel_a_col_, bc = sel_b_col_;
  if (al > bl || (al == bl && ac > bc)) { std::swap(al, bl); std::swap(ac, bc); }
  if (line < al || line > bl) return false;
  if (line == al && col < ac) return false;
  if (line == bl && col > bc) return false;
  return true;
}

static inline Fl_Color rgbc(const uint8_t c[3]) { return fl_rgb_color(c[0], c[1], c[2]); }

void TermWidget::draw() {
  fl_push_clip(x(), y(), w(), h());
  fl_color(rgbc(colors_.bg));
  fl_rectf(x(), y(), w(), h());
  const bool focused = Fl::focus() == this && window() && window()->visible();
  Fl_Font base = fld::mono_font();

  for (int r = 0; r < rows_; r++) {
    long line = abs_line_of_row(r);
    int ry = y() + pad_ + r * ch_;
    for (int c = 0; c < cols_;) {
      VTermScreenCell cell;
      if (!get_cell(line, c, cell)) { c++; continue; }
      int width = cell.width > 0 ? cell.width : 1;
      uint8_t fg[3], bg[3];
      resolve_color(cell.fg, true, fg);
      resolve_color(cell.bg, false, bg);
      bool rev = cell.attrs.reverse ^ reverse_video_;
      if (rev) { uint8_t t[3]; memcpy(t, fg, 3); memcpy(fg, bg, 3); memcpy(bg, t, 3); }
      if (in_selection(line, c)) memcpy(bg, colors_.sel_bg, 3);
      bool is_cursor = scroll_ == 0 && cursor_visible_ && r == cursor_.row && c == cursor_.col;
      int cx = x() + pad_ + c * cw_;
      int cwidth = cw_ * width;
      if (memcmp(bg, colors_.bg, 3) != 0) {
        fl_color(rgbc(bg));
        // extend the last column / row backgrounds into the padding
        int ex = (c + width >= cols_) ? x() + w() - cx : cwidth;
        fl_rectf(cx, ry, ex, ch_);
      }
      bool block_cursor = is_cursor && focused && (blink_on_ || !cursor_blink_) &&
                          cursor_shape_ == VTERM_PROP_CURSORSHAPE_BLOCK;
      if (block_cursor) {
        fl_color(rgbc(colors_.cursor));
        fl_rectf(cx, ry, cwidth, ch_);
        memcpy(fg, colors_.bg, 3);
      }
      if (cell.chars[0] && cell.chars[0] != ' ' && !cell.attrs.conceal) {
        char buf[VTERM_MAX_CHARS_PER_CELL * 4 + 1];
        int n = 0;
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
          n += fl_utf8encode(cell.chars[i], buf + n);
        buf[n] = 0;
        Fl_Font f = base + (cell.attrs.bold ? FL_BOLD : 0) + (cell.attrs.italic ? FL_ITALIC : 0);
        fl_font(f, font_size_);
        fl_color(rgbc(fg));
        fl_draw(buf, n, cx, ry + ascent_);
      }
      if (cell.attrs.underline) {
        fl_color(rgbc(fg));
        fl_xyline(cx, ry + ch_ - 2, cx + cwidth - 1);
        if (cell.attrs.underline == VTERM_UNDERLINE_DOUBLE) fl_xyline(cx, ry + ch_ - 4, cx + cwidth - 1);
      }
      if (cell.attrs.strike) {
        fl_color(rgbc(fg));
        fl_xyline(cx, ry + ch_ / 2, cx + cwidth - 1);
      }
      if (is_cursor && !block_cursor) {
        fl_color(rgbc(colors_.cursor));
        if (!focused) fl_rect(cx, ry, cwidth, ch_);
        else if (blink_on_ || !cursor_blink_) {
          if (cursor_shape_ == VTERM_PROP_CURSORSHAPE_UNDERLINE) fl_rectf(cx, ry + ch_ - 2, cwidth, 2);
          else if (cursor_shape_ == VTERM_PROP_CURSORSHAPE_BAR_LEFT) fl_rectf(cx, ry, 2, ch_);
        }
      }
      c += width;
    }
  }
  // scroll position indicator while looking at the scrollback
  if (scroll_ > 0 && !sb_.empty()) {
    int total = (int)sb_.size() + rows_;
    int bar_h = std::max(20, h() * rows_ / total);
    int top = (int)((long)(h() - bar_h) * ((long)sb_.size() - scroll_) / (long)sb_.size());
    fl_color(fl_color_average(rgbc(colors_.fg), rgbc(colors_.bg), 0.45f));
    fl_rectf(x() + w() - 5, y() + top, 3, bar_h);
  }
  fl_pop_clip();
}

void TermWidget::reset_blink() {
  blink_on_ = true;
  Fl::remove_timeout(blink_cb, this);
  Fl::add_timeout(0.53, blink_cb, this);
}

void TermWidget::blink_cb(void *data) {
  TermWidget *t = static_cast<TermWidget *>(data);
  if (t->cursor_blink_ && Fl::focus() == t) {
    t->blink_on_ = !t->blink_on_;
    t->redraw();
  }
  Fl::repeat_timeout(0.53, blink_cb, data);
}

// ---- selection & clipboard -----------------------------------------------------------

std::string TermWidget::selection_text() const {
  if (!has_sel_) return "";
  long al = sel_a_line_, bl = sel_b_line_;
  int ac = sel_a_col_, bc = sel_b_col_;
  if (al > bl || (al == bl && ac > bc)) { std::swap(al, bl); std::swap(ac, bc); }
  std::string out;
  for (long l = al; l <= bl; l++) {
    int c0 = (l == al) ? ac : 0, c1 = (l == bl) ? bc : cols_ - 1;
    std::string line;
    for (int c = c0; c <= c1; c++) {
      VTermScreenCell cell;
      if (!get_cell(l, c, cell)) break;
      if (!cell.chars[0]) { line += ' '; continue; }
      char buf[8];
      for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
        line.append(buf, fl_utf8encode(cell.chars[i], buf));
      if (cell.width > 1) c += cell.width - 1;
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    out += line;
    if (l != bl) out += '\n';
  }
  return out;
}

void TermWidget::copy_selection(int clipboard) {
  std::string s = selection_text();
  if (!s.empty()) Fl::copy(s.c_str(), (int)s.size(), clipboard);
}

void TermWidget::paste(int clipboard) { Fl::paste(*this, clipboard); }

static bool word_char(uint32_t ch) {
  if (ch == 0) return false;
  if (ch > 127) return true;
  return isalnum((int)ch) || strchr("-_./~:@%+?=&#", (int)ch);
}

void TermWidget::select_word(long line, int col) {
  VTermScreenCell cell;
  if (!get_cell(line, col, cell) || !word_char(cell.chars[0])) {
    sel_a_line_ = sel_b_line_ = line;
    sel_a_col_ = sel_b_col_ = col;
    return;
  }
  int a = col, b = col;
  while (a > 0 && get_cell(line, a - 1, cell) && word_char(cell.chars[0])) a--;
  while (b < cols_ - 1 && get_cell(line, b + 1, cell) && word_char(cell.chars[0])) b++;
  sel_a_line_ = sel_b_line_ = line;
  sel_a_col_ = a;
  sel_b_col_ = b;
}

// ---- input ---------------------------------------------------------------------------------

VTermModifier TermWidget::modifiers() const {
  int s = Fl::event_state();
  int m = VTERM_MOD_NONE;
  if (s & FL_SHIFT) m |= VTERM_MOD_SHIFT;
  if (s & FL_ALT) m |= VTERM_MOD_ALT;
  if (s & FL_CTRL) m |= VTERM_MOD_CTRL;
  return (VTermModifier)m;
}

int TermWidget::key_to_vterm(int key, VTermKey &out) const {
  switch (key) {
    case FL_Enter: case FL_KP_Enter: out = VTERM_KEY_ENTER; return 1;
    case FL_Tab: out = VTERM_KEY_TAB; return 1;
    case FL_BackSpace: out = VTERM_KEY_BACKSPACE; return 1;
    case FL_Escape: out = VTERM_KEY_ESCAPE; return 1;
    case FL_Up: out = VTERM_KEY_UP; return 1;
    case FL_Down: out = VTERM_KEY_DOWN; return 1;
    case FL_Left: out = VTERM_KEY_LEFT; return 1;
    case FL_Right: out = VTERM_KEY_RIGHT; return 1;
    case FL_Insert: out = VTERM_KEY_INS; return 1;
    case FL_Delete: out = VTERM_KEY_DEL; return 1;
    case FL_Home: out = VTERM_KEY_HOME; return 1;
    case FL_End: out = VTERM_KEY_END; return 1;
    case FL_Page_Up: out = VTERM_KEY_PAGEUP; return 1;
    case FL_Page_Down: out = VTERM_KEY_PAGEDOWN; return 1;
  }
  if (key > FL_F && key <= FL_F_Last) {
    out = (VTermKey)VTERM_KEY_FUNCTION(key - FL_F);
    return 1;
  }
  return 0;
}

int TermWidget::handle(int e) {
  switch (e) {
    case FL_FOCUS:
    case FL_UNFOCUS:
      if (focus_report_) {
        if (e == FL_FOCUS) vterm_state_focus_in(state_);
        else vterm_state_focus_out(state_);
      }
      reset_blink();
      redraw();
      return 1;

    case FL_KEYBOARD: {
      int key = Fl::event_key();
      int st = Fl::event_state();
      bool ctrl = st & FL_CTRL, shift = st & FL_SHIFT, alt = st & FL_ALT;
      // terminal shortcuts
      if (ctrl && shift) {
        switch (key) {
          case 'c': copy_selection(1); return 1;
          case 'v': paste(1); return 1;
        }
      }
      if (shift && !ctrl && !alt) {
        if (key == FL_Page_Up) { scroll_to(scroll_ + rows_ - 1); return 1; }
        if (key == FL_Page_Down) { scroll_to(scroll_ - rows_ + 1); return 1; }
        if (key == FL_Insert) { paste(0); return 1; }
      }
      if (ctrl && !alt) {
        if (key == '=' || key == '+' || key == FL_KP + '+') { set_font_size(font_size_ + 1); return 2; }
        if (key == '-' || key == FL_KP + '-') { set_font_size(font_size_ - 1); return 2; }
        if (key == '0' && !shift) { set_font_size(fld::mono_size()); return 2; }
      }
      VTermModifier mod = modifiers();
      VTermKey vk;
      if (key_to_vterm(key, vk)) {
        vterm_keyboard_key(vt_, vk, mod);
      } else if (key >= FL_KP && key <= FL_KP_Last && Fl::event_length() > 0) {
        vterm_keyboard_unichar(vt_, (unsigned char)Fl::event_text()[0], (VTermModifier)(mod & ~VTERM_MOD_SHIFT));
      } else if (ctrl && key < 128 && key >= ' ') {
        // control characters from the unshifted key
        vterm_keyboard_unichar(vt_, key, (VTermModifier)(mod & ~VTERM_MOD_SHIFT));
      } else if (Fl::event_length() > 0) {
        const char *t = Fl::event_text();
        const char *end = t + Fl::event_length();
        // shift is already part of the produced text
        VTermModifier m = (VTermModifier)(mod & ~VTERM_MOD_SHIFT);
        while (t < end) {
          int len;
          unsigned ch = fl_utf8decode(t, end, &len);
          vterm_keyboard_unichar(vt_, ch, m);
          t += len;
        }
      } else {
        return 0;
      }
      if (scroll_) scroll_to(0);
      has_sel_ = false;
      reset_blink();
      return 1;
    }

    case FL_PASTE: {
      std::string text(Fl::event_text(), Fl::event_length());
      for (auto &ch : text)
        if (ch == '\n') ch = '\r';
      vterm_keyboard_start_paste(vt_);
      write_pty(text.data(), text.size());
      vterm_keyboard_end_paste(vt_);
      if (scroll_) scroll_to(0);
      return 1;
    }

    case FL_ENTER:
      fl_cursor(FL_CURSOR_INSERT);
      return 1;
    case FL_LEAVE:
      fl_cursor(FL_CURSOR_DEFAULT);
      return 1;

    case FL_MOUSEWHEEL: {
      int dy = Fl::event_dy();
      if (mouse_mode_ != VTERM_PROP_MOUSE_NONE && !(Fl::event_state() & FL_SHIFT)) {
        int row, col;
        cell_at(Fl::event_x(), Fl::event_y(), row, col);
        vterm_mouse_move(vt_, row, col, modifiers());
        vterm_mouse_button(vt_, dy < 0 ? 4 : 5, true, modifiers());
        return 1;
      }
      if (altscreen_) {
        for (int i = 0; i < 3; i++) vterm_keyboard_key(vt_, dy < 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
        return 1;
      }
      scroll_to(scroll_ - dy * 3);
      return 1;
    }

    case FL_PUSH:
    case FL_DRAG:
    case FL_RELEASE:
    case FL_MOVE: {
      if (e == FL_PUSH && Fl::focus() != this) take_focus();
      int row, col;
      cell_at(Fl::event_x(), Fl::event_y(), row, col);
      bool app_mouse = mouse_mode_ != VTERM_PROP_MOUSE_NONE && !(Fl::event_state() & FL_SHIFT);
      if (app_mouse) {
        if (e == FL_MOVE && mouse_mode_ != VTERM_PROP_MOUSE_MOVE) return 1;
        if (e == FL_DRAG && mouse_mode_ < VTERM_PROP_MOUSE_DRAG) return 1;
        if (row != last_mouse_row_ || col != last_mouse_col_ || e != FL_DRAG) {
          vterm_mouse_move(vt_, row, col, modifiers());
          last_mouse_row_ = row;
          last_mouse_col_ = col;
        }
        if (e == FL_PUSH || e == FL_RELEASE)
          vterm_mouse_button(vt_, Fl::event_button(), e == FL_PUSH, modifiers());
        return 1;
      }
      if (e == FL_MOVE) return 1;
      long line = abs_line_of_row(row);
      if (e == FL_PUSH) {
        if (Fl::event_button() == FL_MIDDLE_MOUSE) { paste(0); return 1; }
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          if (on_menu) on_menu(this, cb_data);
          return 1;
        }
        click_count_ = Fl::event_clicks() + 1;
        if (click_count_ == 2) {
          select_word(line, col);
          has_sel_ = true;
          copy_selection(0);
        } else if (click_count_ >= 3) {
          sel_a_line_ = sel_b_line_ = line;
          sel_a_col_ = 0;
          sel_b_col_ = cols_ - 1;
          has_sel_ = true;
          copy_selection(0);
        } else {
          sel_a_line_ = sel_b_line_ = line;
          sel_a_col_ = sel_b_col_ = col;
          has_sel_ = false;
          selecting_ = true;
        }
        redraw();
        return 1;
      }
      if (e == FL_DRAG && selecting_) {
        // auto-scroll when dragging past the edges
        if (Fl::event_y() < y()) scroll_to(scroll_ + 1);
        else if (Fl::event_y() > y() + h()) scroll_to(scroll_ - 1);
        sel_b_line_ = abs_line_of_row(row);
        sel_b_col_ = col;
        has_sel_ = sel_b_line_ != sel_a_line_ || sel_b_col_ != sel_a_col_;
        redraw();
        return 1;
      }
      if (e == FL_RELEASE && selecting_) {
        selecting_ = false;
        if (has_sel_) copy_selection(0);
        return 1;
      }
      return 1;
    }
  }
  return Fl_Widget::handle(e);
}
