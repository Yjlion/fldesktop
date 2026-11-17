// Proxy: inherit the session's proxy variables, no proxy, or manual proxies
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Round_Button.H>
#include <FL/fl_ask.H>

#include <cstdlib>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "proxy_env.h"
#include "settings_common.h"
#include "settings_page.h"

extern char **environ;

namespace fld {
namespace {

// the proxy variables the session started with
std::map<std::string, std::string> session_inherited() {
  std::string text;
  if (read_file(session_env_snapshot_path(), text)) return inherited_proxy(parse_env_text(text));
  // no snapshot (fl-wm started without fl-session): our own environment
  std::map<std::string, std::string> env;
  for (char **e = environ; e && *e; e++) {
    std::string s = *e;
    size_t eq = s.find('=');
    if (eq != std::string::npos) env[s.substr(0, eq)] = s.substr(eq + 1);
  }
  return inherited_proxy(env);
}

class ProxyPage : public Fl_Group {
public:
  ConfigFile desk{desktop_ini_path()}, wm{wm_ini_path()};
  std::map<std::string, std::string> inherited;
  Fl_Round_Button *m_inherit, *m_none, *m_manual;
  Fl_Box *inherited_box;
  Fl_Input *http, *https, *ftp, *socks, *ignore;
  Fl_Check_Button *all;
  Fl_Button *apply;
  Fl_Group *manual_group;

  ProxyPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Proxy", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    inherited = session_inherited();
    ProxySettings p = load_proxy(desk.ini);
    if (!desk.ini.has("Proxy", "Mode")) p.mode = default_mode(inherited);

    f.heading("Connection");
    f.row("", 26, wx, wy, ww);
    m_inherit = new Fl_Round_Button(wx, wy, ww, 26, "Use the proxy settings the session started with");
    m_inherit->type(FL_RADIO_BUTTON);
    std::string desc;
    for (auto &kv : inherited) desc += kv.first + "=" + kv.second + "\n";
    if (desc.empty()) desc = "The session started without proxy variables (no proxy).";
    else desc = "From the environment:\n" + desc;
    int lines = 1;
    for (char c : desc) lines += c == '\n';
    f.row("", 18 * std::min(lines, 8), wx, wy, ww);
    inherited_box = new Fl_Box(wx + 24, wy, ww - 24, 18 * std::min(lines, 8));
    inherited_box->copy_label(escape_at(trim(desc)).c_str());
    inherited_box->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    inherited_box->labelsize(12);
    inherited_box->labelfont(FL_COURIER);
    inherited_box->labelcolor(palette().fg_dim);
    f.row("", 26, wx, wy, ww);
    m_none = new Fl_Round_Button(wx, wy, ww, 26, "No proxy");
    m_none->type(FL_RADIO_BUTTON);
    f.row("", 26, wx, wy, ww);
    m_manual = new Fl_Round_Button(wx, wy, ww, 26, "Manual proxy configuration");
    m_manual->type(FL_RADIO_BUTTON);
    for (auto *b : {m_inherit, m_none, m_manual})
      b->callback([](Fl_Widget *, void *d) { ((ProxyPage *)d)->mode_changed(); }, this);
    (p.mode == ProxySettings::Inherit ? m_inherit : p.mode == ProxySettings::None ? m_none : m_manual)->setonly();

    f.heading("Manual Proxies");
    int gy = f.y();
    manual_group = new Fl_Group(X + 24, gy, std::min(W - 48, 640), 6 * 36 + 40);
    auto field = [&](const char *label, const std::string &v) {
      f.row(label, 28, wx, wy, ww);
      Fl_Input *in = new Fl_Input(wx, wy, ww, 28);
      in->value(v.c_str());
      in->when(FL_WHEN_CHANGED);
      in->callback([](Fl_Widget *, void *d) { ((ProxyPage *)d)->apply->activate(); }, this);
      return in;
    };
    http = field("HTTP proxy:", p.http);
    http->tooltip("host:port, e.g. proxy.example.com:3128");
    f.row("", 26, wx, wy, ww);
    all = new Fl_Check_Button(wx, wy, ww, 26, "Use this proxy for HTTPS and FTP too");
    all->value(p.use_http_for_all);
    all->callback([](Fl_Widget *, void *d) {
      ProxyPage *pg = (ProxyPage *)d;
      pg->update_fields();
      pg->apply->activate();
    }, this);
    https = field("HTTPS proxy:", p.https);
    ftp = field("FTP proxy:", p.ftp);
    socks = field("SOCKS host:", p.socks);
    socks->tooltip("host:port of a SOCKS 5 proxy (all_proxy)");
    ignore = field("Ignore hosts:", p.no_proxy);
    ignore->tooltip("Comma separated hosts and domains that are reached directly (no_proxy)");
    f.row("", 30, wx, wy, ww);
    apply = new Fl_Button(wx, wy, 120, 30, "Apply");
    apply->callback([](Fl_Widget *, void *d) { ((ProxyPage *)d)->save(); }, this);
    apply->deactivate();
    manual_group->end();
    f.space(6);
    f.note("Programs started from now on use the new settings; running programs keep theirs until they restart.",
           2);
    g->end();
    end();
    update_fields();
  }

  void update_fields() {
    bool manual = m_manual->value();
    if (manual) manual_group->activate();
    else manual_group->deactivate();
    if (all->value()) {
      https->deactivate();
      ftp->deactivate();
      socks->deactivate();
    } else if (manual) {
      https->activate();
      ftp->activate();
      socks->activate();
    }
    redraw();
  }

  void mode_changed() {
    update_fields();
    if (!m_manual->value()) save();
    else apply->activate();
  }

  void save() {
    ProxySettings p;
    p.mode = m_inherit->value() ? ProxySettings::Inherit : m_none->value() ? ProxySettings::None : ProxySettings::Manual;
    p.http = trim(http->value());
    p.https = trim(https->value());
    p.ftp = trim(ftp->value());
    p.socks = trim(socks->value());
    p.use_http_for_all = all->value();
    p.no_proxy = trim(ignore->value());
    if (p.mode == ProxySettings::Manual) {
      for (auto *in : {http, https, ftp, socks})
        if (in->active() && !valid_proxy(in->value())) {
          fl_alert("\"%s\" is not a proxy address. Use host:port, for example proxy.example.com:3128.",
                   in->value());
          in->take_focus();
          return;
        }
    }
    desk.ini.load(desk.path);
    save_proxy(desk.ini, p);
    desk.save();
    wm.ini.load(wm.path);
    auto env = proxy_environment(p);
    for (auto &kv : env) wm.ini.set("environment", kv.first, kv.second);
    save_wm(wm);  // fl-wm applies [environment] when it reloads
    // D-Bus and systemd activated services get the values too
    if (!which("dbus-update-activation-environment").empty()) {
      std::vector<std::string> cmd{"dbus-update-activation-environment", "--systemd"};
      for (auto &kv : env) {
        std::string v = kv.second;
        if (v == "@inherit") {
          auto it = inherited.find(kv.first);
          v = it == inherited.end() ? "" : it->second;
        }
        cmd.push_back(kv.first + "=" + v);
      }
      spawn(cmd);
    }
    apply->deactivate();
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new ProxyPage(x, y, w, h); }

}  // namespace

PageInfo page_proxy() {
  return {"proxy", "Proxy", "preferences-system-network", "Network",
          "proxy http https socks ftp no_proxy environment internet connection", create};
}

}  // namespace fld
