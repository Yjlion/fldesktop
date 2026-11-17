// fl-polkit-agent: polkit authentication agent for fldesktop
//
// The polkit listener (GObject) runs on the GLib thread (fld-glib); the
// dialogs run in the FLTK thread. Requests are identified by a number; all
// polkit objects stay on the GLib side.
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE 1
#include <polkitagent/polkitagent.h>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_ask.H>

#include <map>
#include <pwd.h>
#include <grp.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "fld/glib_bridge.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

// ---------------------------------------------------------------------------
// Data shared between the threads (plain values only)

struct RequestInfo {
  int id;
  std::string action_id, message, icon_name, vendor;
  std::vector<std::pair<std::string, std::string>> details;
  std::vector<std::string> user_names; // one per identity (display names)
  int default_identity = 0;
};

// GLib side state of a request
struct PendingRequest {
  int id;
  GTask *task = nullptr;
  std::string cookie;
  std::vector<PolkitIdentity *> identities;
  PolkitAgentSession *session = nullptr;
  std::string password;
  bool password_used = false;
  GCancellable *cancellable = nullptr;
  gulong cancel_handler = 0;
  bool finished = false;
};

static std::map<int, PendingRequest *> g_pending; // GLib thread only
static int g_next_id = 1;

// UI side
static void ui_show_request(const RequestInfo &info);
static void ui_close_request(int id);
static void ui_message(int id, const std::string &msg, bool error);
static void ui_auth_failed(int id);
static void ui_extra_prompt(int id, const std::string &prompt, bool echo);

// ---------------------------------------------------------------------------
// GLib thread

static void finish_request(PendingRequest *r, bool cancelled) {
  if (r->finished) return;
  r->finished = true;
  if (r->session) {
    g_signal_handlers_disconnect_by_data(r->session, r);
    polkit_agent_session_cancel(r->session);
    g_object_unref(r->session);
    r->session = nullptr;
  }
  if (r->cancellable && r->cancel_handler) g_cancellable_disconnect(r->cancellable, r->cancel_handler);
  if (cancelled)
    g_task_return_new_error(r->task, POLKIT_ERROR, POLKIT_ERROR_CANCELLED, "Authentication was cancelled");
  else
    g_task_return_boolean(r->task, TRUE);
  g_object_unref(r->task);
  for (auto *i : r->identities) g_object_unref(i);
  int id = r->id;
  g_pending.erase(id);
  delete r;
  fld::ui_call([id] { ui_close_request(id); });
}

static void on_session_request(PolkitAgentSession *session, const gchar *prompt, gboolean echo_on, gpointer data) {
  PendingRequest *r = (PendingRequest *)data;
  if (!r->password_used && !echo_on) {
    // the first secret prompt (normally "Password:") gets the typed password
    r->password_used = true;
    polkit_agent_session_response(session, r->password.c_str());
    // wipe our copy
    std::fill(r->password.begin(), r->password.end(), '\0');
    r->password.clear();
    return;
  }
  int id = r->id;
  std::string p = prompt ? prompt : "";
  bool echo = echo_on;
  fld::ui_call([id, p, echo] { ui_extra_prompt(id, p, echo); });
}

static void on_session_show_error(PolkitAgentSession *, const gchar *text, gpointer data) {
  int id = ((PendingRequest *)data)->id;
  std::string t = text ? text : "";
  fld::ui_call([id, t] { ui_message(id, t, true); });
}

static void on_session_show_info(PolkitAgentSession *, const gchar *text, gpointer data) {
  int id = ((PendingRequest *)data)->id;
  std::string t = text ? text : "";
  fld::ui_call([id, t] { ui_message(id, t, false); });
}

static void on_session_completed(PolkitAgentSession *session, gboolean gained, gpointer data) {
  PendingRequest *r = (PendingRequest *)data;
  g_signal_handlers_disconnect_by_data(session, r);
  g_object_unref(r->session);
  r->session = nullptr;
  if (gained) {
    finish_request(r, false);
  } else {
    int id = r->id;
    fld::ui_call([id] { ui_auth_failed(id); });
  }
}

