#include "term_widget.h"
#include "fld/atspi.h"

#include <FL/Fl.H>
#include <FL/Fl_RGB_Image.H>
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
#include <time.h>
#include <unistd.h>

#include "fld/mime.h"
#include "fld/primary.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "sixel.h"

#if defined(FLTK_USE_WAYLAND)
#include <FL/platform.H>
#include <cairo.h>
#endif

static inline Fl_Color rgbc(const uint8_t c[3]) { return fl_rgb_color(c[0], c[1], c[2]); }
static inline uint32_t pack_rgb(const uint8_t c[3]) { return (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2]; }
static inline Fl_Color unpack_rgb(uint32_t v) { return fl_rgb_color(v >> 16 & 0xff, v >> 8 & 0xff, v & 0xff); }

static double mono_now() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// shortest time between two repaints; cursor blink period; the cursor stops
// blinking this long after the last key press or output (as GTK does)
static constexpr double kFrame = 1.0 / 60, kBlink = 0.53, kBlinkStop = 10.0;

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
  static VTermStateFallbacks fb;
  memset(&fb, 0, sizeof fb);
  fb.osc = osc_cb;
  fb.dcs = dcs_cb;
  vterm_screen_set_unrecognised_fallbacks(screen_, &fb, this);
  static VTermSelectionCallbacks sel;
  sel.set = selection_set_cb;
  sel.query = selection_query_cb;
  vterm_state_set_selection_callbacks(state_, &sel, this, sel_scratch_, sizeof sel_scratch_);
  vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_SCROLL);
  vterm_screen_enable_altscreen(screen_, 1);
  vterm_screen_enable_reflow(screen_, true);
  vterm_state_set_bold_highbright(state_, 1);
  vterm_screen_reset(screen_, 1);
  measure_font();
  // screen readers: a terminal with the text it shows
  fld::atspi_describe(this, 60 /* ROLE_TERMINAL */, "Terminal");
  fld::atspi_describe_text(this, [this] { return screen_text(); });
}

