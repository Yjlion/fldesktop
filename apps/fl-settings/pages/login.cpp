// Login Screen: /etc/fldesktop/greeter.ini for fl-greet (greetd), saved as
// root through pkexec fl-settings-helper
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <pwd.h>
#include <set>
#include <thread>
#include <unistd.h>

#include "fld/accents.h"
#include "fld/icons.h"
#include "fld/osk.h"
#include "fld/pickers.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "greeter_config.h"
#include "settings_common.h"
#include "settings_page.h"

#ifndef FLD_LIBEXECDIR
#define FLD_LIBEXECDIR "/usr/libexec/fldesktop"
#endif

namespace fld {
namespace {

std::vector<std::string> themes_with(const char *sub, bool cursors) {
  std::set<std::string> names;
  std::vector<std::string> bases{"/usr/share/icons", "/usr/local/share/icons"};
  for (auto &d : data_dirs())
    if (starts_with(d, "/usr/")) bases.push_back(path_join(d, "icons"));
  for (auto &b : bases)
    for (auto &n : list_dir(b)) {
      std::string p = path_join(b, n);
      if (cursors ? is_dir(p + "/cursors") : (file_exists(p + "/index.theme") && !is_dir(p + "/cursors") && n != "default"))
        names.insert(n);
    }
  (void)sub;
  return std::vector<std::string>(names.begin(), names.end());
}

void fill(Fl_Choice *c, const std::vector<std::string> &items, const std::string &cur) {
  c->clear();
  int sel = 0;
  for (size_t i = 0; i < items.size(); i++) {
    std::string e;
    for (char ch : items[i]) {
      if (ch == '/' || ch == '\\' || ch == '&' || ch == '_') e += '\\';
      e += ch;
    }
    c->add(e.c_str());
    if (items[i] == cur) sel = (int)i;
  }
  c->value(sel);
}

class LoginPage : public Fl_Group {
public:
  Ini cfg;
  Fl_Box *status;
  Fl_Choice *theme, *icons, *cursor, *kb_mode, *kb_layout;
  Fl_Spinner *cursor_size, *font_size, *min_uid;
  AccentSwatches *accent;
  WallpaperGallery *gallery;
  Fl_Check_Browser *users;
  Fl_Check_Button *remember, *clock24, *power;
  Fl_Button *apply, *revert;
  std::vector<std::string> icon_themes, cursor_themes, user_names, layouts;
  std::string background, custom_background;  // custom = a file to copy

  LoginPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Login Screen", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    auto changed = [](Fl_Widget *, void *d) { ((LoginPage *)d)->set_changed(); };

    f.row("", 36, wx, wy, ww);
    status = new Fl_Box(f.widget_x() - 200, wy, ww + 200, 36);
    status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    status->labelsize(13);

    f.heading("Appearance");
    f.row("Style:", 28, wx, wy, ww);
    theme = new Fl_Choice(wx, wy, 160, 28);
    theme->add("Light");
    theme->add("Dark");
    theme->callback(changed, this);
    f.row("Accent color:", 28, wx, wy, ww);
    accent = new AccentSwatches(wx, wy, AccentSwatches::preferred_width(28), 28);
    accent->callback(changed, this);
    f.row("Icon theme:", 28, wx, wy, ww);
    icons = new Fl_Choice(wx, wy, ww, 28);
    icons->callback(changed, this);
    f.row("Cursor:", 28, wx, wy, ww);
    cursor = new Fl_Choice(wx, wy, ww - 90, 28);
    cursor->callback(changed, this);
    cursor_size = new Fl_Spinner(wx + ww - 80, wy, 80, 28);
    cursor_size->range(16, 96);
    cursor_size->step(8);
    cursor_size->callback(changed, this);
    f.row("Text size:", 28, wx, wy, ww);
    font_size = new Fl_Spinner(wx, wy, 80, 28);
    font_size->range(8, 32);
    font_size->callback(changed, this);
    f.row("Background:", 96, wx, wy, ww);
    gallery = new WallpaperGallery(wx, wy, ww, 96);
    // only system wallpapers: the greeter cannot read home folders
    gallery->load({fld_data_dir() + "/backgrounds", "/usr/share/backgrounds"});
    gallery->callback([](Fl_Widget *, void *d) {
      LoginPage *p = (LoginPage *)d;
      p->background = p->gallery->value();
      p->custom_background = starts_with(p->background, "/usr/") || starts_with(p->background, "/etc/fldesktop/")
                                 ? "" : p->background;
      p->set_changed();
    }, this);
    f.row("", 28, wx, wy, ww);
    Fl_Button *other = new Fl_Button(wx, wy, 140, 28, "Other Image…");
    other->tooltip("The image is copied to /etc/fldesktop so the login screen can read it");
    other->callback([](Fl_Widget *, void *d) { ((LoginPage *)d)->choose_image(); }, this);

