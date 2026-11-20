// org.freedesktop.impl.portal.{Screenshot,Access,Account,Wallpaper}
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Radio_Round_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_draw.H>

#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>
#include <thread>

#include "fld/desktop_entry.h"
#include "fld/glib_bridge.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "portal.h"

namespace portal {

std::string app_name(const std::string &app_id) {
  if (app_id.empty()) return "An application";
  fld::DesktopEntry e;
  if (fld::find_application(app_id, e) && !e.name.empty()) return e.name;
  return app_id;
}

namespace {

// A small question dialog: icon, heading, text, optional choices, two buttons.
class AskWindow : public Fl_Double_Window {
public:
  std::function<void(bool, const std::vector<std::pair<std::string, std::string>> &)> done;
  std::vector<fld::FileChoice> choices;
  std::vector<Fl_Widget *> widgets;
  bool finished = false;
  Fl_Box *image_box = nullptr;

  AskWindow(const std::string &title, const std::string &icon, const std::string &heading, const std::string &body,
            const std::string &grant, const std::string &deny, const std::vector<fld::FileChoice> &ch,
            Fl_Image *preview = nullptr)
      : Fl_Double_Window(460, 200), choices(ch) {
    copy_label(title.c_str());
    int y = 16;
    Fl_Box *ic = new Fl_Box(16, y, 48, 48);
    ic->image(fld::icon_first({icon, "dialog-question"}, 48));
    Fl_Box *hd = new Fl_Box(76, y, 368, 28);
    hd->copy_label(fld::escape_at(heading).c_str());
    hd->labelfont(FL_HELVETICA_BOLD);
    hd->labelsize(FL_NORMAL_SIZE + 2);
    hd->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
    y += 30;
    if (!body.empty()) {
      fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
      int bw = 368, bh = 0;
      fl_measure(body.c_str(), bw, bh, 0);
      bh = std::max(bh, 20);
      Fl_Box *b = new Fl_Box(76, y, 368, bh);
      b->copy_label(fld::escape_at(body).c_str());
      b->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
      y += bh + 8;
    }
    y = std::max(y, 72);
    if (preview) {
      image_box = new Fl_Box(76, y, 368, preview->h() + 8);
      image_box->image(preview);
      image_box->box(FL_DOWN_BOX);
      y += preview->h() + 16;
    }
    for (auto &c : choices) {
      if (c.options.empty()) {
        auto *cb = new Fl_Check_Button(76, y, 368, 26);
        cb->copy_label(c.label.c_str());
        cb->value(c.selected == "true");
        widgets.push_back(cb);
        y += 30;
      } else {
        auto *l = new Fl_Box(76, y, 368, 22);
        l->copy_label(c.label.c_str());
        l->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
        auto *chw = new Fl_Choice(76, y + 24, 260, 28);
        int sel = 0;
        for (size_t i = 0; i < c.options.size(); i++) {
          chw->add(c.options[i].second.c_str());
          if (c.options[i].first == c.selected) sel = (int)i;
        }
        chw->value(sel);
        widgets.push_back(chw);
        y += 58;
      }
    }
    y += 8;
    std::string d = deny, g = grant;
    for (std::string *s : {&d, &g}) s->erase(std::remove(s->begin(), s->end(), '_'), s->end());
    Fl_Button *no = new Fl_Button(460 - 16 - 120 - 8 - 120, y, 120, 30);
    no->copy_label(d.c_str());
    no->callback([](Fl_Widget *w, void *) { ((AskWindow *)w->window())->finish(false); });
    Fl_Return_Button *yes = new Fl_Return_Button(460 - 16 - 120, y, 120, 30);
    yes->copy_label(g.c_str());
    yes->callback([](Fl_Widget *w, void *) { ((AskWindow *)w->window())->finish(true); });
    end();
    size(460, y + 30 + 16);
    callback([](Fl_Widget *w, void *) { ((AskWindow *)w)->finish(false); });
    fld::set_window_icon(this, icon.c_str());
  }