// called from the UI with the chosen identity and the password
static void glib_authenticate(int id, int identity, const std::string &password) {
  auto it = g_pending.find(id);
  if (it == g_pending.end()) return;
  PendingRequest *r = it->second;
  if (identity < 0 || identity >= (int)r->identities.size()) identity = 0;
  if (r->session) {
    g_signal_handlers_disconnect_by_data(r->session, r);
    polkit_agent_session_cancel(r->session);
    g_object_unref(r->session);
  }
  r->password = password;
  r->password_used = false;
  r->session = polkit_agent_session_new(r->identities[identity], r->cookie.c_str());
  g_signal_connect(r->session, "request", G_CALLBACK(on_session_request), r);
  g_signal_connect(r->session, "show-error", G_CALLBACK(on_session_show_error), r);
  g_signal_connect(r->session, "show-info", G_CALLBACK(on_session_show_info), r);
  g_signal_connect(r->session, "completed", G_CALLBACK(on_session_completed), r);
  polkit_agent_session_initiate(r->session);
}

static void glib_respond(int id, const std::string &text) {
  auto it = g_pending.find(id);
  if (it == g_pending.end() || !it->second->session) return;
  polkit_agent_session_response(it->second->session, text.c_str());
}

static void glib_cancel(int id) {
  auto it = g_pending.find(id);
  if (it != g_pending.end()) finish_request(it->second, true);
}

static void on_cancelled(GCancellable *, gpointer data) {
  // polkitd cancelled the request (e.g. the caller went away). Called on the
  // GLib thread; finish from an idle so we are outside the cancellable's lock.
  int id = GPOINTER_TO_INT(data);
  fld::glib_call([id] { glib_cancel(id); });
}

static std::string identity_display(PolkitIdentity *identity) {
  if (POLKIT_IS_UNIX_USER(identity)) {
    uid_t uid = polkit_unix_user_get_uid(POLKIT_UNIX_USER(identity));
    struct passwd *pw = getpwuid(uid);
    if (!pw) return "uid " + std::to_string(uid);
    std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
    gecos = gecos.substr(0, gecos.find(','));
    return gecos.empty() ? pw->pw_name : gecos + " (" + pw->pw_name + ")";
  }
  gchar *s = polkit_identity_to_string(identity);
  std::string r = s ? s : "?";
  g_free(s);
  return r;
}

// ---- the PolkitAgentListener subclass ------------------------------------------

typedef struct {
  PolkitAgentListener parent;
} FlAgent;
typedef struct {
  PolkitAgentListenerClass parent_class;
} FlAgentClass;

G_DEFINE_TYPE(FlAgent, fl_agent, POLKIT_AGENT_TYPE_LISTENER)

static void initiate_authentication(PolkitAgentListener *listener, const gchar *action_id, const gchar *message,
                                    const gchar *icon_name, PolkitDetails *details, const gchar *cookie,
                                    GList *identities, GCancellable *cancellable, GAsyncReadyCallback callback,
                                    gpointer user_data) {
  PendingRequest *r = new PendingRequest;
  r->id = g_next_id++;
  r->task = g_task_new(listener, cancellable, callback, user_data);
  r->cookie = cookie ? cookie : "";
  RequestInfo info;
  info.id = r->id;
  info.action_id = action_id ? action_id : "";
  info.message = message ? message : "";
  info.icon_name = icon_name ? icon_name : "";
  // unix users only (groups are expanded by polkit for the default rules);
  // prefer the current user, then root
  uid_t me = getuid();
  for (GList *l = identities; l; l = l->next) {
    PolkitIdentity *id = POLKIT_IDENTITY(l->data);
    if (!POLKIT_IS_UNIX_USER(id)) continue;
    r->identities.push_back((PolkitIdentity *)g_object_ref(id));
    info.user_names.push_back(identity_display(id));
    if ((uid_t)polkit_unix_user_get_uid(POLKIT_UNIX_USER(id)) == me) info.default_identity = (int)r->identities.size() - 1;
  }
  if (details) {
    gchar **keys = polkit_details_get_keys(details);
    for (gchar **k = keys; k && *k; k++) {
      const gchar *v = polkit_details_lookup(details, *k);
      info.details.push_back({*k, v ? v : ""});
    }
    g_strfreev(keys);
  }
  if (r->identities.empty()) {
    g_task_return_new_error(r->task, POLKIT_ERROR, POLKIT_ERROR_FAILED, "No user to authenticate as");
    g_object_unref(r->task);
    delete r;
    return;
  }
  if (cancellable) {
    r->cancellable = cancellable;
    r->cancel_handler = g_cancellable_connect(cancellable, G_CALLBACK(on_cancelled), GINT_TO_POINTER(r->id), nullptr);
  }
  g_pending[r->id] = r;
  fld::ui_call([info] { ui_show_request(info); });
}

