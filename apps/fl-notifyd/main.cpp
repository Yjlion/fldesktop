// fl-notifyd: the fldesktop notification server (org.freedesktop.Notifications 1.2)
//
// D-Bus runs on the GLib thread (fld/glib_bridge.h); the popups are one
// borderless FLTK window titled "fldesktop.notification", which fl-wm keeps
// in the top right corner without giving it the keyboard focus.
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>
#include <gio/gio.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fld/config_watch.h"
#include "fld/glib_bridge.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/notify.h"
#include "fld/theme.h"
#include "fld/util.h"

namespace {

const char *kIntrospection = R"XML(
<node>
  <interface name="org.freedesktop.Notifications">
    <method name="GetCapabilities"><arg type="as" direction="out"/></method>
    <method name="Notify">
      <arg type="s" name="app_name" direction="in"/>
      <arg type="u" name="replaces_id" direction="in"/>
      <arg type="s" name="app_icon" direction="in"/>
      <arg type="s" name="summary" direction="in"/>
      <arg type="s" name="body" direction="in"/>
      <arg type="as" name="actions" direction="in"/>
      <arg type="a{sv}" name="hints" direction="in"/>
      <arg type="i" name="expire_timeout" direction="in"/>
      <arg type="u" direction="out"/>
    </method>
    <method name="CloseNotification"><arg type="u" name="id" direction="in"/></method>
    <method name="GetServerInformation">
      <arg type="s" name="name" direction="out"/>
      <arg type="s" name="vendor" direction="out"/>
      <arg type="s" name="version" direction="out"/>
      <arg type="s" name="spec_version" direction="out"/>
    </method>
    <signal name="NotificationClosed"><arg type="u" name="id"/><arg type="u" name="reason"/></signal>
    <signal name="ActionInvoked"><arg type="u" name="id"/><arg type="s" name="action_key"/></signal>
    <signal name="ActivationToken"><arg type="u" name="id"/><arg type="s" name="activation_token"/></signal>
  </interface>
  <interface name="org.fldesktop.Notifications">
    <method name="GetHistory">
      <arg type="a(ussssx)" name="history" direction="out"/>
      <arg type="b" name="do_not_disturb" direction="out"/>
    </method>
    <method name="Clear"/>
    <method name="Remove"><arg type="u" name="id" direction="in"/></method>
    <method name="SetDoNotDisturb"><arg type="b" name="on" direction="in"/></method>
    <signal name="Changed"/>
  </interface>
</node>
)XML";

const char *kPath = "/org/freedesktop/Notifications";

enum CloseReason { EXPIRED = 1, DISMISSED = 2, CLOSED_BY_CALL = 3, UNDEFINED = 4 };

// One notification as the UI sees it (built on the GLib thread).
struct Note {
  uint32_t id = 0;
  std::string app, summary, body, icon, desktop_entry, category;
  std::vector<std::pair<std::string, std::string>> actions;
  int urgency = 1;
  int timeout = -1;
  bool transient = false, resident = false;
  int64_t time = 0;
  std::vector<unsigned char> pixels; // RGBA image-data
  int img_w = 0, img_h = 0;
  std::string image_path;
};

// ---- shared state (both threads) ------------------------------------------

std::mutex g_mutex;
std::vector<fld::HistoryEntry> g_history; // newest first
bool g_dnd = false;
int g_timeout_s = 6;
int g_max_history = 100;
std::vector<std::string> g_muted; // app names
std::atomic<uint32_t> g_next_id{1};

GDBusConnection *g_conn = nullptr; // GLib thread

std::string config_path() { return fld::fld_config_dir() + "/notifications.ini"; }

void load_settings() {
  fld::Ini ini;
  ini.load(config_path());
  std::lock_guard<std::mutex> l(g_mutex);
  g_dnd = ini.get_bool("Notifications", "DoNotDisturb", false);
  g_timeout_s = std::max(1, ini.get_int("Notifications", "Timeout", 6));
  g_max_history = std::max(0, ini.get_int("Notifications", "HistorySize", 100));
  g_muted.clear();
  for (auto &a : fld::split(ini.get("Notifications", "Muted"), ';')) {
    std::string t = fld::trim(a);
    if (!t.empty()) g_muted.push_back(t);
  }
}

