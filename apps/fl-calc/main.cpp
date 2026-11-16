// fl-calc: calculator for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <cstring>
#include <string>
#include <vector>

#include "calc_engine.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

struct State {
  std::string expr;
  std::string result;   // live preview or final result
  bool just_evaluated = false;
  bool error = false;
  calc::Options opt;
  std::vector<std::pair<std::string, std::string>> history;
  double memory = 0;
  bool scientific = false;
};

static State S;
static Fl_Double_Window *g_win;
static Fl_Widget *g_display;
static Fl_Group *g_sci;
static Fl_Group *g_basic;
static Fl_Hold_Browser *g_hist;
static Fl_Menu_Bar *g_menu;

static void update() {
  if (!S.just_evaluated) {
    S.error = false;
    if (S.expr.empty()) S.result = "0";
    else {
      calc::Result r = calc::evaluate(S.expr, S.opt);
      S.result = r.ok ? calc::format(r.value) : "";
    }
  }
  g_display->redraw();
}

class Display : public Fl_Widget {
public:
  Display(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_color(p.bg2);
    fl_rectf(x(), y(), w(), h());
    fl_color(p.border);
    fl_xyline(x(), y() + h() - 1, x() + w());
    // expression (scrolls to show its end)
    fl_font(FL_HELVETICA, 18);
    std::string e = S.expr.empty() ? " " : S.expr;
    while (fl_width(e.c_str()) > w() - 24 && e.size() > 1) e.erase(0, fl_utf8len1(e[0]) > 0 ? fl_utf8len1(e[0]) : 1);
    fl_color(S.just_evaluated ? p.fg_dim : p.fg);
    fl_draw(fld::escape_at(e).c_str(), x() + 12, y() + 8, w() - 24, 28, FL_ALIGN_RIGHT);
    // result
    int size = 40;
    fl_font(FL_HELVETICA_BOLD, size);
    while (size > 14 && fl_width(S.result.c_str()) > w() - 24) fl_font(FL_HELVETICA_BOLD, size -= 2);
    fl_color(S.error ? p.danger : (S.just_evaluated ? p.fg : p.fg_dim));
    fl_draw(fld::escape_at(S.result).c_str(), x() + 12, y() + 38, w() - 24, h() - 46, FL_ALIGN_RIGHT);
    // indicators
    fl_font(FL_HELVETICA, 11);
    fl_color(p.fg_dim);
    std::string ind = S.opt.degrees ? "DEG" : "RAD";
    if (S.memory != 0) ind += "   M";
    fl_draw(ind.c_str(), x() + 12, y() + 4, 100, 14, FL_ALIGN_LEFT);
  }
  int handle(int e) override;
};

static void evaluate() {
  if (S.expr.empty()) return;
  calc::Result r = calc::evaluate(S.expr, S.opt);
  if (r.ok) {
    S.result = calc::format(r.value);
    S.opt.ans = r.value;
    S.history.push_back({S.expr, S.result});
    g_hist->add(fld::escape_at(S.expr + " = " + S.result).c_str());
    g_hist->bottomline(g_hist->size());
    S.error = false;
  } else {
    S.result = r.error;
    S.error = true;
  }
  S.just_evaluated = true;
  g_display->redraw();
}

static void input(const std::string &t) {
  if (S.just_evaluated) {
    // continuing with an operator keeps the result, anything else starts fresh
    bool op = t == "+" || t == "−" || t == "-" || t == "×" || t == "÷" || t == "*" || t == "/" || t == "^" ||
              t == "!" || t == "%" || t == "²";
    if (S.error) S.expr.clear();
    else if (op) S.expr = "ans";
    else S.expr.clear();
    S.just_evaluated = false;
  }
  S.expr += t;
  update();
}