    f.heading("Users");
    f.row("", 26, wx, wy, ww);
    remember = new Fl_Check_Button(wx, wy, ww, 26, "Preselect the user who logged in last");
    remember->callback(changed, this);
    f.row("Lowest user ID:", 28, wx, wy, ww);
    min_uid = new Fl_Spinner(wx, wy, 90, 28);
    min_uid->range(0, 65535);
    min_uid->callback([](Fl_Widget *, void *d) {
      LoginPage *p = (LoginPage *)d;
      p->load_users();
      p->set_changed();
    }, this);
    f.row("Show:", 110, wx, wy, ww);
    users = new Fl_Check_Browser(wx, wy, ww, 110);
    users->callback(changed, this);
    users->when(FL_WHEN_CHANGED);

    f.heading("Other");
    f.row("", 26, wx, wy, ww);
    clock24 = new Fl_Check_Button(wx, wy, 200, 26, "24-hour clock");
    clock24->callback(changed, this);
    power = new Fl_Check_Button(wx + 210, wy, 260, 26, "Restart and Shut Down buttons");
    power->callback(changed, this);
    f.row("On-screen keyboard:", 28, wx, wy, ww);
    kb_mode = new Fl_Choice(wx, wy, 180, 28);
    kb_mode->add("Button to show it");
    kb_mode->add("Always shown");
    kb_mode->add("Never");
    kb_mode->callback(changed, this);
    kb_layout = new Fl_Choice(wx + 190, wy, 150, 28);
    for (auto &l : osk_layouts())
      if (l.id != "symbols") {
        layouts.push_back(l.id);
        kb_layout->add(escape_at(l.name).c_str());
      }
    kb_layout->callback(changed, this);

    f.space(10);
    f.row("", 30, wx, wy, ww);
    apply = new Fl_Button(wx, wy, 120, 30, "Apply…");
    apply->tooltip("Saving the login screen settings needs an administrator password");
    apply->callback([](Fl_Widget *, void *d) { ((LoginPage *)d)->save(); }, this);
    revert = new Fl_Button(wx + 130, wy, 120, 30, "Revert");
    revert->callback([](Fl_Widget *, void *d) { ((LoginPage *)d)->load(); }, this);
    g->end();
    end();
    icon_themes = themes_with("", false);
    cursor_themes = themes_with("", true);
    load();
  }

  void set_changed() {
    apply->activate();
    revert->activate();
  }

  void load_users() {
    std::set<std::string> hidden;
    for (auto &h : split(cfg.get("Greeter", "HideUsers", ""), ',')) hidden.insert(trim(h));
    // keep the user's unsaved choices
    for (int i = 1; i <= users->nitems(); i++)
      if (!users->checked(i)) hidden.insert(user_names[i - 1]);
    users->clear();
    user_names.clear();
    setpwent();
    std::vector<std::pair<std::string, std::string>> list;
    while (struct passwd *pw = getpwent()) {
      if ((int)pw->pw_uid < (int)min_uid->value() || pw->pw_uid >= 60000) continue;
      std::string shell = pw->pw_shell ? pw->pw_shell : "";
      if (ends_with(shell, "nologin") || ends_with(shell, "false")) continue;
      std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
      list.push_back({pw->pw_name, gecos.substr(0, gecos.find(','))});
    }
    endpwent();
    std::sort(list.begin(), list.end());
    for (auto &u : list) {
      user_names.push_back(u.first);
      std::string label = u.second.empty() ? u.first : u.second + " (" + u.first + ")";
      users->add(escape_at(label).c_str(), hidden.count(u.first) ? 0 : 1);
    }
  }

  void load() {
    cfg = Ini();
    bool exists = cfg.load(greeter_ini_path());
    std::string toml;
    read_file("/etc/greetd/config.toml", toml);
    std::string s;
    if (toml.empty()) s = "greetd is not set up on this computer: these settings take effect once it runs fl-greet.";
    else if (toml.find("fl-greet") == std::string::npos)
      s = "greetd does not run fl-greet (/etc/greetd/config.toml): these settings only apply to fl-greet.";
    else s = "The login screen is fl-greet, run by greetd.";
    if (!exists) s += " No " + std::string(greeter_ini_path()) + " yet: these are the defaults.";
    status->copy_label(s.c_str());
    theme->value(lower(cfg.get("Appearance", "Theme", "dark")) == "dark" ? 1 : 0);
    accent->value(cfg.get("Appearance", "Accent", "#3584e4"));
    fill(icons, icon_themes, cfg.get("General", "IconTheme", default_icon_theme()));
    fill(cursor, cursor_themes, cfg.get("Cursor", "Theme", default_cursor_theme()));
    cursor_size->value(cfg.get_int("Cursor", "Size", 24));
    font_size->value(cfg.get_int("Appearance", "FontSize", 14));
    background = cfg.get("Greeter", "Background", fld_data_dir() + "/backgrounds/default.svg");
    custom_background.clear();
    if (file_exists(background)) gallery->add_file(background);
    gallery->value(background);
    remember->value(cfg.get_bool("Greeter", "RememberUser", true));
    min_uid->value(cfg.get_int("Greeter", "MinUid", 1000));
    users->clear();
    load_users();
    clock24->value(cfg.get_bool("Greeter", "Clock24", true));
    power->value(cfg.get_bool("Greeter", "ShowPowerButtons", true));
    std::string km = lower(cfg.get("Greeter", "Keyboard", "button"));
    kb_mode->value(km == "always" ? 1 : km == "never" ? 2 : 0);
    std::string kl = cfg.get("Greeter", "KeyboardLayout", "us");
    kb_layout->value(0);
    for (size_t i = 0; i < layouts.size(); i++)
      if (layouts[i] == kl) kb_layout->value((int)i);
    apply->deactivate();
    revert->deactivate();
    redraw();
  }