void save_dnd(bool on) {
  fld::Ini ini;
  ini.load(config_path());
  ini.set_bool("Notifications", "DoNotDisturb", on);
  ini.save(config_path());
}

void emit(const char *iface, const char *signal, GVariant *args) {
  GVariant *a = args ? g_variant_ref_sink(args) : nullptr;
  std::string i = iface, s = signal;
  fld::glib_call([i, s, a] {
    if (g_conn) g_dbus_connection_emit_signal(g_conn, nullptr, kPath, i.c_str(), s.c_str(), a, nullptr);
    if (a) g_variant_unref(a);
  });
}

void emit_changed() { emit("org.fldesktop.Notifications", "Changed", nullptr); }
void emit_closed(uint32_t id, CloseReason r) {
  emit("org.freedesktop.Notifications", "NotificationClosed", g_variant_new("(uu)", id, (guint32)r));
}
void emit_action(uint32_t id, const std::string &key) {
  emit("org.freedesktop.Notifications", "ActionInvoked", g_variant_new("(us)", id, key.c_str()));
}

// ---- popups (FLTK thread) --------------------------------------------------

const int kWidth = 380, kPad = 12, kIcon = 40, kGap = 8, kMaxVisible = 5, kBtnH = 28;

struct Card {
  Note n;
  Fl_Image *img = nullptr;        // owned when own_img
  bool own_img = false;
  double remaining = 0;           // seconds; <0 = never expires
  int y = 0, h = 0;               // inside the window
  std::string summary, body;      // plain text
  // geometry of the action buttons and the close button (window coords)
  struct Btn { int x, y, w, h; std::string key, label; };
  std::vector<Btn> buttons;
  int close_x = 0, close_y = 0;
  ~Card() {
    if (own_img) delete img;
  }
};

class PopupWindow;
PopupWindow *g_win = nullptr;
std::vector<std::unique_ptr<Card>> g_cards; // newest first
bool g_hover = false;
int g_hover_btn = -1;      // index into the hovered card's buttons, -2 = close
Card *g_hover_card = nullptr;

void layout_cards();

int text_height(const std::string &s, Fl_Font f, int size, int w) {
  if (s.empty()) return 0;
  fl_font(f, size);
  int tw = w, th = 0;
  fl_measure(s.c_str(), tw, th, 0);
  return th;
}

void prepare_card(Card *c) {
  c->summary = fld::notify_strip_markup(c->n.summary);
  c->body = fld::notify_strip_markup(c->n.body);
  // a very long body is cut: the popup is not a reader
  if (c->body.size() > 600) c->body = c->body.substr(0, 600) + "…";
  if (!c->n.pixels.empty()) {
    auto *rgb = new Fl_RGB_Image(c->n.pixels.data(), c->n.img_w, c->n.img_h, 4);
    rgb->alloc_array = 0;
    Fl_Image *scaled = rgb->copy(kIcon, kIcon);
    delete rgb;
    c->img = scaled;
    c->own_img = true;
  } else {
    std::string ic = !c->n.image_path.empty() ? c->n.image_path : c->n.icon;
    if (fld::starts_with(ic, "file://")) ic = fld::uri_decode(ic.substr(7));
    if (!ic.empty() && ic[0] == '/') {
      Fl_Image *im = fld::load_image(ic);
      if (im) {
        c->img = im->copy(kIcon, kIcon);
        c->own_img = true;
        delete im;
      }
    } else if (!ic.empty()) {
      c->img = fld::icon(ic, kIcon);
    }
    if (!c->img && !c->n.desktop_entry.empty()) c->img = fld::icon(c->n.desktop_entry, kIcon);
    if (!c->img)
      c->img = fld::icon_first({c->n.urgency == 2 ? "dialog-warning" : "dialog-information", "preferences-system-notifications"},
                               kIcon);
  }
  int t = c->n.timeout;
  if (c->n.urgency == 2 || t == 0) c->remaining = -1;
  else if (t < 0) {
    std::lock_guard<std::mutex> l(g_mutex);
    c->remaining = g_timeout_s;
  } else c->remaining = t / 1000.0;
}

