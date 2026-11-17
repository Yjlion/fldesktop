// fl-greet: an FLTK greeter for greetd
//
// Runs fullscreen inside a kiosk compositor (cage), e.g. in /etc/greetd/config.toml:
//   command = "cage -s -d -- fl-greet"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <pwd.h>
#include <string>
#include <vector>

#include "fld/desktop_entry.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/osk.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "greetd_ipc.h"

using fld::Json;

struct User {
  std::string name, real_name, home;
};

struct Session {
  std::string name, exec, desktop_names;
};

static std::vector<User> g_users;
static std::vector<Session> g_sessions;
static std::unique_ptr<GreetdClient> g_greetd;
static bool g_demo = false;
static bool g_clock24 = true, g_remember = true;

static Fl_Double_Window *g_win;
static Fl_Choice *g_user_choice;
static Fl_Input *g_user_input; // used when there is no user list
static Fl_Secret_Input *g_pass;
static Fl_Choice *g_session_choice;
static Fl_Box *g_message;

static bool caps_lock_on() { return (Fl::event_state() & FL_CAPS_LOCK) != 0; }
static Fl_Button *g_login_btn;
static int g_card_y = 300;
static int g_card_y_normal = 300;
static class Card *g_card;
static fld::OskWidget *g_osk;
static fld::FltkKeySink g_osk_sink;

static std::string state_file() { return fld::cache_home() + "/fl-greet.state"; }

// ---- data ----------------------------------------------------------------------

static void load_users(int min_uid, const std::vector<std::string> &hidden) {
  setpwent();
  while (struct passwd *pw = getpwent()) {
    if ((int)pw->pw_uid < min_uid || pw->pw_uid >= 60000) continue;
    std::string shell = pw->pw_shell ? pw->pw_shell : "";
    if (fld::ends_with(shell, "nologin") || fld::ends_with(shell, "false")) continue;
    if (std::find(hidden.begin(), hidden.end(), pw->pw_name) != hidden.end()) continue;
    User u;
    u.name = pw->pw_name;
    std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
    u.real_name = gecos.substr(0, gecos.find(','));
    u.home = pw->pw_dir ? pw->pw_dir : "";
    g_users.push_back(u);
  }
  endpwent();
  std::sort(g_users.begin(), g_users.end(), [](const User &a, const User &b) { return a.name < b.name; });
}

