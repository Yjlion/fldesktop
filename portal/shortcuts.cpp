// org.freedesktop.impl.portal.GlobalShortcuts: key combinations that reach
// an application whichever window has the focus.
//
// BindShortcuts asks the user once which keys to use (the application's
// preferred ones filled in) and stores them in global-shortcuts.ini; fl-wm
// binds them and, when one is pressed, runs "xdg-desktop-portal-fl
// --activate-shortcut <app> <id>", which calls our
// org.fldesktop.GlobalShortcuts.Activate; we then send Activated and
// Deactivated to the application's sessions. The Shortcuts page of Settings
// lists and removes them.
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scroll.H>

#include <chrono>
#include <map>
#include <memory>

#include "fld/glib_bridge.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "portal.h"

namespace portal {

namespace {

const char *kSessionXml = R"XML(
<node>
  <interface name="org.freedesktop.impl.portal.Session">
    <method name="Close"/>
    <signal name="Closed"/>
    <property name="version" type="u" access="read"/>
  </interface>
</node>
)XML";

struct Session {
  std::string app_id;
  guint reg = 0;
};
std::map<std::string, Session> g_sessions; // GLib thread

void session_method(GDBusConnection *, const char *, const char *path, const char *, const char *, GVariant *,
                    GDBusMethodInvocation *inv, gpointer) {
  g_dbus_method_invocation_return_value(inv, nullptr);
  auto it = g_sessions.find(path);
  if (it == g_sessions.end()) return;
  g_dbus_connection_unregister_object(connection(), it->second.reg);
  g_sessions.erase(it);
}

GVariant *session_property(GDBusConnection *, const char *, const char *, const char *, const char *, GError **,
                           gpointer) {
  return g_variant_new_uint32(1);
}

struct Wanted {
  std::string id, description, preferred;
};

GVariant *shortcuts_variant(const std::vector<StoredShortcut> &list) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a(sa{sv})"));
  for (const StoredShortcut &s : list) {
    GVariantBuilder d;
    g_variant_builder_init(&d, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&d, "{sv}", "description", g_variant_new_string(s.description.c_str()));
    g_variant_builder_add(&d, "{sv}", "trigger_description", g_variant_new_string(s.combo.c_str()));
    g_variant_builder_add(&b, "(s@a{sv})", s.id.c_str(), g_variant_builder_end(&d));
  }
  return g_variant_builder_end(&b);
}

GVariant *results_with(const std::vector<StoredShortcut> &list) {
  GVariantBuilder r;
  g_variant_builder_init(&r, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&r, "{sv}", "shortcuts", shortcuts_variant(list));
  return g_variant_builder_end(&r);
}

// Asks which keys to use. FLTK thread.
class BindWindow : public Fl_Double_Window {
public:
  std::vector<Wanted> wanted;
  std::vector<Fl_Input *> inputs;
  std::function<void(bool, std::vector<StoredShortcut>)> done;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
  std::string handle;
  bool finished = false;

  BindWindow(const std::string &hdl, const std::string &app, const std::vector<Wanted> &w,
             const std::vector<StoredShortcut> &old)
      : Fl_Double_Window(480, 150 + 34 * (int)std::min<size_t>(w.size(), 8), "Keyboard Shortcuts"), wanted(w),
        handle(hdl) {
    Fl_Box *t = new Fl_Box(16, 12, 448, 26);
    t->copy_label(fld::escape_at(app + " wants global keyboard shortcuts").c_str());
    t->labelfont(FL_HELVETICA_BOLD);
    t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    Fl_Box *n = new Fl_Box(16, 38, 448, 40,
                           "They work in every window. Change or clear a key combination (for example "
                           "Ctrl+Alt+K); an empty one is not bound.");
    n->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
    n->labelsize(12);
    n->labelcolor(fld::palette().fg_dim);
    int listh = 34 * (int)std::min<size_t>(w.size(), 8);
    Fl_Scroll *sc = new Fl_Scroll(16, 84, 448, listh);
    sc->type(Fl_Scroll::VERTICAL);
    int y = 84;
    for (const Wanted &x : wanted) {
      Fl_Box *d = new Fl_Box(16, y, 250, 30);
      d->copy_label(fld::escape_at(x.description.empty() ? x.id : x.description).c_str());
      d->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
      Fl_Input *in = new Fl_Input(270, y + 2, 180, 26);
      std::string combo = trigger_to_combo(x.preferred);
      for (const StoredShortcut &o : old)
        if (o.id == x.id) combo = o.combo;
      in->value(combo.c_str());
      inputs.push_back(in);
      y += 34;
    }
    sc->end();
    Fl_Button *c = new Fl_Button(240, h() - 44, 110, 30, "Cancel");
    c->callback([](Fl_Widget *b, void *) { ((BindWindow *)b->window())->finish(false); });
    Fl_Return_Button *ok = new Fl_Return_Button(358, h() - 44, 106, 30, "Allow");
    ok->callback([](Fl_Widget *b, void *) { ((BindWindow *)b->window())->finish(true); });
    end();
    callback([](Fl_Widget *w, void *) { ((BindWindow *)w)->finish(false); });
    fld::set_window_icon(this, "preferences-desktop-keyboard-shortcuts");
  }