// height of a card and its button geometry relative to (0, y)
int measure_card(Card *c, int y) {
  int tx = kPad + kIcon + kPad, tw = kWidth - tx - kPad - 18;
  int h = kPad;
  h += text_height(c->summary, FL_HELVETICA_BOLD, fld::ui_font_size() + 1, tw);
  if (!c->body.empty()) h += 4 + std::min(text_height(c->body, FL_HELVETICA, fld::ui_font_size(), tw), 8 * (fld::ui_font_size() + 4));
  h = std::max(h, kPad + kIcon);
  c->buttons.clear();
  int bx = tx;
  bool any = false;
  for (auto &a : c->n.actions) {
    if (a.first == "default") continue;
    fl_font(FL_HELVETICA, fld::ui_font_size());
    int bw = std::max(72, (int)fl_width(a.second.c_str()) + 24);
    if (bx + bw > kWidth - kPad) break;
    if (!any) h += kGap;
    any = true;
    c->buttons.push_back({bx, y + h, bw, kBtnH, a.first, a.second});
    bx += bw + 6;
  }
  if (any) h += kBtnH;
  h += kPad;
  c->close_x = kWidth - kPad - 14;
  c->close_y = y + kPad - 2;
  return h;
}

class PopupWindow : public Fl_Double_Window {
public:
  PopupWindow() : Fl_Double_Window(kWidth, 100, "fldesktop.notification") {
    border(0);
    box(FL_NO_BOX);
    end();
  }

  Card *card_at(int my) {
    for (auto &c : g_cards)
      if (my >= c->y && my < c->y + c->h) return c.get();
    return nullptr;
  }

  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_color(p.bg);
    fl_rectf(0, 0, w(), h());
    for (auto &cp : g_cards) {
      Card *c = cp.get();
      Fl_Color bg = c->n.urgency == 2 ? fl_color_average(p.danger, p.bg, 0.15f) : p.bg;
      fl_color(bg);
      fl_rectf(0, c->y, w(), c->h);
      fl_color(c->n.urgency == 2 ? p.danger : p.border);
      fl_rect(0, c->y, w(), c->h);
      if (c->n.urgency == 2) fl_rectf(0, c->y, 4, c->h);
      if (c->img) c->img->draw(kPad, c->y + kPad);
      int tx = kPad + kIcon + kPad, tw = w() - tx - kPad - 18;
      int ty = c->y + kPad;
      fl_color(p.fg);
      fl_font(FL_HELVETICA_BOLD, fld::ui_font_size() + 1);
      int sh = text_height(c->summary, FL_HELVETICA_BOLD, fld::ui_font_size() + 1, tw);
      fl_font(FL_HELVETICA_BOLD, fld::ui_font_size() + 1);
      fl_draw(c->summary.c_str(), tx, ty, tw, sh, FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE, nullptr, 0);
      ty += sh + 4;
      if (!c->body.empty()) {
        int maxh = 8 * (fld::ui_font_size() + 4);
        int bh = std::min(text_height(c->body, FL_HELVETICA, fld::ui_font_size(), tw), maxh);
        fl_push_clip(tx, ty, tw, bh);
        fl_color(p.fg_dim);
        fl_font(FL_HELVETICA, fld::ui_font_size());
        fl_draw(c->body.c_str(), tx, ty, tw, bh, FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE, nullptr, 0);
        fl_pop_clip();
      }
      // app name, small, next to the close button
      for (size_t i = 0; i < c->buttons.size(); i++) {
        auto &b = c->buttons[i];
        bool hot = g_hover_card == c && g_hover_btn == (int)i;
        fl_color(hot ? p.accent : fl_color_average(p.fg, bg, 0.12f));
        fl_rectf(b.x, b.y, b.w, b.h);
        fl_color(hot ? p.accent_fg : p.fg);
        fl_font(FL_HELVETICA, fld::ui_font_size());
        fl_draw(b.label.c_str(), b.x, b.y, b.w, b.h, FL_ALIGN_CENTER, nullptr, 0);
      }
      bool hot = g_hover_card == c && g_hover_btn == -2;
      fl_color(hot ? p.fg : p.fg_dim);
      fl_line_style(FL_SOLID, 2);
      int cx = c->close_x, cy = c->close_y + 4;
      fl_line(cx, cy, cx + 9, cy + 9);
      fl_line(cx + 9, cy, cx, cy + 9);
      fl_line_style(0);
    }
  }

  int handle(int e) override {
    switch (e) {
    case FL_ENTER:
      g_hover = true;
      return 1;
    case FL_LEAVE:
      g_hover = false;
      g_hover_card = nullptr;
      g_hover_btn = -1;
      redraw();
      return 1;
    case FL_MOVE: {
      Card *c = card_at(Fl::event_y());
      int hb = -1;
      if (c) {
        int mx = Fl::event_x(), my = Fl::event_y();
        if (mx >= c->close_x - 6 && mx < c->close_x + 16 && my >= c->close_y - 4 && my < c->close_y + 18) hb = -2;
        for (size_t i = 0; i < c->buttons.size(); i++) {
          auto &b = c->buttons[i];
          if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) hb = (int)i;
        }
      }
      if (c != g_hover_card || hb != g_hover_btn) {
        g_hover_card = c;
        g_hover_btn = hb;
        redraw();
      }
      return 1;
    }
    case FL_PUSH:
      return 1;
    case FL_RELEASE: {
      Card *c = card_at(Fl::event_y());
      if (!c) return 1;
      uint32_t id = c->n.id;
      int mx = Fl::event_x(), my = Fl::event_y();
      if (mx >= c->close_x - 6 && mx < c->close_x + 16 && my >= c->close_y - 4 && my < c->close_y + 18) {
        close_card(id, DISMISSED);
        return 1;
      }
      for (auto &b : c->buttons) {
        if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) {
          emit_action(id, b.key);
          if (!c->n.resident) close_card(id, DISMISSED);
          return 1;
        }
      }
      // a click on the body runs the default action
      for (auto &a : c->n.actions)
        if (a.first == "default") emit_action(id, "default");
      if (!c->n.resident || Fl::event_button() == FL_RIGHT_MOUSE) close_card(id, DISMISSED);
      return 1;
    }
    }
    return Fl_Double_Window::handle(e);
  }

  static void close_card(uint32_t id, CloseReason r);
};

