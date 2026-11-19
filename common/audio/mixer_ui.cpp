#include "mixer_ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Toggle_Button.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "fld/menus.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/widgets.h"

namespace fld {

static const int ROW_H = 84;
static const float MAX_LEVEL = 1.5f; // sliders go to 150 %

// level meter drawn under the slider
class PeakMeter : public Fl_Widget {
public:
  float value = -1;
  PeakMeter(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void draw() override {
    const Palette &p = palette();
    fl_color(fl_color_average(p.bg2, p.fg, 0.88f));
    fl_rectf(x(), y(), w(), h());
    if (value <= 0) return;
    // the slider is linear in the perceptual (cubic) scale, so is the meter
    float v = std::cbrt(std::min(1.0f, value)) / MAX_LEVEL;
    int vw = (int)(w() * std::min(1.0f, v));
    fl_color(value > 0.98f ? p.danger : fl_rgb_color(0x3f, 0xb9, 0x50));
    fl_rectf(x(), y(), vw, h());
  }
};

class ChannelRow : public Fl_Group {
public:
  MixerView *view;
  uint32_t id = Audio::InvalidId;
  bool is_device = true;
  Fl_Box *icon, *title, *subtitle, *percent;
  Fl_Slider *slider;
  Fl_Toggle_Button *mute;
  Fl_Choice *target;   // output device of a stream, port of a device, profile of a card
  Fl_Button *make_default;
  PeakMeter *meter;
  std::vector<uint32_t> target_ids;
  std::vector<std::string> target_names;
  std::string title_s, subtitle_s, percent_s;

  ChannelRow(MixerView *v, int x, int y, int w) : Fl_Group(x, y, w, ROW_H), view(v) {
    const Palette &p = palette();
    box(FL_FLAT_BOX);
    color(p.bg2);
    icon = new Fl_Box(x + 10, y + 10, 40, 40);
    title = new Fl_Box(x + 60, y + 6, w - 380, 22);
    title->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    title->labelfont(FL_HELVETICA_BOLD);
    title->labelsize(14);
    subtitle = new Fl_Box(x + 60, y + 26, w - 380, 20);
    subtitle->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    subtitle->labelsize(12);
    subtitle->labelcolor(p.fg_dim);
    target = new Fl_Choice(x + w - 312, y + 8, 300, 26);
    target->textsize(12);
    target->callback(target_cb, this);
    make_default = new Fl_Button(x + w - 312, y + 8, 300, 26, "Set as default");
    make_default->callback(default_cb, this);
    make_default->hide();
    slider = new Fl_Slider(x + 60, y + 50, w - 200, 20);
    slider->type(FL_HOR_NICE_SLIDER);
    slider->bounds(0, MAX_LEVEL);
    slider->step(0.01);
    slider->callback(slider_cb, this);
    slider->when(FL_WHEN_CHANGED);
    slider->color(fl_color_average(p.bg2, p.fg, 0.85f));
    slider->selection_color(p.accent);
    meter = new PeakMeter(x + 66, y + 72, w - 212, 4);
    percent = new Fl_Box(x + w - 136, y + 48, 56, 24);
    percent->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
    percent->labelsize(12);
    mute = new Fl_Toggle_Button(x + w - 74, y + 46, 62, 28, "Mute");
    mute->labelsize(12);
    mute->callback(mute_cb, this);
    mute->selection_color(p.danger);
    end();
    resizable(slider);
    // right-click: mute, default, a few levels, and where it plays
    fld::set_context_menu(this, [this](fld::Menu &m) {
      m.button(mute).button(make_default).divider();
      for (int pct : {0, 25, 50, 75, 100}) {
        char l[16];
        snprintf(l, sizeof l, "%d %%", pct);
        bool on = (int)lround(slider->value() * 100) == pct;
        m.radio(l, on, [this, pct] {
          slider->value(pct / 100.0);
          slider_cb(slider, this);
        });
      }
      if (target->visible() && target->size() > 1) {
        m.divider().submenu(view->mode() == MixerView::CARDS ? "Profile" : 
                            (view->mode() == MixerView::PLAYBACK || view->mode() == MixerView::RECORDING) ? "Device" : "Port");
        for (int i = 0; i < target->size() - 1; i++) {
          const char *t = target->text(i);
          if (!t) continue;
          m.radio(fld::Menu::text(t), target->value() == i, [this, i] {
            target->value(i);
            target_cb(target, this);
          });
        }
        m.end_submenu();
      }
    }, false);
  }