static void backspace() {
  if (S.just_evaluated) { S.just_evaluated = false; S.expr.clear(); update(); return; }
  if (S.expr.empty()) return;
  // remove a whole function name like "sin(" at once
  static const char *funcs[] = {"asin(", "acos(", "atan(", "sinh(", "cosh(", "tanh(", "sin(", "cos(",
                                "tan(", "sqrt(", "log(", "ln(", "exp(", "abs(", "ans", "pi"};
  for (auto f : funcs) {
    if (fld::ends_with(S.expr, f)) {
      S.expr.erase(S.expr.size() - strlen(f));
      update();
      return;
    }
  }
  // remove one UTF-8 character
  size_t i = S.expr.size() - 1;
  while (i > 0 && ((unsigned char)S.expr[i] & 0xC0) == 0x80) i--;
  S.expr.erase(i);
  update();
}

static void clear_all() {
  S.expr.clear();
  S.just_evaluated = false;
  update();
}

int Display::handle(int e) {
  if (e == FL_FOCUS || e == FL_UNFOCUS) return 1;
  if (e == FL_PUSH) { take_focus(); return 1; }
  if (e == FL_PASTE) {
    std::string t(Fl::event_text(), Fl::event_length());
    for (auto &c : t) if (c == '\n') c = ' ';
    input(t);
    return 1;
  }
  return Fl_Widget::handle(e);
}

// ---- buttons ------------------------------------------------------------------------

struct Key {
  const char *label;
  const char *insert; // text to insert, or nullptr for special actions
  int kind;           // 0 digit, 1 operator, 2 function, 3 action, 4 equals
};

static void key_cb(Fl_Widget *w, void *data) {
  const Key *k = (const Key *)data;
  std::string lab = k->label;
  if (k->insert) input(k->insert);
  else if (lab == "C") clear_all();
  else if (lab == "⌫") backspace();
  else if (lab == "=") evaluate();
  else if (lab == "±") {
    if (S.just_evaluated) { S.expr = "-(ans)"; S.just_evaluated = false; }
    else S.expr = fld::starts_with(S.expr, "-(") && fld::ends_with(S.expr, ")") ? S.expr.substr(2, S.expr.size() - 3)
                                                                                : "-(" + S.expr + ")";
    update();
  } else if (lab == "DEG" || lab == "RAD") {
    S.opt.degrees = !S.opt.degrees;
    w->copy_label(S.opt.degrees ? "RAD" : "DEG");
    w->copy_tooltip(S.opt.degrees ? "Switch to radians" : "Switch to degrees");
    update();
  } else if (lab == "MC") { S.memory = 0; update(); }
  else if (lab == "MR") input(calc::format(S.memory));
  else if (lab == "M+" || lab == "M−") {
    calc::Result r = calc::evaluate(S.just_evaluated ? "ans" : S.expr, S.opt);
    if (r.ok) S.memory += lab == "M+" ? r.value : -r.value;
    update();
  }
  g_display->take_focus();
}

static Fl_Button *make_key(int x, int y, int w, int h, const Key *k) {
  const fld::Palette &p = fld::palette();
  Fl_Button *b = new Fl_Button(x, y, w, h, k->label);
  b->labelsize(k->kind == 2 || k->kind == 3 ? 15 : 19);
  b->box(FL_FLAT_BOX);
  b->down_box(FL_FLAT_BOX);
  b->clear_visible_focus();
  switch (k->kind) {
    case 0: b->color(p.bg2); b->labelcolor(p.fg); break;
    case 1: b->color(fl_color_average(p.bg, p.bg2, 0.5f)); b->labelcolor(p.accent); b->labelfont(FL_HELVETICA_BOLD); break;
    case 2: b->color(p.bg); b->labelcolor(p.fg); break;
    case 3: b->color(p.bg); b->labelcolor(p.fg); break;
    case 4: b->color(p.accent); b->labelcolor(p.accent_fg); b->labelfont(FL_HELVETICA_BOLD); break;
  }
  b->down_color(fl_color_average(b->color(), p.accent, 0.6f));
  b->callback(key_cb, (void *)k);
  return b;
}

