// Keyboard: layouts, switching key, compose, caps lock, repeat
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Value_Slider.H>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"
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
      {"Switch layout with:", "grp",
       {{"", "(none)"}, {"grp:win_space_toggle", "Super+Space"}, {"grp:alt_shift_toggle", "Alt+Shift"},
        {"grp:ctrl_shift_toggle", "Ctrl+Shift"}, {"grp:alt_space_toggle", "Alt+Space"}, {"grp:caps_toggle", "Caps Lock"}}},
      {"Compose key:", "compose",
       {{"", "(none)"}, {"compose:ralt", "Right Alt"}, {"compose:rwin", "Right Super"}, {"compose:menu", "Menu"},
        {"compose:rctrl", "Right Ctrl"}, {"compose:caps", "Caps Lock"}}},
      {"Caps Lock key:", "caps",
       {{"", "Caps Lock"}, {"ctrl:nocaps", "Ctrl"}, {"caps:escape", "Escape"}, {"caps:backspace", "Backspace"},
        {"caps:none", "Disabled"}}},
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
    Fl_Group *g = page_group(X, Y, W, H, "Keyboard", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    layouts = xkb::split_layouts(wm.ini.get("keyboard", "layout", ""), wm.ini.get("keyboard", "variant", ""));
    if (layouts.empty()) layouts.push_back({"us", ""});
    f.heading("Layouts");
    f.row("", 130, wx, wy, ww);
    list = new Fl_Hold_Browser(wx - 200, wy, ww + 200 - 110, 130);
    list->textsize(13);
    Fl_Button *add = new Fl_Button(wx + ww - 100, wy, 100, 28, "Add…");
    add->callback([](Fl_Widget *, void *d) { ((KeyboardPage *)d)->add_layout(); }, this);
    Fl_Button *rm = new Fl_Button(wx + ww - 100, wy + 34, 100, 28, "Remove");
    rm->callback([](Fl_Widget *, void *d) {
      KeyboardPage *p = (KeyboardPage *)d;
      int i = p->list->value() - 1;
      if (i < 0 || p->layouts.size() <= 1) return;
      p->layouts.erase(p->layouts.begin() + i);
      p->fill();
      p->save();
    }, this);
    Fl_Button *up = new Fl_Button(wx + ww - 100, wy + 68, 100, 28, "Move Up");
    up->callback([](Fl_Widget *, void *d) {
      KeyboardPage *p = (KeyboardPage *)d;
      int i = p->list->value() - 1;
      if (i < 1) return;
      std::swap(p->layouts[i], p->layouts[i - 1]);
      p->fill();
      p->list->value(i);
      p->save();
    }, this);
    f.note("The first layout is used when the session starts.");

    f.heading("Keys");
    std::vector<std::string> cur_opts = split(wm.ini.get("keyboard", "options", ""), ',');
    for (auto &oc : opt_choices()) {
      f.row(oc.label, 28, wx, wy, ww);
      Fl_Choice *c = new Fl_Choice(wx, wy, 260, 28);
      int sel = 0;
      for (size_t i = 0; i < oc.items.size(); i++) {
        c->add(oc.items[i].second);
        for (auto &o : cur_opts) if (o == oc.items[i].first) sel = (int)i;
      }
      c->value(sel);
      c->callback([](Fl_Widget *, void *d) { ((KeyboardPage *)d)->save(); }, this);
      opts.push_back(c);
    }
    f.heading("Repeat");
    f.row("Delay (ms):", 26, wx, wy, ww);
    delay = new Fl_Value_Slider(wx, wy, ww, 26);
    delay->type(FL_HOR_NICE_SLIDER);
    delay->bounds(150, 1000);
    delay->step(10);
    delay->value(wm.ini.get_int("keyboard", "repeat_delay", 600));
    delay->callback([](Fl_Widget *, void *d) { ((KeyboardPage *)d)->save(); }, this);
    delay->when(FL_WHEN_RELEASE);
    f.row("Speed (per s):", 26, wx, wy, ww);
    rate = new Fl_Value_Slider(wx, wy, ww, 26);
    rate->type(FL_HOR_NICE_SLIDER);
    rate->bounds(5, 60);
    rate->step(1);
    rate->value(wm.ini.get_int("keyboard", "repeat_rate", 25));
    rate->callback([](Fl_Widget *, void *d) { ((KeyboardPage *)d)->save(); }, this);
    rate->when(FL_WHEN_RELEASE);
    f.row("Test here:", 28, wx, wy, ww);
    new Fl_Input(wx, wy, ww, 28);
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
    Fl_Choice *variant = new Fl_Choice(90, 336, 360, 28, "Variant:");
    Fl_Button *cancel = new Fl_Button(240, 440, 100, 30, "Cancel");
    Fl_Return_Button *ok = new Fl_Return_Button(350, 440, 100, 30, "Add");
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
      c->variant->add("(default)");
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
  return {"keyboard", "Keyboard", "input-keyboard", "Hardware",
          "keyboard layout language input xkb compose caps lock repeat delay rate switch", create};
}

} // namespace fld
