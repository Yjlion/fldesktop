// fl-welcome: a short tour at the first login. Light or dark, the accent
// colour, a wallpaper, the panel at the top or bottom, the keyboard layout,
// and Flathub for more applications. Every choice is saved at once (the same
// settings as in fl-settings) and can be changed there later.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Radio_Round_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/Fl_Wizard.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cstring>
#include <string>
#include <vector>

#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "welcome_util.h"
#ifdef FLD_HAVE_XKB_RULES
#include "xkb_rules.h"
#endif

namespace {

const int W = 720, H = 520;
Fl_Double_Window *g_win;
Fl_Wizard *g_wiz;
Fl_Button *g_back;
Fl_Return_Button *g_next;
std::vector<Fl_Group *> g_pages;

std::string desktop_ini() { return fld::desktop_config_path(); }
std::string wm_ini() { return fld::wm_config_path(); }

void set_desktop(const std::string &group, const std::string &key, const std::string &value) {
  fld::Ini ini;
  ini.load(desktop_ini());
  ini.set(group, key, value);
  ini.save(desktop_ini());
}

void set_wm(const std::string &group, const std::string &key, const std::string &value) {
  fld::Ini ini;
  ini.load(wm_ini());
  ini.set(group, key, value);
  ini.save(wm_ini());
  fld::reload_wm();
}

Fl_Group *page(const char *title, const char *text) {
  Fl_Group *g = new Fl_Group(0, 0, W, H - 60);
  Fl_Box *t = new Fl_Box(40, 30, W - 80, 40, title);
  t->labelfont(FL_HELVETICA_BOLD);
  t->labelsize(24);
  t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  if (text) {
    Fl_Box *b = new Fl_Box(40, 74, W - 80, 50, text);
    b->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
    b->labelcolor(fld::palette().fg_dim);
  }
  g_pages.push_back(g);
  return g;
}

void update_buttons() {
  size_t i = 0;
  for (; i < g_pages.size(); i++)
    if (g_wiz->value() == g_pages[i]) break;
  i == 0 ? g_back->deactivate() : g_back->activate();
  g_next->label(i + 1 == g_pages.size() ? "Done" : "Next");
}

void finish() {
  fld::write_file(welcome::done_marker(fld::fld_config_dir()), "");
  g_win->hide();
}

// ---- the pages ----------------------------------------------------------------------------

void look_page() {
  Fl_Group *g = page("How should it look?", "Light or dark, and a colour for buttons and selections.");
  fld::Ini ini;
  ini.load(desktop_ini());
  bool dark = fld::lower(ini.get("Appearance", "Theme", "light")) == "dark";
  // two big buttons drawing a small window each
  struct ThemeBtn : Fl_Button {
    bool dark;
    ThemeBtn(int x, int y, bool d) : Fl_Button(x, y, 220, 150), dark(d) { type(FL_RADIO_BUTTON); }
    void draw() override {
      Fl_Color bg = dark ? fl_rgb_color(0x24, 0x24, 0x28) : fl_rgb_color(0xf6, 0xf6, 0xf6);
      Fl_Color fg = dark ? fl_rgb_color(0xee, 0xee, 0xee) : fl_rgb_color(0x22, 0x22, 0x22);
      fl_color(value() ? fld::palette().accent : fld::palette().border);
      fl_rectf(x(), y(), w(), h());
      fl_color(bg);
      fl_rectf(x() + 4, y() + 4, w() - 8, h() - 8);
      fl_color(dark ? fl_rgb_color(0x36, 0x36, 0x3c) : fl_rgb_color(0xe0, 0xe0, 0xe0));
      fl_rectf(x() + 4, y() + 4, w() - 8, 22);
      fl_color(fld::palette().accent);
      fl_rectf(x() + 20, y() + h() - 44, 70, 24);
      fl_color(fg);
      fl_font(FL_HELVETICA_BOLD, 16);
      fl_draw(dark ? "Dark" : "Light", x() + 20, y() + 40, 100, 30, FL_ALIGN_LEFT);
    }
  };
  ThemeBtn *l = new ThemeBtn(40, 130, false), *d = new ThemeBtn(290, 130, true);
  (dark ? d : l)->value(1);
  auto cb = [](Fl_Widget *w, void *) {
    set_desktop("Appearance", "Theme", ((ThemeBtn *)w)->dark ? "dark" : "light");
  };
  l->callback(cb);
  d->callback(cb);
  Fl_Box *al = new Fl_Box(40, 300, 200, 26, "Accent colour:");
  al->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  int x = 40;
  std::string cur = ini.get("Appearance", "Accent", "#3584e4");
  for (auto &a : welcome::accents()) {
    Fl_Button *b = new Fl_Button(x, 332, 44, 44);
    unsigned v = (unsigned)strtoul(a.second.c_str() + 1, nullptr, 16);
    b->color(fl_rgb_color((uchar)(v >> 16), (uchar)(v >> 8), (uchar)v));
    b->box(FL_ROUND_UP_BOX);
    b->tooltip(a.first.c_str());
    b->type(FL_RADIO_BUTTON);
    b->down_box(FL_ROUND_DOWN_BOX);
    b->selection_color(b->color());
    if (a.second == cur) b->value(1);
    b->callback([](Fl_Widget *w, void *hex) { set_desktop("Appearance", "Accent", (const char *)hex); },
                (void *)a.second.c_str());
    x += 52;
  }
  g->end();
}

void wallpaper_page() {
  Fl_Group *g = page("A wallpaper", "Pick a picture for the desktop. Settings > Appearance has more choices.");
  Fl_Scroll *sc = new Fl_Scroll(40, 130, W - 80, H - 60 - 150);
  sc->type(Fl_Scroll::VERTICAL);
  std::vector<std::string> dirs = {fld::fld_data_dir() + "/backgrounds", "/usr/share/backgrounds"};
  static std::vector<std::string> pics;
  pics = welcome::wallpapers(dirs);
  int x = 40, y = 130, tw = 150, th = 96;
  for (size_t i = 0; i < pics.size() && i < 60; i++) {
    Fl_Button *b = new Fl_Button(x, y, tw, th);
    b->box(FL_FLAT_BOX);
    b->tooltip(fld::basename(pics[i]).c_str());
    if (Fl_Shared_Image *img = Fl_Shared_Image::get(pics[i].c_str())) {
      Fl_Image *copy = img->copy(tw - 8, th - 8);
      img->release();
      b->image(copy);
    } else {
      b->copy_label(fld::basename(pics[i]).c_str());
    }
    b->callback([](Fl_Widget *w, void *p) {
      set_desktop("Desktop", "Wallpaper", (const char *)p);
      Fl_Group *parent = w->parent();
      for (int k = 0; k < parent->children(); k++) parent->child(k)->box(FL_FLAT_BOX);
      w->box(FL_BORDER_FRAME);
      parent->redraw();
    }, (void *)pics[i].c_str());
    x += tw + 8;
    if (x + tw > W - 40) {
      x = 40;
      y += th + 8;
    }
  }
  if (pics.empty()) new Fl_Box(40, 130, W - 80, 40, "No pictures found in the backgrounds folders.");
  sc->end();
  g->end();
}

void panel_page() {
  Fl_Group *g = page("Where should the panel be?",
                     "The panel holds the applications button, the open windows, the clock and the status icons.");
  fld::Ini ini;
  ini.load(wm_ini());
  bool top = fld::lower(ini.get("behaviour", "panel_position", "bottom")) == "top";
  struct PanelBtn : Fl_Button {
    bool top;
    PanelBtn(int x, int y, bool t) : Fl_Button(x, y, 220, 150), top(t) { type(FL_RADIO_BUTTON); }
    void draw() override {
      fl_color(value() ? fld::palette().accent : fld::palette().border);
      fl_rectf(x(), y(), w(), h());
      fl_color(fl_rgb_color(0x5b, 0x8d, 0xc8));
      fl_rectf(x() + 4, y() + 4, w() - 8, h() - 8);
      fl_color(fl_rgb_color(0x26, 0x28, 0x2c));
      fl_rectf(x() + 4, top ? y() + 4 : y() + h() - 22, w() - 8, 18);
      fl_color(FL_WHITE);
      fl_font(FL_HELVETICA_BOLD, 16);
      fl_draw(top ? "At the top" : "At the bottom", x(), y(), w(), h(), FL_ALIGN_CENTER);
    }
  };
  PanelBtn *b = new PanelBtn(40, 140, false), *t = new PanelBtn(290, 140, true);
  (top ? t : b)->value(1);
  auto cb = [](Fl_Widget *w, void *) {
    set_wm("behaviour", "panel_position", ((PanelBtn *)w)->top ? "top" : "bottom");
  };
  b->callback(cb);
  t->callback(cb);
  g->end();
}

void keyboard_page() {
  Fl_Group *g = page("Your keyboard", "The layout of the keys. More layouts and a key to switch between them are in "
                                      "Settings > Keyboard.");
  Fl_Choice *c = new Fl_Choice(160, 140, 400, 30, "Layout:");
  static std::vector<std::string> codes;
  codes.clear();
  fld::Ini ini;
  ini.load(wm_ini());
  std::string cur = ini.get("keyboard", "layout", "");
  if (cur.find(',') != std::string::npos) cur = cur.substr(0, cur.find(','));
  if (cur.empty()) cur = welcome::layout_from_locale(fld::getenv_or("LANG", "en_US.UTF-8"));
  int pick = -1;
#ifdef FLD_HAVE_XKB_RULES
  static xkb::Rules rules;
  if (rules.load() || rules.load("/usr/share/X11/xkb/rules/evdev.lst")) {
    for (auto &l : rules.layouts) {
      codes.push_back(l.name);
      c->add(fld::Menu::text(l.description + "  (" + l.name + ")").c_str());
      if (l.name == cur) pick = (int)codes.size() - 1;
    }
  }
#endif
  if (codes.empty()) {
    for (const char *l : {"us", "gb", "de", "fr", "es", "it", "pt", "br", "se", "no", "dk", "fi", "pl", "ru", "jp"}) {
      codes.push_back(l);
      c->add(l);
      if (cur == l) pick = (int)codes.size() - 1;
    }
  }
  if (pick >= 0) c->value(pick);
  c->callback([](Fl_Widget *w, void *) {
    int i = ((Fl_Choice *)w)->value();
    if (i < 0 || i >= (int)codes.size()) return;
    std::string layout, variant;
    welcome::layout_code(codes[(size_t)i], layout, variant);
    fld::Ini ini;
    ini.load(wm_ini());
    ini.set("keyboard", "layout", layout);
    ini.set("keyboard", "variant", variant);
    ini.save(wm_ini());
    fld::reload_wm();
  });
  Fl_Input *t = new Fl_Input(160, 190, 400, 30, "Try it:");
  t->tooltip("Type here to see the layout");
  g->end();
}

void apps_page() {
  Fl_Group *g = page("More applications", "fldesktop comes with its own small applications. More come from your "
                                          "distribution's packages and from Flathub.");
  bool have_flatpak = !fld::which("flatpak").empty();
  Fl_Check_Button *fh = new Fl_Check_Button(40, 140, W - 80, 28, "Add Flathub, the app store for Linux (flatpak)");
  if (!have_flatpak) {
    fh->label("Flathub: install flatpak to use it");
    fh->deactivate();
  }
  fh->callback([](Fl_Widget *w, void *) {
    if (!((Fl_Check_Button *)w)->value()) return;
    std::string out;
    int rc = fld::run_capture(welcome::flathub_command(), out);
    if (rc != 0) {
      ((Fl_Check_Button *)w)->value(0);
      fl_alert("Flathub could not be added: %s", fld::trim(out).c_str());
    } else {
      w->label("Flathub is added");
      w->deactivate();
    }
  });
  int x = 40;
  auto launch = [&](const char *label, const char *prog) {
    if (fld::which(prog).empty()) return;
    Fl_Button *b = new Fl_Button(x, 190, 190, 34, label);
    b->callback([](Fl_Widget *, void *p) { fld::spawn({(const char *)p}); }, (void *)prog);
    x += 198;
  };
  launch("Software (Flatpak)", "fl-flatpak");
  launch("Packages", "fl-packages");
  launch("Settings", "fl-settings");
  Fl_Box *tips = new Fl_Box(40, 250, W - 80, 160,
                            "Tips:\n"
                            "  Super+Space opens the applications.\n"
                            "  Super+arrows put a window in half or a quarter of the screen.\n"
                            "  Super+L locks the screen; Print takes a screenshot.\n"
                            "  Right-click almost anything for more.");
  tips->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP);
  g->end();
}

} // namespace

