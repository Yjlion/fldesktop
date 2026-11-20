// fl-snip: screen recording
//
// A screen or window is chosen through the ScreenCast portal
// (org.freedesktop.portal.ScreenCast; in fldesktop xdg-desktop-portal-fl
// shows its picker), which hands over a PipeWire node; GStreamer records it
// to WebM until Stop. A small window shows the time and the Stop button.
#include "record.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gst/gst.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "fld/notify.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "snip_util.h"

namespace record {

namespace {

// ---- the ScreenCast portal (synchronous, for a command line program) -------

struct Portal {
  GDBusConnection *conn = nullptr;
  std::string session;
  std::string sender; // our unique name as the portal uses it in paths
  int token = 0;
};

const char *kDest = "org.freedesktop.portal.Desktop";
const char *kPath = "/org/freedesktop/portal/desktop";
const char *kCast = "org.freedesktop.portal.ScreenCast";

struct Wait {
  GMainLoop *loop;
  guint32 response = 2;
  GVariant *results = nullptr;
};

void on_response(GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *params,
                 gpointer data) {
  Wait *w = (Wait *)data;
  g_variant_get(params, "(u@a{sv})", &w->response, &w->results);
  g_main_loop_quit(w->loop);
}

// Calls a portal method that answers through a Request object and waits for
// its Response. Returns the results (owned) or nullptr; response is 0 for ok.
GVariant *request(Portal &p, const char *method, GVariant *args_without_options, GVariantBuilder *options,
                  guint32 *response, std::string &err) {
  std::string token = "flsnip" + std::to_string(++p.token);
  g_variant_builder_add(options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
  std::string req = std::string(kPath) + "/request/" + p.sender + "/" + token;
  GMainContext *ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);
  Wait w;
  w.loop = g_main_loop_new(ctx, FALSE);
  // subscribed before the call, so the Response cannot come too early
  guint sub = g_dbus_connection_signal_subscribe(p.conn, kDest, "org.freedesktop.portal.Request", "Response",
                                                 req.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_response, &w,
                                                 nullptr);
  // the arguments, then the options dictionary
  GVariantBuilder all;
  g_variant_builder_init(&all, G_VARIANT_TYPE_TUPLE);
  if (args_without_options) {
    GVariantIter it;
    g_variant_iter_init(&it, args_without_options);
    GVariant *c;
    while ((c = g_variant_iter_next_value(&it))) {
      g_variant_builder_add_value(&all, c);
      g_variant_unref(c);
    }
    g_variant_unref(g_variant_ref_sink(args_without_options));
  }
  g_variant_builder_add_value(&all, g_variant_builder_end(options));
  GError *e = nullptr;
  GVariant *ret = g_dbus_connection_call_sync(p.conn, kDest, kPath, kCast, method, g_variant_builder_end(&all),
                                              G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &e);
  if (!ret) {
    err = std::string(method) + ": " + (e ? e->message : "?");
    g_clear_error(&e);
  } else {
    g_variant_unref(ret);
    g_main_loop_run(w.loop); // until the Response (the user may take a while in the picker)
  }
  g_dbus_connection_signal_unsubscribe(p.conn, sub);
  g_main_loop_unref(w.loop);
  g_main_context_pop_thread_default(ctx);
  g_main_context_unref(ctx);
  *response = ret ? w.response : 2;
  return ret ? w.results : nullptr;
}

bool portal_start(Portal &p, bool window_too, int *fd, uint32_t *node, std::string &err) {
  GError *e = nullptr;
  p.conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &e);
  if (!p.conn) {
    err = std::string("no session bus: ") + (e ? e->message : "?");
    g_clear_error(&e);
    return false;
  }
  p.sender = g_dbus_connection_get_unique_name(p.conn) + 1; // without the ':'
  for (char &c : p.sender)
    if (c == '.') c = '_';
  guint32 resp;
  GVariantBuilder o;
  // CreateSession
  g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&o, "{sv}", "session_handle_token", g_variant_new_string("flsnip"));
  GVariant *r = request(p, "CreateSession", nullptr, &o, &resp, err);
  if (!r || resp != 0) {
    if (err.empty()) err = "the portal refused a screen cast session";
    if (r) g_variant_unref(r);
    return false;
  }
  const char *s = nullptr;
  if (g_variant_lookup(r, "session_handle", "&s", &s)) p.session = s;
  g_variant_unref(r);
  if (p.session.empty()) {
    err = "no session from the portal";
    return false;
  }
  // SelectSources: a screen (or a window), the pointer drawn in
  g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&o, "{sv}", "types", g_variant_new_uint32(window_too ? 3 : 1));
  g_variant_builder_add(&o, "{sv}", "multiple", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&o, "{sv}", "cursor_mode", g_variant_new_uint32(2));
  r = request(p, "SelectSources", g_variant_new("(o)", p.session.c_str()), &o, &resp, err);
  if (r) g_variant_unref(r);
  if (resp != 0) {
    if (err.empty()) err = resp == 1 ? "" : "the portal refused the sources";
    return false;
  }
  // Start: the user picks; the answer has the streams
  g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
  r = request(p, "Start", g_variant_new("(os)", p.session.c_str(), ""), &o, &resp, err);
  if (!r || resp != 0) {
    if (err.empty() && resp != 1) err = "the screen cast did not start";
    if (r) g_variant_unref(r);
    return false;
  }
  GVariant *streams = g_variant_lookup_value(r, "streams", G_VARIANT_TYPE("a(ua{sv})"));
  bool have = streams && g_variant_n_children(streams) > 0;
  if (have) {
    GVariant *props;
    g_variant_get_child(streams, 0, "(u@a{sv})", node, &props);
    g_variant_unref(props);
  }
  if (streams) g_variant_unref(streams);
  g_variant_unref(r);
  if (!have) {
    err = "the portal sent no stream";
    return false;
  }
  // the PipeWire connection that may see the node
  GUnixFDList *fds = nullptr;
  GVariant *ret = g_dbus_connection_call_with_unix_fd_list_sync(
      p.conn, kDest, kPath, kCast, "OpenPipeWireRemote", g_variant_new("(oa{sv})", p.session.c_str(), nullptr),
      G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fds, nullptr, &e);
  if (!ret) {
    err = std::string("OpenPipeWireRemote: ") + (e ? e->message : "?");
    g_clear_error(&e);
    return false;
  }
  gint32 idx;
  g_variant_get(ret, "(h)", &idx);
  g_variant_unref(ret);
  *fd = g_unix_fd_list_get(fds, idx, nullptr);
  g_object_unref(fds);
  return *fd >= 0;
}

