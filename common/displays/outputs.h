// The screens as the compositor reports them through wlr-output-management,
// and applying a new arrangement. Works with any compositor that has the
// protocol (fl-wm, sway, labwc, Hyprland ...). Used by the Displays page of
// fl-settings and by fl-displays.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct wl_display;
struct zwlr_output_mode_v1;
struct zwlr_output_head_v1;

namespace fld {
class Ini;

namespace outputs {

struct Mode {
  zwlr_output_mode_v1 *obj = nullptr;
  int w = 0, h = 0, refresh = 0;  // refresh in mHz
  bool preferred = false;
};

struct Head {
  zwlr_output_head_v1 *obj = nullptr;
  std::string name, description, make, model;
  std::vector<Mode *> modes;  // owned by the manager
  Mode *current = nullptr;
  bool enabled = false;
  int x = 0, y = 0, transform = 0;
  double scale = 1.0;
};

// what to set on one head
struct HeadConfig {
  std::string name;
  bool enabled = true;
  Mode *mode = nullptr;
  int x = 0, y = 0, transform = 0;
  double scale = 1.0;
};

// Binds the manager on `dpy` (FLTK's display) and waits for the first
// configuration. False when the compositor does not have the protocol.
bool connect(wl_display *dpy);
bool available();
const std::vector<std::unique_ptr<Head>> &heads();
// called after the compositor sends a new configuration
extern std::function<void()> on_changed;

std::vector<HeadConfig> current_config();
// Applies cfg and waits (running the FLTK loop) for the answer.
bool apply(const std::vector<HeadConfig> &cfg);

// "1920 × 1080 @ 60.00 Hz" (a star for the preferred mode)
std::string mode_label(const Mode &m);

// The scales offered, and rotations as wl_output transforms 0-3.
extern const double kScales[7];

// Writes cfg as fl-wm's [output:<name>] sections (enabled, mode, scale,
// position, transform). Pure: for tests and both programs.
void write_wm_config(Ini &wm, const std::vector<HeadConfig> &cfg);

}  // namespace outputs
}  // namespace fld