TermWidget::~TermWidget() {
  fld::atspi_forget(this);
  Fl::remove_timeout(blink_cb, this);
  Fl::remove_timeout(repaint_cb, this);
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
  // A run of cells is drawn with one call when the font's advance is the
  // cell width; with fractional advances the text would drift off the grid
  // and the cells are drawn one by one.
  static const char probe[] = "MMMMMMMMMMiiiiiiiiii..........WW__##@@ll";
  const int n = (int)sizeof probe - 1;
  for (int st = 0; st < 4; st++) {
    fl_font(fld::mono_font() + (st & 1 ? FL_BOLD : 0) + (st & 2 ? FL_ITALIC : 0), font_size_);
    runs_ok_[st] = std::fabs(fl_width(probe, n) - (double)n * cw_) < 0.1;
  }
  fl_font(fld::mono_font(), font_size_);
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

void TermWidget::set_opacity(double o) {
  o = std::max(0.2, std::min(1.0, o));
  if (o == opacity_) return;
  opacity_ = o;
  redraw();
}

// The default background: translucent when the opacity is below 1. FLTK
// draws with cairo on Wayland; the window's buffer is ARGB, so painting with
// the SOURCE operator leaves the alpha in the buffer for the compositor (the
// window must not be an Fl_Double_Window there, see fl-term's main window).
void TermWidget::fill_background(int X, int Y, int W, int H) {
#if defined(FLTK_USE_WAYLAND)
  cairo_t *cr = opacity_ < 0.999 && fl_wl_display() ? fl_wl_gc() : nullptr;
  if (cr) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    // premultiplied by cairo: plain colour + alpha here
    cairo_set_source_rgba(cr, colors_.bg[0] / 255.0, colors_.bg[1] / 255.0, colors_.bg[2] / 255.0, opacity_);
    cairo_rectangle(cr, X, Y, W, H);
    cairo_fill(cr);
    cairo_restore(cr);
    return;
  }
#endif
  fl_color(rgbc(colors_.bg));
  fl_rectf(X, Y, W, H);
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

bool TermWidget::has_children() const {
  if (fd_ < 0 || pid_ <= 0) return false;
  pid_t fg = tcgetpgrp(fd_);
  return fg > 0 && fg != pid_;
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
      write_vt(buf, (size_t)n);
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
    bool failed = !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (!hold_ && !(hold_on_error_ && failed) && on_exit) on_exit(this, status, cb_data);
    else {
      char msg[96];
      if (WIFEXITED(status) && WEXITSTATUS(status))
        snprintf(msg, sizeof msg, "\r\n\x1b[2m[process exited with status %d]\x1b[0m", WEXITSTATUS(status));
      else snprintf(msg, sizeof msg, "\r\n\x1b[2m[process exited]\x1b[0m");
      vterm_input_write(vt_, msg, strlen(msg));
      redraw();
    }
    return;
  }
  if (got) {
    vterm_screen_flush_damage(screen_);
    // the screen is repainted at most once a frame; output that arrives in
    // between is read and only marks rows
    scrollbar_dirty_ = true;
    schedule_repaint();
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
  TermWidget *t = static_cast<TermWidget *>(user);
  // libvterm's answer to "what are you" (DA1): add 4, Sixel graphics
  static const char da1[] = "\x1b[?1;2c";
  if (len == sizeof da1 - 1 && memcmp(s, da1, len) == 0) {
    static const char ours[] = "\x1b[?1;2;4c";
    t->write_pty(ours, sizeof ours - 1);
    return;
  }
  t->write_pty(s, len);
  if (t->typing_ && t->on_typed) t->on_typed(t, s, len);
}

int TermWidget::damage_cb(VTermRect rect, void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  for (int r = rect.start_row; r < rect.end_row; r++) t->mark_row(r);
  return 1;
}

int TermWidget::movecursor_cb(VTermPos pos, VTermPos oldpos, int visible, void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  t->mark_row(oldpos.row);
  t->cursor_ = pos;
  t->cursor_visible_ = visible;
  t->mark_row(pos.row);
  t->reset_blink();
  return 1;
}

int TermWidget::settermprop_cb(VTermProp prop, VTermValue *val, void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: t->cursor_visible_ = val->boolean; t->mark_cursor(); break;
    case VTERM_PROP_CURSORBLINK:
      t->cursor_blink_ = val->boolean;
      t->reset_blink();
      t->mark_cursor();
      break;
    case VTERM_PROP_ALTSCREEN:
      t->altscreen_ = val->boolean;
      t->scroll_ = 0;
      t->update_scrollbar();
      t->mark_all();
      break;
    case VTERM_PROP_TITLE:
      // libvterm 0.3 may deliver the title in fragments
      if (val->string.initial) t->title_.clear();
      t->title_.append(val->string.str, val->string.len);
      if (val->string.final && t->on_title) t->on_title(t, t->title_.c_str(), t->cb_data);
      break;
    case VTERM_PROP_REVERSE: t->reverse_video_ = val->boolean; t->mark_all(); break;
    case VTERM_PROP_CURSORSHAPE: t->cursor_shape_ = val->number; t->mark_cursor(); break;
    case VTERM_PROP_MOUSE: t->mouse_mode_ = val->number; break;
    case VTERM_PROP_FOCUSREPORT: t->focus_report_ = val->boolean; break;
    default: return 0;
  }
  // changes come with output: read_pty() repaints
  return 1;
}

int TermWidget::bell_cb(void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  if (!t->window() || !t->window()->visible() || Fl::focus() != t) fl_beep(FL_BEEP_DEFAULT);
  return 1;
}

int TermWidget::sb_pushline_cb(int cols, const VTermScreenCell *cells, void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  bool dropped = t->sb_.push(cells, cols);
  if (t->scroll_ > 0) t->scroll_++; // keep the viewed text in place
  if (dropped && t->has_sel_) {
    t->sel_a_line_--;
    t->sel_b_line_--;
  }
  if (dropped) {
    for (auto &l : t->links_) l.line--;
    t->link_line_--;
    t->links_.erase(std::remove_if(t->links_.begin(), t->links_.end(), [](const Link &l) { return l.line < 0; }),
                    t->links_.end());
    for (auto &im : t->images_) im.line--;
    t->images_.erase(std::remove_if(t->images_.begin(), t->images_.end(),
                                    [](const SixelImage &im) { return im.line + im.rows <= 0; }),
                     t->images_.end());
  }
  if (t->scroll_ > (int)t->sb_.size()) t->scroll_ = (int)t->sb_.size();
  return 1;
}

