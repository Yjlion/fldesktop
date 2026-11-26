#include "fld/i18n.h"
// fl-snip: screenshots, like a snipping tool
//
// The screen is captured first (grim) and shown frozen in an overlay over all
// outputs (fl-wm's "fldesktop.snip" role), where a rectangle, a freeform
// shape, a window or the whole screen is chosen. The snip then opens in an
// editor (pen, highlighter, shapes, text, pixelate, crop) or, for the quick
// modes the Print keys use, is saved and copied right away.
#include "snip_app.h"

double now_s() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---------------------------------------------------------------------------
// Settings


Settings S;

const char *mode_name(Mode m) {
  switch (m) {
    case Mode::Rect: return "Rectangle";
    case Mode::Freeform: return "Freeform";
    case Mode::Window: return "Window";
    case Mode::Full: return "Full Screen";
    case Mode::Pick: return "Colour Picker";
    case Mode::Ruler: return "Ruler";
  }
  return "";
}

std::string screenshots_dir() {
  std::string pics = fld::user_dir("PICTURES");
  if (pics.empty()) pics = fld::home_dir() + "/Pictures";
  return pics + "/Screenshots";
}

// ---------------------------------------------------------------------------
// The main window: New, mode, delay

static Fl_Double_Window *g_main;
static Fl_Choice *g_mode_choice, *g_delay_choice;
static const int kDelays[] = {0, 3, 5, 10};

// a snip from the main window or an editor: it opens in the editor
void new_snip(Mode mode, int delay) {
  std::string err;
  Image img = take_snip(mode, delay, err);
  if (!err.empty()) {
    fl_alert("%s", err.c_str());
    return;
  }
  if (img.empty()) return;
  std::string saved;
  if (S.autosave) saved = auto_save(img);
  if (S.copy) copy_image(img, false);
  if (g_main) g_main->hide();
  open_editor(std::move(img), saved);
}

void show_settings() {
  Fl_Double_Window dlg(420, 230, "Snip Settings");
  Fl_Check_Button copy(16, 14, 390, 26, "Copy each snip to the clipboard");
  copy.value(S.copy);
  Fl_Check_Button save(16, 42, 390, 26, "Save each snip to Pictures/Screenshots");
  save.value(S.autosave);
  Fl_Check_Button cursor(16, 70, 390, 26, "Include the mouse pointer");
  cursor.value(S.cursor);
  Fl_Check_Button note(16, 98, 390, 26, "Show a notification after a quick snip (Print key)");
  note.value(S.notify);
  Fl_Button cancel(208, 186, 96, 30, "Cancel");
  Fl_Return_Button ok(310, 186, 96, 30, "Save");
  int result = -1;
  cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 0; }, &result);
  ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
  dlg.end();
  dlg.set_modal();
  dlg.show();
  while (result < 0 && dlg.shown()) Fl::wait();
  if (result != 1) return;
  S.copy = copy.value();
  S.autosave = save.value();
  S.cursor = cursor.value();
  S.notify = note.value();
  S.save();
}