  void finish(bool ok) {
    if (finished) return;
    finished = true;
    hide();
    std::vector<std::pair<std::string, std::string>> out;
    for (size_t i = 0; i < choices.size() && i < widgets.size(); i++) {
      auto &c = choices[i];
      if (c.options.empty()) out.push_back({c.id, ((Fl_Check_Button *)widgets[i])->value() ? "true" : "false"});
      else {
        int v = ((Fl_Choice *)widgets[i])->value();
        out.push_back({c.id, v >= 0 && v < (int)c.options.size() ? c.options[v].first : ""});
      }
    }
    auto cb = done;
    Fl::delete_widget(this);
    if (cb) cb(ok, out);
  }
};

// Shows an AskWindow tied to a portal request handle (Close cancels it).
void ask(const std::string &handle, AskWindow *w,
         std::function<void(bool, const std::vector<std::pair<std::string, std::string>> &)> done) {
  auto alive = std::make_shared<bool>(true);
  w->done = [handle, done, alive](bool ok, const std::vector<std::pair<std::string, std::string>> &c) {
    *alive = false;
    request_end(handle);
    done(ok, c);
  };
  w->show();
  request_begin(handle, [w, alive] {
    if (*alive) w->finish(false);
  });
}

GVariant *choices_variant(const std::vector<std::pair<std::string, std::string>> &c) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a(ss)"));
  for (auto &p : c) g_variant_builder_add(&b, "(ss)", p.first.c_str(), p.second.c_str());
  return g_variant_builder_end(&b);
}

// ---- screenshot -------------------------------------------------------------

std::string screenshot_path() {
  std::string dir = fld::user_dir("PICTURES");
  if (dir.empty() || !fld::is_dir(dir)) dir = fld::home_dir();
  dir = fld::path_join(dir, "Screenshots");
  fld::mkdir_p(dir);
  return fld::unique_name(dir, fld::format_time(time(nullptr), "Screenshot from %Y-%m-%d %H-%M-%S.png"));
}

// grim (and slurp for an area); runs in a worker thread, replies itself
void take_screenshot(GDBusMethodInvocation *inv, bool area, int delay) {
  std::thread([inv, area, delay] {
    if (delay > 0) sleep((unsigned)delay);
    std::string out = screenshot_path();
    std::vector<std::string> cmd = {"grim"};
    if (area) {
      std::string geo;
      if (fld::run_capture({"slurp"}, geo) != 0 || fld::trim(geo).empty()) {
        reply(inv, CANCELLED);
        return;
      }
      cmd.push_back("-g");
      cmd.push_back(fld::trim(geo));
    }
    cmd.push_back(out);
    std::string dummy;
    if (fld::which("grim").empty() || fld::run_capture(cmd, dummy) != 0 || !fld::file_exists(out)) {
      g_printerr("xdg-desktop-portal-fl: grim failed (is it installed?)\n");
      reply(inv, OTHER);
      return;
    }
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "uri", g_variant_new_string(path_to_uri(out).c_str()));
    reply(inv, SUCCESS, g_variant_builder_end(&b));
  }).detach();
}

class ShotWindow : public Fl_Double_Window {
public:
  GDBusMethodInvocation *inv;
  std::string handle;
  Fl_Radio_Round_Button *whole, *area;
  Fl_Spinner *delay;
  bool finished = false;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  ShotWindow(GDBusMethodInvocation *i, const std::string &h, const std::string &app)
      : Fl_Double_Window(380, 220, "Screenshot"), inv(i), handle(h) {
    Fl_Box *t = new Fl_Box(16, 12, 348, 28);
    t->copy_label(fld::escape_at(app + " wants to take a screenshot").c_str());
    t->labelfont(FL_HELVETICA_BOLD);
    t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    whole = new Fl_Radio_Round_Button(24, 48, 300, 26, "Whole screen");
    area = new Fl_Radio_Round_Button(24, 76, 300, 26, "Select an area");
    whole->value(1);
    delay = new Fl_Spinner(150, 112, 70, 28, "Delay (seconds):");
    delay->range(0, 30);
    delay->value(0);
    Fl_Button *c = new Fl_Button(124, 172, 112, 30, "Cancel");
    c->callback([](Fl_Widget *w, void *) { ((ShotWindow *)w->window())->finish(false); });
    Fl_Return_Button *ok = new Fl_Return_Button(244, 172, 120, 30, "Take Screenshot");
    ok->callback([](Fl_Widget *w, void *) { ((ShotWindow *)w->window())->finish(true); });
    end();
    callback([](Fl_Widget *w, void *) { ((ShotWindow *)w)->finish(false); });
    fld::set_window_icon(this, "applets-screenshooter");
  }
  void finish(bool ok) {
    if (finished) return;
    finished = true;
    *alive = false;
    request_end(handle);
    hide();
    if (ok) {
      // let the compositor drop the dialog before the shot
      int d = (int)delay->value();
      take_screenshot(inv, area->value(), d > 0 ? d : 1);
    } else {
      reply(inv, CANCELLED);
    }
    Fl::delete_widget(this);
  }
};

