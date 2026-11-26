// Default Applications: web, mail, files, terminal, text, images, music, video, PDF
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Choice.H>

#include <algorithm>
#include <set>

#include "fld/desktop_entry.h"
#include "fld/ini.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "gnupg_conf.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

struct Kind {
  const char *label;
  std::vector<std::string> mimes; // the first one decides what is shown
  const char *category;           // fallback: apps of this category
};

const std::vector<Kind> &kinds() {
  static const std::vector<Kind> k = {
      {"Web browser:", {"x-scheme-handler/https", "x-scheme-handler/http", "text/html"}, "WebBrowser"},
      {"Mail:", {"x-scheme-handler/mailto"}, "Email"},
      {"Files:", {"inode/directory"}, "FileManager"},
      {"Text editor:", {"text/plain"}, "TextEditor"},
      {"Images:", {"image/png", "image/jpeg", "image/gif", "image/svg+xml", "image/bmp", "image/webp"}, "Viewer"},
      {"Music:", {"audio/mpeg", "audio/ogg", "audio/flac", "audio/x-wav"}, "Audio"},
      {"Video:", {"video/mp4", "video/x-matroska", "video/webm"}, "Video"},
      {"PDF documents:", {"application/pdf"}, "Viewer"},
      {"E-books and comics:", {"application/epub+zip", "application/vnd.comicbook+zip"}, "Viewer"},
  };
  return k;
}

class DefaultAppsPage : public Fl_Group {
public:
  std::vector<Fl_Choice *> choices;
  std::vector<std::vector<DesktopEntry>> candidates;
  Fl_Choice *terminal, *pinentry = nullptr;
  std::vector<std::string> terminals, pinentries; // "" = GnuPG's own choice

  DefaultAppsPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Default Applications"), cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    auto all = all_applications();
    for (size_t k = 0; k < kinds().size(); k++) {
      const Kind &kind = kinds()[k];
      std::vector<DesktopEntry> apps = apps_for_mime(kind.mimes[0]);
      std::set<std::string> ids;
      for (auto &a : apps) ids.insert(a.id);
      for (auto &a : all)
        if (std::find(a.categories.begin(), a.categories.end(), kind.category) != a.categories.end() && !ids.count(a.id)) {
          apps.push_back(a);
          ids.insert(a.id);
        }
      f.row(kind.label, 28, wx, wy, ww);
      Fl_Choice *c = new Fl_Choice(wx, wy, ww, 28);
      DesktopEntry def;
      bool have_def = default_app_for_mime(kind.mimes[0], def);
      int sel = -1;
      for (size_t i = 0; i < apps.size(); i++) {
        std::string e;
        for (char ch : escape_at(apps[i].name)) { if (ch == '/' || ch == '\\' || ch == '&' || ch == '_') e += '\\'; e += ch; }
        c->add(e.c_str());
        if (have_def && apps[i].id == def.id) sel = (int)i;
      }
      if (apps.empty()) { c->add(_("(no application installed)")); c->deactivate(); }
      c->value(sel < 0 ? 0 : sel);
      c->callback([](Fl_Widget *w, void *d) { ((DefaultAppsPage *)d)->changed((Fl_Choice *)w); }, this);
      choices.push_back(c);
      candidates.push_back(apps);
    }
    f.row(_("Terminal:"), 28, wx, wy, ww);
    terminal = new Fl_Choice(wx, wy, ww, 28);
    Ini cfg;
    cfg.load(desktop_ini_path());
    std::string cur = cfg.get("General", "Terminal", "");
    for (const char *t : {"fl-term", "foot", "alacritty", "kitty", "konsole", "gnome-terminal", "xfce4-terminal", "xterm"}) {
      if (which(t).empty()) continue;
      terminals.push_back(t);
      terminal->add(t);
      if (cur == t) terminal->value((int)terminals.size() - 1);
    }
    if (terminal->value() < 0) terminal->value(0);
    terminal->callback([](Fl_Widget *, void *d) {
      DefaultAppsPage *p = (DefaultAppsPage *)d;
      ConfigFile c(desktop_ini_path());
      int i = p->terminal->value();
      if (i >= 0 && i < (int)p->terminals.size()) c.ini.set("General", "Terminal", p->terminals[i]);
      c.save();
    }, this);
    // GnuPG: who asks for passphrases (gpg-agent.conf pinentry-program)
    if (!which("gpg-agent").empty() || !which("gpgconf").empty()) {
      f.row(_("GnuPG passphrases:"), 28, wx, wy, ww);
      pinentry = new Fl_Choice(wx, wy, ww, 28);
      std::string conf, cur_p;
      read_file(gnupg_conf::conf_path(), conf);
      cur_p = gnupg_conf::pinentry(conf);
      pinentries.push_back("");
      pinentry->add(_("GnuPG's default"));
      for (const char *p : {"pinentry-fldesktop", "pinentry-gnome3", "pinentry-qt", "pinentry-gtk", "pinentry-gtk-2",
                            "pinentry-curses"}) {
        std::string path = which(p);
        if (path.empty()) continue;
        pinentries.push_back(path);
        pinentry->add(p);
        if (cur_p == path || basename(cur_p) == p) pinentry->value((int)pinentries.size() - 1);
      }
      if (pinentry->value() < 0) {
        if (!cur_p.empty()) { // something else, set by hand: keep showing it
          pinentries.push_back(cur_p);
          pinentry->add(escape_at(basename(cur_p)).c_str());
          pinentry->value((int)pinentries.size() - 1);
        } else {
          pinentry->value(0);
        }
      }
      pinentry->tooltip(_("The window gpg-agent opens to ask for a passphrase (gpg-agent.conf: pinentry-program)"));
      pinentry->callback([](Fl_Widget *, void *d) { ((DefaultAppsPage *)d)->set_pinentry(); }, this);
    }
    g->end();
    end();
  }

  void set_pinentry() {
    int i = pinentry->value();
    if (i < 0 || i >= (int)pinentries.size()) return;
    std::string path = gnupg_conf::conf_path(), conf;
    read_file(path, conf);
    mkdir_p(dirname(path), 0700);
    if (!write_file(path, gnupg_conf::with_pinentry(conf, pinentries[(size_t)i]))) return;
    // the running agent picks it up for the next prompt
    if (!which("gpgconf").empty()) spawn({"gpgconf", "--reload", "gpg-agent"});
  }

  void changed(Fl_Choice *c) {
    for (size_t k = 0; k < choices.size(); k++) {
      if (choices[k] != c) continue;
      int i = c->value();
      if (i < 0 || i >= (int)candidates[k].size()) return;
      for (auto &m : kinds()[k].mimes) set_default_app_for_mime(m, candidates[k][i].id);
    }
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new DefaultAppsPage(x, y, w, h); }

} // namespace

PageInfo page_defaultapps() {
  return {"defaultapps", _("Default Applications"), "preferences-desktop-default-applications", "Personal",
          _("default applications browser mail email file manager terminal editor image viewer music video pdf open with"),
          create};
}

} // namespace fld
