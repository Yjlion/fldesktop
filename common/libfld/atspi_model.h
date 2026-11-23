// The AT-SPI2 view of FLTK widgets, without FLTK and D-Bus (unit tested):
// role and state numbers, accessible names, and the tree snapshot the
// bridge (atspi.cpp) answers questions from.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fld {
namespace atspi {

// AtspiRole values used here
enum Role : uint32_t {
  ROLE_INVALID = 0, ROLE_CHECK_BOX = 7, ROLE_COMBO_BOX = 11, ROLE_DIAL = 15, ROLE_DIALOG = 16, ROLE_FILLER = 20,
  ROLE_FRAME = 23, ROLE_IMAGE = 27, ROLE_LABEL = 29, ROLE_LIST = 31, ROLE_LIST_ITEM = 32, ROLE_MENU = 33,
  ROLE_MENU_BAR = 34, ROLE_PAGE_TAB = 37, ROLE_PAGE_TAB_LIST = 38, ROLE_PANEL = 39, ROLE_PASSWORD_TEXT = 40,
  ROLE_PROGRESS_BAR = 42, ROLE_PUSH_BUTTON = 43, ROLE_RADIO_BUTTON = 44, ROLE_SCROLL_BAR = 48,
  ROLE_SCROLL_PANE = 49, ROLE_SLIDER = 51, ROLE_SPIN_BUTTON = 52, ROLE_STATUS_BAR = 54, ROLE_TERMINAL = 60,
  ROLE_TEXT = 61, ROLE_TOGGLE_BUTTON = 62, ROLE_TOOL_BAR = 63, ROLE_TREE = 65, ROLE_UNKNOWN = 67,
  ROLE_WINDOW = 69, ROLE_APPLICATION = 75, ROLE_ENTRY = 79,
};
const char *role_name(uint32_t role); // "push button", as AT-SPI names them

// AtspiStateType values
enum State : int {
  STATE_ACTIVE = 1, STATE_CHECKED = 4, STATE_EDITABLE = 7, STATE_ENABLED = 8, STATE_FOCUSABLE = 11,
  STATE_FOCUSED = 12, STATE_HAS_TOOLTIP = 13, STATE_HORIZONTAL = 14, STATE_MODAL = 16, STATE_MULTI_LINE = 17,
  STATE_PRESSED = 20, STATE_RESIZABLE = 21, STATE_SELECTABLE = 22, STATE_SELECTED = 23, STATE_SENSITIVE = 24,
  STATE_SHOWING = 25, STATE_SINGLE_LINE = 26, STATE_VERTICAL = 29, STATE_VISIBLE = 30, STATE_CHECKABLE = 41,
  STATE_IS_DEFAULT = 39, STATE_READ_ONLY = 43,
};
inline uint64_t bit(int s) { return 1ull << s; }
// GetState's answer: the low and the high 32 bits
inline std::pair<uint32_t, uint32_t> state_words(uint64_t s) { return {(uint32_t)s, (uint32_t)(s >> 32)}; }

// A label as people read it: without "&" shortcut markers and "@" symbols
// (symbols=false keeps "@" as it is), squashed white space
std::string clean_label(const std::string &label, bool symbols = true);
// a browser line without its format codes ("@b", "@C8"; "@." ends them)
std::string clean_browser_line(const std::string &line);
// the name: the label, else the tooltip
std::string accessible_name(const std::string &label, const std::string &tooltip);

struct Node {
  int id = 0, parent = -1;
  std::vector<int> children;
  uint32_t role = ROLE_UNKNOWN;
  std::string name, description;
  uint64_t states = 0;
  int x = 0, y = 0, w = 0, h = 0; // on the screen (window position + place in it)
  std::string action;             // "click", "toggle", "select", or "" when none
  bool has_text = false;
  std::string text;
  int caret = 0;
  bool has_value = false;
  double vmin = 0, vmax = 0, vstep = 0, value = 0;
};

// one picture of all windows; node 0 is the application
struct Snapshot {
  std::map<int, Node> nodes;
  int focus = -1;  // the focused node
  int active = -1; // the active window's node
  const Node *get(int id) const {
    auto it = nodes.find(id);
    return it == nodes.end() ? nullptr : &it->second;
  }
  // the deepest node whose extents contain the point (screen coordinates), or -1
  int at_point(int x, int y) const;
  int index_in_parent(int id) const;
};

// what changed between two snapshots, as AT-SPI events
struct Event {
  int node;
  std::string kind;   // "focus", "state", "name", "window-activate", "text", "value"
  std::string detail; // the state's name for "state"
  int value = 0;
};
std::vector<Event> diff(const Snapshot &before, const Snapshot &after);

} // namespace atspi
} // namespace fld