void tick(void *);

// The countdown of the cards runs only while one of them can expire: with
// nothing on screen fl-notifyd does not wake up.
void ensure_tick() {
  if (Fl::has_timeout(tick)) return;
  for (auto &c : g_cards)
    if (c->remaining >= 0) {
      Fl::add_timeout(0.25, tick);
      return;
    }
}

void layout_cards() {
  if (g_cards.empty()) {
    if (g_win->shown()) g_win->hide();
    return;
  }
  ensure_tick();
  int y = 0;
  for (auto &c : g_cards) {
    c->h = measure_card(c.get(), y);
    c->y = y;
    y += c->h + kGap;
  }
  int h = y - kGap;
  if (h < 10) h = 10; // never configure a window with height 0
  if (g_win->h() != h) g_win->size(kWidth, h);
  if (!g_win->shown()) g_win->show();
  g_win->redraw();
}

void PopupWindow::close_card(uint32_t id, CloseReason r) {
  auto it = std::find_if(g_cards.begin(), g_cards.end(), [id](auto &c) { return c->n.id == id; });
  if (it == g_cards.end()) return;
  if (g_hover_card == it->get()) g_hover_card = nullptr;
  g_cards.erase(it);
  emit_closed(id, r);
  layout_cards();
}

void add_note(Note n) {
  bool dnd, muted;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    dnd = g_dnd;
    muted = std::find(g_muted.begin(), g_muted.end(), n.app) != g_muted.end();
    if (!n.transient && g_max_history > 0) {
      g_history.erase(std::remove_if(g_history.begin(), g_history.end(),
                                     [&](const fld::HistoryEntry &e) { return e.id == n.id; }),
                      g_history.end());
      std::string ic = n.icon.empty() ? n.desktop_entry : n.icon;
      g_history.insert(g_history.begin(), {n.id, n.app, fld::notify_strip_markup(n.summary),
                                           fld::notify_strip_markup(n.body), ic, n.time});
      if ((int)g_history.size() > g_max_history) g_history.resize(g_max_history);
    }
  }
  if (!n.transient) emit_changed();
  // replacing: update the card in place
  for (auto &c : g_cards) {
    if (c->n.id == n.id) {
      if (c->own_img) delete c->img;
      c->img = nullptr;
      c->own_img = false;
      c->n = std::move(n);
      prepare_card(c.get());
      layout_cards();
      return;
    }
  }
  // do not disturb and muted apps: only critical notifications pop up
  if ((dnd || muted) && n.urgency != 2) return;
  auto c = std::make_unique<Card>();
  c->n = std::move(n);
  prepare_card(c.get());
  g_cards.insert(g_cards.begin(), std::move(c));
  while ((int)g_cards.size() > kMaxVisible) {
    uint32_t id = g_cards.back()->n.id;
    g_cards.pop_back();
    emit_closed(id, EXPIRED);
  }
  layout_cards();
}

