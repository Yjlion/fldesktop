// Keyboard: layouts, switching key, compose, caps lock, repeat
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <unistd.h>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Value_Slider.H>

#include "fld/menus.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"
#include "locale_env.h"
#include "xkb_rules.h"

namespace fld {
namespace {

struct OptChoice {
  const char *label;
  const char *group;            // option group, e.g. "grp"
  std::vector<std::pair<const char *, const char *>> items; // option, label
};

const std::vector<OptChoice> &opt_choices() {
  static const std::vector<OptChoice> o = {
      {N_("Switch layout with:"), "grp",
       {{"", N_("(none)")}, {"grp:win_space_toggle", "Super+Space"}, {"grp:alt_shift_toggle", "Alt+Shift"},
        {"grp:ctrl_shift_toggle", "Ctrl+Shift"}, {"grp:alt_space_toggle", "Alt+Space"}, {"grp:caps_toggle", N_("Caps Lock")}}},
      {N_("Compose key:"), "compose",
       {{"", N_("(none)")}, {"compose:ralt", N_("Right Alt")}, {"compose:rwin", N_("Right Super")}, {"compose:menu", N_("Menu")},
        {"compose:rctrl", N_("Right Ctrl")}, {"compose:caps", N_("Caps Lock")}}},
      {N_("Caps Lock key:"), "caps",
       {{"", N_("Caps Lock")}, {"ctrl:nocaps", "Ctrl"}, {"caps:escape", N_("Escape")}, {"caps:backspace", N_("Backspace")},
        {"caps:none", N_("Disabled")}}},
  };
  return o;
}

class KeyboardPage : public Fl_Group {
public:
  ConfigFile wm{wm_ini_path()};
  xkb::Rules rules;
  std::vector<std::pair<std::string, std::string>> layouts;
  Fl_Hold_Browser *list;
  std::vector<Fl_Choice *> opts;
  Fl_Value_Slider *delay, *rate;

  KeyboardPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    rules.load();
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Keyboard"), cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    layouts = xkb::split_layouts(wm.ini.get("keyboard", "layout", ""), wm.ini.get("keyboard", "variant", ""));
    if (layouts.empty()) layouts.push_back({"us", ""});
    f.heading(_("Layouts"));
    f.row("", 130, wx, wy, ww);
    list = new Fl_Hold_Browser(wx - 200, wy, ww + 200 - 110, 130);
    list->textsize(13);
    Fl_Button *add = new Fl_Button(wx + ww - 100, wy, 100, 28, _("Add…"));
    add->callback([](Fl_Widget *, void *d) { ((KeyboardPage *)d)->add_layout(); }, this);
    Fl_Button *rm = new Fl_Button(wx + ww - 100, wy + 34, 100, 28, _("Remove"));
    rm->callback([](Fl_Widget *, void *d) {
      KeyboardPage *p = (KeyboardPage *)d;
      int i = p->list->value() - 1;
      if (i < 0 || p->layouts.size() <= 1) return;
      p->layouts.erase(p->layouts.begin() + i);
      p->fill();
      p->save();
    }, this);
    Fl_Button *up = new Fl_Button(wx + ww - 100, wy + 68, 100, 28, _("Move Up"));
    fld::set_context_menu(list, [add, rm, up](fld::Menu &m) { m.button(add).button(rm).divider().button(up); });
    up->callback([](Fl_Widget *, void *d) {
      KeyboardPage *p = (KeyboardPage *)d;
      int i = p->list->value() - 1;
      if (i < 1) return;
      std::swap(p->layouts[i], p->layouts[i - 1]);
      p->fill();
      p->list->value(i);
      p->save();
    }, this);
    f.note(_("The first layout is used when the session starts."));

