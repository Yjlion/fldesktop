// fl-fonts: the installed fonts. Preview them with any text and size,
// install font files for yourself and remove the ones you installed.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tile.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <fontconfig/fontconfig.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fonts_util.h"

namespace {

Fl_Double_Window *g_win;
Fl_Hold_Browser *g_list;
Fl_Input *g_search, *g_sample;
Fl_Check_Button *g_mono_only, *g_mine_only;
Fl_Value_Slider *g_size;
Fl_Button *g_remove;
Fl_Box *g_status;
class Preview;
Preview *g_preview;
std::vector<fonts::Family> g_all;
std::vector<int> g_shown;

std::string home() { return fld::home_dir(); }
std::string user_dir() { return fld::data_home() + "/fonts"; }

void status(const std::string &s) {
  static std::string keep;
  keep = s;
  g_status->label(keep.c_str());
  g_status->redraw();
}

std::vector<fonts::Face> list_faces() {
  std::vector<fonts::Face> out;
  FcInitReinitialize();
  FcPattern *pat = FcPatternCreate();
  FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, FC_WEIGHT, FC_SLANT, nullptr);
  FcFontSet *fs = FcFontList(nullptr, pat, os);
  for (int i = 0; fs && i < fs->nfont; i++) {
    FcChar8 *fam = nullptr, *style = nullptr, *file = nullptr;
    int weight = 80, slant = 0;
    FcPatternGetString(fs->fonts[i], FC_FAMILY, 0, &fam);
    FcPatternGetString(fs->fonts[i], FC_STYLE, 0, &style);
    FcPatternGetString(fs->fonts[i], FC_FILE, 0, &file);
    FcPatternGetInteger(fs->fonts[i], FC_WEIGHT, 0, &weight);
    FcPatternGetInteger(fs->fonts[i], FC_SLANT, 0, &slant);
    if (!fam || !file) continue;
    out.push_back({(const char *)fam, style ? (const char *)style : "Regular", (const char *)file, weight, slant});
  }
  if (fs) FcFontSetDestroy(fs);
  FcObjectSetDestroy(os);
  FcPatternDestroy(pat);
  return out;
}

const fonts::Family *selected() {
  int i = g_list->value() - 1;
  return i >= 0 && i < (int)g_shown.size() ? &g_all[(size_t)g_shown[(size_t)i]] : nullptr;
}

// Each style of the chosen family, at the chosen size, with the sample text.
class Preview : public Fl_Widget {
public:
  std::vector<std::string> names; // FLTK (Pango) font names, kept alive
  Preview(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  int needed_h() const {
    const fonts::Family *f = selected();
    if (!f) return 10;
    int sz = (int)g_size->value();
    return 20 + (int)f->faces.size() * (sz + 34) + 40;
  }
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_color(p.bg2);
    fl_rectf(x(), y(), w(), h());
    const fonts::Family *f = selected();
    if (!f) return;
    int sz = (int)g_size->value(), yy = y() + 16;
    names.resize(f->faces.size());
    for (size_t i = 0; i < f->faces.size() && i < 64; i++) {
      const fonts::Face &face = f->faces[i];
      std::string st = face.style;
      names[i] = st == "Regular" || st == "Book" || st == "Normal" || st == "Roman" ? f->name : f->name + " " + st;
      Fl_Font fi = (Fl_Font)(FL_FREE_FONT + i);
      Fl::set_font(fi, names[i].c_str());
      fl_font(FL_HELVETICA, 12);
      fl_color(p.fg_dim);
      fl_draw((face.style + "   " + fld::tilde(face.file)).c_str(), x() + 16, yy + 12);
      yy += 18;
      fl_font(fi, sz);
      fl_color(p.fg);
      std::string sample = *g_sample->value() ? g_sample->value() : "The quick brown fox jumps over the lazy dog";
      fl_push_clip(x(), yy, w(), sz + 14);
      fl_draw(sample.c_str(), x() + 16, yy + sz);
      fl_pop_clip();
      yy += sz + 16;
    }
  }
};

Fl_Scroll *g_pscroll;

void relayout_preview() {
  g_preview->resize(g_pscroll->x(), g_pscroll->y(), g_pscroll->w() - 16, std::max(g_pscroll->h(), g_preview->needed_h()));
  g_pscroll->scroll_to(0, 0);
  g_pscroll->redraw();
}

void fill() {
  std::string sel = selected() ? selected()->name : "";
  std::string q = fld::lower(g_search->value());
  g_shown.clear();
  g_list->clear();
  int mine = 0;
  for (size_t i = 0; i < g_all.size(); i++) {
    const fonts::Family &f = g_all[i];
    mine += f.user;
    if (g_mono_only->value() && !f.monospace) continue;
    if (g_mine_only->value() && !f.user) continue;
    if (!q.empty() && fld::lower(f.name).find(q) == std::string::npos) continue;
    g_shown.push_back((int)i);
    std::string line = f.name + (f.user ? "  (mine)" : "");
    g_list->add(fld::escape_at(line).c_str());
    if (f.name == sel) g_list->select(g_list->size());
  }
  if (!g_list->value() && g_list->size()) g_list->select(1);
  status(std::to_string(g_all.size()) + " font families, " + std::to_string(mine) + " installed by you");
  const fonts::Family *f = selected();
  if (f && f->user) g_remove->activate();
  else g_remove->deactivate();
  relayout_preview();
}

void load() {
  g_all = fonts::group(list_faces(), home());
  fill();
}

void refresh_cache() {
  // fc-cache in a thread, then the list again
  fl_cursor(FL_CURSOR_WAIT);
  std::thread([] {
    std::string out;
    fld::run_capture({"fc-cache", "-f", user_dir()}, out);
    Fl::awake([](void *) {
      fl_cursor(FL_CURSOR_DEFAULT);
      load();
    }, nullptr);
  }).detach();
}

