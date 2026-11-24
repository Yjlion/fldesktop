// fl-mixer: PipeWire volume mixer for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Tabs.H>

#include <cstring>
#include <string>

#include "audiobackend.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "mixer_ui.h"

static fld::MixerView *g_views[5];
static Fl_Box *g_status;
static AudioBackend *g_backend;

static void update_status() {
  const Audio::Model &m = g_backend->model();
  static std::string s;
  s = m.connected ? (m.serverName.empty() ? "Connected to PipeWire" : m.serverName)
                  : "Not connected to PipeWire — retrying…";
  g_status->label(s.c_str());
}

int main(int argc, char **argv) {
  fld::init_app("fl-mixer", argc, argv);
  Fl::lock(); // the backend reports from its own thread
  std::string start_tab;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-mixer [--tab playback|recording|outputs|inputs|configuration]\n");
      return 0;
    }
    if (a == "--tab" && i + 1 < argc) start_tab = argv[++i];
  }
  g_backend = AudioBackend::create();
  if (!g_backend) {
    fprintf(stderr, "fl-mixer: built without PipeWire support\n");
    return 1;
  }
  const int W = 820, H = 560, ST = 24;
  Fl_Double_Window *win = new Fl_Double_Window(W, H, "Volume Mixer");
  Fl_Tabs *tabs = new fld::Tabs(0, 0, W, H - ST);
  const char *labels[] = {"Playback", "Recording", "Output Devices", "Input Devices", "Configuration"};
  fld::MixerView::Mode modes[] = {fld::MixerView::PLAYBACK, fld::MixerView::RECORDING, fld::MixerView::OUTPUTS,
                                  fld::MixerView::INPUTS, fld::MixerView::CARDS};
  for (int i = 0; i < 5; i++) {
    g_views[i] = new fld::MixerView(0, 30, W, H - ST - 30, g_backend, modes[i]);
    g_views[i]->label(labels[i]);
  }
  tabs->end();
  tabs->selection_color(fld::palette().bg2);
  g_status = new Fl_Box(0, H - ST, W, ST);
  g_status->box(FL_FLAT_BOX);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  g_status->labelsize(12);
  g_status->labelcolor(fld::palette().fg_dim);
  win->resizable(tabs);
  win->end();
  win->size_range(520, 320);
  fld::set_window_icon(win, "fl-mixer");

  const char *ids[] = {"playback", "recording", "outputs", "inputs", "configuration"};
  for (int i = 0; i < 5; i++)
    if (start_tab == ids[i]) tabs->value(g_views[i]);

  g_backend->on_changed = [] {
    for (auto *v : g_views) v->refresh();
    update_status();
  };
  g_backend->on_connection = [](bool) { update_status(); };
  g_backend->setPeaksEnabled(true);
  update_status();
  win->show(); // not show(argc, argv): it resets the theme colors
  int r = Fl::run();
  delete g_backend;
  return r;
}