static void make_main_window() {
  const fld::Palette &p = fld::palette();
#ifdef FLD_HAVE_RECORD
  const int RX = 100; // room for the Record button
#else
  const int RX = 0;
#endif
  g_main = new Fl_Double_Window(520 + RX, 64, "Snip");
  g_main->color(p.bg);
  fld::FlatButton *nw = new fld::FlatButton(8, 12, 96, 40, "New");
  nw->icon_name("applets-screenshooter", 22);
  nw->align(FL_ALIGN_INSIDE | FL_ALIGN_CENTER | FL_ALIGN_IMAGE_NEXT_TO_TEXT);
  nw->tooltip("Take a snip (Ctrl+N)");
  nw->shortcut(FL_CTRL + 'n');
  nw->callback([](Fl_Widget *, void *) {
    S.mode = (Mode)g_mode_choice->value();
    S.delay = kDelays[g_delay_choice->value()];
    S.save();
    new_snip(S.mode, S.delay);
    if (g_editors.empty() && g_main) g_main->show();
  });
#ifdef FLD_HAVE_RECORD
  fld::FlatButton *rb = new fld::FlatButton(108, 12, 96, 40, "Record");
  rb->icon_name("media-record", 22);
  rb->align(FL_ALIGN_INSIDE | FL_ALIGN_CENTER | FL_ALIGN_IMAGE_NEXT_TO_TEXT);
  rb->tooltip("Record a screen or a window (Ctrl+R)");
  rb->shortcut(FL_CTRL + 'r');
  rb->callback([](Fl_Widget *, void *) {
    // a separate process: the recording outlives this window
    fld::spawn({"fl-snip", "--record"});
    if (g_editors.empty() && g_main) g_main->hide();
  });
#endif
  g_mode_choice = new Fl_Choice(170 + RX, 18, 130, 28, "Mode:");
  for (Mode m : {Mode::Rect, Mode::Freeform, Mode::Window, Mode::Full}) g_mode_choice->add(mode_name(m));
  g_mode_choice->value((int)S.mode);
  g_delay_choice = new Fl_Choice(356 + RX, 18, 100, 28, "Delay:");
  g_delay_choice->add("None");
  g_delay_choice->add("3 seconds");
  g_delay_choice->add("5 seconds");
  g_delay_choice->add("10 seconds");
  int di = 0;
  for (int i = 0; i < 4; i++)
    if (kDelays[i] == S.delay) di = i;
  g_delay_choice->value(di);
  fld::FlatButton *more = new fld::FlatButton(470 + RX, 12, 40, 40);
  more->icon_name("open-menu", 20);
  if (!fld::icon("open-menu", 20)) more->label("…");
  more->tooltip("More");
  more->callback([](Fl_Widget *w, void *) {
    fld::Menu m;
    m.item("Settings…", [] { show_settings(); });
#ifdef FLD_HAVE_RECORD
    m.item("Record with Sound…", [] {
      fld::spawn({"fl-snip", "--record", "--audio"});
      if (g_editors.empty() && g_main) g_main->hide();
    });
#endif
    m.item("Pick a Colour on the Screen", [] { fld::spawn({"fl-snip", "--pick-color"}); });
    m.item("Measure on the Screen", [] { fld::spawn({"fl-snip", "--ruler"}); }).divider();
    m.item("Open Screenshots Folder", [] {
      fld::mkdir_p(screenshots_dir());
      fld::spawn({"fl-files", screenshots_dir()});
    });
    m.item("Open Image…", [] {
      Fl_Native_File_Chooser fc;
      fc.title("Open Image");
      fc.filter("Images\t*.{png,jpg,jpeg,bmp,gif}");
      fc.directory(screenshots_dir().c_str());
      if (fc.show() != 0 || !fc.filename()) return;
      std::unique_ptr<Fl_Image> im(fld::load_image(fc.filename()));
      Fl_RGB_Image *rgb = dynamic_cast<Fl_RGB_Image *>(im.get());
      if (!rgb || rgb->d() < 3) {
        fl_alert("Could not open the image.");
        return;
      }
      Image img;
      img.w = rgb->data_w();
      img.h = rgb->data_h();
      img.px.resize((size_t)img.w * img.h * 4);
      const uint8_t *d = (const uint8_t *)rgb->data()[0];
      int ld = rgb->ld() ? rgb->ld() : img.w * rgb->d();
      for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
          const uint8_t *s = d + (size_t)y * ld + (size_t)x * rgb->d();
          uint8_t *o = img.at(x, y);
          o[0] = s[0];
          o[1] = s[1];
          o[2] = s[2];
          o[3] = rgb->d() == 4 ? s[3] : 255;
        }
      g_main->hide();
      open_editor(std::move(img), fc.filename());
    }).divider();
    m.item("About", [] {
      fld::about_dialog("Snip", "applets-screenshooter", "Screenshots of a part of the screen, a window or all of it.");
    });
    m.popup(w->x(), w->y() + w->h());
  });
  g_main->end();
  fld::set_window_icon(g_main, "applets-screenshooter");
}

// ---------------------------------------------------------------------------
// Quick snips (the Print keys): save, copy, notify, print the path

static int quick(Mode mode, int delay, bool edit) {
  std::string err;
  Image img = take_snip(mode, delay, err);
  if (!err.empty()) {
    fprintf(stderr, "fl-snip: %s\n", err.c_str());
    fld::Notification n;
    n.summary = "Screenshot";
    n.body = err;
    n.icon = "dialog-error";
    n.transient = true;
    fld::notify(n);
    return 1;
  }
  if (img.empty()) return 1; // cancelled
  std::string path = S.autosave || !edit ? auto_save(img) : "";
  if (S.copy) copy_image(img, !edit);
  if (!path.empty()) printf("%s\n", path.c_str());
  fflush(stdout);
  if (edit) {
    open_editor(std::move(img), path);
    return Fl::run();
  }
  if (!S.notify || path.empty()) return 0;
  // the notification's buttons work while fl-snip waits a little for them
  fld::Notification n;
  n.app_name = "Snip";
  n.desktop_entry = "fl-snip";
  n.summary = "Screenshot taken";
  n.body = fld::basename(path) + (S.copy ? " — copied to the clipboard" : "");
  n.icon = path;
  n.transient = true;
  n.actions = {{"default", "Open"}, {"edit", "Edit"}, {"folder", "Show in Files"}};
  static bool answered = false;
  static std::string action;
  fld::notify(n, [](const std::string &a) {
    action = a;
    answered = true;
  });
  double until = now_s() + 15;
  while (!answered && now_s() < until) Fl::wait(0.2);
  if (action == "default") fld::spawn({"fl-view", path});
  else if (action == "folder") fld::spawn({"fl-files", fld::dirname(path)});
  else if (action == "edit") {
    open_editor(std::move(img), path);
    return Fl::run();
  }
  return 0;
}