static gboolean initiate_authentication_finish(PolkitAgentListener *, GAsyncResult *res, GError **error) {
  return g_task_propagate_boolean(G_TASK(res), error);
}

static void fl_agent_init(FlAgent *) {}

static void fl_agent_class_init(FlAgentClass *klass) {
  PolkitAgentListenerClass *lc = POLKIT_AGENT_LISTENER_CLASS(klass);
  lc->initiate_authentication = initiate_authentication;
  lc->initiate_authentication_finish = initiate_authentication_finish;
}

// ---------------------------------------------------------------------------
// FLTK side: the dialog

struct Dialog {
  int id;
  RequestInfo info;
  Fl_Double_Window *win;
  Fl_Choice *who = nullptr;
  Fl_Box *who_label = nullptr;
  Fl_Secret_Input *password;
  Fl_Box *status;
  Fl_Return_Button *ok;
  Fl_Button *cancel, *details_btn;
  Fl_Multiline_Output *details;
  bool busy = false;
  std::string status_text;
};

static std::map<int, Dialog *> g_dialogs;

static void set_busy(Dialog *d, bool busy) {
  d->busy = busy;
  if (busy) { d->ok->deactivate(); d->password->deactivate(); if (d->who) d->who->deactivate(); }
  else { d->ok->activate(); d->password->activate(); if (d->who) d->who->activate(); d->password->take_focus(); }
}

static void dialog_ok(Fl_Widget *, void *data) {
  Dialog *d = (Dialog *)data;
  if (d->busy) return;
  int identity = d->who ? d->who->value() : d->info.default_identity;
  std::string pw = d->password->value();
  d->status_text = "Authenticating…";
  d->status->label(d->status_text.c_str());
  d->status->labelcolor(fld::palette().fg_dim);
  set_busy(d, true);
  int id = d->id;
  fld::glib_call([id, identity, pw] { glib_authenticate(id, identity, pw); });
}

static void dialog_cancel(Fl_Widget *, void *data) {
  Dialog *d = (Dialog *)data;
  int id = d->id;
  fld::glib_call([id] { glib_cancel(id); });
}

static void dialog_details(Fl_Widget *, void *data) {
  Dialog *d = (Dialog *)data;
  int extra = d->details->h() + 10;
  if (d->details->visible()) {
    d->details->hide();
    d->win->size(d->win->w(), d->win->h() - extra);
    d->details_btn->label("Details ▸");
  } else {
    d->win->size(d->win->w(), d->win->h() + extra);
    d->details->show();
    d->details_btn->label("Details ▾");
  }
}

static void ui_show_request(const RequestInfo &info) {
  Dialog *d = new Dialog;
  d->id = info.id;
  d->info = info;
  const fld::Palette &p = fld::palette();
  const int W = 480;
  int y = 16;
  d->win = new Fl_Double_Window(W, 260, "Authentication Required");
  Fl_Box *icon = new Fl_Box(16, y, 48, 48);
  icon->image(fld::icon_first({info.icon_name, "dialog-password", "security-high", "system-lock-screen"}, 48));
  Fl_Box *title = new Fl_Box(76, y, W - 92, 24, "Authentication Required");
  title->labelfont(FL_HELVETICA_BOLD);
  title->labelsize(16);
  title->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  Fl_Box *msg = new Fl_Box(76, y + 26, W - 92, 44);
  msg->copy_label(fld::escape_at(info.message.empty() ? "An application wants to perform an action that needs privileges."
                                                       : info.message).c_str());
  msg->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  msg->labelsize(13);
  y += 82;
  if (info.user_names.size() > 1) {
    d->who = new Fl_Choice(140, y, W - 156, 28, "Authenticate as:");
    for (auto &n : info.user_names) d->who->add(fld::escape_at(n).c_str());
    d->who->value(info.default_identity);
    y += 36;
  } else {
    static std::string lbl;
    d->who_label = new Fl_Box(16, y, W - 32, 24);
    d->who_label->copy_label(fld::escape_at("Password for " + info.user_names[0] + ":").c_str());
    d->who_label->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    y += 26;
  }
  d->password = new Fl_Secret_Input(info.user_names.size() > 1 ? 140 : 16, y,
                                    info.user_names.size() > 1 ? W - 156 : W - 32, 30,
                                    info.user_names.size() > 1 ? "Password:" : nullptr);
  d->password->when(FL_WHEN_ENTER_KEY_ALWAYS);
  d->password->callback(dialog_ok, d);
  y += 36;
  d->status = new Fl_Box(16, y, W - 32, 22);
  d->status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  d->status->labelsize(12);
  y += 28;
  d->details_btn = new Fl_Button(16, y, 100, 30, "Details ▸");
  d->details_btn->box(FL_FLAT_BOX);
  d->details_btn->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  d->details_btn->callback(dialog_details, d);
  d->cancel = new Fl_Button(W - 256, y, 100, 30, "Cancel");
  d->cancel->callback(dialog_cancel, d);
  d->ok = new Fl_Return_Button(W - 148, y, 132, 30, "Authenticate");
  d->ok->callback(dialog_ok, d);
  d->ok->color(p.accent);
  d->ok->labelcolor(p.accent_fg);
  y += 40;
  std::string det = "Action: " + info.action_id;
  for (auto &kv : info.details) det += "\n" + kv.first + ": " + kv.second;
  d->details = new Fl_Multiline_Output(16, y, W - 32, 90);
  d->details->value(det.c_str());
  d->details->textsize(11);
  d->details->hide();
  d->win->end();
  d->win->size(W, y);
  d->win->callback(dialog_cancel, d); // closing the window cancels
  d->win->set_non_modal();
  g_dialogs[d->id] = d;
  d->win->show();
  d->password->take_focus();
}

