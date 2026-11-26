#include "displays_ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cmath>
#include <string>

#include "fld/i18n.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

namespace fld {

using namespace outputs;

namespace {
const char *kRotations[] = {N_("Normal"), "90°", "180°", "270°"};
const int kArrH = 170, kListW = 200, kLabelW = 110;
}  // namespace

// drawing of the output arrangement
class DisplaysView::Arrangement : public Fl_Widget {
public:
  int selected = 0;
  Arrangement(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void draw() override {
    const Palette &p = palette();
    fl_color(fl_color_average(p.bg2, p.fg, 0.92f));
    fl_rectf(x(), y(), w(), h());
    auto &hs = heads();
    if (hs.empty()) return;
    int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30);
    auto size_of = [](Head *hd, int &ow, int &oh) {
      ow = hd->current ? hd->current->w : 1920;
      oh = hd->current ? hd->current->h : 1080;
      if (hd->transform % 2) std::swap(ow, oh);
      ow = (int)(ow / hd->scale);
      oh = (int)(oh / hd->scale);
    };
    for (auto &hd : hs) {
      int ow, oh;
      size_of(hd.get(), ow, oh);
      minx = std::min(minx, hd->x);
      miny = std::min(miny, hd->y);
      maxx = std::max(maxx, hd->x + ow);
      maxy = std::max(maxy, hd->y + oh);
    }
    double s = std::min((w() - 40.0) / std::max(1, maxx - minx), (h() - 40.0) / std::max(1, maxy - miny));
    int ox = x() + (int)((w() - (maxx - minx) * s) / 2), oy = y() + (int)((h() - (maxy - miny) * s) / 2);
    for (size_t i = 0; i < hs.size(); i++) {
      Head *hd = hs[i].get();
      int ow, oh;
      size_of(hd, ow, oh);
      int rx = ox + (int)((hd->x - minx) * s), ry = oy + (int)((hd->y - miny) * s);
      int rw = (int)(ow * s), rh = (int)(oh * s);
      fl_color(!hd->enabled ? p.fg_dim : (int)i == selected ? p.accent : fl_color_average(p.accent, p.bg2, 0.5f));
      fl_rectf(rx + 2, ry + 2, rw - 4, rh - 4);
      fl_color(p.accent_fg);
      fl_font(FL_HELVETICA_BOLD, 13);
      fl_draw(escape_at(hd->name).c_str(), rx, ry, rw, rh, FL_ALIGN_CENTER | FL_ALIGN_CLIP);
    }
  }
};

static Fl_Box *row_label(int x, int y, int w, const char *text) {
  Fl_Box *b = new Fl_Box(x, y, w - 8, 28, text);
  b->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  return b;
}

DisplaysView::DisplaysView(int X, int Y, int W) : Fl_Group(X, Y, W, kHeight) {
  arr_ = new Arrangement(X, Y, W, kArrH);
  int ly = Y + kArrH + 10;
  list_ = new Fl_Hold_Browser(X, ly, kListW, 210);
  list_->textsize(13);
  list_->callback([](Fl_Widget *, void *d) {
    DisplaysView *v = (DisplaysView *)d;
    if (v->list_->value() > 0) v->sel_ = v->list_->value() - 1;
    v->arr_->selected = v->sel_;
    v->arr_->redraw();
    v->show_head();
  }, this);
  int fx = X + kListW + 10, wx = fx + kLabelW, ww = std::max(160, W - (wx - X));
  int y = ly;
  enabled_ = new fld::CheckButton(wx, y, ww, 26, _("Enabled"));
  enabled_->callback([](Fl_Widget *, void *d) { ((DisplaysView *)d)->edit(); }, this);
  y += 34;
  row_label(fx, y, kLabelW, _("Resolution:"));
  mode_ = new Fl_Choice(wx, y, ww, 28);
  mode_->callback([](Fl_Widget *, void *d) { ((DisplaysView *)d)->edit(); }, this);
  y += 36;
  row_label(fx, y, kLabelW, _("Scale:"));
  scale_ = new Fl_Choice(wx, y, 140, 28);
  for (double s : kScales) scale_->add((std::to_string((int)lround(s * 100)) + "\\%").c_str());
  scale_->callback([](Fl_Widget *, void *d) { ((DisplaysView *)d)->edit(); }, this);
  y += 36;
  row_label(fx, y, kLabelW, _("Rotation:"));
  rotation_ = new Fl_Choice(wx, y, 140, 28);
  for (auto r : kRotations) rotation_->add(_(r));
  rotation_->callback([](Fl_Widget *, void *d) { ((DisplaysView *)d)->edit(); }, this);
  y += 36;
  pos_label_ = row_label(fx, y, kLabelW, _("Position:"));
  px_ = new Fl_Spinner(wx, y, 100, 28);
  py_ = new Fl_Spinner(wx + 110, y, 100, 28);
  for (Fl_Spinner *s : {px_, py_}) {
    s->range(-32768, 32768);
    s->step(10);
    s->callback([](Fl_Widget *, void *d) { ((DisplaysView *)d)->edit(); }, this);
  }
  int by = Y + kHeight - 32;
  apply_btn_ = new Fl_Button(X + W - 120, by, 120, 32, _("Apply"));
  apply_btn_->callback([](Fl_Widget *, void *d) { ((DisplaysView *)d)->do_apply(); }, this);
  apply_btn_->deactivate();
  status_ = new Fl_Box(X, by, W - 130, 32);
  status_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  status_->labelsize(12);
  status_->labelcolor(palette().fg_dim);
  end();
  resizable(nullptr);
  if (!available()) status_->label(_("The compositor does not support output management (wlr-output-management)."));
  refresh();
}

// One screen has nothing to arrange: the arrangement, the list of screens
// and the position go, and the rest moves up and left.
void DisplaysView::set_single(bool single) {
  if (single == single_) return;
  single_ = single;
  int dx = single ? -(kListW + 10) : kListW + 10, dy = single ? -(kArrH + 10) : kArrH + 10;
  for (int i = 0; i < children(); i++) {
    Fl_Widget *w = child(i);
    if (w == arr_ || w == list_) continue;
    if (w == apply_btn_ || w == status_) continue;  // they stay at the bottom
    w->position(w->x() + dx, w->y() + dy);
  }
  for (Fl_Widget *w : {(Fl_Widget *)arr_, (Fl_Widget *)list_, (Fl_Widget *)px_, (Fl_Widget *)py_,
                       (Fl_Widget *)pos_label_})
    single ? w->hide() : w->show();
  int nh = single ? kSingleHeight : kHeight;
  int dh = nh - h();
  for (Fl_Widget *w : {(Fl_Widget *)apply_btn_, (Fl_Widget *)status_}) w->position(w->x(), w->y() + dh);
  size(w(), nh);
  init_sizes();
  if (parent()) parent()->redraw();
  if (on_resize) on_resize(dh);
}

void DisplaysView::refresh() {
  if (dirty_) return;  // do not overwrite unsaved edits
  set_single(heads().size() == 1);
  edited_ = current_config();
  list_->clear();
  for (auto &h : heads()) {
    std::string l = h->name + (h->enabled ? "" : _(" (off)"));
    list_->add(escape_at(l).c_str());
  }
  if (sel_ >= (int)heads().size()) sel_ = 0;
  list_->value(sel_ + 1);
  arr_->selected = sel_;
  arr_->redraw();
  show_head();
}

void DisplaysView::show_head() {
  if (sel_ >= (int)heads().size()) return;
  Head *h = heads()[sel_].get();
  HeadConfig &c = edited_[sel_];
  enabled_->value(c.enabled);
  mode_->clear();
  int mi = 0;
  for (size_t i = 0; i < h->modes.size(); i++) {
    mode_->add(escape_at(mode_label(*h->modes[i])).c_str());
    if (h->modes[i] == c.mode) mi = (int)i;
  }
  mode_->value(h->modes.empty() ? -1 : mi);
  int si = 0;
  for (int i = 0; i < 7; i++)
    if (std::fabs(kScales[i] - c.scale) < 0.01) si = i;
  scale_->value(si);
  rotation_->value(c.transform & 3);
  px_->value(c.x);
  py_->value(c.y);
  static std::string desc;
  desc = h->description.empty() ? h->make + " " + h->model : h->description;
  list_->tooltip(desc.c_str());
}

void DisplaysView::edit() {
  if (sel_ >= (int)heads().size()) return;
  Head *h = heads()[sel_].get();
  HeadConfig &c = edited_[sel_];
  c.enabled = enabled_->value();
  int mi = mode_->value();
  if (mi >= 0 && mi < (int)h->modes.size()) c.mode = h->modes[mi];
  c.scale = kScales[std::max(0, scale_->value())];
  c.transform = std::max(0, rotation_->value());
  c.x = (int)px_->value();
  c.y = (int)py_->value();
  dirty_ = true;
  apply_btn_->activate();
}

// asks to keep the new configuration; reverts after 15 seconds
bool DisplaysView::confirm() {
  Fl_Double_Window w(400, 130, _("Keep Display Settings?"));
  Fl_Box *msg = new Fl_Box(16, 12, 368, 60);
  msg->align(FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_LEFT);
  Fl_Button *revert = new Fl_Button(176, 86, 100, 30, _("Revert"));
  Fl_Return_Button *keep = new Fl_Return_Button(284, 86, 100, 30, _("Keep"));
  static int result;
  result = 0;
  static std::string text;
  auto set_text = [&](int s) {
    text = format(_("Do you want to keep these display settings? They are reverted in %s s."), {std::to_string(s)});
    msg->label(text.c_str());
    msg->redraw();
  };
  set_text(15);
  revert->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  keep->callback([](Fl_Widget *b, void *) {
    result = 1;
    b->window()->hide();
  });
  w.end();
  w.set_modal();
  w.show();
  for (int t = 150; t > 0 && w.shown(); t--) {
    Fl::wait(0.1);
    if (t % 10 == 0) set_text(t / 10);
  }
  w.hide();
  return result == 1;
}

void DisplaysView::do_apply() {
  std::vector<HeadConfig> before = current_config();
  bool any_enabled = false;
  for (auto &c : edited_)
    if (c.enabled) any_enabled = true;
  if (!any_enabled) {
    fl_alert("%s", _("At least one display must stay enabled."));
    return;
  }
  if (!apply(edited_)) {
    fl_alert("%s", _("The display configuration could not be applied."));
    dirty_ = false;
    refresh();
    return;
  }
  if (confirm()) {
    if (on_saved) on_saved(edited_);
    status_->label(_("Saved."));
  } else {
    apply(before);
    status_->label(_("Reverted."));
  }
  dirty_ = false;
  apply_btn_->deactivate();
  // the manager sends a new configuration (done) which refreshes the view
  Fl::wait(0.2);
  refresh();
}

}  // namespace fld
