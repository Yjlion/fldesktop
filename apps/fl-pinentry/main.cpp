// pinentry-fldesktop: GnuPG's passphrase prompt (gpg-agent runs it and
// talks Assuan over stdin/stdout). Optionally keeps passphrases in the
// password manager (the Secret Service) when gpg-agent allows it.
//
//   pinentry-fldesktop [--display D] [--ttyname T] [... the usual pinentry options]
//
// Without a graphical session it hands over to pinentry-curses or pinentry-tty.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "assuan.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#ifdef FLD_HAVE_SECRET
#include "secret.h"
#endif

namespace {

bool g_ui_ready = false;

void ui_init() {
  if (g_ui_ready) return;
  g_ui_ready = true;
  fld::init_app("pinentry-fldesktop");
}

// "_OK" (GTK mnemonics, as gpg-agent sends them) -> "&OK"
std::string button_label(const std::string &s, const char *fallback) {
  std::string in = s.empty() ? fallback : s, r;
  bool mn = false;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '&') r += "&&";
    else if (in[i] == '_' && !mn && i + 1 < in.size()) {
      r += '&';
      mn = true;
    } else if (in[i] == '@') r += "@@";
    else r += in[i];
  }
  return r;
}

// the label of an Fl_Box that wraps: '@' escaped
std::string box_text(const std::string &s) { return fld::escape_at(s); }

#ifdef FLD_HAVE_SECRET
// pinentry-gnome3's schema, so passphrases saved there are found here too
std::vector<std::pair<std::string, std::string>> cache_attrs(const std::string &keyinfo) {
  return {{"xdg:schema", "org.gnupg.Passphrase"}, {"keygrip", keyinfo}};
}
bool g_secret_ok = false, g_secret_tried = false;
bool secret_ready() {
  if (!g_secret_tried) {
    g_secret_tried = true;
    std::string err;
    g_secret_ok = secret::connect(err);
  }
  return g_secret_ok;
}
bool cache_lookup(const std::string &keyinfo, std::string &pin) {
  if (!secret_ready()) return false;
  for (auto &it : secret::search(cache_attrs(keyinfo))) {
    std::string err;
    if (it.locked && !secret::unlock(it.path, err)) continue;
    if (secret::get_secret(it.path, pin, err)) return true;
  }
  return false;
}
void cache_store(const std::string &keyinfo, const std::string &desc, const std::string &pin) {
  if (!secret_ready()) return;
  std::string err;
  auto found = secret::search(cache_attrs(keyinfo));
  if (!found.empty()) {
    secret::set_secret(found[0].path, pin, err);
    return;
  }
  std::string col = secret::default_collection();
  if (col.empty()) return;
  std::string first = desc.substr(0, desc.find('\n'));
  secret::create_item(col, "GnuPG: " + (first.empty() ? keyinfo : first), cache_attrs(keyinfo), pin, err);
}
void cache_forget(const std::string &keyinfo) {
  if (!secret_ready()) return;
  std::string err;
  for (auto &it : secret::search(cache_attrs(keyinfo))) secret::delete_item(it.path, err);
}
#endif

// "n/KEYGRIP" asks not to cache, "s/..." (or "u/...") may be cached
bool cacheable(const assuan::Request &r) {
  return r.allow_external_cache && !r.keyinfo.empty() && r.keyinfo.compare(0, 2, "n/") != 0;
}

struct Dialog {
  Fl_Double_Window *win = nullptr;
  Fl_Secret_Input *pin = nullptr, *repeat = nullptr;
  Fl_Progress *quality = nullptr;
  Fl_Box *error = nullptr;
  Fl_Check_Button *save = nullptr;
  int result = -1; // 0 cancel, 1 ok, 2 not ok, 3 timeout
};

void timeout_cb(void *d) {
  ((Dialog *)d)->result = 3;
}