int TermWidget::sb_popline_cb(int cols, VTermScreenCell *cells, void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  VTermColor fg, bg;
  vterm_state_get_default_colors(t->state_, &fg, &bg);
  if (!t->sb_.pop(cells, cols, fg, bg)) return 0;
  if (t->scroll_ > (int)t->sb_.size()) t->scroll_ = (int)t->sb_.size();
  return 1;
}

int TermWidget::sb_clear_cb(void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  t->links_.clear();
  t->images_.clear();
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
  if (cols != cols_ || rows != rows_ || (int)dirty_.size() != rows) {
    cols_ = cols;
    rows_ = rows;
    dirty_.assign(rows_, 0);
    any_dirty_ = false;
    full_ = true;
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
    if (sb_.get((size_t)line, col, cell)) return true;
    memset(&cell, 0, sizeof cell);
    cell.width = 1;
    vterm_state_get_default_colors(state_, &cell.fg, &cell.bg);
    return true;
  }
  VTermPos pos{(int)(line - nsb), col};
  if (pos.row >= rows_ || col >= cols_) return false;
  return vterm_screen_get_cell(screen_, pos, &cell) != 0;
}

// ---- OSC 8 hyperlinks and OSC 52 clipboard ------------------------------------

int TermWidget::osc_cb(int command, VTermStringFragment frag, void *user) {
  if (command != 8) return 0;
  TermWidget *t = static_cast<TermWidget *>(user);
  if (frag.initial) t->osc_buf_.clear();
  if (t->osc_buf_.size() + frag.len < 8192) t->osc_buf_.append(frag.str, frag.len);
  if (frag.final) t->osc8(t->osc_buf_);
  return 1;
}

// DCS: Sixel images ("P1;P2;P3q" then the picture); other DCS are ignored
int TermWidget::dcs_cb(const char *command, size_t len, VTermStringFragment frag, void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  if (frag.initial) {
    t->dcs_buf_.clear();
    t->dcs_sixel_ = len > 0 && command[len - 1] == 'q';
  }
  if (!t->dcs_sixel_) return 0;
  // a bound on what a program can make us keep (32 MB of sixel data)
  if (t->dcs_buf_.size() + frag.len <= (32u << 20)) t->dcs_buf_.append(frag.str, frag.len);
  else t->dcs_sixel_ = false;
  if (frag.final && t->dcs_sixel_) t->sixel_done(std::string(command, len > 0 ? len - 1 : 0));
  return 1;
}

void TermWidget::sixel_done(const std::string &params) {
  sixel::Image im;
  bool ok = sixel::decode(params, dcs_buf_, im);
  dcs_buf_.clear();
  dcs_buf_.shrink_to_fit();
  if (!ok) return;
  VTermPos pos;
  vterm_state_get_cursorpos(state_, &pos);
  SixelImage s;
  s.line = (long)sb_.size() + pos.row;
  s.col = pos.col;
  s.rows = std::max(1, (im.h + ch_ - 1) / std::max(1, ch_));
  s.pixels.swap(im.rgba);
  s.img.reset(new Fl_RGB_Image(s.pixels.data(), im.w, im.h, 4));
  images_.push_back(std::move(s));
  // at most 64 million pixels of images: the oldest go
  long total = 0;
  for (auto &x : images_) total += (long)x.img->w() * x.img->h();
  while (images_.size() > 1 && total > (64L << 20)) {
    total -= (long)images_.front().img->w() * images_.front().img->h();
    images_.erase(images_.begin());
  }
  pending_rows_ = images_.back().rows;  // the cursor goes below it (after the ST)
  mark_all();
  schedule_repaint();
}