void tick(void *) {
  const double dt = 0.25;
  if (!g_hover) {
    std::vector<uint32_t> expired;
    for (auto &c : g_cards) {
      if (c->remaining < 0) continue;
      c->remaining -= dt;
      if (c->remaining <= 0) expired.push_back(c->n.id);
    }
    for (uint32_t id : expired) PopupWindow::close_card(id, EXPIRED);
  }
  // closing a card ran ensure_tick(): keep a single timer
  Fl::remove_timeout(tick);
  for (auto &c : g_cards)
    if (c->remaining >= 0) {
      Fl::repeat_timeout(dt, tick);
      return;
    }
}

// ---- D-Bus (GLib thread) ---------------------------------------------------

Note parse_notify(GVariant *params) {
  Note n;
  const char *app, *icon, *summary, *body;
  guint32 replaces;
  GVariantIter *acts, *hints;
  gint32 timeout;
  g_variant_get(params, "(&su&s&s&sasa{sv}i)", &app, &replaces, &icon, &summary, &body, &acts, &hints, &timeout);
  n.app = app;
  n.icon = icon;
  n.summary = summary;
  n.body = body;
  n.timeout = timeout;
  n.id = replaces ? replaces : g_next_id++;
  n.time = (int64_t)time(nullptr);
  const char *s;
  std::vector<std::string> list;
  while (g_variant_iter_next(acts, "&s", &s)) list.push_back(s);
  for (size_t i = 0; i + 1 < list.size(); i += 2) n.actions.push_back({list[i], list[i + 1]});
  g_variant_iter_free(acts);
  const char *key;
  GVariant *v;
  while (g_variant_iter_next(hints, "{&sv}", &key, &v)) {
    std::string k = key;
    if (k == "urgency") {
      if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE)) n.urgency = g_variant_get_byte(v);
      else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) n.urgency = (int)g_variant_get_uint32(v);
      else if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32)) n.urgency = g_variant_get_int32(v);
      n.urgency = std::clamp(n.urgency, 0, 2);
    } else if (k == "category" && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
      n.category = g_variant_get_string(v, nullptr);
    } else if (k == "desktop-entry" && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
      n.desktop_entry = g_variant_get_string(v, nullptr);
    } else if ((k == "image-path" || k == "image_path") && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
      n.image_path = g_variant_get_string(v, nullptr);
    } else if (k == "transient" && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
      n.transient = g_variant_get_boolean(v);
    } else if (k == "resident" && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
      n.resident = g_variant_get_boolean(v);
    } else if ((k == "image-data" || k == "image_data" || k == "icon_data") &&
               g_variant_is_of_type(v, G_VARIANT_TYPE("(iiibiiay)"))) {
      gint32 w, h, rs, bits, ch;
      gboolean alpha;
      GVariant *data;
      g_variant_get(v, "(iiibii@ay)", &w, &h, &rs, &alpha, &bits, &ch, &data);
      gsize len = 0;
      const unsigned char *px = (const unsigned char *)g_variant_get_fixed_array(data, &len, 1);
      if (fld::notify_image_to_rgba(w, h, rs, alpha, bits, ch, px, len, n.pixels)) {
        n.img_w = w;
        n.img_h = h;
      }
      g_variant_unref(data);
    }
    g_variant_unref(v);
  }
  g_variant_iter_free(hints);
  if (n.app.empty()) n.app = n.desktop_entry;
  return n;
}