// The dialog for GETPIN (with_pin) and CONFIRM / MESSAGE
assuan::Answer run_dialog(const assuan::Request &r, bool with_pin) {
  ui_init();
  const fld::Palette &pal = fld::palette();
  const int W = 460;
  Dialog d;
  std::string title = r.title.empty() ? (with_pin ? "Passphrase" : "Confirm") : r.title;
  d.win = new Fl_Double_Window(W, 200);
  d.win->copy_label(title.c_str());
  int y = 16;
  Fl_Box *icon = new Fl_Box(16, y, 48, 48);
  icon->image(fld::icon_first({with_pin ? "dialog-password" : "dialog-question", "security-high"}, 48));
  // the description: as tall as it needs
  std::string desc = r.description.empty() ? (with_pin ? "Enter the passphrase." : "") : r.description;
  fl_font(FL_HELVETICA, 13);
  int dw = W - 92, dh = 0, ww = dw;
  fl_measure(box_text(desc).c_str(), ww, dh);
  dh = std::max(24, std::min(dh + 6, 240));
  Fl_Box *msg = new Fl_Box(76, y, dw, dh);
  msg->copy_label(box_text(desc).c_str());
  msg->labelsize(13);
  msg->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  y += std::max(56, dh + 8);
  if (!r.error.empty()) {
    d.error = new Fl_Box(76, y, dw, 22);
    d.error->copy_label(box_text(r.error).c_str());
    d.error->labelcolor(pal.danger);
    d.error->labelsize(13);
    d.error->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
    y += 26;
  }
  if (with_pin) {
    std::string prompt = r.prompt.empty() ? "Passphrase:" : r.prompt;
    if (prompt.back() != ':') prompt += ":";
    d.pin = new Fl_Secret_Input(170, y, W - 186, 28);
    d.pin->copy_label(box_text(prompt).c_str());
    y += 36;
    if (!r.repeat.empty()) {
      d.repeat = new Fl_Secret_Input(170, y, W - 186, 28);
      d.repeat->copy_label(box_text(r.repeat).c_str());
      y += 36;
    }
    if (!r.quality_bar.empty()) {
      d.quality = new Fl_Progress(170, y + 4, W - 186, 16);
      d.quality->copy_label(box_text(r.quality_bar).c_str());
      d.quality->align(FL_ALIGN_LEFT);
      d.quality->minimum(0);
      d.quality->maximum(100);
      d.quality->value(0);
      d.quality->selection_color(pal.danger);
      if (!r.quality_tooltip.empty()) d.quality->copy_tooltip(r.quality_tooltip.c_str());
      d.pin->when(FL_WHEN_CHANGED);
      d.pin->callback([](Fl_Widget *w, void *v) {
        Dialog *d = (Dialog *)v;
        int q = assuan::quality(((Fl_Input *)w)->value());
        d->quality->value((float)q);
        d->quality->selection_color(q < 40 ? fld::palette().danger : q < 70 ? fl_rgb_color(0xe5, 0xa5, 0x0a)
                                                                         : fl_rgb_color(0x2e, 0xc2, 0x7e));
        d->quality->redraw();
      }, &d);
      y += 28;
    }
#ifdef FLD_HAVE_SECRET
    if (cacheable(r) && secret_ready()) {
      d.save = new fld::CheckButton(170, y, W - 186, 26, "Save in the password manager");
      d.save->tooltip("The next time, the passphrase comes from the password manager without asking");
      y += 32;
    }
#endif
  }
  y += 8;
  int bx = W - 16;
  auto button = [&](Fl_Button *b) {
    fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
    int bw = std::max(90, (int)fl_width(b->label()) + 28);
    bx -= bw;
    b->resize(bx, y, bw, 30);
    bx -= 8;
  };
  Fl_Return_Button *ok = new Fl_Return_Button(0, 0, 10, 30);
  ok->copy_label(button_label(r.ok, with_pin ? "OK" : "OK").c_str());
  button(ok);
  ok->callback([](Fl_Widget *, void *v) { ((Dialog *)v)->result = 1; }, &d);
  if (!r.one_button) {
    if (!with_pin && !r.notok.empty()) {
      Fl_Button *no = new Fl_Button(0, 0, 10, 30);
      no->copy_label(button_label(r.notok, "No").c_str());
      button(no);
      no->callback([](Fl_Widget *, void *v) { ((Dialog *)v)->result = 2; }, &d);
    }
    Fl_Button *cancel = new Fl_Button(0, 0, 10, 30);
    cancel->copy_label(button_label(r.cancel, "Cancel").c_str());
    button(cancel);
    cancel->callback([](Fl_Widget *, void *v) { ((Dialog *)v)->result = 0; }, &d);
  }
  d.win->size(W, y + 46);
  d.win->end();
  d.win->callback([](Fl_Widget *, void *v) {
    // Escape and the close button cancel (a message just closes)
    ((Dialog *)v)->result = 0;
  }, &d);
  d.win->set_modal();
  fld::set_window_icon(d.win, "dialog-password");
  d.win->show();
  if (d.pin) d.pin->take_focus();
  if (r.timeout > 0) Fl::add_timeout(r.timeout, timeout_cb, &d);

  assuan::Answer a;
  for (;;) {
    d.result = -1;
    while (d.result < 0 && d.win->shown()) Fl::wait();
    if (d.result < 0) d.result = 0;
    if (d.result == 1 && d.repeat && strcmp(d.pin->value(), d.repeat->value()) != 0) {
      // the two entries differ: say so and ask again
      if (!d.error) {
        d.error = new Fl_Box(76, d.pin->y() - 26, W - 92, 22);
        d.win->add(d.error);
        d.error->labelcolor(pal.danger);
        d.error->labelsize(13);
        d.error->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
      }
      d.error->copy_label(box_text(r.repeat_error).c_str());
      d.win->redraw();
      d.repeat->value("");
      d.pin->take_focus();
      d.pin->insert_position(0, d.pin->size());
      continue;
    }
    break;
  }
  Fl::remove_timeout(timeout_cb, &d);
  a.kind = d.result == 1 ? assuan::Answer::Ok : d.result == 2 ? assuan::Answer::NotOk
           : d.result == 3 ? assuan::Answer::Timeout : assuan::Answer::Cancel;
  if (d.pin && a.kind == assuan::Answer::Ok) {
    a.pin = d.pin->value();
    a.repeated = d.repeat != nullptr;
#ifdef FLD_HAVE_SECRET
    if (d.save && d.save->value()) cache_store(r.keyinfo, r.description, a.pin);
#endif
  }
  // the passphrase stays in memory as short as possible
  if (d.pin) {
    std::string zero(strlen(d.pin->value()), '\0');
    d.pin->value(zero.c_str());
  }
  d.win->hide();
  delete d.win;
  Fl::flush();
  return a;
}