// Input for libvterm, cut after each string terminator (ESC \): an image
// that just ended moves the cursor below itself before the text after it.
void TermWidget::write_vt(const char *buf, size_t n) {
  size_t start = 0;
  for (size_t i = 0; i + 1 < n; i++) {
    if (buf[i] != 0x1b || buf[i + 1] != '\\') continue;
    vterm_input_write(vt_, buf + start, i + 2 - start);
    start = i + 2;
    if (pending_rows_ > 0) {
      std::string mv(pending_rows_, '\n');
      mv += '\r';
      pending_rows_ = 0;
      vterm_input_write(vt_, mv.data(), mv.size());
    }
  }
  if (start < n) vterm_input_write(vt_, buf + start, n - start);
}

// "params;URI" opens a link at the cursor, "params;" (no URI) ends it
void TermWidget::osc8(const std::string &payload) {
  size_t semi = payload.find(';');
  std::string uri = semi == std::string::npos ? "" : payload.substr(semi + 1);
  VTermPos pos;
  vterm_state_get_cursorpos(state_, &pos);
  long line = (long)sb_.size() + pos.row;
  if (link_open_) {
    // the link covers what was printed since it was opened
    for (long l = link_line_; l <= line; l++) {
      int c0 = l == link_line_ ? link_col_ : 0, c1 = l == line ? pos.col : cols_;
      if (c1 > c0) links_.push_back({l, c0, c1, link_uri_});
    }
    if (links_.size() > 4000) links_.erase(links_.begin(), links_.begin() + (links_.size() - 4000));
    link_open_ = false;
    mark_all();
    schedule_repaint();
  }
  // only links a program may sensibly offer; others are ignored
  if (!uri.empty() && (fld::starts_with(uri, "http://") || fld::starts_with(uri, "https://") ||
                       fld::starts_with(uri, "file://") || fld::starts_with(uri, "mailto:"))) {
    link_open_ = true;
    link_line_ = line;
    link_col_ = pos.col;
    link_uri_ = uri;
  }
}

const TermWidget::Link *TermWidget::link_at(long line, int col) const {
  for (auto it = links_.rbegin(); it != links_.rend(); ++it)
    if (it->line == line && col >= it->c0 && col < it->c1) return &*it;
  return nullptr;
}

int TermWidget::selection_set_cb(VTermSelectionMask mask, VTermStringFragment frag, void *user) {
  TermWidget *t = static_cast<TermWidget *>(user);
  if (frag.initial) t->sel_buf_.clear();
  if (t->sel_buf_.size() + frag.len < (1u << 20)) t->sel_buf_.append(frag.str, frag.len);
  if (frag.final && !t->sel_buf_.empty()) {
    if (mask & VTERM_SELECTION_CLIPBOARD) Fl::copy(t->sel_buf_.data(), (int)t->sel_buf_.size(), 1);
    if (mask & (VTERM_SELECTION_PRIMARY | VTERM_SELECTION_SELECT))
      Fl::copy(t->sel_buf_.data(), (int)t->sel_buf_.size(), 0);
  }
  return 1;
}

// reading the clipboard would let any program in the terminal see it: answer nothing
int TermWidget::selection_query_cb(VTermSelectionMask, void *) { return 1; }

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

std::vector<std::u32string> TermWidget::all_lines() const {
  std::vector<std::u32string> out;
  long n = line_count();
  out.reserve((size_t)n);
  VTermScreenCell cell;
  for (long l = 0; l < n; l++) {
    std::u32string line;
    for (int c = 0; c < cols_; c++) {
      if (!get_cell(l, c, cell)) break;
      // the second column of a wide character holds nothing
      line += cell.chars[0] == (uint32_t)-1 ? (char32_t)0 : cell.chars[0] ? (char32_t)cell.chars[0] : U' ';
    }
    while (!line.empty() && line.back() == U' ') line.pop_back();
    out.push_back(std::move(line));
  }
  return out;
}