  void finish(bool accepted) {
    if (finished) return;
    finished = true;
    *alive = false;
    std::vector<StoredShortcut> out;
    for (size_t i = 0; i < wanted.size(); i++) {
      StoredShortcut s;
      s.id = wanted[i].id;
      s.description = wanted[i].description;
      s.combo = trigger_to_combo(inputs[i]->value());
      out.push_back(s);
    }
    request_end(handle);
    hide();
    done(accepted, out);
    Fl::delete_widget(this);
  }
};

void emit(const std::string &session, const char *signal, const std::string &id) {
  guint64 now = (guint64)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  g_dbus_connection_emit_signal(connection(), nullptr, "/org/freedesktop/portal/desktop",
                                "org.freedesktop.impl.portal.GlobalShortcuts", signal,
                                g_variant_new("(ost@a{sv})", session.c_str(), id.c_str(), now,
                                              g_variant_new("a{sv}", nullptr)),
                                nullptr);
}

} // namespace

void shortcuts_activate(const std::string &app_id, const std::string &id) {
  for (auto &s : g_sessions)
    if (s.second.app_id == app_id) {
      emit(s.first, "Activated", id);
      emit(s.first, "Deactivated", id); // fl-wm only knows the press
    }
}

void shortcuts_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  if (method == "CreateSession") {
    const char *handle, *session, *app_id;
    GVariant *options;
    g_variant_get(params, "(&o&o&s@a{sv})", &handle, &session, &app_id, &options);
    g_variant_unref(options);
    static GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(kSessionXml, nullptr);
    static const GDBusInterfaceVTable vt = {session_method, session_property, nullptr, {}};
    guint reg = g_dbus_connection_register_object(connection(), session, info->interfaces[0], &vt, nullptr, nullptr,
                                                  nullptr);
    if (!reg) {
      reply(inv, OTHER);
      return;
    }
    g_sessions[session] = {app_id, reg};
    reply(inv, SUCCESS);
    return;
  }
  if (method == "ListShortcuts") {
    const char *handle, *session;
    g_variant_get(params, "(&o&o)", &handle, &session);
    auto it = g_sessions.find(session);
    if (it == g_sessions.end()) {
      reply(inv, OTHER);
      return;
    }
    reply(inv, SUCCESS, results_with(shortcuts_load(shortcuts_path(), it->second.app_id)));
    return;
  }
  if (method == "BindShortcuts") {
    const char *handle, *session, *parent;
    GVariant *list, *options;
    g_variant_get(params, "(&o&o@a(sa{sv})&s@a{sv})", &handle, &session, &list, &parent, &options);
    g_variant_unref(options);
    auto it = g_sessions.find(session);
    if (it == g_sessions.end()) {
      g_variant_unref(list);
      reply(inv, OTHER);
      return;
    }
    std::vector<Wanted> wanted;
    GVariantIter iter;
    g_variant_iter_init(&iter, list);
    const char *id;
    GVariant *props;
    while (g_variant_iter_next(&iter, "(&s@a{sv})", &id, &props)) {
      Wanted w;
      w.id = id;
      w.description = opt_string(props, "description");
      w.preferred = opt_string(props, "preferred_trigger");
      wanted.push_back(w);
      g_variant_unref(props);
    }
    g_variant_unref(list);
    std::string app_id = it->second.app_id;
    std::vector<StoredShortcut> old = shortcuts_load(shortcuts_path(), app_id);
    // asked before for exactly these: no question again
    bool known = !old.empty() && old.size() == wanted.size();
    for (const Wanted &w : wanted) {
      bool found = false;
      for (const StoredShortcut &o : old)
        if (o.id == w.id) found = true;
      known = known && found;
    }
    if (known) {
      reply(inv, SUCCESS, results_with(old));
      return;
    }
    std::string h = handle, app = app_name(app_id);
    fld::ui_call([inv, h, app, app_id, wanted, old] {
      BindWindow *w = new BindWindow(h, app, wanted, old);
      w->done = [inv, app_id](bool ok, std::vector<StoredShortcut> list) {
        if (!ok) {
          reply(inv, CANCELLED);
          return;
        }
        shortcuts_save(shortcuts_path(), app_id, list);
        fld::reload_wm();
        reply(inv, SUCCESS, results_with(list));
      };
      auto alive = w->alive;
      w->show();
      request_begin(h, [w, alive] {
        if (*alive) w->finish(false);
      });
    });
    return;
  }
  g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method.c_str());
}

} // namespace portal
