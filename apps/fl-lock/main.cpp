// fl-lock: the fldesktop screen locker
//
//   fl-lock            asks fl-wm to lock the session (SIGUSR1)
//   fl-lock --locker   the lock screen; fl-wm starts it and unlocks the
//                      session only when it exits with status 0, which it
//                      does after a successful authentication (PAM "fl-lock")
//   fl-lock --daemon   locks on logind's Lock signal and before suspend
//                      ([Lock] OnSuspend in desktop.ini), started at login
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_draw.H>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <pwd.h>
#include <string>
#include <thread>
#include <unistd.h>

#include "auth.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/osk.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

int run_daemon();  // daemon.cpp

namespace {

struct LockConfig {
  std::string background;  // image, "" = the desktop wallpaper
  bool show_clock = true, clock24 = true, keyboard = true;
};

LockConfig load_config() {
  LockConfig c;
  fld::Ini ini;
  ini.load(fld::desktop_config_path());
  c.background = fld::expand_tilde(ini.get("Lock", "Background", ""));
  if (c.background.empty()) c.background = fld::expand_tilde(ini.get("Desktop", "Wallpaper", ""));
  if (c.background.empty() || !fld::file_exists(c.background))
    c.background = fld::fld_data_dir() + "/backgrounds/default.svg";
  c.show_clock = ini.get_bool("Lock", "ShowClock", true);
  c.clock24 = ini.get_bool("Lock", "Clock24", true);
  c.keyboard = ini.get_bool("Lock", "Keyboard", true);
  return c;
}

LockConfig g_cfg;
std::string g_user, g_real_name, g_avatar;
Fl_Double_Window *g_win;
Fl_Secret_Input *g_pass;
Fl_Button *g_unlock;
Fl_Box *g_msg;
fld::OskWidget *g_osk;
Fl_Button *g_kb;
fld::FltkKeySink g_sink;
std::atomic<bool> g_busy{false};
int g_failures = 0;

class Screen : public Fl_Group {
public:
  std::unique_ptr<Fl_Image> src, scaled, avatar;
  int sw = 0, sh = 0;
  Screen(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    src.reset(fld::load_image(g_cfg.background));
    if (!g_avatar.empty()) {
      Fl_Image *a = fld::load_image(g_avatar);
      if (a) {
        avatar.reset(a->copy(96, 96));
        delete a;
      }
    }
  }
  int card_y() const { return y() + h() / 2 - 40; }
  void draw() override {
    fl_color(fl_rgb_color(0x1e, 0x1f, 0x22));
    fl_rectf(x(), y(), w(), h());
    if (src && src->w() > 0 && src->h() > 0) {
      if (!scaled || sw != w() || sh != h()) {
        double s = std::max((double)w() / src->w(), (double)h() / src->h());
        scaled.reset(src->copy((int)std::ceil(src->w() * s), (int)std::ceil(src->h() * s)));
        scaled->color_average(FL_BLACK, 0.55f);  // dim it so the text stands out
        sw = w();
        sh = h();
      }
      fl_push_clip(x(), y(), w(), h());
      scaled->draw(x() + (w() - scaled->w()) / 2, y() + (h() - scaled->h()) / 2);
      fl_pop_clip();
    }
    int cy = card_y();
    if (g_cfg.show_clock) {
      time_t now = time(nullptr);
      struct tm tm;
      localtime_r(&now, &tm);
      char t[32], d[64];
      strftime(t, sizeof t, g_cfg.clock24 ? "%H:%M" : "%I:%M %p", &tm);
      strftime(d, sizeof d, "%A, %d %B", &tm);
      fl_color(FL_WHITE);
      fl_font(FL_HELVETICA, 84);
      int top = std::max(y() + 20, cy - 260);
      fl_draw(t[0] == '0' && !g_cfg.clock24 ? t + 1 : t, x(), top, w(), 100, FL_ALIGN_CENTER);
      fl_font(FL_HELVETICA, 22);
      fl_draw(d, x(), top + 100, w(), 32, FL_ALIGN_CENTER);
    }
    // the user: avatar (or initial) and name
    int ax = x() + w() / 2 - 48, ay = cy;
    if (avatar) {
      avatar->draw(ax, ay);
    } else {
      fl_color(fld::palette().accent);
      fl_pie(ax, ay, 96, 96, 0, 360);
      std::string init(1, (char)toupper((unsigned char)(g_user.empty() ? '?' : g_user[0])));
      fl_color(FL_WHITE);
      fl_font(FL_HELVETICA_BOLD, 44);
      fl_draw(init.c_str(), ax, ay, 96, 96, FL_ALIGN_CENTER);
    }
    fl_color(FL_WHITE);
    fl_font(FL_HELVETICA_BOLD, 20);
    std::string name = g_real_name.empty() ? g_user : g_real_name;
    fl_draw(fld::escape_at(name).c_str(), x(), ay + 104, w(), 30, FL_ALIGN_CENTER);
    fl_font(FL_HELVETICA, 14);
    fl_color(fl_rgb_color(0xd0, 0xd0, 0xd0));
    if (Fl::event_state(FL_CAPS_LOCK)) fl_draw("Caps Lock is on", x(), ay + 186, w(), 20, FL_ALIGN_CENTER);
    draw_children();
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);  // children are placed by layout()
    layout();
  }
  void layout() {
    int cy = card_y() + 140, iw = 300;
    if (g_pass) g_pass->resize(x() + (w() - iw) / 2 - 20, cy, iw - 44, 36);
    if (g_unlock) g_unlock->resize(x() + (w() - iw) / 2 + iw - 60, cy, 80, 36);
    if (g_msg) g_msg->resize(x() + (w() - 500) / 2, cy + 66, 500, 44);
    if (g_kb) g_kb->resize(x() + w() - 130, y() + h() - 50, 116, 34);
    if (g_osk) {
      int ow = std::min(w() - 20, 1000), oh = std::max(180, std::min(280, h() * 30 / 100));
      g_osk->resize(x() + (w() - ow) / 2, y() + h() - oh - 10, ow, oh);
    }
  }
  int handle(int e) override {
    // keep the caps lock note current
    if (e == FL_KEYDOWN || e == FL_KEYUP) redraw();
    return Fl_Group::handle(e);
  }
};