    f.heading(_("Keys"));
    std::vector<std::string> cur_opts = split(wm.ini.get("keyboard", "options", ""), ',');
    for (auto &oc : opt_choices()) {
      f.row(_(oc.label), 28, wx, wy, ww);
      Fl_Choice *c = new Fl_Choice(wx, wy, 260, 28);
      int sel = 0;
      for (size_t i = 0; i < oc.items.size(); i++) {
        c->add(_(oc.items[i].second));
        for (auto &o : cur_opts) if (o == oc.items[i].first) sel = (int)i;
      }
      c->value(sel);
      c->callback([](Fl_Widget *, void *d) { ((KeyboardPage *)d)->save(); }, this);
      opts.push_back(c);
    }
    f.heading(_("Repeat"));
    f.row(_("Delay (ms):"), 26, wx, wy, ww);
    delay = new Fl_Value_Slider(wx, wy, ww, 26);
    delay->type(FL_HOR_NICE_SLIDER);
    delay->bounds(150, 1000);
    delay->step(10);
    delay->value(wm.ini.get_int("keyboard", "repeat_delay", 600));
    delay->callback([](Fl_Widget *, void *d) { ((KeyboardPage *)d)->save(); }, this);
    delay->when(FL_WHEN_RELEASE);
    f.row(_("Speed (per s):"), 26, wx, wy, ww);
    rate = new Fl_Value_Slider(wx, wy, ww, 26);
    rate->type(FL_HOR_NICE_SLIDER);
    rate->bounds(5, 60);
    rate->step(1);
    rate->value(wm.ini.get_int("keyboard", "repeat_rate", 25));
    rate->callback([](Fl_Widget *, void *d) { ((KeyboardPage *)d)->save(); }, this);
    rate->when(FL_WHEN_RELEASE);
    f.heading(_("Input Method"));
    f.row(_("Input method:"), 28, wx, wy, ww);
    im_choice = new Fl_Choice(wx, wy, 220, 28);
    {
      locale_env::EnvFile env;
      env.load(env_path());
      std::string cur = locale_env::input_method_from_env(env);
      for (size_t i = 0; i < kInputMethods.size(); i++) {
        const auto &m = kInputMethods[i];
        bool have = !m.binary || !which(m.binary).empty();
        im_choice->add(m.label, 0, nullptr, nullptr, have ? 0 : FL_MENU_INACTIVE);
        if (cur == m.id) im_choice->value((int)i);
      }
      if (im_choice->value() < 0) im_choice->value(0);
    }
    im_choice->callback([](Fl_Widget *, void *d) { ((KeyboardPage *)d)->save_im(); }, this);
    f.note(_("For Chinese, Japanese, Korean and other composed input. Takes effect at the next login."), 2);
    f.row(_("Test here:"), 28, wx, wy, ww);
    new Fl_Input(wx, wy, ww, 28);
    f.space(8);
    reset_page_button(f.widget_x(), f.y(), {{wm_ini_path(), {{"keyboard", "repeat_delay"}, {"keyboard", "repeat_rate"}, {"keyboard", "options"}}}});
    g->end();
    end();
    fill();
  }

  void fill() {
    list->clear();
    for (size_t i = 0; i < layouts.size(); i++) {
      std::string d = rules.layout_description(layouts[i].first, layouts[i].second);
      std::string code = layouts[i].first + (layouts[i].second.empty() ? "" : "(" + layouts[i].second + ")");
      list->add(escape_at(d + "   [" + code + "]").c_str());
    }
    list->value(1);
  }