void install() {
  Fl_Native_File_Chooser fc;
  fc.title("Install Fonts");
  fc.type(Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
  fc.filter("Fonts\t*.{ttf,otf,ttc,otc,woff,woff2,pfb,TTF,OTF}");
  std::string dl = fld::user_dir("DOWNLOAD");
  if (!dl.empty()) fc.directory(dl.c_str());
  if (fc.show() != 0) return;
  fld::mkdir_p(user_dir());
  int n = 0;
  for (int i = 0; i < fc.count(); i++) {
    std::string src = fc.filename(i);
    if (!fonts::is_font_file(src)) continue;
    std::string data;
    if (!fld::read_file(src, data, 64u << 20)) continue;
    if (fld::write_file(fonts::install_path(user_dir(), src), data)) n++;
  }
  status("Installed " + std::to_string(n) + " font file(s); updating the font list…");
  refresh_cache();
}

void remove_family() {
  const fonts::Family *f = selected();
  if (!f || !f->user) return;
  if (fl_choice("Remove the font \"%s\" (%d file(s))? Programs that use it will pick another font.", "Cancel",
                "Remove", nullptr, f->name.c_str(), (int)f->faces.size()) != 1)
    return;
  // every file of the family (also the styles merged away as duplicates)
  for (const fonts::Face &face : list_faces())
    if (fld::trim(face.family.substr(0, face.family.find(','))) == f->name && fonts::is_user_font(face.file, home()))
      unlink(face.file.c_str());
  status("Removed " + f->name + "; updating the font list…");
  refresh_cache();
}

} // namespace

int main(int argc, char **argv) {
  fld::init_app("fl-fonts", argc, argv);
  Fl::lock();
  FcInit();
  std::vector<std::string> to_install;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-fonts [FONT-FILE...]  (files given are offered for installing)\n");
      return 0;
    }
    if (a[0] != '-') to_install.push_back(fld::abs_path(a));
  }
  const int W = 1000, H = 640, LW = 300;
  g_win = new Fl_Double_Window(W, H, "Fonts");
  g_search = new Fl_Input(60, 8, LW - 68, 28, "Find:");
  g_search->when(FL_WHEN_CHANGED);
  g_search->callback([](Fl_Widget *, void *) { fill(); });
  g_mono_only = new Fl_Check_Button(8, 40, 130, 24, "Monospace");
  g_mono_only->callback([](Fl_Widget *, void *) { fill(); });
  g_mine_only = new Fl_Check_Button(140, 40, 150, 24, "Installed by me");
  g_mine_only->callback([](Fl_Widget *, void *) { fill(); });
  g_sample = new Fl_Input(LW + 70, 8, W - LW - 300, 28, "Sample:");
  g_sample->value("The quick brown fox jumps over the lazy dog");
  g_sample->when(FL_WHEN_CHANGED);
  g_sample->callback([](Fl_Widget *, void *) { g_preview->redraw(); });
  g_size = new Fl_Value_Slider(W - 220, 8, 212, 28);
  g_size->type(FL_HOR_NICE_SLIDER);
  g_size->bounds(8, 96);
  g_size->step(1);
  g_size->value(28);
  g_size->tooltip("Size");
  g_size->callback([](Fl_Widget *, void *) { relayout_preview(); });
  Fl_Tile *tile = new Fl_Tile(0, 70, W, H - 70 - 66);
  g_list = new Fl_Hold_Browser(0, 70, LW, H - 70 - 66);
  g_list->textsize(13);
  g_list->callback([](Fl_Widget *, void *) {
    const fonts::Family *f = selected();
    if (f && f->user) g_remove->activate();
    else g_remove->deactivate();
    relayout_preview();
  });
  g_pscroll = new Fl_Scroll(LW, 70, W - LW, H - 70 - 66);
  g_pscroll->type(Fl_Scroll::VERTICAL);
  g_preview = new Preview(LW, 70, W - LW - 16, H - 70 - 66);
  g_pscroll->end();
  tile->end();
  Fl_Button *inst = new Fl_Button(8, H - 62, 130, 30, "Install Fonts…");
  inst->tooltip("Copy font files to " "~/.local/share/fonts (for you only)");
  inst->callback([](Fl_Widget *, void *) { install(); });
  g_remove = new Fl_Button(144, H - 62, 100, 30, "Remove…");
  g_remove->tooltip("Remove a font you installed");
  g_remove->callback([](Fl_Widget *, void *) { remove_family(); });
  Fl_Button *open = new Fl_Button(250, H - 62, 130, 30, "Open My Fonts");
  open->callback([](Fl_Widget *, void *) {
    fld::mkdir_p(user_dir());
    fld::open_path(user_dir());
  });
  g_status = new Fl_Box(8, H - 26, W - 16, 24);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  g_status->labelcolor(fld::palette().fg_dim);
  g_win->end();
  g_win->resizable(tile);
  g_win->size_range(700, 400);
  fld::set_window_icon(g_win, "fl-fonts");
  g_win->show();
  load();
  // font files on the command line (double-clicked in the file manager)
  if (!to_install.empty()) {
    std::string names;
    for (auto &f : to_install) names += "\n" + fld::basename(f);
    if (fl_choice("Install these fonts for you?%s", "Cancel", "Install", nullptr, names.c_str()) == 1) {
      fld::mkdir_p(user_dir());
      for (auto &f : to_install) {
        std::string data;
        if (fonts::is_font_file(f) && fld::read_file(f, data, 64u << 20))
          fld::write_file(fonts::install_path(user_dir(), f), data);
      }
      refresh_cache();
    }
  }
  return Fl::run();
}