assuan::Answer get_pin(const assuan::Request &r) {
#ifdef FLD_HAVE_SECRET
  if (cacheable(r)) {
    std::string pin;
    // an error means the saved one was wrong: ask (and forget it)
    if (!r.error.empty()) cache_forget(r.keyinfo);
    else if (cache_lookup(r.keyinfo, pin)) {
      assuan::Answer a;
      a.kind = assuan::Answer::Ok;
      a.pin = pin;
      a.from_cache = true;
      return a;
    }
  }
#endif
  return run_dialog(r, true);
}

assuan::Answer confirm(const assuan::Request &r) { return run_dialog(r, false); }

// --test-autoreply: answers without a window (the tests); PIN from $FLD_PINENTRY_TEST_PIN
assuan::Answer test_answer(const assuan::Request &, bool pin) {
  assuan::Answer a;
  const char *p = getenv("FLD_PINENTRY_TEST_PIN");
  a.kind = p && *p ? assuan::Answer::Ok : assuan::Answer::Cancel;
  if (pin && p) a.pin = p;
  return a;
}

void send(const std::string &line) {
  fputs(line.c_str(), stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
  bool test = false;
  std::string display;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--test-autoreply") test = true;
    else if ((a == "--display" || a == "-D") && i + 1 < argc) display = argv[++i];
    else if (a == "-h" || a == "--help") {
      printf("Usage: pinentry-fldesktop [--display DISPLAY] [pinentry options]\n"
             "GnuPG's passphrase prompt for fldesktop. Set it in ~/.gnupg/gpg-agent.conf:\n"
             "  pinentry-program /usr/bin/pinentry-fldesktop\n");
      return 0;
    } else if (a == "--version") {
      printf("pinentry-fldesktop 1.0.0\n");
      return 0;
    }
  }
  // gpg-agent passes the display it knows; under fl-wm that is Xwayland's
  if (!display.empty() && !getenv("DISPLAY")) setenv("DISPLAY", display.c_str(), 1);
  if (!test && !getenv("WAYLAND_DISPLAY") && !getenv("DISPLAY")) {
    // a text console: the terminal pinentries
    for (const char *alt : {"pinentry-curses", "pinentry-tty"}) {
      std::string path = fld::which(alt);
      if (!path.empty()) {
        argv[0] = (char *)alt;
        execv(path.c_str(), argv);
      }
    }
    fprintf(stderr, "pinentry-fldesktop: no display and no pinentry-curses\n");
  }
  assuan::Ui ui;
  if (test) {
    ui.get_pin = [](const assuan::Request &r) { return test_answer(r, true); };
    ui.confirm = [](const assuan::Request &r) { return test_answer(r, false); };
  } else {
    ui.get_pin = get_pin;
    ui.confirm = confirm;
  }
  assuan::Session s(ui);
  send(assuan::Session::greeting());
  char buf[4096];
  while (!s.finished() && fgets(buf, sizeof buf, stdin)) {
    for (auto &l : s.handle(buf)) send(l);
  }
  return 0;
}