  void add_layout() {
    Fl_Double_Window w(460, 480, "Add Keyboard Layout");
    Fl_Input *search = new Fl_Input(10, 10, 440, 28);
    search->when(FL_WHEN_CHANGED);
    Fl_Hold_Browser *lb = new Fl_Hold_Browser(10, 46, 440, 280);
    Fl_Choice *variant = new Fl_Choice(90, 336, 360, 28, _("Variant:"));
    Fl_Button *cancel = new Fl_Button(240, 440, 100, 30, _("Cancel"));
    Fl_Return_Button *ok = new Fl_Return_Button(350, 440, 100, 30, _("Add"));
    std::vector<const xkb::Item *> shown;
    struct Ctx { xkb::Rules *rules; Fl_Input *search; Fl_Hold_Browser *lb; Fl_Choice *variant; std::vector<const xkb::Item *> *shown; int result; };
    Ctx ctx{&rules, search, lb, variant, &shown, 0};
    auto refill = [](Fl_Widget *, void *d) {
      Ctx *c = (Ctx *)d;
      std::string q = lower(c->search->value());
      c->lb->clear();
      c->shown->clear();
      for (auto &l : c->rules->layouts)
        if (q.empty() || lower(l.description).find(q) != std::string::npos || lower(l.name) == q) {
          c->shown->push_back(&l);
          c->lb->add(escape_at(l.description + "  [" + l.name + "]").c_str());
        }
    };
    search->callback(refill, &ctx);
    lb->callback([](Fl_Widget *, void *d) {
      Ctx *c = (Ctx *)d;
      c->variant->clear();
      c->variant->add(_("(default)"));
      int i = c->lb->value() - 1;
      if (i >= 0 && i < (int)c->shown->size()) {
        auto it = c->rules->variants.find((*c->shown)[i]->name);
        if (it != c->rules->variants.end())
          for (auto &v : it->second) {
            std::string e;
            for (char ch : escape_at(v.description)) { if (ch == '/' || ch == '\\' || ch == '&' || ch == '_') e += '\\'; e += ch; }
            c->variant->add(e.c_str());
          }
      }
      c->variant->value(0);
      if (Fl::event_clicks()) { c->result = 1; c->lb->window()->hide(); }
    }, &ctx);
    cancel->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
    ok->callback([](Fl_Widget *b, void *d) { ((Ctx *)d)->result = 1; b->window()->hide(); }, &ctx);
    refill(nullptr, &ctx);
    w.end();
    w.set_modal();
    w.show();
    while (w.shown()) Fl::wait();
    int i = lb->value() - 1;
    if (!ctx.result || i < 0 || i >= (int)shown.size()) return;
    std::string var;
    int vi = variant->value();
    auto it = rules.variants.find(shown[i]->name);
    if (vi > 0 && it != rules.variants.end() && vi - 1 < (int)it->second.size()) var = it->second[vi - 1].name;
    layouts.push_back({shown[i]->name, var});
    fill();
    save();
  }

  struct InputMethod {
    const char *id, *label, *binary;
  };
  const std::vector<InputMethod> kInputMethods = {
      {"", "None", nullptr}, {"fcitx5", "Fcitx 5", "fcitx5"}, {"ibus", "IBus", "ibus-daemon"}};
  Fl_Choice *im_choice = nullptr;

  static std::string env_path() { return fld_config_dir() + "/environment"; }
  static std::string im_autostart_path() { return config_home() + "/autostart/fl-input-method.desktop"; }

  // the session environment and an autostart entry for the chosen input method
  void save_im() {
    int i = std::max(0, im_choice->value());
    std::string id = kInputMethods[(size_t)i].id;
    locale_env::EnvFile env;
    env.load(env_path());
    locale_env::set_input_method(env, id);
    env.save(env_path());
    std::string cmd = locale_env::input_method_command(id);
    if (cmd.empty()) {
      unlink(im_autostart_path().c_str());
      return;
    }
    mkdir_p(config_home() + "/autostart");
    write_file(im_autostart_path(), "[Desktop Entry]\nType=Application\nName=Input Method\n"
                                    "Comment=Started by fldesktop Settings (Keyboard)\nExec=" + cmd +
                                    "\nOnlyShowIn=fldesktop;\nNoDisplay=true\n");
  }

  void save() {
    wm.ini.load(wm.path);
    std::string l, v;
    xkb::join_layouts(layouts, l, v);
    wm.ini.set("keyboard", "layout", l);
    wm.ini.set("keyboard", "variant", v);
    // keep options we do not manage, replace ours
    std::vector<std::string> keep;
    for (auto &o : split(wm.ini.get("keyboard", "options", ""), ',')) {
      bool ours = false;
      for (auto &oc : opt_choices()) if (starts_with(o, std::string(oc.group) + ":") || (std::string(oc.group) == "caps" && starts_with(o, "ctrl:nocaps"))) ours = true;
      if (!ours) keep.push_back(o);
    }
    for (size_t k = 0; k < opts.size(); k++) {
      int i = opts[k]->value();
      const char *o = opt_choices()[k].items[std::max(0, i)].first;
      if (*o) keep.push_back(o);
    }
    wm.ini.set("keyboard", "options", join(keep, ","));
    wm.ini.set_int("keyboard", "repeat_delay", (int)delay->value());
    wm.ini.set_int("keyboard", "repeat_rate", (int)rate->value());
    save_wm(wm);
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new KeyboardPage(x, y, w, h); }

} // namespace

PageInfo page_keyboard() {
  return {"keyboard", _("Keyboard"), "input-keyboard", "Hardware",
          _("keyboard layout language input xkb compose caps lock repeat delay rate switch input method ime fcitx ibus chinese japanese korean"), create};
}

} // namespace fld
