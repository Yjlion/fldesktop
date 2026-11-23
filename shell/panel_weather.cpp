// fl-panel: the weather (Open-Meteo, no key needed). Hidden until a place
// is chosen ([Weather] in panel.ini: City, Latitude, Longitude, Units);
// updated every half hour with curl (or wget), cached in
// ~/.cache/fldesktop/weather.json.
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <unistd.h>

#include <cmath>
#include <ctime>
#include <string>
#include <thread>

#include "applets_util.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/util.h"
#include "panel.h"

namespace {

fld::FlatButton *g_btn;
applets::Weather g_w;
std::string g_city;
double g_lat = 0, g_lon = 0;
bool g_fahrenheit = false, g_have_place = false, g_fetching = false;

std::string ini_path() { return fld::fld_config_dir() + "/panel.ini"; }
std::string cache_path() { return fld::cache_home() + "/fldesktop/weather.json"; }

// curl or wget; "" on failure
std::string fetch(const std::string &url) {
  std::string out;
  int rc;
  if (!fld::which("curl").empty()) rc = fld::run_capture({"curl", "-fsSL", "--max-time", "20", url}, out);
  else if (!fld::which("wget").empty()) rc = fld::run_capture({"wget", "-qO-", "--timeout=20", url}, out);
  else return "";
  return rc == 0 ? out : "";
}

void show() {
  if (!g_w.ok) {
    if (g_btn->visible()) {
      g_btn->hide();
      relayout();
    }
    return;
  }
  panel_icon(g_btn, applets::weather_icon(g_w.code, g_w.day), "weather-overcast");
  static std::string label, tip;
  label = applets::degrees(g_w.temp);
  g_btn->label(label.c_str());
  tip = g_city + ": " + applets::weather_text(g_w.code) + ", " + label;
  g_btn->tooltip(tip.c_str());
  if (!g_btn->visible()) {
    g_btn->show();
    relayout();
  }
  g_btn->redraw();
}

void update(void *);

void start_fetch() {
  if (!g_have_place || g_fetching) return;
  g_fetching = true;
  std::string url = applets::forecast_url(g_lat, g_lon, g_fahrenheit);
  std::thread([url] {
    std::string *json = new std::string(fetch(url));
    Fl::awake([](void *d) {
      std::unique_ptr<std::string> j((std::string *)d);
      g_fetching = false;
      applets::Weather w = applets::parse_forecast(*j);
      if (w.ok) {
        g_w = w;
        fld::mkdir_p(fld::dirname(cache_path()));
        fld::write_file(cache_path(), *j);
        show();
        Fl::repeat_timeout(30 * 60, update);
      } else {
        Fl::repeat_timeout(5 * 60, update); // offline: try again soon
      }
    }, json);
  }).detach();
}

void update(void *) { start_fetch(); }

// asks for a place and saves it (the panel restarts with it)
void choose_place() {
  const char *q = fl_input("Weather for which place?", g_city.c_str());
  if (!q || !*q) return;
  fl_cursor(FL_CURSOR_WAIT);
  Fl::flush();
  std::vector<applets::Place> places = applets::parse_places(fetch(applets::search_url(q)));
  fl_cursor(FL_CURSOR_DEFAULT);
  if (places.empty()) {
    fl_alert("No place called \"%s\" was found (or there is no network).", q);
    return;
  }
  int pick = 0;
  if (places.size() > 1) {
    Fl_Double_Window d(380, 260, "Which One?");
    d.set_modal();
    Fl_Hold_Browser list(10, 10, 360, 200);
    for (auto &p : places)
      list.add(fld::escape_at(p.name + (p.region.empty() ? "" : ", " + p.region) + ", " + p.country).c_str());
    list.select(1);
    Fl_Button cancel(160, 220, 100, 30, "Cancel");
    Fl_Return_Button ok(268, 220, 102, 30, "Choose");
    int result = 0;
    cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 2; }, &result);
    ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
    list.callback([](Fl_Widget *, void *r) {
      if (Fl::event_clicks()) *(int *)r = 1;
    }, &result);
    d.end();
    d.show();
    while (!result && d.shown()) Fl::wait();
    if (result != 1 || list.value() <= 0) return;
    pick = list.value() - 1;
  }
  const applets::Place &p = places[(size_t)pick];
  fld::Ini ini;
  ini.load(ini_path());
  ini.set("Weather", "City", p.name);
  char b[32];
  snprintf(b, sizeof b, "%.4f", p.lat);
  ini.set("Weather", "Latitude", b);
  snprintf(b, sizeof b, "%.4f", p.lon);
  ini.set("Weather", "Longitude", b);
  ini.set_bool("Weather", "Enabled", true);
  unlink(cache_path().c_str());
  ini.save(ini_path());
}

