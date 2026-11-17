// Sound: output and input devices (PipeWire), link to the mixer
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Tabs.H>

#include "audiobackend.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "mixer_ui.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

AudioBackend *g_backend;
MixerView *g_views[3];

class SoundPage : public Fl_Group {
public:
  SoundPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Sound", cy);
    resizable(g);
    if (!g_backend) g_backend = AudioBackend::create();
    Fl_Tabs *tabs = new Fl_Tabs(X + 16, cy, W - 32, H - (cy - Y) - 56);
    const char *labels[] = {"Output", "Input", "Applications"};
    MixerView::Mode modes[] = {MixerView::OUTPUTS, MixerView::INPUTS, MixerView::PLAYBACK};
    for (int i = 0; i < 3; i++) {
      g_views[i] = new MixerView(X + 16, cy + 30, W - 32, H - (cy - Y) - 86, g_backend, modes[i]);
      g_views[i]->label(labels[i]);
    }
    tabs->end();
    g->resizable(tabs);
    Fl_Button *mixer = new Fl_Button(X + W - 16 - 160, Y + H - 46, 160, 32, "Open Volume Mixer");
    mixer->callback([](Fl_Widget *, void *) { spawn({"fl-mixer"}); });
    g->end();
    end();
    g_backend->on_changed = [] { for (auto *v : g_views) v->refresh(); };
    g_backend->setPeaksEnabled(true);
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new SoundPage(x, y, w, h); }

} // namespace

PageInfo page_sound() {
  return {"sound", "Sound", "audio-volume-high", "Hardware",
          "sound audio volume speakers headphones microphone input output pipewire mute", create};
}

} // namespace fld