static void load_sessions() {
  std::vector<std::string> seen;
  for (auto &d : fld::data_dirs()) {
    std::string dir = fld::path_join(d, "wayland-sessions");
    for (auto &n : fld::list_dir(dir)) {
      if (!fld::ends_with(n, ".desktop") || std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
      seen.push_back(n);
      fld::DesktopEntry e;
      if (!e.load(fld::path_join(dir, n)) || e.hidden || e.exec.empty()) continue;
      if (!e.try_exec.empty() && fld::which(e.try_exec).empty()) continue;
      fld::Ini ini;
      ini.load(fld::path_join(dir, n));
      g_sessions.push_back({e.name, e.exec, ini.get("Desktop Entry", "DesktopNames")});
    }
  }
  // fldesktop first
  std::stable_sort(g_sessions.begin(), g_sessions.end(),
                   [](const Session &a, const Session &b) { return a.name == "fldesktop" && b.name != "fldesktop"; });
  if (g_sessions.empty()) g_sessions.push_back({"Shell", fld::getenv_or("SHELL", "/bin/sh"), ""});
}

static std::string selected_user() {
  if (g_user_input && g_user_input->visible()) return fld::trim(g_user_input->value());
  int i = g_user_choice->value();
  return (i >= 0 && i < (int)g_users.size()) ? g_users[i].name : "";
}

// ---- login flow ----------------------------------------------------------------------

static void set_message(const std::string &m, bool error) {
  g_message->copy_label(fld::escape_at(m).c_str());
  g_message->labelcolor(error ? fl_rgb_color(0xff, 0x8a, 0x8a) : fl_rgb_color(0xdd, 0xe3, 0xea));
  g_win->redraw();
  Fl::flush();
}

static void cancel() {
  Json r;
  std::string err;
  g_greetd->cancel_session(r, err);
}

static void save_state(const std::string &user, int session) {
  fld::Ini st;
  st.set("State", "User", user);
  st.set_int("State", "Session", session);
  st.save(state_file());
}

static bool start_selected_session() {
  int si = std::max(0, g_session_choice->value());
  const Session &s = g_sessions[si];
  std::vector<std::string> cmd = fld::shell_split(s.exec);
  std::vector<std::string> env{"XDG_SESSION_TYPE=wayland"};
  if (!s.desktop_names.empty()) {
    std::string names = s.desktop_names;
    std::replace(names.begin(), names.end(), ';', ':');
    while (!names.empty() && names.back() == ':') names.pop_back();
    env.push_back("XDG_CURRENT_DESKTOP=" + names);
    env.push_back("XDG_SESSION_DESKTOP=" + names.substr(0, names.find(':')));
  }
  Json reply;
  std::string err;
  if (!g_greetd->start_session(cmd, env, reply, err)) {
    set_message(err, true);
    return false;
  }
  if (reply["type"].as_string() != "success") {
    std::string d = reply["description"].as_string();
    set_message(d.empty() ? "The session could not be started." : "The session could not be started: " + d, true);
    cancel();
    return false;
  }
  save_state(selected_user(), si);
  return true;
}

static void login() {
  std::string user = selected_user();
  if (user.empty()) {
    set_message("Choose a user", true);
    return;
  }
  g_login_btn->deactivate();
  set_message("Authenticating…", false);
  Json reply;
  std::string err;
  if (!g_greetd->create_session(user, reply, err)) {
    set_message(err, true);
    g_login_btn->activate();
    return;
  }
  bool password_used = false;
  for (int guard = 0; guard < 16; guard++) {
    std::string type = reply["type"].as_string();
    if (type == "success") {
      if (start_selected_session()) {
        set_message("Starting session…", false);
        if (g_demo) {
          set_message("Demo: the session would start now", false);
          g_login_btn->activate();
          return;
        }
        g_win->hide();
        return;
      }
      break;
    }
    if (type == "error") {
      std::string m = greetd_error_text(reply);
      if (reply["error_type"].as_string() == "auth_error" && caps_lock_on())
        m += " Caps Lock is on.";
      set_message(m, true);
      cancel();
      break;
    }
    if (type == "auth_message") {
      std::string mtype = reply["auth_message_type"].as_string();
      std::string msg = reply["auth_message"].as_string();
      bool ok;
      if (mtype == "secret" || mtype == "visible") {
        std::string answer;
        if (!password_used && mtype == "secret") {
          answer = g_pass->value();
          password_used = true;
        } else {
          // further prompts (e.g. a one time code): ask with a dialog
          const char *a = mtype == "secret" ? fl_password("%s", "", msg.c_str()) : fl_input("%s", "", msg.c_str());
          if (!a) { cancel(); set_message("Login cancelled", true); break; }
          answer = a;
        }
        ok = g_greetd->post_response(&answer, reply, err);
      } else {
        // info / error messages: show them and acknowledge
        set_message(msg, mtype == "error");
        ok = g_greetd->post_response(nullptr, reply, err);
      }
      if (!ok) {
        set_message(err, true);
        break;
      }
      continue;
    }
    set_message("Unexpected reply from greetd", true);
    cancel();
    break;
  }
  g_pass->value("");
  g_pass->take_focus();
  g_login_btn->activate();
}

// ---- UI -------------------------------------------------------------------------

class Background : public Fl_Widget {
public:
  std::unique_ptr<Fl_Image> src, scaled;
  int sw = 0, sh = 0;
  Background(int x, int y, int w, int h, const std::string &path) : Fl_Widget(x, y, w, h) {
    src.reset(fld::load_image(path));
  }
  void draw() override {
    fl_color(fl_rgb_color(0x1b, 0x2a, 0x44));
    fl_rectf(x(), y(), w(), h());
    if (src) {
      if (!scaled || sw != w() || sh != h()) {
        double s = std::max((double)w() / src->w(), (double)h() / src->h());
        scaled.reset(src->copy((int)std::ceil(src->w() * s), (int)std::ceil(src->h() * s)));
        sw = w();
        sh = h();
      }
      scaled->draw(x() + (w() - scaled->w()) / 2, y() + (h() - scaled->h()) / 2);
    }
    // clock
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char t[32], d[64];
    strftime(t, sizeof t, g_clock24 ? "%H:%M" : "%l:%M %p", &tm);
    strftime(d, sizeof d, "%A, %d %B", &tm);
    fl_font(FL_HELVETICA, 72);
    fl_color(FL_WHITE);
    int top = std::max(y() + 10, g_card_y - 150);
    fl_draw(t, x(), top, w(), 90, FL_ALIGN_CENTER);
    fl_font(FL_HELVETICA, 20);
    fl_draw(d, x(), top + 88, w(), 30, FL_ALIGN_CENTER);
  }
};

class Card : public Fl_Group {
public:
  Card(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {}
  void draw() override {
    fl_color(fl_rgb_color(0x10, 0x16, 0x22));
    fl_rounded_rectf(x(), y(), w(), h(), 14);
    // avatar circle with the user's initial
    std::string u = selected_user();
    int cx = x() + w() / 2, cy = y() + 56;
    fl_color(fld::palette().accent);
    fl_pie(cx - 36, cy - 36, 72, 72, 0, 360);
    std::string init = u.empty() ? "?" : std::string(1, (char)toupper((unsigned char)u[0]));
    fl_font(FL_HELVETICA_BOLD, 34);
    fl_color(FL_WHITE);
    fl_draw(init.c_str(), cx - 36, cy - 36, 72, 72, FL_ALIGN_CENTER);
    draw_children();
  }
};

// Shows or hides the on-screen keyboard and moves the card out of its way.
static void show_keyboard(bool on) {
  if (!g_osk) return;
  int H = g_win->h();
  int target = g_card_y_normal;
  if (on) {
    g_osk->show();
    int space_bottom = g_osk->y() - 10;
    if (target + g_card->h() > space_bottom) target = std::max(10, space_bottom - g_card->h());
  } else {
    g_osk->hide();
  }
  (void)H;
  g_card->position(g_card->x(), target);
  g_card_y = target;
  g_win->redraw();
  if (g_user_input->visible() && !*g_user_input->value()) g_user_input->take_focus();
  else g_pass->take_focus();
}

static void tick(void *) {
  g_win->redraw();
  Fl::repeat_timeout(30.0, tick);
}

static void power(const char *what) {
  const char *q = !strcmp(what, "poweroff") ? "Shut down the computer?" : "Restart the computer?";
  if (fl_choice("%s", "Cancel", !strcmp(what, "poweroff") ? "Shut Down" : "Restart", nullptr, q) != 1) return;
  if (g_demo) { set_message(std::string("Demo: would run systemctl ") + what, false); return; }
  fld::spawn({"systemctl", what});
}

static void style_input(Fl_Widget *w) {
  w->labelcolor(FL_WHITE);
  w->labelsize(13);
  w->align(FL_ALIGN_TOP_LEFT);
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--demo") g_demo = true;
    else if (a == "-h" || a == "--help") {
      printf("Usage: fl-greet [--demo]\n  --demo  test the greeter without greetd (password: demo)\n");
      return 0;
    }
  }
  // greeter.ini also holds the login screen's appearance ([Appearance],
  // [General] IconTheme) in the same form as a user's desktop.ini
  const char *ini_path = "/etc/fldesktop/greeter.ini";
  if (fld::file_exists(ini_path)) fld::set_desktop_config_path(ini_path);
  fld::init_app("fl-greet", 0, nullptr);
  fld::Ini cfg;
  cfg.load(ini_path);
  g_clock24 = cfg.get_bool("Greeter", "Clock24", true);
  g_remember = cfg.get_bool("Greeter", "RememberUser", true);
  std::vector<std::string> hidden;
  for (auto &h : fld::split(cfg.get("Greeter", "HideUsers", ""), ',')) hidden.push_back(fld::trim(h));
  load_users(cfg.get_int("Greeter", "MinUid", 1000), hidden);
  load_sessions();
  std::string bg = cfg.get("Greeter", "Background", fld::fld_data_dir() + "/backgrounds/default.svg");

  g_greetd.reset(g_demo ? (GreetdClient *)new DemoGreetd : new GreetdClient);
  std::string err;
  bool connected = g_greetd->connect(err);

  int W = Fl::w() > 0 ? Fl::w() : 1280, H = Fl::h() > 0 ? Fl::h() : 800;
  g_win = new Fl_Double_Window(W, H, "fl-greet");
  g_win->border(0);
  Background *back = new Background(0, 0, W, H, bg);
  (void)back;

  const int CW = 380, CH = 330;
  g_card_y = g_card_y_normal = std::max(150, (H - CH) / 2 + 30);
  Card *card = new Card((W - CW) / 2, g_card_y, CW, CH);
  g_card = card;
  int cx = card->x() + 30, cy = card->y() + 106, iw = CW - 60;
  g_user_choice = new Fl_Choice(cx, cy + 18, iw, 32, "User");
  style_input(g_user_choice);
  g_user_input = new Fl_Input(cx, cy + 18, iw, 32, "User name");
  style_input(g_user_input);
  for (auto &u : g_users) {
    std::string label = u.real_name.empty() ? u.name : u.real_name + " (" + u.name + ")";
    g_user_choice->add(fld::escape_at(label).c_str());
  }
  if (g_users.empty()) g_user_choice->hide();
  else g_user_input->hide();
  g_user_choice->callback([](Fl_Widget *, void *) { g_win->redraw(); g_pass->take_focus(); });

  g_pass = new Fl_Secret_Input(cx, cy + 74, iw, 32, "Password");
  style_input(g_pass);
  g_pass->when(FL_WHEN_ENTER_KEY_ALWAYS);
  g_pass->callback([](Fl_Widget *, void *) { login(); });

  g_session_choice = new Fl_Choice(cx, cy + 130, iw - 110, 32, "Session");
  style_input(g_session_choice);
  for (auto &s : g_sessions) g_session_choice->add(fld::escape_at(s.name).c_str());
  g_session_choice->value(0);

  g_login_btn = new Fl_Button(cx + iw - 100, cy + 130, 100, 32, "Log In");
  g_login_btn->color(fld::palette().accent);
  g_login_btn->labelcolor(FL_WHITE);
  g_login_btn->callback([](Fl_Widget *, void *) { login(); });

  g_message = new Fl_Box(card->x() + 10, card->y() + CH - 46, CW - 20, 36);
  g_message->align(FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_CENTER);
  g_message->labelsize(13);
  card->end();

  bool power_buttons = cfg.get_bool("Greeter", "ShowPowerButtons", true);
  if (power_buttons) {
    Fl_Button *reboot = new Fl_Button(W - 240, H - 50, 110, 34, "Restart");
    reboot->callback([](Fl_Widget *, void *) { power("reboot"); });
    Fl_Button *off = new Fl_Button(W - 124, H - 50, 110, 34, "Shut Down");
    off->callback([](Fl_Widget *, void *) { power("poweroff"); });
  }
  // on-screen keyboard: Keyboard=button (default) | always | never
  std::string kb_mode = fld::lower(cfg.get("Greeter", "Keyboard", "button"));
  if (kb_mode != "never") {
    int ow = std::min(W - 20, 1000), oh = std::max(180, std::min(280, H * 32 / 100));
    g_osk = new fld::OskWidget((W - ow) / 2, H - 60 - oh, ow, oh);
    g_osk->sink(&g_osk_sink);
    g_osk->layout(cfg.get("Greeter", "KeyboardLayout", "us"));
    g_osk->on_hide = [](fld::OskWidget *, void *) { show_keyboard(false); };
    g_osk->hide();
    Fl_Button *kb = new Fl_Button(power_buttons ? W - 356 : W - 124, H - 50, 110, 34, "Keyboard");
    kb->tooltip("Show or hide the on-screen keyboard");
    kb->callback([](Fl_Widget *, void *) { show_keyboard(!g_osk->visible()); });
    kb->clear_visible_focus();
  }
  if (g_demo) {
    Fl_Box *demo = new Fl_Box(14, H - 46, 400, 30, "Demo mode: any user, password \"demo\"");
    demo->labelcolor(FL_WHITE);
    demo->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  }
  g_win->end();
  g_win->resizable(back);

  // restore the last user and session
  fld::Ini st;
  if (g_remember && st.load(state_file())) {
    std::string last = st.get("State", "User");
    for (size_t i = 0; i < g_users.size(); i++)
      if (g_users[i].name == last) g_user_choice->value((int)i);
    int ls = st.get_int("State", "Session", 0);
    if (ls >= 0 && ls < (int)g_sessions.size()) g_session_choice->value(ls);
  } else if (!g_users.empty()) {
    g_user_choice->value(0);
  }

  if (!connected) set_message(err, true);
  g_win->fullscreen();
  g_win->show();
  if (g_user_input->visible()) g_user_input->take_focus();
  else g_pass->take_focus();
  if (g_osk && kb_mode == "always") show_keyboard(true);
  Fl::add_timeout(30.0, tick);
  return Fl::run();
}