// PickColor: slurp -p picks a point, grim writes one PPM pixel
void pick_color(GDBusMethodInvocation *inv) {
  std::thread([inv] {
    std::string pt, ppm;
    if (fld::run_capture({"slurp", "-p", "-f", "%x,%y 1x1"}, pt) != 0 || fld::trim(pt).empty()) {
      reply(inv, CANCELLED);
      return;
    }
    if (fld::run_capture({"grim", "-g", fld::trim(pt), "-t", "ppm", "-"}, ppm) != 0 || ppm.size() < 3) {
      reply(inv, OTHER);
      return;
    }
    // "P6\n1 1\n255\n" followed by 3 bytes
    size_t pos = 0;
    int fields = 0;
    while (pos < ppm.size() && fields < 4) {
      while (pos < ppm.size() && isspace((unsigned char)ppm[pos])) pos++;
      while (pos < ppm.size() && !isspace((unsigned char)ppm[pos])) pos++;
      fields++;
    }
    pos++; // the single whitespace after maxval
    if (pos + 3 > ppm.size()) {
      reply(inv, OTHER);
      return;
    }
    double r = (unsigned char)ppm[pos] / 255.0, g = (unsigned char)ppm[pos + 1] / 255.0,
           b = (unsigned char)ppm[pos + 2] / 255.0;
    GVariantBuilder vb;
    g_variant_builder_init(&vb, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&vb, "{sv}", "color", g_variant_new("(ddd)", r, g, b));
    reply(inv, SUCCESS, g_variant_builder_end(&vb));
  }).detach();
}

} // namespace

void screenshot_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  const char *handle, *app_id, *parent;
  GVariant *options;
  g_variant_get(params, "(&o&s&s@a{sv})", &handle, &app_id, &parent, &options);
  bool interactive = opt_bool(options, "interactive");
  bool checked = opt_bool(options, "permission_store_checked");
  g_variant_unref(options);
  std::string h = handle, app = app_name(app_id);
  if (method == "PickColor") {
    pick_color(inv);
    return;
  }
  if (method != "Screenshot") {
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method.c_str());
    return;
  }
  if (interactive) {
    fld::ui_call([inv, h, app] {
      ShotWindow *w = new ShotWindow(inv, h, app);
      auto alive = w->alive;
      w->show();
      request_begin(h, [w, alive] {
        if (*alive) w->finish(false);
      });
    });
  } else if (!checked) {
    fld::ui_call([inv, h, app] {
      auto *w = new AskWindow("Screenshot", "applets-screenshooter", "Allow " + app + " to take screenshots?",
                              app + " wants to see the contents of your screen.", "Allow", "Deny", {});
      ask(h, w, [inv](bool ok, const std::vector<std::pair<std::string, std::string>> &) {
        if (ok) take_screenshot(inv, false, 0);
        else reply(inv, CANCELLED);
      });
    });
  } else {
    take_screenshot(inv, false, 0);
  }
}

void access_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  const char *handle, *app_id, *parent, *title, *subtitle, *body;
  GVariant *options;
  g_variant_get(params, "(&o&s&s&s&s&s@a{sv})", &handle, &app_id, &parent, &title, &subtitle, &body, &options);
  std::string deny = opt_string(options, "deny_label", "Deny"), grant = opt_string(options, "grant_label", "Allow");
  std::string icon = opt_string(options, "icon", "dialog-question");
  std::vector<fld::FileChoice> ch;
  if (GVariant *c = g_variant_lookup_value(options, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"))) {
    ch = parse_choices(c);
    g_variant_unref(c);
  }
  g_variant_unref(options);
  std::string h = handle, t = title, st = subtitle, b = body;
  std::string text = st + (b.empty() ? "" : "\n\n" + b);
  std::string app = app_name(app_id);
  fld::ui_call([inv, h, t, text, deny, grant, icon, ch, app] {
    auto *w = new AskWindow(app, icon, t, text, grant, deny, ch);
    ask(h, w, [inv](bool ok, const std::vector<std::pair<std::string, std::string>> &c) {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
      if (!c.empty()) g_variant_builder_add(&b, "{sv}", "choices", choices_variant(c));
      reply(inv, ok ? SUCCESS : CANCELLED, g_variant_builder_end(&b));
    });
  });
}