void method_call(GDBusConnection *, const char *, const char *, const char *iface, const char *method,
                 GVariant *params, GDBusMethodInvocation *inv, gpointer) {
  std::string m = method;
  if (!strcmp(iface, "org.freedesktop.Notifications")) {
    if (m == "GetCapabilities") {
      const char *caps[] = {"actions", "body", "body-markup", "icon-static", "persistence", nullptr};
      g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", caps));
    } else if (m == "GetServerInformation") {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(ssss)", "fl-notifyd", "fldesktop", FLD_VERSION, "1.2"));
    } else if (m == "Notify") {
      Note n = parse_notify(params);
      uint32_t id = n.id;
      fld::ui_call([n]() mutable { add_note(std::move(n)); });
      g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", id));
    } else if (m == "CloseNotification") {
      guint32 id;
      g_variant_get(params, "(u)", &id);
      fld::ui_call([id] { PopupWindow::close_card(id, CLOSED_BY_CALL); });
      g_dbus_method_invocation_return_value(inv, nullptr);
    } else {
      g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
    }
    return;
  }
  // org.fldesktop.Notifications
  if (m == "GetHistory") {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(ussssx)"));
    bool dnd;
    {
      std::lock_guard<std::mutex> l(g_mutex);
      for (auto &e : g_history)
        g_variant_builder_add(&b, "(ussssx)", e.id, e.app.c_str(), e.summary.c_str(), e.body.c_str(), e.icon.c_str(),
                              (gint64)e.time);
      dnd = g_dnd;
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(a(ussssx)b)", &b, (gboolean)dnd));
  } else if (m == "Clear") {
    {
      std::lock_guard<std::mutex> l(g_mutex);
      g_history.clear();
    }
    g_dbus_method_invocation_return_value(inv, nullptr);
    emit_changed();
  } else if (m == "Remove") {
    guint32 id;
    g_variant_get(params, "(u)", &id);
    {
      std::lock_guard<std::mutex> l(g_mutex);
      g_history.erase(std::remove_if(g_history.begin(), g_history.end(),
                                     [id](const fld::HistoryEntry &e) { return e.id == id; }),
                      g_history.end());
    }
    g_dbus_method_invocation_return_value(inv, nullptr);
    emit_changed();
  } else if (m == "SetDoNotDisturb") {
    gboolean on;
    g_variant_get(params, "(b)", &on);
    {
      std::lock_guard<std::mutex> l(g_mutex);
      g_dnd = on;
    }
    g_dbus_method_invocation_return_value(inv, nullptr);
    fld::ui_call([on] { save_dnd(on); });
    emit_changed();
  } else {
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
  }
}

bool g_replace = false;

void start_dbus() {
  fld::glib_call([] {
    GError *err = nullptr;
    g_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!g_conn) {
      fprintf(stderr, "fl-notifyd: no session bus: %s\n", err ? err->message : "?");
      exit(1);
    }
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(kIntrospection, nullptr);
    static const GDBusInterfaceVTable vtable = {method_call, nullptr, nullptr, {}};
    for (int i = 0; info->interfaces[i]; i++)
      g_dbus_connection_register_object(g_conn, kPath, info->interfaces[i], &vtable, nullptr, nullptr, nullptr);
    GBusNameOwnerFlags flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
    if (g_replace) flags = (GBusNameOwnerFlags)(flags | G_BUS_NAME_OWNER_FLAGS_REPLACE);
    g_bus_own_name_on_connection(
        g_conn, "org.freedesktop.Notifications", flags, nullptr,
        [](GDBusConnection *, const char *, gpointer) {
          fprintf(stderr, "fl-notifyd: another notification server owns org.freedesktop.Notifications\n");
          exit(0);
        },
        nullptr, nullptr);
  });
}

} // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-r" || a == "--replace") g_replace = true;
    else if (a == "-h" || a == "--help") {
      printf("Usage: fl-notifyd [--replace]\nShows desktop notifications (org.freedesktop.Notifications).\n");
      return 0;
    }
  }
  fld::init_app("fl-notifyd", argc, argv);
  Fl::lock();
  load_settings();
  static fld::ConfigWatcher watch(config_path(), [] {
    load_settings();
    emit_changed();
  });
  g_win = new PopupWindow();
  fld::glib_start();
  start_dbus();
  // stays running without a window
  while (true) Fl::wait(1e20);
  return 0;
}