  AudioBackend *be() const { return view->backend(); }

  void set_level_label(float level) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d%%", (int)lround(level * 100));
    percent_s = buf;
    percent->label(percent_s.c_str());
  }

  static void slider_cb(Fl_Widget *w, void *d) {
    ChannelRow *r = (ChannelRow *)d;
    float level = (float)((Fl_Slider *)w)->value();
    // snap to 100 %
    if (std::fabs(level - 1.0f) < 0.03f) level = 1.0f;
    if (r->is_device) r->be()->setDeviceLevel(r->id, level);
    else r->be()->setStreamLevel(r->id, level);
    r->set_level_label(level);
  }
  static void mute_cb(Fl_Widget *w, void *d) {
    ChannelRow *r = (ChannelRow *)d;
    bool m = ((Fl_Toggle_Button *)w)->value();
    if (r->is_device) r->be()->setDeviceMute(r->id, m);
    else r->be()->setStreamMute(r->id, m);
  }
  static void default_cb(Fl_Widget *, void *d) {
    ChannelRow *r = (ChannelRow *)d;
    r->be()->setDefaultDevice(r->id);
  }
  static void target_cb(Fl_Widget *w, void *d) {
    ChannelRow *r = (ChannelRow *)d;
    int i = ((Fl_Choice *)w)->value();
    if (i < 0) return;
    switch (r->view->mode()) {
      case MixerView::PLAYBACK:
      case MixerView::RECORDING:
        if (i < (int)r->target_ids.size()) r->be()->moveStream(r->id, r->target_ids[i]);
        break;
      case MixerView::OUTPUTS:
      case MixerView::INPUTS:
        if (i < (int)r->target_names.size()) r->be()->setDevicePort(r->id, r->target_names[i]);
        break;
      case MixerView::CARDS:
        if (i < (int)r->target_names.size()) r->be()->setCardProfile(r->id, r->target_names[i]);
        break;
    }
  }

  void set_icon(const std::vector<std::string> &names) {
    Fl_Image *img = icon_first(names, 40);
    icon->image(img);
    icon->redraw();
  }

  void set_texts(const std::string &t, const std::string &s) {
    if (t != title_s) { title_s = t; title->copy_label(escape_at(t).c_str()); }
    if (s != subtitle_s) { subtitle_s = s; subtitle->copy_label(escape_at(s).c_str()); }
  }

  void set_choices(const std::vector<std::string> &labels, int selected) {
    // rebuild only when the entries changed (keeps an open menu stable)
    bool same = target->size() - 1 == (int)labels.size();
    for (int i = 0; same && i < (int)labels.size(); i++)
      if (!target->text(i) || escape_at(labels[i]) != target->text(i)) same = false;
    if (!same) {
      target->clear();
      for (auto &l : labels) {
        // menu paths treat '/' and '&' specially
        std::string e;
        for (char c : escape_at(l)) {
          if (c == '/' || c == '\\' || c == '&' || c == '_') e += '\\';
          e += c;
        }
        target->add(e.c_str());
      }
    }
    if (labels.empty()) target->hide();
    else target->show();
    target->value(selected);
  }