static const Key kBasic[] = {
    {"%", "%", 2}, {"x²", "^2", 2}, {"√", "sqrt(", 2}, {"⌫", nullptr, 3},
    {"C", nullptr, 3}, {"(", "(", 1}, {")", ")", 1}, {"÷", "÷", 1},
    {"7", "7", 0}, {"8", "8", 0}, {"9", "9", 0}, {"×", "×", 1},
    {"4", "4", 0}, {"5", "5", 0}, {"6", "6", 0}, {"−", "−", 1},
    {"1", "1", 0}, {"2", "2", 0}, {"3", "3", 0}, {"+", "+", 1},
    {"±", nullptr, 3}, {"0", "0", 0}, {".", ".", 0}, {"=", nullptr, 4},
};

static const Key kSci[] = {
    {"DEG", nullptr, 3}, {"ans", "ans", 2}, {"xʸ", "^", 2}, {"|x|", "abs(", 2},
    {"sin", "sin(", 2}, {"cos", "cos(", 2}, {"tan", "tan(", 2}, {"π", "pi", 2},
    {"sin⁻¹", "asin(", 2}, {"cos⁻¹", "acos(", 2}, {"tan⁻¹", "atan(", 2}, {"e", "e", 2},
    {"ln", "ln(", 2}, {"log", "log(", 2}, {"eˣ", "exp(", 2}, {"n!", "!", 2},
    {"sinh", "sinh(", 2}, {"cosh", "cosh(", 2}, {"tanh", "tanh(", 2}, {"∛", "cbrt(", 2},
    {"MC", nullptr, 3}, {"MR", nullptr, 3}, {"M+", nullptr, 3}, {"M−", nullptr, 3},
};

static const int KW = 72, KH = 54, GAP = 2, DISP_H = 104, MENU_H = 28, HIST_W = 220;

static void layout() {
  int basic_w = 4 * KW + 3 * GAP;
  int sci_w = S.scientific ? 4 * KW + 3 * GAP + 8 : 0;
  int rows = 6;
  int keys_h = rows * KH + (rows - 1) * GAP;
  int W = basic_w + sci_w + 16, H = MENU_H + DISP_H + keys_h + 16;
  if (g_hist->Fl_Widget::visible()) W += HIST_W + 8;
  g_win->size_range(W, H, W, H);
  g_win->size(W, H);
  g_menu->resize(0, 0, W, MENU_H);
  g_display->resize(0, MENU_H, W - (g_hist->Fl_Widget::visible() ? HIST_W + 8 : 0), DISP_H);
  int y0 = MENU_H + DISP_H + 8;
  if (S.scientific) {
    g_sci->show();
    g_sci->position(8, y0);
  } else g_sci->hide();
  g_basic->position(8 + sci_w, y0);
  g_hist->resize(W - HIST_W - 8, MENU_H + 8, HIST_W, H - MENU_H - 16);
  g_win->redraw();
}

static void menu_cb(Fl_Widget *, void *v) {
  long id = (long)v;
  fld::Ini cfg;
  std::string path = fld::fld_config_dir() + "/calc.ini";
  cfg.load(path);
  switch (id) {
    case 1: S.scientific = false; layout(); break;
    case 2: S.scientific = true; layout(); break;
    case 3: if (g_hist->Fl_Widget::visible()) g_hist->hide(); else g_hist->show(); layout(); break;
    case 4: {
      std::string r = S.just_evaluated && !S.error ? S.result : S.result;
      Fl::copy(r.c_str(), (int)r.size(), 1);
      break;
    }
    case 5: Fl::paste(*g_display, 1); break;
    case 6: g_hist->clear(); S.history.clear(); break;
    case 7: fld::about_dialog("Calculator", "fl-calc", "A basic and scientific calculator built with FLTK."); break;
    case 8: g_win->hide(); break;
  }
  cfg.set_bool("Calc", "Scientific", S.scientific);
  cfg.set_bool("Calc", "History", g_hist->Fl_Widget::visible());
  cfg.set_bool("Calc", "Degrees", S.opt.degrees);
  cfg.save(path);
}

