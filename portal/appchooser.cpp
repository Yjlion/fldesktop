// org.freedesktop.impl.portal.AppChooser: "Open with" dialog
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Return_Button.H>

#include <algorithm>
#include <map>
#include <memory>

#include "fld/desktop_entry.h"
#include "fld/glib_bridge.h"
#include "fld/icons.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "portal.h"

namespace portal {

namespace {

class ChooserWindow;
std::map<std::string, ChooserWindow *> g_open; // handle -> window (FLTK thread)

class ChooserWindow : public Fl_Double_Window {
public:
  GDBusMethodInvocation *inv;
  std::string handle;
  std::vector<std::string> ids; // desktop ids without .desktop
  Fl_Hold_Browser *list;
  Fl_Box *title;
  bool done = false;
  std::string last;

  ChooserWindow(GDBusMethodInvocation *i, const std::string &h, const std::string &what, const std::string &last_choice)
      : Fl_Double_Window(420, 440, "Open With"), inv(i), handle(h), last(last_choice) {
    title = new Fl_Box(16, 12, 388, 44);
    title->copy_label(fld::escape_at(what).c_str());
    title->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
    title->labelfont(FL_HELVETICA_BOLD);
    list = new Fl_Hold_Browser(16, 62, 388, 318);
    list->format_char(0);
    list->textsize(14);
    list->callback([](Fl_Widget *, void *d) {
      ChooserWindow *w = (ChooserWindow *)d;
      if (Fl::event_clicks() > 0) {
        Fl::event_clicks(0);
        w->finish(true);
      }
    }, this);
    Fl_Button *cancel = new Fl_Button(188, 396, 104, 30, "Cancel");
    cancel->callback([](Fl_Widget *w, void *) { ((ChooserWindow *)w->window())->finish(false); });
    Fl_Return_Button *ok = new Fl_Return_Button(300, 396, 104, 30, "Open");
    ok->callback([](Fl_Widget *w, void *) { ((ChooserWindow *)w->window())->finish(true); });
    end();
    resizable(list);
    callback([](Fl_Widget *w, void *) { ((ChooserWindow *)w)->finish(false); });
    fld::set_window_icon(this, "system-run");
  }

  void set_choices(const std::vector<std::string> &choices) {
    std::string sel = list->value() > 0 && list->value() <= (int)ids.size() ? ids[list->value() - 1] : last;
    ids.clear();
    list->clear();
    for (auto &c : choices) {
      fld::DesktopEntry e;
      if (!fld::find_application(c, e)) continue;
      std::string id = c;
      if (fld::ends_with(id, ".desktop")) id = id.substr(0, id.size() - 8);
      if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
      ids.push_back(id);
      list->add(e.name.c_str());
      list->icon(list->size(), fld::icon_first({e.icon, "application-x-executable"}, 24));
      if (id == sel) list->value(list->size());
    }
    if (!list->value() && list->size()) list->value(1);
    redraw();
  }

  void finish(bool ok) {
    if (done) return;
    done = true;
    hide();
    g_open.erase(handle);
    request_end(handle);
    int i = list->value() - 1;
    if (ok && i >= 0 && i < (int)ids.size()) {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&b, "{sv}", "choice", g_variant_new_string(ids[i].c_str()));
      reply(inv, SUCCESS, g_variant_builder_end(&b));
    } else {
      reply(inv, CANCELLED);
    }
    Fl::delete_widget(this);
  }
};

std::vector<std::string> strv(GVariant *v) {
  std::vector<std::string> out;
  GVariantIter it;
  g_variant_iter_init(&it, v);
  const char *s;
  while (g_variant_iter_next(&it, "&s", &s)) out.push_back(s);
  return out;
}

} // namespace

void appchooser_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  if (method == "UpdateChoices") {
    const char *handle;
    GVariant *choices;
    g_variant_get(params, "(&o@as)", &handle, &choices);
    std::vector<std::string> c = strv(choices);
    g_variant_unref(choices);
    std::string h = handle;
    fld::ui_call([h, c] {
      auto it = g_open.find(h);
      if (it != g_open.end()) it->second->set_choices(c);
    });
    g_dbus_method_invocation_return_value(inv, nullptr);
    return;
  }
  if (method != "ChooseApplication") {
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method.c_str());
    return;
  }
  const char *handle, *app_id, *parent;
  GVariant *choices, *options;
  g_variant_get(params, "(&o&s&s@as@a{sv})", &handle, &app_id, &parent, &choices, &options);
  std::vector<std::string> c = strv(choices);
  std::string last = opt_string(options, "last_choice");
  std::string type = opt_string(options, "content_type");
  std::string file = opt_string(options, "filename");
  std::string uri = opt_string(options, "uri");
  g_variant_unref(choices);
  g_variant_unref(options);
  std::string what;
  if (!file.empty()) what = "Open \"" + file + "\" with:";
  else if (!uri.empty()) what = "Open " + uri + " with:";
  else if (!type.empty()) what = "Open " + fld::mime_description(type) + " with:";
  else what = "Choose an application:";
  std::string h = handle;
  fld::ui_call([inv, h, what, last, c] {
    ChooserWindow *w = new ChooserWindow(inv, h, what, last);
    w->set_choices(c);
    g_open[h] = w;
    w->show();
    request_begin(h, [h] {
      auto it = g_open.find(h);
      if (it != g_open.end()) it->second->finish(false);
    });
  });
}

} // namespace portal