Screen *g_screen;

void set_message(const std::string &m, bool error) {
  g_msg->copy_label(fld::escape_at(m).c_str());
  g_msg->labelcolor(error ? fl_rgb_color(0xff, 0x9a, 0x9a) : fl_rgb_color(0xe0, 0xe0, 0xe0));
  g_screen->redraw();
}

void finish_auth(const AuthResult &r) {
  g_busy = false;
  if (r.ok) {
    // fl-wm unlocks when we exit with status 0
    fflush(stdout);
    _exit(0);
  }
  g_failures++;
  std::string m = r.message.empty() ? "Wrong password, try again." : r.message;
  set_message(m, true);
  g_pass->value("");
  g_pass->activate();
  g_unlock->activate();
  g_pass->take_focus();
}

void try_unlock() {
  if (g_busy) return;
  std::string pw = g_pass->value();
  if (pw.empty()) return;
  g_busy = true;
  g_pass->deactivate();
  g_unlock->deactivate();
  set_message("Checking…", false);
  // PAM can take seconds (and delays failures on purpose): not in the UI thread
  std::string user = g_user;
  std::thread([user, pw]() mutable {
    AuthResult r = authenticate(user, pw);
    std::fill(pw.begin(), pw.end(), '\0');
    Fl::awake([](void *p) {
      std::unique_ptr<AuthResult> res((AuthResult *)p);
      finish_auth(*res);
    }, new AuthResult(r));
  }).detach();
}

void tick(void *) {
  g_screen->redraw();
  Fl::repeat_timeout(10.0, tick);
}

std::string find_avatar(const std::string &user, const std::string &home) {
  for (std::string p : {home + "/.face", home + "/.face.icon", "/var/lib/AccountsService/icons/" + user})
    if (fld::file_exists(p) && !fld::is_dir(p)) return p;
  return "";
}