static const char *kUsage =
    "Usage: fl-snip [OPTION]\n"
    "  (none)          the Snip window: choose a mode and a delay, then New\n"
    "  --snip          choose a part of the screen, then edit it\n"
    "  --area          choose a rectangle; save and copy it (Shift+Print)\n"
    "  --window        choose a window; save and copy it\n"
    "  --full          the whole screen; save and copy it (Print)\n"
    "  --freeform      draw a shape around a part; save and copy it\n"
    "  --pick-color    pick a colour on the screen; print and copy it (#rrggbb)\n"
    "  --ruler         measure on the screen in pixels; Enter prints and copies it\n"
    "  --delay N       wait N seconds first\n"
    "  --edit FILE     open an image in the editor\n"
#ifdef FLD_HAVE_RECORD
    "  --record        record a screen or a window (WebM) until Stop\n"
    "    --audio         with the desktop's sound\n"
    "    --seconds N     stop after N seconds\n"
    "    -o FILE         the file (default: Videos/Screencasts)\n"
    "    --record-node N record PipeWire node N (no portal)\n"
    "    --no-window     no time window and no notification\n"
#endif
    "Quick snips print the saved file's path.\n";

int main(int argc, char **argv) {
  enum { MAIN, SNIP, QUICK, EDIT, RECORD, TOOL } what = MAIN;
#ifdef FLD_HAVE_RECORD
  record::Options rec;
#endif
  Mode mode = Mode::Rect;
  int delay = -1;
  std::string edit_file;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      fputs(kUsage, stdout);
      return 0;
    }
    if (a == "-v" || a == "--version") {
      printf("fl-snip %s\n", FLD_VERSION);
      return 0;
    }
    if (a == "--snip") what = SNIP;
    else if (a == "--area") { what = QUICK; mode = Mode::Rect; }
    else if (a == "--window") { what = QUICK; mode = Mode::Window; }
    else if (a == "--full") { what = QUICK; mode = Mode::Full; }
    else if (a == "--freeform") { what = QUICK; mode = Mode::Freeform; }
    else if (a == "--pick-color" || a == "--pick-colour") { what = TOOL; mode = Mode::Pick; }
    else if (a == "--ruler") { what = TOOL; mode = Mode::Ruler; }
    else if (a == "--delay" && i + 1 < argc) delay = std::max(0, std::min(60, atoi(argv[++i])));
    else if (a == "--edit" && i + 1 < argc) { what = EDIT; edit_file = argv[++i]; }
#ifdef FLD_HAVE_RECORD
    else if (a == "--record") what = RECORD;
    else if (a == "--audio") rec.audio = true;
    else if (a == "--seconds" && i + 1 < argc) rec.seconds = std::max(0, atoi(argv[++i]));
    else if ((a == "-o" || a == "--output") && i + 1 < argc) rec.output = argv[++i];
    else if (a == "--record-node" && i + 1 < argc) { what = RECORD; rec.node = (uint32_t)atol(argv[++i]); }
    else if (a == "--no-window") { rec.window = false; rec.notify = false; }
#endif
    else {
      fprintf(stderr, "fl-snip: unknown option %s\n%s", a.c_str(), kUsage);
      return 2;
    }
  }
#ifdef FLD_HAVE_RECORD
  if (what == RECORD && !rec.window) return record::run(rec);
#endif
  fld::init_app("fl-snip", argc, argv);
  Fl::lock(); // notification answers come through Fl::awake
  fl_register_images();
  S.load();
  if (delay < 0) delay = what == MAIN ? S.delay : 0;
  switch (what) {
    case QUICK: return quick(mode, delay, false);
    case SNIP: return quick(S.mode, delay, true);
    case TOOL: return run_tool(mode);
    case EDIT: {
      std::unique_ptr<Fl_Image> im(fld::load_image(edit_file));
      Fl_RGB_Image *rgb = dynamic_cast<Fl_RGB_Image *>(im.get());
      if (!rgb || rgb->d() < 3) {
        fprintf(stderr, "fl-snip: cannot open %s\n", edit_file.c_str());
        return 1;
      }
      Image img;
      img.w = rgb->data_w();
      img.h = rgb->data_h();
      img.px.resize((size_t)img.w * img.h * 4);
      const uint8_t *d = (const uint8_t *)rgb->data()[0];
      int ld = rgb->ld() ? rgb->ld() : img.w * rgb->d();
      for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
          const uint8_t *s = d + (size_t)y * ld + (size_t)x * rgb->d();
          uint8_t *o = img.at(x, y);
          o[0] = s[0];
          o[1] = s[1];
          o[2] = s[2];
          o[3] = rgb->d() == 4 ? s[3] : 255;
        }
      open_editor(std::move(img), fld::abs_path(edit_file));
      return Fl::run();
    }
#ifdef FLD_HAVE_RECORD
    case RECORD: return record::run(rec);
#endif
    case MAIN:
      break;
  }
  make_main_window();
  g_main->show();
  return Fl::run();
}