static void ui_close_request(int id) {
  auto it = g_dialogs.find(id);
  if (it == g_dialogs.end()) return;
  it->second->win->hide();
  Fl::delete_widget(it->second->win);
  delete it->second;
  g_dialogs.erase(it);
}

static void ui_message(int id, const std::string &text, bool error) {
  auto it = g_dialogs.find(id);
  if (it == g_dialogs.end()) return;
  Dialog *d = it->second;
  d->status_text = fld::escape_at(text); // the label points into status_text
  d->status->label(d->status_text.c_str());
  d->status->labelcolor(error ? fld::palette().danger : fld::palette().fg_dim);
  d->win->redraw();
}

static void ui_auth_failed(int id) {
  auto it = g_dialogs.find(id);
  if (it == g_dialogs.end()) return;
  Dialog *d = it->second;
  ui_message(id, "Sorry, that did not work. Please try again.", true);
  d->password->value("");
  set_busy(d, false);
}

static void ui_extra_prompt(int id, const std::string &prompt, bool echo) {
  auto it = g_dialogs.find(id);
  if (it == g_dialogs.end()) return;
  const char *answer = echo ? fl_input("%s", "", prompt.c_str()) : fl_password("%s", "", prompt.c_str());
  if (!answer) {
    fld::glib_call([id] { glib_cancel(id); });
    return;
  }
  std::string a = answer;
  fld::glib_call([id, a] { glib_respond(id, a); });
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  pid_t process = 0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--process" && i + 1 < argc) process = atoi(argv[++i]);
    else if (a == "-h" || a == "--help") {
      printf("Usage: fl-polkit-agent [--process PID]\n"
             "Registers as the polkit authentication agent of the current session\n"
             "(or only for PID, for testing).\n");
      return 0;
    }
  }
  fld::init_app("fl-polkit-agent", argc, argv);
  Fl::lock();
  fld::glib_start();

  std::string error;
  bool ok = false;
  fld::glib_call_sync([&] {
    GError *err = nullptr;
    PolkitSubject *subject = nullptr;
    if (process > 0) subject = polkit_unix_process_new_for_owner(process, 0, getuid());
    else subject = polkit_unix_session_new_for_process_sync(getpid(), nullptr, &err);
    if (!subject) {
      error = std::string("not running in a login session: ") + (err ? err->message : "unknown error");
      if (err) g_error_free(err);
      return;
    }
    PolkitAgentListener *listener = (PolkitAgentListener *)g_object_new(fl_agent_get_type(), nullptr);
    gpointer handle = polkit_agent_listener_register(listener, POLKIT_AGENT_REGISTER_FLAGS_NONE, subject,
                                                     "/org/fldesktop/PolicyKit1/AuthenticationAgent", nullptr, &err);
    g_object_unref(subject);
    if (!handle) {
      error = std::string("cannot register the authentication agent: ") + (err ? err->message : "unknown error");
      if (err) g_error_free(err);
      return;
    }
    ok = true;
  });
  if (!ok) {
    fprintf(stderr, "fl-polkit-agent: %s\n", error.c_str());
    fld::glib_stop();
    return 1;
  }
  fprintf(stderr, "fl-polkit-agent: registered%s\n", process > 0 ? " for one process" : "");
  // run even without windows
  while (true) Fl::wait(1e20);
  return 0;
}