void account_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  const char *handle, *app_id, *window;
  GVariant *options;
  g_variant_get(params, "(&o&s&s@a{sv})", &handle, &app_id, &window, &options);
  std::string reason = opt_string(options, "reason");
  g_variant_unref(options);
  struct passwd *pw = getpwuid(getuid());
  std::string user = pw ? pw->pw_name : fld::getenv_or("USER", "user");
  std::string real = pw && pw->pw_gecos ? fld::split(pw->pw_gecos, ',', true)[0] : "";
  if (real.empty()) real = user;
  std::string face = fld::path_join(fld::home_dir(), ".face");
  std::string image = fld::file_exists(face) ? path_to_uri(face) : "";
  std::string h = handle, app = app_name(app_id);
  std::string text = "Name: " + real + "\nUser name: " + user;
  if (!reason.empty()) text = reason + "\n\n" + text;
  fld::ui_call([inv, h, app, text, user, real, image] {
    auto *w = new AskWindow("Share Details", "avatar-default", "Share your personal information with " + app + "?",
                            text, "Share", "Cancel", {});
    ask(h, w, [inv, user, real, image](bool ok, const std::vector<std::pair<std::string, std::string>> &) {
      if (!ok) {
        reply(inv, CANCELLED);
        return;
      }
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&b, "{sv}", "id", g_variant_new_string(user.c_str()));
      g_variant_builder_add(&b, "{sv}", "name", g_variant_new_string(real.c_str()));
      g_variant_builder_add(&b, "{sv}", "image", g_variant_new_string(image.c_str()));
      reply(inv, SUCCESS, g_variant_builder_end(&b));
    });
  });
}

void wallpaper_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  const char *handle, *app_id, *parent, *uri;
  GVariant *options;
  g_variant_get(params, "(&o&s&s&s@a{sv})", &handle, &app_id, &parent, &uri, &options);
  bool preview = opt_bool(options, "show-preview");
  std::string set_on = opt_string(options, "set-on", "both");
  g_variant_unref(options);
  auto answer = [inv](Response r) {
    fld::glib_call([inv, r] { g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", (guint32)r)); });
  };
  gchar *p = g_filename_from_uri(uri, nullptr, nullptr);
  std::string src = p ? p : "";
  g_free(p);
  if (src.empty() || !fld::file_exists(src)) {
    answer(OTHER);
    return;
  }
  if (set_on == "lockscreen") {
    // fldesktop has no lock screen background of its own
    answer(SUCCESS);
    return;
  }
  std::string h = handle, app = app_name(app_id);
  auto apply = [src, answer] {
    // keep a copy: the document portal path goes away with the app
    std::string dir = fld::path_join(fld::data_home(), "fldesktop/wallpapers");
    fld::mkdir_p(dir);
    std::string dst = fld::path_join(dir, fld::basename(src)), data;
    if (dst != src && fld::read_file(src, data, 256u << 20)) fld::write_file(dst, data);
    else dst = src;
    fld::Ini ini;
    ini.load(fld::desktop_config_path());
    ini.set("Desktop", "Wallpaper", dst);
    ini.save(fld::desktop_config_path());
    answer(SUCCESS);
  };
  if (!preview) {
    fld::ui_call(apply);
    return;
  }
  fld::ui_call([h, app, src, apply, answer] {
    Fl_Image *img = fld::load_image(src);
    Fl_Image *thumb = nullptr;
    if (img) {
      double s = std::min(360.0 / img->w(), 200.0 / img->h());
      thumb = img->copy(std::max(1, (int)(img->w() * s)), std::max(1, (int)(img->h() * s)));
      delete img;
    }
    auto *w = new AskWindow("Set Background", "preferences-desktop-wallpaper", "Set the desktop background?",
                            app + " wants to change your desktop background.", "Set", "Cancel", {}, thumb);
    ask(h, w, [apply, answer, thumb](bool ok, const std::vector<std::pair<std::string, std::string>> &) {
      delete thumb;
      if (ok) apply();
      else answer(CANCELLED);
    });
  });
}

} // namespace portal
