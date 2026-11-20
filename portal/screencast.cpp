// org.freedesktop.impl.portal.ScreenCast: screen and window sharing
//
// CreateSession exports an org.freedesktop.impl.portal.Session; Start asks
// which screen or window to share (or takes the one remembered in
// restore_data) and answers with PipeWire nodes made by cast_capture.c.
// Session state lives on the GLib thread; the picker on the FLTK thread.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Multi_Browser.H>
#include <FL/Fl_Return_Button.H>

#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "cast_capture.h"
#include "fld/glib_bridge.h"
#include "fld/theme.h"
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

const char *kSessionIface = "org.freedesktop.impl.portal.Session";

// A running stream and the session path its closed callback reports.
struct Running {
  flcast_stream *stream = nullptr;
  std::string *path = nullptr; // owned; outlives the capture thread
};

struct Session {
  std::string path, app_id;
  guint reg = 0;
  guint32 types = CAST_MONITOR, cursor_mode = CAST_CURSOR_HIDDEN, persist_mode = 0;
  bool multiple = false, starting = false;
  std::vector<CastSource> restore;
  std::vector<Running> streams;
};

// GLib thread only
std::map<std::string, std::unique_ptr<Session>> g_sessions;
GDBusNodeInfo *g_session_info = nullptr;

void stop_streams(std::vector<Running> streams) {
  if (streams.empty()) return;
  // joining the capture threads takes a moment: not on the D-Bus thread
  std::thread([streams] {
    for (const Running &r : streams) {
      flcast_stream_stop(r.stream);
      delete r.path;
    }
  }).detach();
}

void close_session(const std::string &path, bool emit) {
  auto it = g_sessions.find(path);
  if (it == g_sessions.end()) return;
  std::unique_ptr<Session> s = std::move(it->second);
  g_sessions.erase(it);
  if (emit)
    g_dbus_connection_emit_signal(connection(), nullptr, path.c_str(), kSessionIface, "Closed", nullptr, nullptr);
  if (s->reg) g_dbus_connection_unregister_object(connection(), s->reg);
  stop_streams(std::move(s->streams));
}

Session *find_session(const std::string &path) {
  auto it = g_sessions.find(path);
  return it == g_sessions.end() ? nullptr : it->second.get();
}

void session_method(GDBusConnection *, const char *, const char *path, const char *, const char *method, GVariant *,
                    GDBusMethodInvocation *inv, gpointer) {
  if (std::string(method) == "Close") close_session(path, false);
  g_dbus_method_invocation_return_value(inv, nullptr);
}

GVariant *session_property(GDBusConnection *, const char *, const char *, const char *, const char *prop, GError **,
                           gpointer) {
  if (std::string(prop) == "version") return g_variant_new_uint32(1);
  return nullptr;
}

// capture thread: the shared screen or window went away
void on_stream_closed(void *data) {
  std::string path = *(std::string *)data;
  fld::glib_call([path] { close_session(path, true); });
}

std::vector<CastSource> list_sources(guint32 types, bool *ok) {
  std::vector<CastSource> out;
  flcast_sources s;
  *ok = flcast_list_sources(&s) == 0;
  if (!*ok) return out;
  if (types & s.types & CAST_MONITOR)
    for (int i = 0; i < s.n_outputs; i++)
      out.push_back({CAST_MONITOR, s.outputs[i].name, s.outputs[i].desc, ""});
  if (types & s.types & CAST_WINDOW)
    for (int i = 0; i < s.n_windows; i++)
      out.push_back({CAST_WINDOW, s.windows[i].id, s.windows[i].title, s.windows[i].app_id});
  flcast_sources_free(&s);
  return out;
}

// worker thread: starts the streams and answers Start
void start_streams(GDBusMethodInvocation *inv, const std::string &path, const std::vector<CastSource> &chosen,
                   bool cursor, guint32 persist_mode) {
  std::vector<Running> running;
  GVariantBuilder sb;
  g_variant_builder_init(&sb, G_VARIANT_TYPE("a(ua{sv})"));
  for (const CastSource &c : chosen) {
    char err[256] = "";
    Running r;
    r.path = new std::string(path);
    r.stream = flcast_stream_start(c.kind == CAST_WINDOW ? FLCAST_WINDOW : FLCAST_MONITOR, c.id.c_str(), cursor,
                                   on_stream_closed, r.path, err, sizeof err);
    if (!r.stream) {
      g_printerr("xdg-desktop-portal-fl: cannot share %s: %s\n", c.id.c_str(), err);
      delete r.path;
      g_variant_builder_clear(&sb);
      stop_streams(running);
      fld::glib_call([path] { close_session(path, true); });
      reply(inv, OTHER);
      return;
    }
    running.push_back(r);
    int w = 0, h = 0;
    flcast_stream_size(r.stream, &w, &h);
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "source_type", g_variant_new_uint32((guint32)c.kind));
    g_variant_builder_add(&props, "{sv}", "size", g_variant_new("(ii)", w, h));
    if (c.kind == CAST_MONITOR) g_variant_builder_add(&props, "{sv}", "position", g_variant_new("(ii)", 0, 0));
    g_variant_builder_add(&sb, "(u@a{sv})", flcast_stream_node(r.stream), g_variant_builder_end(&props));
  }
  GVariantBuilder rb;
  g_variant_builder_init(&rb, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&rb, "{sv}", "streams", g_variant_builder_end(&sb));
  if (persist_mode) {
    g_variant_builder_add(&rb, "{sv}", "persist_mode", g_variant_new_uint32(persist_mode));
    g_variant_builder_add(&rb, "{sv}", "restore_data", cast_restore_to_variant(chosen));
  }
  GVariant *results = g_variant_ref_sink(g_variant_builder_end(&rb));
  fld::glib_call([inv, path, running, results] {
    Session *s = find_session(path);
    if (!s) {
      // closed while we were starting
      stop_streams(running);
      reply(inv, CANCELLED);
    } else {
      s->starting = false;
      s->streams = running;
      reply(inv, SUCCESS, results);
    }
    g_variant_unref(results);
  });
}