void portal_close(Portal &p) {
  if (p.conn && !p.session.empty())
    g_dbus_connection_call_sync(p.conn, kDest, p.session.c_str(), "org.freedesktop.portal.Session", "Close", nullptr,
                                nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
  if (p.conn) g_object_unref(p.conn);
  p.conn = nullptr;
}

// ---- GStreamer --------------------------------------------------------------

struct Recorder {
  GstElement *pipe = nullptr;
  std::string error;

  bool start(int fd, uint32_t node, bool audio, const std::string &path) {
    GError *e = nullptr;
    pipe = gst_parse_launch(snip::record_pipeline(fd, node, audio).c_str(), &e);
    if (!pipe) {
      error = std::string("GStreamer: ") + (e ? e->message : "?") +
              " (are gst-plugin-pipewire and the good plugins installed?)";
      g_clear_error(&e);
      return false;
    }
    if (e) g_clear_error(&e); // a warning about an optional part
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
    g_object_set(sink, "location", path.c_str(), nullptr);
    gst_object_unref(sink);
    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      error = "the recording did not start";
      return false;
    }
    return true;
  }

  // an error on the bus, if any (does not wait)
  bool failed() {
    GstBus *bus = gst_element_get_bus(pipe);
    GstMessage *m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (!m) return false;
    GError *e = nullptr;
    gst_message_parse_error(m, &e, nullptr);
    error = e ? e->message : "recording error";
    g_clear_error(&e);
    gst_message_unref(m);
    return true;
  }

  // ends the file properly: EOS through the pipeline, then stop
  void stop() {
    if (!pipe) return;
    gst_element_send_event(pipe, gst_event_new_eos());
    GstBus *bus = gst_element_get_bus(pipe);
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (m) gst_message_unref(m);
    gst_object_unref(bus);
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(pipe);
    pipe = nullptr;
  }
};