int main(int argc, char **argv) {
  bool first_run = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--first-run")) first_run = true;
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      printf("Usage: fl-welcome [--first-run]\n  --first-run  only when the tour has not run yet\n");
      return 0;
    }
  }
  if (first_run && fld::file_exists(welcome::done_marker(fld::fld_config_dir()))) return 0;
  fld::init_app("fl-welcome", argc, argv);
  fl_register_images();
  g_win = new Fl_Double_Window(W, H, "Welcome");
  g_wiz = new Fl_Wizard(0, 0, W, H - 60);
  {
    Fl_Group *g = page("Welcome to fldesktop", nullptr);
    Fl_Box *logo = new Fl_Box(40, 90, 128, 128);
    logo->image(fld::icon_first({"fldesktop", "start-here"}, 128));
    Fl_Box *t = new Fl_Box(190, 90, W - 230, 200,
                           "A light desktop built with FLTK.\n\nThis short tour sets the look, a wallpaper, "
                           "the panel and the keyboard. It takes a minute, and you can change everything "
                           "later in Settings.");
    t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
    g->end();
  }
  look_page();
  wallpaper_page();
  panel_page();
  keyboard_page();
  apps_page();
  {
    Fl_Group *g = page("All set", "Enjoy fldesktop. Settings has everything this tour did, and much more.");
    Fl_Button *s = new Fl_Button(40, 140, 180, 34, "Open Settings");
    s->callback([](Fl_Widget *, void *) { fld::spawn({"fl-settings"}); });
    g->end();
  }
  g_wiz->end();
  Fl_Button *skip = new Fl_Button(20, H - 46, 120, 32, "Skip the Tour");
  skip->callback([](Fl_Widget *, void *) { finish(); });
  g_back = new Fl_Button(W - 250, H - 46, 110, 32, "Back");
  g_back->callback([](Fl_Widget *, void *) {
    g_wiz->prev();
    update_buttons();
  });
  g_next = new Fl_Return_Button(W - 130, H - 46, 110, 32, "Next");
  g_next->callback([](Fl_Widget *, void *) {
    if (g_wiz->value() == g_pages.back()) {
      finish();
      return;
    }
    g_wiz->next();
    update_buttons();
  });
  g_win->end();
  g_win->callback([](Fl_Widget *, void *) { finish(); });
  fld::set_window_icon(g_win, "fl-welcome");
  update_buttons();
  g_win->show();
  return Fl::run();
}
