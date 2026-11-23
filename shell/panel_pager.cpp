// fl-panel: the workspace pager (ext-workspace-v1). One numbered cell per
// workspace; a click goes there, the mouse wheel steps through them. Hidden
// while the compositor offers fewer than two workspaces.
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <wayland-client.h>
#include "ext-workspace-v1-client-protocol.h"

#include "fld/util.h"
#include "panel.h"

namespace {

struct Workspace {
  ext_workspace_handle_v1 *handle = nullptr;
  std::string name;
  uint32_t coord = 0;
  bool has_coord = false;
  bool active = false;
  uint64_t order = 0;
};

ext_workspace_manager_v1 *g_mgr = nullptr;
std::vector<std::unique_ptr<Workspace>> g_ws;
uint64_t g_counter = 0;

const int kCell = 24;

// workspaces in display order
std::vector<Workspace *> sorted() {
  std::vector<Workspace *> v;
  for (auto &w : g_ws) v.push_back(w.get());
  std::stable_sort(v.begin(), v.end(), [](Workspace *a, Workspace *b) {
    if (a->has_coord && b->has_coord && a->coord != b->coord) return a->coord < b->coord;
    return a->order < b->order;
  });
  return v;
}

class Pager : public Fl_Widget {
public:
  int hover = -1;
  Pager() : Fl_Widget(0, 0, kCell, PANEL_H - 4) { tooltip("Workspaces"); }
  int index_at(int ex) const {
    int i = (ex - x() - 2) / kCell;
    return i >= 0 && i < (int)g_ws.size() ? i : -1;
  }
  void draw() override {
    fl_color(panel_bg());
    fl_rectf(x(), y(), w(), h());
    auto list = sorted();
    fl_font(FL_HELVETICA_BOLD, 12);
    for (size_t i = 0; i < list.size(); i++) {
      Workspace *ws = list[i];
      int cx = x() + 2 + (int)i * kCell, cy = y() + (h() - 22) / 2;
      Fl_Color bg = ws->active ? fld::palette().accent : (int)i == hover ? panel_hover() : panel_bg();
      fl_color(bg);
      fl_rounded_rectf(cx + 1, cy, kCell - 2, 22, 4);
      fl_color(ws->active ? fld::palette().accent_fg : panel_fg());
      std::string label = ws->name.empty() ? std::to_string(i + 1) : ws->name;
      fl_draw(fld::escape_at(label).c_str(), cx, cy, kCell, 22, FL_ALIGN_CENTER | FL_ALIGN_CLIP);
    }
  }
  void activate_index(int i) {
    auto list = sorted();
    if (i < 0 || i >= (int)list.size() || list[i]->active || !g_mgr) return;
    ext_workspace_handle_v1_activate(list[i]->handle);
    ext_workspace_manager_v1_commit(g_mgr);
    wl_display_flush(fl_wl_display());
  }
  int handle(int e) override {
    switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int h = index_at(Fl::event_x());
      if (h != hover) { hover = h; redraw(); }
      return 1;
    }
    case FL_LEAVE:
      hover = -1;
      redraw();
      return 1;
    case FL_PUSH:
      if (Fl::event_button() == FL_LEFT_MOUSE) activate_index(index_at(Fl::event_x()));
      return 1;
    case FL_MOUSEWHEEL: {
      auto list = sorted();
      int cur = -1;
      for (size_t i = 0; i < list.size(); i++)
        if (list[i]->active) cur = (int)i;
      int d = Fl::event_dy() ? Fl::event_dy() : Fl::event_dx();
      if (cur >= 0 && d) activate_index(std::clamp(cur + (d > 0 ? 1 : -1), 0, (int)list.size() - 1));
      return 1;
    }
    }
    return Fl_Widget::handle(e);
  }
};

Pager *g_pager = nullptr;

void update() {
  if (!g_pager) return;
  int n = (int)g_ws.size();
  bool vis = n >= 2;
  int w = std::max(1, n) * kCell + 4;
  bool changed = vis != (bool)g_pager->visible() || w != g_pager->w();
  if (vis) g_pager->show(); else g_pager->hide();
  g_pager->size(w, g_pager->h());
  if (changed) relayout();
  g_pager->redraw();
}