  void set_volume(float level, bool muted, bool has_volume) {
    if (Fl::pushed() != slider) {
      slider->value(level);
      set_level_label(level);
    }
    mute->value(muted);
    if (has_volume) { slider->activate(); mute->activate(); }
    else { slider->deactivate(); mute->deactivate(); }
  }
};

MixerView::MixerView(int x, int y, int w, int h, AudioBackend *backend, Mode mode)
    : Fl_Group(x, y, w, h), backend_(backend), mode_(mode) {
  box(FL_FLAT_BOX);
  color(palette().bg);
  scroll_ = new Fl_Scroll(x, y, w, h);
  scroll_->type(Fl_Scroll::VERTICAL);
  scroll_->box(FL_FLAT_BOX);
  scroll_->color(palette().bg);
  scroll_->end();
  end();
  resizable(scroll_);
  if (mode != CARDS) Fl::add_timeout(0.05, meter_tick, this);
  refresh();
}

MixerView::~MixerView() { Fl::remove_timeout(meter_tick, this); }

void MixerView::meter_tick(void *data) {
  MixerView *v = static_cast<MixerView *>(data);
  if (v->visible_r()) {
    for (auto &kv : v->rows_) {
      float pk = v->backend_->peak(kv.first);
      if (pk != kv.second->meter->value) {
        kv.second->meter->value = pk;
        kv.second->meter->redraw();
      }
    }
  }
  Fl::repeat_timeout(0.05, meter_tick, data);
}

static std::string stream_title(const Audio::Stream &s) {
  return !s.appName.empty() ? s.appName : !s.binary.empty() ? s.binary : "Stream " + std::to_string(s.id);
}

void MixerView::refresh() {
  const Audio::Model &m = backend_->model();
  std::map<uint32_t, bool> seen;
  auto row_for = [&](uint32_t id, bool device) {
    ChannelRow *r;
    auto it = rows_.find(id);
    if (it != rows_.end()) r = it->second;
    else {
      scroll_->begin();
      r = new ChannelRow(this, scroll_->x(), scroll_->y(), scroll_->w() - 16);
      scroll_->end();
      r->id = id;
      rows_[id] = r;
    }
    r->is_device = device;
    seen[id] = true;
    return r;
  };

  if (mode_ == PLAYBACK || mode_ == RECORDING) {
    Audio::StreamKind sk = mode_ == PLAYBACK ? Audio::Playback : Audio::Recording;
    Audio::DeviceKind dk = mode_ == PLAYBACK ? Audio::Sink : Audio::Source;
    auto devs = backend_->devices(dk);
    for (const Audio::Stream *s : backend_->streams(sk)) {
      ChannelRow *r = row_for(s->id, false);
      r->set_texts(stream_title(*s), s->mediaName + (s->corked ? "  (paused)" : ""));
      r->set_icon({s->iconName, s->binary, mode_ == PLAYBACK ? "audio-volume-high" : "audio-input-microphone",
                   "multimedia-volume-control"});
      std::vector<std::string> labels;
      r->target_ids.clear();
      int sel = -1;
      for (auto *d : devs) {
        labels.push_back(d->description);
        if (d->id == s->deviceId) sel = (int)labels.size() - 1;
        r->target_ids.push_back(d->id);
      }
      r->set_choices(labels, sel);
      r->make_default->hide();
      r->set_volume(Audio::level(s->volumes), s->muted, s->hasVolume);
    }
  } else if (mode_ == OUTPUTS || mode_ == INPUTS) {
    Audio::DeviceKind dk = mode_ == OUTPUTS ? Audio::Sink : Audio::Source;
    const Audio::Device *def = backend_->defaultDevice(dk);
    for (const Audio::Device *d : backend_->devices(dk)) {
      ChannelRow *r = row_for(d->id, true);
      bool is_def = def && def->id == d->id;
      r->set_texts(d->description + (is_def ? "  ✓ default" : ""), d->name);
      r->set_icon({d->iconName, mode_ == OUTPUTS ? "audio-card" : "audio-input-microphone", "audio-speakers"});
      std::vector<std::string> labels;
      r->target_names.clear();
      int sel = -1;
      for (auto &p : d->ports) {
        labels.push_back(p.description + (p.available ? "" : " (unplugged)"));
        r->target_names.push_back(p.name);
        if (p.name == d->activePort) sel = (int)labels.size() - 1;
      }
      if (labels.size() > 1 || is_def) {
        r->set_choices(labels.size() > 1 ? labels : std::vector<std::string>{}, sel);
        r->make_default->hide();
      } else {
        r->set_choices({}, -1);
      }
      if (!is_def && labels.size() <= 1) {
        r->make_default->show();
      } else if (!is_def) {
        // ports and default: put the button left of the port list
        r->make_default->show();
        r->make_default->resize(r->target->x() - 130, r->target->y(), 124, 26);
      } else {
        r->make_default->hide();
      }
      r->set_volume(Audio::level(d->volumes), d->muted, d->hasVolume);
    }
  } else { // CARDS
    for (auto &kv : m.cards) {
      const Audio::Card &c = kv.second;
      ChannelRow *r = row_for(c.id, true);
      r->set_texts(c.description, c.name);
      r->set_icon({c.iconName, "audio-card"});
      std::vector<std::string> labels;
      r->target_names.clear();
      int sel = -1;
      for (auto &p : c.profiles) {
        labels.push_back(p.description + (p.available ? "" : " (unavailable)"));
        r->target_names.push_back(p.name);
        if (p.name == c.activeProfile) sel = (int)labels.size() - 1;
      }
      r->set_choices(labels, sel);
      r->make_default->hide();
      r->slider->hide();
      r->mute->hide();
      r->percent->hide();
      r->meter->hide();
    }
  }
  for (auto it = rows_.begin(); it != rows_.end();) {
    if (!seen.count(it->first)) {
      scroll_->remove(it->second);
      Fl::delete_widget(it->second);
      it = rows_.erase(it);
    } else ++it;
  }
  layout_rows();
}

void MixerView::layout_rows() {
  int y = scroll_->y() - scroll_->yposition() + 6;
  for (auto &kv : rows_) {
    ChannelRow *r = kv.second;
    r->resize(scroll_->x() + 6, y, scroll_->w() - 28, ROW_H);
    // keep the right column aligned after a resize
    int X = r->x(), W = r->w(), Y = r->y();
    r->title->resize(X + 60, Y + 6, W - 380, 22);
    r->subtitle->resize(X + 60, Y + 26, W - 380, 20);
    r->target->resize(X + W - 312, Y + 8, 300, 26);
    if (r->make_default->visible() && r->target->visible())
      r->make_default->resize(X + W - 442, Y + 8, 124, 26);
    else
      r->make_default->resize(X + W - 312, Y + 8, 300, 26);
    r->slider->resize(X + 60, Y + 50, W - 200, 20);
    r->meter->resize(X + 66, Y + 72, W - 212, 4);
    r->percent->resize(X + W - 136, Y + 48, 56, 24);
    r->mute->resize(X + W - 74, Y + 46, 62, 28);
    y += ROW_H + 6;
  }
  scroll_->redraw();
  redraw();
}

void MixerView::resize(int X, int Y, int W, int H) {
  Fl_Group::resize(X, Y, W, H);
  layout_rows();
}

void MixerView::draw() {
  Fl_Group::draw();
  if (rows_.empty()) {
    const char *msg = !backend_->isConnected() ? "Not connected to PipeWire"
                      : mode_ == PLAYBACK      ? "No application is playing sound"
                      : mode_ == RECORDING     ? "No application is recording"
                      : mode_ == CARDS         ? "No sound cards"
                                               : "No devices";
    fl_font(FL_HELVETICA, 14);
    fl_color(palette().fg_dim);
    fl_draw(msg, x(), y(), w(), h(), FL_ALIGN_CENTER);
  }
}

} // namespace fld
