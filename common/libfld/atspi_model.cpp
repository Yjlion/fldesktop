#include "atspi_model.h"

#include <cctype>
#include <cstring>

namespace fld {
namespace atspi {

const char *role_name(uint32_t r) {
  switch (r) {
    case ROLE_CHECK_BOX: return "check box";
    case ROLE_COMBO_BOX: return "combo box";
    case ROLE_DIAL: return "dial";
    case ROLE_DIALOG: return "dialog";
    case ROLE_FILLER: return "filler";
    case ROLE_FRAME: return "frame";
    case ROLE_IMAGE: return "image";
    case ROLE_LABEL: return "label";
    case ROLE_LIST: return "list";
    case ROLE_LIST_ITEM: return "list item";
    case ROLE_MENU: return "menu";
    case ROLE_MENU_BAR: return "menu bar";
    case ROLE_PAGE_TAB: return "page tab";
    case ROLE_PAGE_TAB_LIST: return "page tab list";
    case ROLE_PANEL: return "panel";
    case ROLE_PASSWORD_TEXT: return "password text";
    case ROLE_PROGRESS_BAR: return "progress bar";
    case ROLE_PUSH_BUTTON: return "push button";
    case ROLE_RADIO_BUTTON: return "radio button";
    case ROLE_SCROLL_BAR: return "scroll bar";
    case ROLE_SCROLL_PANE: return "scroll pane";
    case ROLE_SLIDER: return "slider";
    case ROLE_SPIN_BUTTON: return "spin button";
    case ROLE_STATUS_BAR: return "status bar";
    case ROLE_TERMINAL: return "terminal";
    case ROLE_TEXT: return "text";
    case ROLE_TOGGLE_BUTTON: return "toggle button";
    case ROLE_TOOL_BAR: return "tool bar";
    case ROLE_TREE: return "tree";
    case ROLE_WINDOW: return "window";
    case ROLE_APPLICATION: return "application";
    case ROLE_ENTRY: return "entry";
  }
  return "unknown";
}

std::string clean_browser_line(const std::string &l) {
  // format codes at the start of a browser line: @b, @i, @C8, @S14, @.
  size_t i = 0;
  while (i + 1 < l.size() && l[i] == '@') {
    char c = l[i + 1];
    if (c == '@') break;
    if (c == '.') {
      i += 2;
      return clean_label(l.substr(i), false);
    }
    if (!std::strchr("bilucmrsfBCFSN-", c)) break;
    i += 2;
    while (i < l.size() && std::isdigit((unsigned char)l[i])) i++;
  }
  return clean_label(l.substr(i));
}

std::string clean_label(const std::string &l, bool symbols) {
  std::string s;
  size_t i = 0;
  for (; i < l.size(); i++) {
    char c = l[i];
    if (c == '&') {
      if (i + 1 < l.size() && l[i + 1] == '&') {
        s += '&';
        i++;
      } else if (i + 1 < l.size() && std::isalnum((unsigned char)l[i + 1])) {
        // a shortcut letter's marker
      } else
        s += c;
      continue;
    }
    if (c == '@' && symbols) {
      if (i + 1 < l.size() && l[i + 1] == '@') {
        s += '@';
        i++;
        continue;
      }
      // an FLTK symbol such as @-> or @+2fileopen: up to the next space
      while (i < l.size() && l[i] != ' ') i++;
      continue;
    }
    s += std::isspace((unsigned char)c) ? ' ' : c;
  }
  // squash spaces
  std::string out;
  for (char c : s) {
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string accessible_name(const std::string &label, const std::string &tooltip) {
  std::string n = clean_label(label);
  // a label cut to fit ("DIRECT-0c-HP M110 Las…"): the tooltip has all of it
  static const std::string ellipsis = "\u2026";
  if (n.size() >= ellipsis.size() && n.compare(n.size() - ellipsis.size(), ellipsis.size(), ellipsis) == 0 &&
      !clean_label(tooltip).empty())
    n = clean_label(tooltip);
  if (n.empty()) n = clean_label(tooltip);
  // "Name:" before a field is read as "Name"
  while (!n.empty() && (n.back() == ':' || n.back() == ' ')) n.pop_back();
  return n;
}

int Snapshot::at_point(int x, int y) const {
  int best = -1;
  // children come after their parent: the last containing node is the deepest
  std::vector<int> stack{0};
  while (!stack.empty()) {
    int id = stack.back();
    stack.pop_back();
    const Node *n = get(id);
    if (!n) continue;
    bool in = id == 0 || (x >= n->x && y >= n->y && x < n->x + n->w && y < n->y + n->h);
    if (!in) continue;
    if (id != 0) best = id;
    for (int c : n->children) stack.push_back(c);
  }
  return best;
}

int Snapshot::index_in_parent(int id) const {
  const Node *n = get(id);
  if (!n) return -1;
  const Node *p = get(n->parent);
  if (!p) return -1;
  for (size_t i = 0; i < p->children.size(); i++)
    if (p->children[i] == id) return (int)i;
  return -1;
}

std::vector<Event> diff(const Snapshot &a, const Snapshot &b) {
  std::vector<Event> out;
  if (a.active != b.active && b.active >= 0) out.push_back({b.active, "window-activate", "", 0});
  if (a.focus != b.focus) {
    if (a.focus >= 0 && b.get(a.focus)) out.push_back({a.focus, "state", "focused", 0});
    if (b.focus >= 0) {
      out.push_back({b.focus, "state", "focused", 1});
      out.push_back({b.focus, "focus", "", 0});
    }
  }
  for (auto &kv : b.nodes) {
    const Node *o = a.get(kv.first);
    if (!o) continue;
    const Node &n = kv.second;
    uint64_t changed = o->states ^ n.states;
    for (int s : {STATE_CHECKED, STATE_SELECTED, STATE_ENABLED, STATE_PRESSED})
      if (changed & bit(s))
        out.push_back({n.id, "state",
                       s == STATE_CHECKED ? "checked" : s == STATE_SELECTED ? "selected" : s == STATE_ENABLED ? "enabled" : "pressed",
                       (n.states & bit(s)) ? 1 : 0});
    if (o->name != n.name) out.push_back({n.id, "name", "", 0});
    if (n.has_text && o->text != n.text) out.push_back({n.id, "text", "", 0});
    if (n.has_value && o->value != n.value) out.push_back({n.id, "value", "", 0});
  }
  return out;
}

} // namespace atspi
} // namespace fld