std::string TermWidget::screen_text() const {
  std::string out;
  long first = (long)sb_.size() - scroll_;
  VTermScreenCell cell;
  for (long l = first; l < first + rows_; l++) {
    std::string line;
    for (int c = 0; c < cols_; c++) {
      if (!get_cell(l, c, cell)) break;
      if (cell.chars[0] == (uint32_t)-1) continue; // the second column of a wide character
      uint32_t ch = cell.chars[0] ? cell.chars[0] : ' ';
      char buf[8];
      int n = fl_utf8encode(ch, buf);
      line.append(buf, (size_t)n);
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    out += line;
    out += '\n';
  }
  while (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
}

void TermWidget::show_match(long line, int col, int len) {
  has_sel_ = len > 0;
  sel_a_line_ = sel_b_line_ = line;
  sel_a_col_ = col;
  sel_b_col_ = col + std::max(1, len) - 1;
  // the live screen when the line is on it, else scrolled back to a few lines above it
  long first_shown = (long)sb_.size() - scroll_;
  if (line < first_shown || line >= first_shown + rows_) {
    long want = (long)sb_.size() - line + rows_ / 3;
    scroll_to((int)std::max(0L, std::min(want, (long)sb_.size())));
    if (line >= (long)sb_.size()) scroll_to(0);
  }
  mark_all();
  schedule_repaint();
  redraw();
}

bool TermWidget::selection_start(long &line, int &col) const {
  if (!has_sel_) return false;
  bool a_first = sel_a_line_ < sel_b_line_ || (sel_a_line_ == sel_b_line_ && sel_a_col_ <= sel_b_col_);
  line = a_first ? sel_a_line_ : sel_b_line_;
  col = a_first ? sel_a_col_ : sel_b_col_;
  return true;
}

void TermWidget::clear_selection() {
  if (!has_sel_) return;
  has_sel_ = false;
  mark_all();
  schedule_repaint();
}

// ---- repainting -------------------------------------------------------------------

void TermWidget::mark_row(int screen_row) {
  if (scroll_ > 0) {
    // the view shows scrollback: live rows are shifted down (or hidden)
    if (screen_row + scroll_ < rows_) full_ = true;
    return;
  }
  if (screen_row < 0 || screen_row >= (int)dirty_.size()) return;
  dirty_[screen_row] = 1;
  any_dirty_ = true;
}

void TermWidget::mark_all() { full_ = true; }

void TermWidget::schedule_repaint() {
  if (!full_ && !any_dirty_ && !scrollbar_dirty_) return;
  if (Fl::has_timeout(repaint_cb, this)) return;
  // At most one repaint a frame, and at least as much time for reading the
  // pty as the last repaint took: a slow repaint must not starve the output.
  double wait = std::max(kFrame, last_cost_) - (mono_now() - last_paint_);
  if (wait <= 0) apply_damage();
  else Fl::add_timeout(wait, repaint_cb, this);
}

void TermWidget::repaint_cb(void *data) { static_cast<TermWidget *>(data)->apply_damage(); }

void TermWidget::apply_damage() {
  if (scrollbar_dirty_) {
    scrollbar_dirty_ = false;
    if (scroll_ == 0) update_scrollbar();
  }
  if (full_) {
    redraw();
    return;
  }
  if (!any_dirty_) return;
  int r0 = -1, r1 = -1;
  for (int r = 0; r < (int)dirty_.size(); r++)
    if (dirty_[r]) {
      if (r0 < 0) r0 = r;
      r1 = r;
    }
  if (r0 < 0) {
    any_dirty_ = false;
    return;
  }
  // only the changed rows: draw() sees FL_DAMAGE_USER1
  damage(FL_DAMAGE_USER1, x(), y() + pad_ + r0 * ch_, w(), (r1 - r0 + 1) * ch_);
}

void TermWidget::draw() {
  const double t0 = mono_now();
  const bool focused = Fl::focus() == this && window() && window()->visible();
  const bool partial = damage() == FL_DAMAGE_USER1 && !full_ && scroll_ == 0;
  const bool was_full = full_ || !partial;
  // rows outside the area being drawn now keep their mark
  std::vector<uint8_t> visible(rows_, 0);
  for (int r = 0; r < rows_; r++) visible[r] = fl_not_clipped(x(), y() + pad_ + r * ch_, w(), ch_) == 1;

  if (!partial) {
    fl_push_clip(x(), y(), w(), h());
    fill_background(x(), y(), w(), h());
    for (int r = 0; r < rows_; r++) draw_row(r, focused);
    // scroll position indicator while looking at the scrollback
    if (scroll_ > 0 && !sb_.empty()) {
      int total = (int)sb_.size() + rows_;
      int bar_h = std::max(20, h() * rows_ / total);
      int top = (int)((long)(h() - bar_h) * ((long)sb_.size() - scroll_) / (long)sb_.size());
      fl_color(fl_color_average(rgbc(colors_.fg), rgbc(colors_.bg), 0.45f));
      fl_rectf(x() + w() - 5, y() + top, 3, bar_h);
    }
    fl_pop_clip();
  } else {
    for (int r = 0; r < rows_; r++) {
      if (r >= (int)dirty_.size() || !dirty_[r]) continue;
      int ry = y() + pad_ + r * ch_;
      fl_push_clip(x(), ry, w(), ch_);
      fill_background(x(), ry, w(), ch_);
      draw_row(r, focused);
      fl_pop_clip();
    }
  }

  any_dirty_ = false;
  for (int r = 0; r < (int)dirty_.size(); r++) {
    if (r < rows_ && visible[r]) dirty_[r] = 0;
    else if (was_full) dirty_[r] = 1;
    any_dirty_ |= dirty_[r] != 0;
  }
  full_ = false;
  last_paint_ = mono_now();
  last_cost_ = last_paint_ - t0;
  if (any_dirty_) schedule_repaint(); // waits for the next frame
}

// One row in four passes: backgrounds, text (runs of cells in one call where
// possible), lines, cursor.
void TermWidget::draw_row(int r, bool focused) {
  const long line = abs_line_of_row(r);
  const int ry = y() + pad_ + r * ch_;
  row_cells_.clear();
  row_cols_.clear();
  for (int c = 0; c < cols_;) {
    VTermScreenCell cell;
    if (!get_cell(line, c, cell)) {
      c++;
      continue;
    }
    if (cell.width < 1) cell.width = 1;
    row_cells_.push_back(cell);
    row_cols_.push_back(c);
    c += cell.width;
  }
  const int n = (int)row_cells_.size();
  run_cells_.resize(n);
  row_bg_.resize(n);
  const bool cursor_row = scroll_ == 0 && cursor_visible_ && r == cursor_.row;
  const bool cursor_on = focused && (blink_on_ || !cursor_blink_);
  int cursor_i = -1;
  bool block = false;
  for (int i = 0; i < n; i++) {
    const VTermScreenCell &cell = row_cells_[i];
    uint8_t fg[3], bg[3];
    resolve_color(cell.fg, true, fg);
    resolve_color(cell.bg, false, bg);
    if (cell.attrs.reverse ^ reverse_video_) {
      uint8_t t[3];
      memcpy(t, fg, 3);
      memcpy(fg, bg, 3);
      memcpy(bg, t, 3);
    }
    if (in_selection(line, row_cols_[i])) memcpy(bg, colors_.sel_bg, 3);
    bool is_cursor = cursor_row && row_cols_[i] == cursor_.col;
    if (is_cursor) {
      cursor_i = i;
      block = cursor_on && cursor_shape_ == VTERM_PROP_CURSORSHAPE_BLOCK;
      if (block) memcpy(fg, colors_.bg, 3);
    }
    int style = (cell.attrs.bold ? 1 : 0) | (cell.attrs.italic ? 2 : 0);
    term::RunCell &rc = run_cells_[i];
    rc.ch = cell.attrs.conceal ? 0 : cell.chars[0];
    rc.width = cell.width;
    rc.key = pack_rgb(fg) << 2 | (uint32_t)style;
    rc.alone = is_cursor || (VTERM_MAX_CHARS_PER_CELL > 1 && cell.chars[1]) || !runs_ok_[style];
    row_bg_[i] = pack_rgb(bg);
  }

  // backgrounds, cells of the same colour in one rectangle; the last column
  // extends into the padding
  const uint32_t def_bg = pack_rgb(colors_.bg);
  for (int i = 0; i < n;) {
    int j = i;
    while (j + 1 < n && row_bg_[j + 1] == row_bg_[i]) j++;
    if (row_bg_[i] != def_bg) {
      int x0 = x() + pad_ + row_cols_[i] * cw_;
      int end_col = row_cols_[j] + row_cells_[j].width;
      int x1 = end_col >= cols_ ? x() + w() : x() + pad_ + end_col * cw_;
      fl_color(unpack_rgb(row_bg_[i]));
      fl_rectf(x0, ry, x1 - x0, ch_);
    }
    i = j + 1;
  }
  if (block) {
    fl_color(rgbc(colors_.cursor));
    fl_rectf(x() + pad_ + row_cols_[cursor_i] * cw_, ry, cw_ * row_cells_[cursor_i].width, ch_);
  }

  // text
  term::split_runs(run_cells_, runs_);
  const Fl_Font base = fld::mono_font();
  int cur_style = -1;
  std::string text;
  for (const term::Run &run : runs_) {
    const term::RunCell &first = run_cells_[run.first];
    int style = (int)(first.key & 3);
    if (style != cur_style) {
      fl_font(base + (style & 1 ? FL_BOLD : 0) + (style & 2 ? FL_ITALIC : 0), font_size_);
      cur_style = style;
    }
    fl_color(unpack_rgb(first.key >> 2));
    int cx = x() + pad_ + row_cols_[run.first] * cw_;
    text.clear();
    if (run.single) {
      const VTermScreenCell &cell = row_cells_[run.first];
      char buf[8];
      for (int k = 0; k < VTERM_MAX_CHARS_PER_CELL && cell.chars[k]; k++)
        text.append(buf, fl_utf8encode(cell.chars[k], buf));
    } else {
      for (int i = run.first; i <= run.last; i++) text += run_cells_[i].ch ? (char)run_cells_[i].ch : ' ';
    }
    fl_draw(text.data(), (int)text.size(), cx, ry + ascent_);
  }

  // underline and strike-through
  for (int i = 0; i < n; i++) {
    const VTermScreenCell &cell = row_cells_[i];
    if (!cell.attrs.underline && !cell.attrs.strike) continue;
    int cx = x() + pad_ + row_cols_[i] * cw_, cwidth = cw_ * cell.width;
    fl_color(unpack_rgb(run_cells_[i].key >> 2));
    if (cell.attrs.underline) {
      fl_xyline(cx, ry + ch_ - 2, cx + cwidth - 1);
      if (cell.attrs.underline == VTERM_UNDERLINE_DOUBLE) fl_xyline(cx, ry + ch_ - 4, cx + cwidth - 1);
    }
    if (cell.attrs.strike) fl_xyline(cx, ry + ch_ / 2, cx + cwidth - 1);
  }

  // Sixel images: this row's slice of each image over it
  for (auto &im : images_) {
    if (line < im.line || line >= im.line + im.rows) continue;
    int sy = (int)(line - im.line) * ch_;
    int ih = std::min(ch_, im.img->h() - sy);
    if (ih <= 0) continue;
    int ix = x() + pad_ + im.col * cw_;
    int iw = std::min(im.img->w(), x() + w() - ix);
    if (iw > 0) im.img->draw(ix, ry, iw, ih, 0, sy);
  }

  // hyperlinks (OSC 8): a dotted underline; Ctrl+click opens them
  for (auto &lk : links_) {
    if (lk.line != line) continue;
    fl_color(rgbc(colors_.fg));
    fl_line_style(FL_DOT, 1);
    fl_xyline(x() + pad_ + lk.c0 * cw_, ry + ch_ - 1, x() + pad_ + lk.c1 * cw_ - 1);
    fl_line_style(0);
  }

  // the other cursor shapes, and an outline without the focus
  if (cursor_i >= 0 && !block) {
    int cx = x() + pad_ + row_cols_[cursor_i] * cw_, cwidth = cw_ * row_cells_[cursor_i].width;
    fl_color(rgbc(colors_.cursor));
    if (!focused) fl_rect(cx, ry, cwidth, ch_);
    else if (blink_on_ || !cursor_blink_) {
      if (cursor_shape_ == VTERM_PROP_CURSORSHAPE_UNDERLINE) fl_rectf(cx, ry + ch_ - 2, cwidth, 2);
      else if (cursor_shape_ == VTERM_PROP_CURSORSHAPE_BAR_LEFT) fl_rectf(cx, ry, 2, ch_);
    }
  }
}

// ---- cursor blinking ---------------------------------------------------------------

void TermWidget::reset_blink() {
  if (!blink_on_) mark_cursor();
  blink_on_ = true;
  blink_until_ = mono_now() + kBlinkStop;
  Fl::remove_timeout(blink_cb, this);
  if (cursor_blink_ && has_focus_) Fl::add_timeout(kBlink, blink_cb, this);
}

void TermWidget::stop_blink() {
  Fl::remove_timeout(blink_cb, this);
  if (!blink_on_) mark_cursor();
  blink_on_ = true;
}

void TermWidget::blink_cb(void *data) {
  TermWidget *t = static_cast<TermWidget *>(data);
  // not focused any more, or idle for a while: the cursor stays visible and
  // nothing wakes the program up
  if (!t->cursor_blink_ || Fl::focus() != t || mono_now() >= t->blink_until_) {
    t->stop_blink();
    t->schedule_repaint();
    return;
  }
  t->blink_on_ = !t->blink_on_;
  t->mark_cursor();
  t->schedule_repaint();
  Fl::repeat_timeout(kBlink, blink_cb, data);
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
  if (s.empty()) return;
  // the selection is shared with other programs (FLTK keeps it in-process on Wayland)
  if (clipboard == 0) fld::primary_copy(s);
  else Fl::copy(s.c_str(), (int)s.size(), clipboard);
}

void TermWidget::paste(int clipboard) {
  if (clipboard == 0) fld::primary_paste(*this);
  else Fl::paste(*this, clipboard);
}

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
      has_focus_ = e == FL_FOCUS;
      if (has_focus_ && on_focus) on_focus(this, cb_data);
      mark_cursor(); // drawn as an outline without the focus
      if (has_focus_) reset_blink();
      else stop_blink();
      schedule_repaint();
      return 1;

    case FL_KEYBOARD: {
      struct Typing {
        bool &f;
        explicit Typing(bool &x) : f(x) { f = true; }
        ~Typing() { f = false; }
      } typing(typing_);
      int key = Fl::event_key();
      int st = Fl::event_state();
      bool ctrl = st & FL_CTRL, shift = st & FL_SHIFT, alt = st & FL_ALT;
      // terminal shortcuts
      if (ctrl && shift) {
        switch (key) {
          case 'c': copy_selection(1); return 1;
          case 'v': paste(1); return 1;
          // window shortcuts: new window / tab, close tab / window, SSH,
          // menu bar, preferences, moving tabs, split panes, find, moving
          // between panes
          case 'n': case 't': case 'w': case 'q': case 's': case 'm': case 'p':
          case 'e': case 'o': case 'f': case 'i':
          case FL_Page_Up: case FL_Page_Down:
          case FL_Left: case FL_Right: case FL_Up: case FL_Down:
            return 0;
        }
      }
      // Ctrl+PgUp / Ctrl+PgDn switch tabs
      if (ctrl && !shift && !alt && (key == FL_Page_Up || key == FL_Page_Down)) return 0;
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
      if (has_sel_) {
        has_sel_ = false;
        mark_all();
      }
      reset_blink();
      schedule_repaint();
      return 1;
    }

    case FL_PASTE: {
      std::string text(Fl::event_text(), Fl::event_length());
      for (auto &ch : text)
        if (ch == '\n') ch = '\r';
      vterm_keyboard_start_paste(vt_);
      write_pty(text.data(), text.size());
      if (on_typed) on_typed(this, text.data(), text.size());
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
      if (e == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE && (Fl::event_state() & FL_CTRL)) {
        if (const Link *lk = link_at(line, col)) {
          fld::open_path(lk->uri);
          return 1;
        }
      }
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