static int key_handler(int e) {
  if (e != FL_SHORTCUT && e != FL_KEYBOARD) return 0;
  if (Fl::focus() != g_display && Fl::focus() != nullptr && Fl::focus() != g_win) return 0;
  int k = Fl::event_key();
  int st = Fl::event_state();
  if (st & (FL_CTRL | FL_ALT | FL_META)) return 0;
  if (k == FL_Enter || k == FL_KP_Enter) { evaluate(); return 1; }
  if (k == FL_BackSpace) { backspace(); return 1; }
  if (k == FL_Escape || k == FL_Delete) { clear_all(); return 1; }
  if (Fl::event_length() == 1) {
    char c = Fl::event_text()[0];
    if (c == '=') { evaluate(); return 1; }
    if (strchr("0123456789.+-*/^()%!", c)) {
      std::string t(1, c);
      if (c == '*') t = "×";
      else if (c == '/') t = "÷";
      else if (c == '-') t = "−";
      input(t);
      return 1;
    }
    if (isalpha((unsigned char)c) || c == ',') { input(std::string(1, c)); return 1; }
  }
  return 0;
}

int main(int argc, char **argv) {
  fld::init_app("fl-calc", argc, argv);
  fld::Ini cfg;
  cfg.load(fld::fld_config_dir() + "/calc.ini");
  S.scientific = cfg.get_bool("Calc", "Scientific", false);
  S.opt.degrees = cfg.get_bool("Calc", "Degrees", true);

  g_win = new Fl_Double_Window(320, 520, "Calculator");
  g_menu = new Fl_Menu_Bar(0, 0, 320, MENU_H);
  g_menu->box(FL_FLAT_BOX);
  g_menu->add("&Calculator/&Basic", FL_CTRL + 'b', menu_cb, (void *)1, FL_MENU_RADIO | (S.scientific ? 0 : FL_MENU_VALUE));
  g_menu->add("&Calculator/&Scientific", FL_CTRL + 's', menu_cb, (void *)2, FL_MENU_RADIO | (S.scientific ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER);
  g_menu->add("&Calculator/&Quit", FL_CTRL + 'q', menu_cb, (void *)8);
  g_menu->add("&Edit/&Copy Result", FL_CTRL + 'c', menu_cb, (void *)4);
  g_menu->add("&Edit/&Paste", FL_CTRL + 'v', menu_cb, (void *)5);
  g_menu->add("&View/&History", FL_CTRL + 'h', menu_cb, (void *)3, FL_MENU_TOGGLE);
  g_menu->add("&View/C&lear History", 0, menu_cb, (void *)6);
  g_menu->add("&Help/&About", 0, menu_cb, (void *)7);
  g_display = new Display(0, MENU_H, 320, DISP_H);

  g_sci = new Fl_Group(8, 0, 4 * KW + 3 * GAP, 6 * KH + 5 * GAP);
  for (int i = 0; i < 24; i++) {
    Fl_Button *b = make_key(8 + (i % 4) * (KW + GAP), (i / 4) * (KH + GAP), KW, KH, &kSci[i]);
    if (i == 0) {
      b->copy_label(S.opt.degrees ? "RAD" : "DEG");
      b->copy_tooltip(S.opt.degrees ? "Switch to radians" : "Switch to degrees");
    }
  }
  g_sci->end();
  g_basic = new Fl_Group(8, 0, 4 * KW + 3 * GAP, 6 * KH + 5 * GAP);
  for (int i = 0; i < 24; i++)
    make_key(8 + (i % 4) * (KW + GAP), (i / 4) * (KH + GAP), KW, KH, &kBasic[i]);
  g_basic->end();
  g_hist = new Fl_Hold_Browser(0, 0, HIST_W, 100);
  g_hist->textsize(13);
  g_hist->callback([](Fl_Widget *w, void *) {
    int line = ((Fl_Hold_Browser *)w)->value();
    if (line < 1 || line > (int)S.history.size()) return;
    S.expr = S.history[line - 1].first;
    S.just_evaluated = false;
    update();
  });
  if (!cfg.get_bool("Calc", "History", false)) g_hist->hide();
  else ((Fl_Menu_Item *)g_menu->find_item("&View/&History"))->set();
  g_win->end();
  fld::set_window_icon(g_win, "fl-calc");
  Fl::add_handler(key_handler);
  layout();
  update();
  g_win->show(); // not show(argc, argv): it resets the theme colors
  g_display->take_focus();
  return Fl::run();
}