  void choose_image() {
    Fl_File_Chooser fc(home_dir().c_str(), "Images (*.{png,jpg,jpeg,svg})", Fl_File_Chooser::SINGLE,
                       "Login Screen Background");
    fc.preview(1);
    fc.show();
    while (fc.shown()) Fl::wait();
    if (!fc.value()) return;
    background = custom_background = fc.value();
    gallery->add_file(background);
    gallery->value(background);
    set_changed();
  }

  void save() {
    Ini n;
    auto pick = [](Fl_Choice *c, const std::vector<std::string> &v) {
      int i = c->value();
      return i >= 0 && i < (int)v.size() ? v[i] : std::string();
    };
    n.set("Appearance", "Theme", theme->value() == 1 ? "dark" : "light");
    n.set("Appearance", "Accent", accent->value());
    n.set_int("Appearance", "FontSize", (int)font_size->value());
    std::string it = pick(icons, icon_themes);
    if (!it.empty()) n.set("General", "IconTheme", it);
    std::string ct = pick(cursor, cursor_themes);
    if (!ct.empty()) n.set("Cursor", "Theme", ct);
    n.set_int("Cursor", "Size", (int)cursor_size->value());
    if (custom_background.empty()) n.set("Greeter", "Background", background);
    n.set_bool("Greeter", "RememberUser", remember->value() != 0);
    n.set_int("Greeter", "MinUid", (int)min_uid->value());
    std::string hide;
    for (int i = 1; i <= users->nitems(); i++)
      if (!users->checked(i)) hide += (hide.empty() ? "" : ",") + user_names[i - 1];
    n.set("Greeter", "HideUsers", hide);
    n.set_bool("Greeter", "Clock24", clock24->value() != 0);
    n.set_bool("Greeter", "ShowPowerButtons", power->value() != 0);
    const char *modes[] = {"button", "always", "never"};
    n.set("Greeter", "Keyboard", modes[std::max(0, kb_mode->value())]);
    std::string kl = pick(kb_layout, layouts);
    if (!kl.empty()) n.set("Greeter", "KeyboardLayout", kl);
    // check here first: the helper refuses the same things
    Ini check;
    std::string err;
    if (!validate_greeter_config(n, check, err)) {
      fl_alert("%s", err.c_str());
      return;
    }
    if (which("pkexec").empty()) {
      fl_alert("pkexec (polkit) is needed to change the login screen.");
      return;
    }
    std::string dir = getenv_or("XDG_RUNTIME_DIR", "/tmp");
    std::string tmp = dir + "/fl-settings-greeter.ini";
    unlink(tmp.c_str());
    if (!n.save(tmp)) {
      fl_alert("Cannot write %s", tmp.c_str());
      return;
    }
    std::vector<std::string> cmd{"pkexec", settings_helper_path(), "greeter-ini", tmp};
    if (!custom_background.empty()) cmd.push_back(custom_background);
    apply->deactivate();
    status->copy_label("Saving… (polkit asks for an administrator password)");
    status->redraw();
    // pkexec waits for the authentication dialog: not in the UI thread
    std::thread([this, cmd, tmp]() {
      std::string out;
      int rc = run_capture(cmd, out);
      unlink(tmp.c_str());
      struct Res {
        LoginPage *page;
        int rc;
        std::string out;
      };
      Fl::awake([](void *p) {
        std::unique_ptr<Res> r((Res *)p);
        r->page->saved(r->rc, r->out);
      }, new Res{this, rc, out});
    }).detach();
  }

  void saved(int rc, const std::string &out) {
    if (rc == 0) {
      load();
      std::string s = std::string(status->label()) + " Saved.";
      status->copy_label(s.c_str());
      return;
    }
    apply->activate();
    // 126/127: the authentication was dismissed or failed
    if (rc == 126 || rc == 127) status->copy_label("Not saved: the authentication was cancelled or failed.");
    else status->copy_label(("Not saved: " + trim(out)).c_str());
    redraw();
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new LoginPage(x, y, w, h); }

}  // namespace

PageInfo page_login() {
  return {"login", "Login Screen", "preferences-desktop-display", "System",
          "login screen greeter greetd fl-greet users background display manager", create};
}

}  // namespace fld