// ---- the picker (FLTK thread) ----------------------------------------------

class CastPicker : public Fl_Double_Window {
public:
  using Done = std::function<void(bool ok, const std::vector<CastSource> &, bool remember)>;
  std::vector<CastSource> sources;
  Done done;
  std::string handle;
  Fl_Browser *list;
  Fl_Check_Button *remember = nullptr;
  Fl_Return_Button *ok;
  bool finished = false;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  CastPicker(const std::string &h, const std::string &app, const std::vector<CastSource> &src, bool multiple,
             bool can_persist, Done d)
      : Fl_Double_Window(500, 420, "Share Screen"), sources(src), done(std::move(d)), handle(h) {
    Fl_Box *t = new Fl_Box(16, 12, 468, 28);
    t->copy_label(fld::escape_at(app + " wants to share your screen").c_str());
    t->labelfont(FL_HELVETICA_BOLD);
    t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    Fl_Box *n = new Fl_Box(16, 40, 468, 22, multiple ? "Choose one or more screens or windows."
                                                       : "Choose a screen or a window.");
    n->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    int bottom = can_persist ? 330 : 360;
    if (multiple) list = new Fl_Multi_Browser(16, 68, 468, bottom - 68 - 8);
    else list = new Fl_Hold_Browser(16, 68, 468, bottom - 68 - 8);
    list->format_char(0); // titles are text, not formatting
    for (const CastSource &s : sources) {
      std::string line;
      if (s.kind == CAST_MONITOR) {
        line = "Screen\t" + s.id;
        if (!s.title.empty()) line += " - " + s.title;
      } else {
        line = "Window\t" + (s.title.empty() ? s.app_id : s.title);
        if (!s.app_id.empty() && !s.title.empty()) line += " (" + s.app_id + ")";
      }
      list->add(line.c_str());
    }
    static const int widths[] = {80, 0};
    list->column_widths(widths);
    list->column_char('\t');
    if (!sources.empty()) list->select(1);
    list->callback([](Fl_Widget *w, void *) {
      CastPicker *p = (CastPicker *)w->window();
      p->update();
      if (Fl::event_clicks() && Fl::event() == FL_RELEASE && !dynamic_cast<Fl_Multi_Browser *>(p->list))
        p->finish(true);
    });
    if (can_persist) {
      remember = new Fl_Check_Button(16, bottom, 468, 26, "Remember this choice for this application");
      remember->value(1);
    }
    Fl_Button *c = new Fl_Button(248, 372, 112, 30, "Cancel");
    c->callback([](Fl_Widget *w, void *) { ((CastPicker *)w->window())->finish(false); });
    ok = new Fl_Return_Button(368, 372, 116, 30, "Share");
    ok->callback([](Fl_Widget *w, void *) { ((CastPicker *)w->window())->finish(true); });
    end();
    callback([](Fl_Widget *w, void *) { ((CastPicker *)w)->finish(false); });
    fld::set_window_icon(this, "video-display");
    update();
  }
  std::vector<CastSource> chosen() const {
    std::vector<CastSource> out;
    for (int i = 1; i <= list->size(); i++)
      if (list->selected(i)) out.push_back(sources[(size_t)i - 1]);
    return out;
  }
  void update() {
    if (chosen().empty()) ok->deactivate();
    else ok->activate();
  }
  void finish(bool accepted) {
    if (finished) return;
    std::vector<CastSource> c = chosen();
    if (accepted && c.empty()) return;
    finished = true;
    *alive = false;
    request_end(handle);
    hide();
    done(accepted, c, remember && remember->value());
    Fl::delete_widget(this);
  }
};

guint32 g_types_cache = 0;

} // namespace