void popup(Fl_Widget *w, void *) {
  int rows = (int)g_w.days.size();
  PanelPopup pop(300, 150 + 26 * rows);
  Fl_Box *ic = new Fl_Box(12, 12, 64, 64);
  ic->image(fld::icon_first({applets::weather_icon(g_w.code, g_w.day), "weather-overcast"}, 64));
  static std::string city, big, more;
  city = fld::escape_at(g_city);
  Fl_Box *c = new Fl_Box(88, 10, 200, 22, city.c_str());
  c->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  c->labelfont(FL_HELVETICA_BOLD);
  big = applets::degrees(g_w.temp) + "  " + applets::weather_text(g_w.code);
  Fl_Box *b = new Fl_Box(88, 32, 200, 30, big.c_str());
  b->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  b->labelsize(18);
  more = "Feels like " + applets::degrees(g_w.feels) + ", wind " + std::to_string((int)std::lround(g_w.wind)) +
         (g_fahrenheit ? " mph" : " km/h") + (g_w.humidity >= 0 ? ", humidity " + std::to_string(g_w.humidity) + "%" : "");
  Fl_Box *m = new Fl_Box(88, 62, 200, 34, more.c_str());
  m->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  m->labelsize(11);
  m->labelcolor(fld::palette().fg_dim);
  static std::vector<std::string> keep;
  keep.clear();
  keep.reserve(g_w.days.size());
  int y = 104;
  for (const applets::Day &d : g_w.days) {
    struct tm tm {};
    strptime(d.date.c_str(), "%Y-%m-%d", &tm);
    char day[32];
    strftime(day, sizeof day, "%a", &tm);
    keep.push_back(std::string(day) + "\t" + applets::weather_text(d.code) + "\t" + applets::degrees(d.tmin) + " / " +
                   applets::degrees(d.tmax));
    Fl_Box *ib = new Fl_Box(12, y, 22, 22);
    ib->image(fld::icon_first({applets::weather_icon(d.code, true), "weather-overcast"}, 20));
    Fl_Box *t = new Fl_Box(40, y, 250, 22);
    t->copy_label(fld::escape_at(keep.back().substr(0, keep.back().find('\t')) + "   " +
                                 applets::weather_text(d.code) + "   " + applets::degrees(d.tmin) + " / " +
                                 applets::degrees(d.tmax)).c_str());
    t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    t->labelsize(12);
    y += 26;
  }
  Fl_Box *src = new Fl_Box(12, y + 8, 160, 22, "Weather data by Open-Meteo");
  src->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  src->labelsize(10);
  src->labelcolor(fld::palette().fg_dim);
  Fl_Button *ch = new Fl_Button(180, y + 6, 108, 26, "Change Place…");
  ch->labelsize(12);
  ch->callback([](Fl_Widget *b, void *) {
    b->window()->hide();
    choose_place();
  });
  pop.end();
  pop.popup_at(w, false);
}

} // namespace

void weather_menu_items(fld::Menu &m) {
  if (!g_have_place) m.item("Show the Weather…", [] { choose_place(); });
}

fld::FlatButton *weather_create() {
  fld::Ini ini;
  ini.load(ini_path());
  g_btn = make_btn(64, "weather-overcast", "Weather", popup);
  g_btn->hide();
  g_city = ini.get("Weather", "City", "");
  g_lat = atof(ini.get("Weather", "Latitude", "0").c_str());
  g_lon = atof(ini.get("Weather", "Longitude", "0").c_str());
  g_fahrenheit = fld::lower(ini.get("Weather", "Units", "")) == "fahrenheit";
  g_have_place = ini.get_bool("Weather", "Enabled", false) && !g_city.empty();
  panel_context_menu(g_btn, [](fld::Menu &m) {
    m.item("Update Now", [] { start_fetch(); });
    m.item("Change Place…", [] { choose_place(); });
    m.item("Hide the Weather", [] {
      fld::Ini c;
      c.load(ini_path());
      c.set_bool("Weather", "Enabled", false);
      c.save(ini_path());
    }).divider();
    panel_menu_items(m);
  });
  if (!g_have_place) return g_btn;
  // the last forecast at once, a fresh one soon
  std::string cached;
  if (fld::read_file(cache_path(), cached)) {
    g_w = applets::parse_forecast(cached);
    show();
  }
  Fl::add_timeout(2, update);
  return g_btn;
}