int run_locker() {
  struct passwd *pw = getpwuid(getuid());
  if (!pw) {
    fprintf(stderr, "fl-lock: who am I?\n");
    return 2;
  }
  g_user = pw->pw_name;
  std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
  g_real_name = gecos.substr(0, gecos.find(','));
  g_avatar = find_avatar(g_user, pw->pw_dir ? pw->pw_dir : "");
  g_cfg = load_config();

  fld::init_app("fldesktop.lock", 0, nullptr);
  Fl_Window::default_xclass("fl-lock");
  Fl::lock();
  int W = Fl::w() > 0 ? Fl::w() : 1280, H = Fl::h() > 0 ? Fl::h() : 800;
  // the title is how fl-wm recognises the lock window (with our pid)
  g_win = new Fl_Double_Window(W, H, "fldesktop.lock");
  g_win->border(0);
  g_screen = new Screen(0, 0, W, H);
  g_pass = new Fl_Secret_Input(0, 0, 10, 10);
  g_pass->when(FL_WHEN_ENTER_KEY_ALWAYS);
  g_pass->callback([](Fl_Widget *, void *) { try_unlock(); });
  g_pass->tooltip("Your password");
  g_unlock = new Fl_Button(0, 0, 10, 10, "Unlock");
  g_unlock->color(fld::palette().accent);
  g_unlock->labelcolor(FL_WHITE);
  g_unlock->callback([](Fl_Widget *, void *) { try_unlock(); });
  g_msg = new Fl_Box(0, 0, 10, 10);
  g_msg->align(FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_CENTER);
  g_msg->labelsize(14);
  if (g_cfg.keyboard) {
    g_osk = new fld::OskWidget(0, 0, 10, 10);
    g_osk->sink(&g_sink);
    fld::Ini wm;
    wm.load(fld::wm_config_path());
    std::string layout = fld::split(wm.get("keyboard", "layout", "us"), ',').empty()
                             ? "us" : fld::trim(fld::split(wm.get("keyboard", "layout", "us"), ',')[0]);
    g_osk->layout(fld::osk_layout(layout) ? layout : "us");
    g_osk->on_hide = [](fld::OskWidget *o, void *) { o->hide(); g_pass->take_focus(); };
    g_osk->hide();
    Fl_Button *kb = g_kb = new Fl_Button(W - 130, H - 50, 116, 34, "Keyboard");
    kb->tooltip("Show or hide the on-screen keyboard");
    kb->clear_visible_focus();
    kb->callback([](Fl_Widget *, void *) {
      if (g_osk->visible()) g_osk->hide();
      else g_osk->show();
      g_screen->redraw();
      g_pass->take_focus();
    });
  }
  g_screen->end();
  g_screen->layout();
  g_win->end();
  g_win->resizable(g_screen);
  g_win->show();
  g_pass->take_focus();
  Fl::add_timeout(10.0, tick);
  return Fl::run();
}

// asks the running fl-wm to lock
int request_lock() {
  const char *pid = getenv("FLWM_PID");
  if (pid && atoi(pid) > 1) {
    std::string comm;
    fld::read_file(std::string("/proc/") + pid + "/comm", comm);
    if (fld::trim(comm) == "fl-wm" && kill(atoi(pid), SIGUSR1) == 0) return 0;
  }
  // not in an fldesktop session: logind can ask the session's own locker
  if (!fld::which("loginctl").empty()) {
    std::string out;
    if (fld::run_capture({"loginctl", "lock-session"}, out) == 0) return 0;
  }
  fprintf(stderr, "fl-lock: no fl-wm session to lock (FLWM_PID is not set)\n");
  return 1;
}

}  // namespace

int main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "--locker") return run_locker();
  if (mode == "--daemon") return run_daemon();
  if (mode == "-h" || mode == "--help") {
    printf("Usage: fl-lock [--locker | --daemon]\n"
           "  (no option)  lock the fldesktop session\n"
           "  --locker     the lock screen itself (started by fl-wm)\n"
           "  --daemon     lock on logind requests and before suspend\n");
    return 0;
  }
  if (!mode.empty()) {
    fprintf(stderr, "fl-lock: unknown option %s\n", mode.c_str());
    return 2;
  }
  return request_lock();
}
