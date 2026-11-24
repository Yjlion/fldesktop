#include "fld/toolkit_theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "fld/accents.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"

namespace fld {

namespace {
const char *kBegin = "/* fldesktop begin: written from Settings > Appearance; changes here are replaced */";
const char *kEnd = "/* fldesktop end */";

bool valid_hex(const std::string &s) {
  if (s.size() != 7 || s[0] != '#') return false;
  for (size_t i = 1; i < 7; i++)
    if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

// black or white text, whichever reads better on c
std::string text_on_hex(const std::string &c) {
  unsigned v = (unsigned)strtoul(c.c_str() + 1, nullptr, 16);
  double r = ((v >> 16) & 255) / 255.0, g = ((v >> 8) & 255) / 255.0, b = (v & 255) / 255.0;
  auto lin = [](double x) { return x <= 0.03928 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); };
  double l = 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
  return (1.05 / (l + 0.05)) >= ((l + 0.05) / 0.05) ? "#ffffff" : "#000000";
}

std::string mix(const std::string &a, const std::string &b, double t) {
  unsigned x = (unsigned)strtoul(a.c_str() + 1, nullptr, 16), y = (unsigned)strtoul(b.c_str() + 1, nullptr, 16);
  char buf[8];
  int c[3];
  for (int i = 0; i < 3; i++) {
    int sh = 16 - 8 * i;
    c[i] = (int)std::lround(((x >> sh) & 255) * (1 - t) + ((y >> sh) & 255) * t);
  }
  snprintf(buf, sizeof buf, "#%02x%02x%02x", c[0], c[1], c[2]);
  return buf;
}
} // namespace

ToolkitTheme toolkit_theme_from(const Ini &d) {
  ToolkitTheme t;
  const std::string A = "Appearance";
  t.dark = lower(d.get(A, "Theme", "light")) == "dark";
  t.high_contrast = lower(d.get(A, "Contrast", "normal")) == "high";
  std::string acc = trim(d.get(A, "Accent", "#3584e4"));
  t.accent = valid_hex(acc) ? lower(acc) : "#3584e4";
  // the same colours as the FLTK programs (theme.cpp)
  if (t.high_contrast) {
    t.accent = t.dark ? "#ffd700" : "#003da5";
    t.window = t.base = t.dark ? "#000000" : "#ffffff";
    t.text = t.dark ? "#ffffff" : "#000000";
    t.text_dim = t.dark ? "#dddddd" : "#222222";
    t.border = t.text;
    t.button = t.window;
  } else if (t.dark) {
    t.window = "#2b2d31"; t.base = "#1e1f22"; t.text = "#e6e6e6";
    t.text_dim = "#9a9da3"; t.border = "#45484e"; t.button = "#35383d";
  } else {
    t.window = "#eeeff1"; t.base = "#ffffff"; t.text = "#222428";
    t.text_dim = "#6b6f76"; t.border = "#c4c7cc"; t.button = "#f6f6f7";
  }
  t.accent_fg = text_on_hex(t.accent);
  t.icon_theme = effective_icon_theme(d, nullptr);
  t.font = trim(d.get(A, "Font", "Sans"));
  if (t.font.empty()) t.font = "Sans";
  double scale = d.get_double(A, "TextScale", 1.0);
  if (scale < 1.0 || scale > 2.0) scale = 1.0;
  // FLTK sizes are pixels at 96 dpi; GTK and Qt want points
  t.font_pt = std::max(6, (int)std::lround(d.get_int(A, "FontSize", 14) * 0.75 * scale));
  return t;
}

std::string gtk_accent_css(const ToolkitTheme &t, int gtk_major) {
  std::string a = t.accent, f = t.accent_fg;
  std::string s = std::string(kBegin) + "\n";
  s += "@define-color accent_bg_color " + a + ";\n";
  s += "@define-color accent_color " + (t.dark ? mix(a, "#ffffff", 0.25) : mix(a, "#000000", 0.1)) + ";\n";
  s += "@define-color accent_fg_color " + f + ";\n";
  if (gtk_major >= 4) {
    // libadwaita 1.6 and later: the CSS variables
    s += ":root { --accent-bg-color: " + a + "; --accent-fg-color: " + f + "; --accent-color: " +
         (t.dark ? mix(a, "#ffffff", 0.25) : mix(a, "#000000", 0.1)) + "; }\n";
  } else {
    // GTK 3's Adwaita has the blue built in: name the parts that use it
    s += "@define-color theme_selected_bg_color " + a + ";\n";
    s += "@define-color theme_selected_fg_color " + f + ";\n";
    s += "@define-color selected_bg_color " + a + ";\n";
    s += "@define-color selected_fg_color " + f + ";\n";
    s += "selection, *:selected, *:selected:focus, .view:selected, iconview:selected,\n"
         "treeview.view:selected, row:selected, flowboxchild:selected, calendar:selected,\n"
         "textview text selection, entry selection, label selection, spinbutton selection {\n"
         "  background-color: " + a + "; color: " + f + "; }\n";
    s += "check:checked, radio:checked, check:indeterminate, radio:indeterminate {\n"
         "  background-image: image(" + a + "); border-color: " + mix(a, "#000000", 0.2) + "; color: " + f + "; }\n";
    s += "switch:checked { background-image: image(" + a + "); border-color: " + mix(a, "#000000", 0.2) + "; }\n";
    s += "progressbar progress, scale highlight, levelbar block.filled {\n"
         "  background-image: image(" + a + "); background-color: " + a + "; border-color: " + mix(a, "#000000", 0.2) + "; }\n";
    s += "button.suggested-action, headerbar button.suggested-action {\n"
         "  background-image: image(" + a + "); border-color: " + mix(a, "#000000", 0.25) + "; color: " + f + "; }\n";
    s += "button.suggested-action:hover { background-image: image(" + mix(a, "#ffffff", 0.1) + "); }\n";
    s += "button.suggested-action:active, button.suggested-action:checked { background-image: image(" + mix(a, "#000000", 0.15) + "); }\n";
    s += "entry:focus, spinbutton:focus:not(.vertical), textview:focus { border-color: " + a +
         "; box-shadow: inset 0 0 0 1px " + a + "; }\n";
    s += "notebook > header > tabs > tab:checked { box-shadow: inset 0 -3px " + a + "; }\n";
    s += "notebook > header.top > tabs > tab:checked { box-shadow: inset 0 -3px " + a + "; }\n";
    s += "*:link, button.link > label { color: " + (t.dark ? mix(a, "#ffffff", 0.3) : a) + "; }\n";
    s += "stackswitcher button:checked, button.toggle:checked { box-shadow: inset 0 -2px " + a + "; }\n";
  }
  s += kEnd;
  s += "\n";
  return s;
}

std::string replace_marked_block(const std::string &css, const std::string &block) {
  size_t b = css.find(kBegin);
  if (b != std::string::npos) {
    size_t e = css.find(kEnd, b);
    if (e != std::string::npos) {
      e += strlen(kEnd);
      if (e < css.size() && css[e] == '\n') e++;
      return css.substr(0, b) + block + css.substr(e);
    }
  }
  // new: first, so the user's own rules after it still win
  return block + (css.empty() || css[0] == '\n' ? "" : "\n") + css;
}

std::string qtct_color_scheme(const ToolkitTheme &t, int qt_major) {
  // QPalette::ColorRole order: WindowText, Button, Light, Midlight, Dark,
  // Mid, Text, BrightText, ButtonText, Base, Window, Shadow, Highlight,
  // HighlightedText, Link, LinkVisited, AlternateBase, NoRole, ToolTipBase,
  // ToolTipText, PlaceholderText (, Accent in Qt 6.6)
  auto row = [&](bool disabled) {
    std::string fg = disabled ? t.text_dim : t.text;
    std::string light = mix(t.button, "#ffffff", 0.6), mid = mix(t.button, t.border, 0.5);
    std::string link = t.dark ? mix(t.accent, "#ffffff", 0.3) : t.accent;
    std::vector<std::string> v = {
        fg, t.button, light, mix(t.button, light, 0.5), t.border, mid,
        fg, "#ffffff", fg, t.base, t.window, t.dark ? "#000000" : "#767676",
        disabled ? mix(t.accent, t.window, 0.5) : t.accent, t.accent_fg,
        link, mix(link, "#800080", 0.4), mix(t.base, t.window, 0.5), "#000000",
        t.dark ? "#101012" : "#303236", "#ffffff", t.text_dim};
    if (qt_major >= 6) v.push_back(t.accent);
    std::string s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + std::string("#ff") + v[i].substr(1);
    return s;
  };
  return "# written from Settings > Appearance (fldesktop)\n[ColorScheme]\nactive_colors=" + row(false) +
         "\ninactive_colors=" + row(false) + "\ndisabled_colors=" + row(true) + "\n";
}

namespace {
bool write_if_changed(const std::string &path, const std::string &data) {
  std::string old;
  if (read_file(path, old) && old == data) return true;
  mkdir_p(dirname(path));
  std::string tmp = path + ".fld-new";
  if (!write_file(tmp, data)) return false;
  return rename(tmp.c_str(), path.c_str()) == 0;
}

bool merge_ini(const std::string &path, const std::string &group,
               const std::vector<std::pair<std::string, std::string>> &kv) {
  Ini ini;
  ini.load(path);
  for (auto &p : kv)
    if (!p.second.empty()) ini.set(group, p.first, p.second);
  return write_if_changed(path, ini.to_string());
}
} // namespace

bool export_toolkit_theme(const Ini &desktop, const std::string &config_dir) {
  if (!desktop.get_bool("Appearance", "ToolkitColors", true)) return false;
  std::string dir = config_dir.empty() ? config_home() : config_dir;
  ToolkitTheme t = toolkit_theme_from(desktop);
  bool ok = true;
  std::string font = t.font + " " + std::to_string(t.font_pt);

  // GTK 3
  std::string gtk3_theme = t.high_contrast ? (t.dark ? "HighContrastInverse" : "HighContrast") : "Adwaita";
  ok &= merge_ini(dir + "/gtk-3.0/settings.ini", "Settings",
                  {{"gtk-application-prefer-dark-theme", t.dark ? "1" : "0"},
                   {"gtk-theme-name", gtk3_theme},
                   {"gtk-icon-theme-name", t.icon_theme},
                   {"gtk-font-name", font}});
  for (int major : {3, 4}) {
    std::string path = dir + "/gtk-" + std::to_string(major) + ".0/gtk.css", css;
    read_file(path, css);
    ok &= write_if_changed(path, replace_marked_block(css, gtk_accent_css(t, major)));
  }
  // GTK 4: libadwaita takes light/dark from the portal and complains about
  // gtk-application-prefer-dark-theme, so only icons and the font here
  ok &= merge_ini(dir + "/gtk-4.0/settings.ini", "Settings",
                  {{"gtk-icon-theme-name", t.icon_theme}, {"gtk-font-name", font}});

  // Qt through qt5ct / qt6ct
  for (int major : {5, 6}) {
    std::string base = dir + "/qt" + std::to_string(major) + "ct";
    std::string scheme = base + "/colors/fldesktop.conf";
    ok &= write_if_changed(scheme, qtct_color_scheme(t, major));
    std::string conf = base + "/qt" + std::to_string(major) + "ct.conf";
    Ini q;
    bool exists = q.load(conf);
    std::string cur = q.get("Appearance", "color_scheme_path", "");
    // the user's own qtNct setup stays as it is
    if (exists && !cur.empty() && cur != scheme) continue;
    if (exists && cur.empty() && q.has("Appearance", "custom_palette")) continue;
    q.set("Appearance", "color_scheme_path", scheme);
    q.set("Appearance", "custom_palette", "true");
    if (!t.icon_theme.empty()) q.set("Appearance", "icon_theme", t.icon_theme);
    if (!q.has("Appearance", "style")) q.set("Appearance", "style", "Fusion");
    ok &= write_if_changed(conf, q.to_string());
  }
  return ok;
}

bool export_toolkit_theme() {
  Ini d;
  d.load(desktop_config_path());
  return export_toolkit_theme(d);
}

} // namespace fld
