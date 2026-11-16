// Language & Region: LANG and the formats locale (written to the session environment)
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "locale_env.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

std::string env_path() { return fld_config_dir() + "/environment"; }

class LanguagePage : public Fl_Group {
public:
  std::vector<locale_env::LocaleInfo> locales;
  Fl_Choice *lang, *formats;
  Fl_Box *preview;
  std::string preview_text;

  LanguagePage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Language & Region", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    locales = locale_env::installed_locales();
    locale_env::EnvFile env;
    env.load(env_path());
    std::string cur_lang = env.get("LANG");
    if (cur_lang.empty()) cur_lang = getenv_or("LANG", "C.UTF-8");
    std::string cur_fmt = env.get("LC_TIME");
    if (cur_fmt.empty()) cur_fmt = getenv_or("LC_TIME", cur_lang);
    cur_lang = locale_env::normalize(cur_lang);
    cur_fmt = locale_env::normalize(cur_fmt);
    f.heading("Language");
    f.row("Language:", 28, wx, wy, ww);
    lang = new Fl_Choice(wx, wy, ww, 28);
    f.row("Formats:", 28, wx, wy, ww);
    formats = new Fl_Choice(wx, wy, ww, 28);
    for (size_t i = 0; i < locales.size(); i++) {
      std::string label = escape_at(locales[i].description + "  —  " + locales[i].name);
      std::string e;
      for (char c : label) { if (c == '/' || c == '\\' || c == '&' || c == '_') e += '\\'; e += c; }
      lang->add(e.c_str());
      formats->add(e.c_str());
      if (locales[i].name == cur_lang) lang->value((int)i);
      if (locales[i].name == cur_fmt) formats->value((int)i);
    }
    lang->callback([](Fl_Widget *, void *d) { ((LanguagePage *)d)->save(); }, this);
    formats->callback([](Fl_Widget *, void *d) { ((LanguagePage *)d)->save(); }, this);
    f.note("Formats are used for dates, times, numbers, currency and paper size.", 1);
    f.heading("Preview");
    f.row("", 70, wx, wy, ww);
    preview = new Fl_Box(wx, wy, ww, 70);
    preview->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP);
    preview->labelfont(FL_COURIER);
    f.note("Changes take effect the next time you log in.", 1);
    if (locales.empty()) f.note("No UTF-8 locales are installed (see locale-gen or the locales package).", 2);
    g->end();
    end();
    update_preview();
  }

  std::string selected(Fl_Choice *c) const {
    int i = c->value();
    return i >= 0 && i < (int)locales.size() ? locales[i].name : "";
  }

  void update_preview() {
    std::string fmt = selected(formats);
    preview_text = escape_at(fmt.empty() ? "" : locale_env::preview(fmt));
    preview->label(preview_text.c_str());
    preview->redraw();
  }

  void save() {
    locale_env::EnvFile env;
    env.load(env_path());
    std::string l = selected(lang), fmt = selected(formats);
    if (!l.empty()) env.set("LANG", l);
    for (const char *k : {"LC_TIME", "LC_NUMERIC", "LC_MONETARY", "LC_PAPER", "LC_MEASUREMENT"})
      env.set(k, fmt == l ? "" : fmt);
    env.save(env_path());
    update_preview();
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new LanguagePage(x, y, w, h); }

} // namespace

PageInfo page_language() {
  return {"language", "Language & Region", "preferences-desktop-locale", "Personal",
          "language locale region formats date time currency numbers translation", create};
}

} // namespace fld