volatile sig_atomic_t g_stop = 0;

std::string recordings_dir() {
  std::string v = fld::user_dir("VIDEOS");
  if (v.empty()) v = fld::home_dir() + "/Videos";
  return v + "/Screencasts";
}

// the small window while recording: time and Stop
struct Status {
  Fl_Double_Window *win = nullptr;
  Fl_Box *time_box = nullptr;
  bool stop = false;
  char text[32] = "";
};

} // namespace

int run(const Options &opt) {
  gst_init(nullptr, nullptr);
  int fd = -1;
  uint32_t node = opt.node;
  Portal portal;
  std::string err;
  if (!opt.node) {
    if (!portal_start(portal, true, &fd, &node, err)) {
      portal_close(portal);
      if (err.empty()) return 1; // cancelled in the picker
      fprintf(stderr, "fl-snip: %s\n", err.c_str());
      fld::Notification n;
      n.summary = "Screen recording";
      n.body = err;
      n.icon = "dialog-error";
      n.transient = true;
      fld::notify(n);
      return 1;
    }
  }
  std::string path = opt.output;
  if (path.empty()) {
    fld::mkdir_p(recordings_dir());
    path = fld::path_join(recordings_dir(), snip::recording_name(time(nullptr)));
  }
  Recorder rec;
  if (!rec.start(fd, node, opt.audio, path)) {
    fprintf(stderr, "fl-snip: %s\n", rec.error.c_str());
    portal_close(portal);
    return 1;
  }
  signal(SIGINT, [](int) { g_stop = 1; });
  signal(SIGTERM, [](int) { g_stop = 1; });
  Status st;
  if (opt.window) {
    st.win = new Fl_Double_Window(230, 56, "Recording");
    st.win->color(fld::palette().bg);
    Fl_Box *dot = new Fl_Box(12, 18, 20, 20, "@-2circle");
    dot->labelcolor(FL_RED);
    st.time_box = new Fl_Box(36, 14, 80, 28, "0:00");
    st.time_box->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    st.time_box->labelfont(FL_HELVETICA_BOLD);
    Fl_Button *b = new Fl_Button(124, 12, 94, 32, "Stop");
    b->callback([](Fl_Widget *, void *d) { ((Status *)d)->stop = true; }, &st);
    st.win->callback([](Fl_Widget *, void *d) { ((Status *)d)->stop = true; }, &st);
    st.win->end();
    fld::set_window_icon(st.win, "media-record");
    st.win->show();
  }
  time_t start = time(nullptr);
  bool failed = false;
  while (!g_stop && !st.stop) {
    if (opt.window) Fl::wait(0.25);
    else usleep(250000);
    long secs = (long)(time(nullptr) - start);
    if (opt.seconds > 0 && secs >= opt.seconds) break;
    if (rec.failed()) {
      failed = true;
      break;
    }
    if (st.time_box) {
      snprintf(st.text, sizeof st.text, "%ld:%02ld", secs / 60, secs % 60);
      st.time_box->label(st.text);
    }
  }
  if (st.win) st.win->hide();
  rec.stop();
  portal_close(portal);
  if (fd >= 0) close(fd);
  if (failed) {
    fprintf(stderr, "fl-snip: recording failed: %s\n", rec.error.c_str());
    return 1;
  }
  printf("%s\n", path.c_str());
  fflush(stdout);
  if (opt.notify) {
    fld::Notification n;
    n.summary = "Screen recording saved";
    n.body = fld::basename(path);
    n.icon = "video-x-generic";
    n.transient = true;
    fld::notify(n);
  }
  return 0;
}

} // namespace record