// ---- workspace handles
void ws_id(void *, ext_workspace_handle_v1 *, const char *) {}
void ws_name(void *data, ext_workspace_handle_v1 *, const char *name) { ((Workspace *)data)->name = name; }
void ws_coordinates(void *data, ext_workspace_handle_v1 *, wl_array *coords) {
  auto *w = (Workspace *)data;
  if (coords->size >= sizeof(uint32_t)) {
    w->coord = *(uint32_t *)coords->data;
    w->has_coord = true;
  }
}
void ws_state(void *data, ext_workspace_handle_v1 *, uint32_t state) {
  ((Workspace *)data)->active = state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
}
void ws_capabilities(void *, ext_workspace_handle_v1 *, uint32_t) {}
void ws_removed(void *, ext_workspace_handle_v1 *h) {
  g_ws.erase(std::remove_if(g_ws.begin(), g_ws.end(), [h](auto &w) { return w->handle == h; }), g_ws.end());
  ext_workspace_handle_v1_destroy(h);
}
const ext_workspace_handle_v1_listener kWsListener = {ws_id, ws_name, ws_coordinates, ws_state, ws_capabilities,
                                                      ws_removed};

// ---- groups (fl-wm has one for all outputs; the pager shows every workspace)
void group_capabilities(void *, ext_workspace_group_handle_v1 *, uint32_t) {}
void group_output_enter(void *, ext_workspace_group_handle_v1 *, wl_output *) {}
void group_output_leave(void *, ext_workspace_group_handle_v1 *, wl_output *) {}
void group_workspace_enter(void *, ext_workspace_group_handle_v1 *, ext_workspace_handle_v1 *) {}
void group_workspace_leave(void *, ext_workspace_group_handle_v1 *, ext_workspace_handle_v1 *) {}
void group_removed(void *, ext_workspace_group_handle_v1 *g) { ext_workspace_group_handle_v1_destroy(g); }
const ext_workspace_group_handle_v1_listener kGroupListener = {group_capabilities,    group_output_enter,
                                                               group_output_leave,    group_workspace_enter,
                                                               group_workspace_leave, group_removed};

// ---- manager
void mgr_group(void *, ext_workspace_manager_v1 *, ext_workspace_group_handle_v1 *g) {
  ext_workspace_group_handle_v1_add_listener(g, &kGroupListener, nullptr);
}
void mgr_workspace(void *, ext_workspace_manager_v1 *, ext_workspace_handle_v1 *h) {
  auto w = std::make_unique<Workspace>();
  w->handle = h;
  w->order = g_counter++;
  ext_workspace_handle_v1_add_listener(h, &kWsListener, w.get());
  g_ws.push_back(std::move(w));
}
void mgr_done(void *, ext_workspace_manager_v1 *) { update(); }
void mgr_finished(void *, ext_workspace_manager_v1 *m) {
  ext_workspace_manager_v1_destroy(m);
  g_mgr = nullptr;
  g_ws.clear();
  update();
}
const ext_workspace_manager_v1_listener kMgrListener = {mgr_group, mgr_workspace, mgr_done, mgr_finished};

} // namespace

Fl_Widget *pager_create() {
  g_pager = new Pager();
  g_pager->hide();
  panel_context_menu(g_pager, [](fld::Menu &m) {
    m.item("Workspace Settings…", [] { fld::spawn({"fl-settings", "shortcuts"}); }).divider();
    panel_menu_items(m);
  });
  return g_pager;
}

bool pager_bind(wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
  if (!g_pager || g_mgr || strcmp(iface, ext_workspace_manager_v1_interface.name)) return false;
  g_mgr = (ext_workspace_manager_v1 *)wl_registry_bind(reg, name, &ext_workspace_manager_v1_interface,
                                                       std::min<uint32_t>(version, 1));
  ext_workspace_manager_v1_add_listener(g_mgr, &kMgrListener, nullptr);
  return true;
}