GVariant *screencast_property(const std::string &prop) {
  if (prop == "version") return g_variant_new_uint32(5);
  if (prop == "AvailableCursorModes") return g_variant_new_uint32(CAST_CURSOR_HIDDEN | CAST_CURSOR_EMBEDDED);
  if (prop == "AvailableSourceTypes") {
    if (!g_types_cache) {
      flcast_sources s;
      if (flcast_list_sources(&s) == 0) {
        g_types_cache = s.types;
        flcast_sources_free(&s);
      }
    }
    return g_variant_new_uint32(g_types_cache ? g_types_cache : (guint32)CAST_MONITOR);
  }
  return nullptr;
}

void screencast_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  if (method == "CreateSession") {
    const char *handle, *session, *app_id;
    GVariant *options;
    g_variant_get(params, "(&o&o&s@a{sv})", &handle, &session, &app_id, &options);
    g_variant_unref(options);
    if (!g_session_info) g_session_info = g_dbus_node_info_new_for_xml(kSessionXml, nullptr);
    static const GDBusInterfaceVTable vt = {session_method, session_property, nullptr, {}};
    GError *err = nullptr;
    guint reg = g_dbus_connection_register_object(connection(), session, g_session_info->interfaces[0], &vt, nullptr,
                                                  nullptr, &err);
    if (!reg) {
      g_printerr("xdg-desktop-portal-fl: cannot export %s: %s\n", session, err ? err->message : "?");
      g_clear_error(&err);
      reply(inv, OTHER);
      return;
    }
    auto s = std::make_unique<Session>();
    s->path = session;
    s->app_id = app_id;
    s->reg = reg;
    g_sessions[session] = std::move(s);
    reply(inv, SUCCESS);
    return;
  }

  if (method == "SelectSources") {
    const char *handle, *session, *app_id;
    GVariant *options;
    g_variant_get(params, "(&o&o&s@a{sv})", &handle, &session, &app_id, &options);
    Session *s = find_session(session);
    if (!s) {
      g_variant_unref(options);
      reply(inv, OTHER);
      return;
    }
    guint32 u;
    gboolean b;
    if (g_variant_lookup(options, "types", "u", &u) && (u & (CAST_MONITOR | CAST_WINDOW))) s->types = u;
    if (g_variant_lookup(options, "multiple", "b", &b)) s->multiple = b;
    if (g_variant_lookup(options, "cursor_mode", "u", &u)) s->cursor_mode = u;
    if (g_variant_lookup(options, "persist_mode", "u", &u)) s->persist_mode = u <= 2 ? u : 0;
    if (GVariant *r = g_variant_lookup_value(options, "restore_data", G_VARIANT_TYPE("(suv)"))) {
      s->restore = cast_parse_restore(r);
      g_variant_unref(r);
    }
    g_variant_unref(options);
    reply(inv, SUCCESS);
    return;
  }

  if (method == "Start") {
    const char *handle, *session, *app_id, *parent;
    GVariant *options;
    g_variant_get(params, "(&o&o&s&s@a{sv})", &handle, &session, &app_id, &parent, &options);
    g_variant_unref(options);
    Session *s = find_session(session);
    if (!s || s->starting || !s->streams.empty()) {
      reply(inv, OTHER);
      return;
    }
    s->starting = true;
    std::string h = handle, path = session, app = app_name(app_id);
    guint32 types = s->types, persist = s->persist_mode;
    bool multiple = s->multiple, cursor = cast_embed_cursor(s->cursor_mode);
    std::vector<CastSource> restore = s->restore;
    std::thread([inv, h, path, app, types, persist, multiple, cursor, restore] {
      bool ok;
      std::vector<CastSource> avail = list_sources(types, &ok);
      if (!ok || avail.empty()) {
        g_printerr("xdg-desktop-portal-fl: nothing to share (no Wayland connection?)\n");
        fld::glib_call([path] { close_session(path, true); });
        reply(inv, OTHER);
        return;
      }
      if (!restore.empty()) {
        std::vector<int> idx = cast_match_restore(restore, avail);
        if (!idx.empty()) {
          std::vector<CastSource> chosen;
          for (int i : idx) chosen.push_back(avail[(size_t)i]);
          start_streams(inv, path, chosen, cursor, persist);
          return;
        }
      }
      fld::ui_call([inv, h, path, app, avail, multiple, persist, cursor] {
        auto *w = new CastPicker(h, app, avail, multiple, persist != 0,
                                 [inv, path, persist, cursor](bool ok, const std::vector<CastSource> &chosen,
                                                              bool remember) {
                                   if (!ok) {
                                     fld::glib_call([path] {
                                       if (Session *s = find_session(path)) s->starting = false;
                                     });
                                     reply(inv, CANCELLED);
                                     return;
                                   }
                                   guint32 pm = remember ? persist : 0;
                                   std::thread(start_streams, inv, path, chosen, cursor, pm).detach();
                                 });
        auto alive = w->alive;
        w->show();
        request_begin(h, [w, alive] {
          if (*alive) w->finish(false);
        });
      });
    }).detach();
    return;
  }

  g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method.c_str());
}

} // namespace portal
