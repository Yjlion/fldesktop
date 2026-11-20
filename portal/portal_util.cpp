// Pure helpers of xdg-desktop-portal-fl (GVariant parsing, settings values)
#include <cmath>
#include <cstring>

#include "fld/util.h"
#include "portal.h"

namespace portal {

fld::FileFilter parse_filter(GVariant *f) {
  fld::FileFilter out;
  if (!f || !g_variant_is_of_type(f, G_VARIANT_TYPE("(sa(us))"))) return out;
  const char *name;
  GVariantIter *it;
  g_variant_get(f, "(&sa(us))", &name, &it);
  out.name = name;
  guint32 kind;
  const char *pat;
  while (g_variant_iter_next(it, "(u&s)", &kind, &pat)) {
    if (kind == 0) out.globs.push_back(pat);
    else out.mimes.push_back(pat);
  }
  g_variant_iter_free(it);
  return out;
}

std::vector<fld::FileFilter> parse_filters(GVariant *filters) {
  std::vector<fld::FileFilter> out;
  if (!filters || !g_variant_is_of_type(filters, G_VARIANT_TYPE("a(sa(us))"))) return out;
  GVariantIter it;
  g_variant_iter_init(&it, filters);
  GVariant *child;
  while ((child = g_variant_iter_next_value(&it))) {
    out.push_back(parse_filter(child));
    g_variant_unref(child);
  }
  return out;
}

GVariant *filter_to_variant(const fld::FileFilter &f) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a(us)"));
  for (auto &g : f.globs) g_variant_builder_add(&b, "(us)", 0u, g.c_str());
  for (auto &m : f.mimes) g_variant_builder_add(&b, "(us)", 1u, m.c_str());
  return g_variant_new("(sa(us))", f.name.c_str(), &b);
}

std::vector<fld::FileChoice> parse_choices(GVariant *choices) {
  std::vector<fld::FileChoice> out;
  if (!choices || !g_variant_is_of_type(choices, G_VARIANT_TYPE("a(ssa(ss)s)"))) return out;
  GVariantIter it;
  g_variant_iter_init(&it, choices);
  const char *id, *label, *sel;
  GVariantIter *opts;
  while (g_variant_iter_next(&it, "(&s&sa(ss)&s)", &id, &label, &opts, &sel)) {
    fld::FileChoice c;
    c.id = id;
    c.label = label;
    c.selected = sel;
    const char *oid, *olabel;
    while (g_variant_iter_next(opts, "(&s&s)", &oid, &olabel)) c.options.push_back({oid, olabel});
    g_variant_iter_free(opts);
    if (c.options.empty() && c.selected.empty()) c.selected = "false";
    out.push_back(c);
  }
  return out;
}

std::string bytestring(GVariant *v) {
  if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING)) return "";
  gsize n = 0;
  const char *s = (const char *)g_variant_get_fixed_array(v, &n, 1);
  std::string r(s, n);
  // drop the terminating NUL
  while (!r.empty() && r.back() == 0) r.pop_back();
  return r;
}

std::string path_to_uri(const std::string &path) {
  gchar *u = g_filename_to_uri(path.c_str(), nullptr, nullptr);
  std::string r = u ? u : "file://" + fld::uri_encode_path(path);
  g_free(u);
  return r;
}

std::string opt_string(GVariant *options, const char *key, const std::string &def) {
  const char *s = nullptr;
  if (options && g_variant_lookup(options, key, "&s", &s) && s) return s;
  return def;
}

bool opt_bool(GVariant *options, const char *key, bool def) {
  gboolean b;
  if (options && g_variant_lookup(options, key, "b", &b)) return b;
  return def;
}

// ---- settings --------------------------------------------------------------

static bool parse_hex(const std::string &s, double &r, double &g, double &b) {
  std::string h = fld::trim(s);
  if (h.size() != 7 || h[0] != '#') return false;
  unsigned v = (unsigned)strtoul(h.c_str() + 1, nullptr, 16);
  r = ((v >> 16) & 255) / 255.0;
  g = ((v >> 8) & 255) / 255.0;
  b = (v & 255) / 255.0;
  return true;
}

Appearance read_appearance(const fld::Ini &ini) {
  Appearance a;
  a.dark = fld::lower(ini.get("Appearance", "Theme", "light")) == "dark";
  parse_hex(ini.get("Appearance", "Accent", "#3584e4"), a.accent_r, a.accent_g, a.accent_b);
  a.icon_theme = ini.get("General", "IconTheme", "Adwaita");
  a.font = ini.get("Appearance", "Font", "Sans");
  a.mono_font = ini.get("Appearance", "MonoFont", "Monospace");
  a.font_size = ini.get_int("Appearance", "FontSize", 14);
  a.mono_size = ini.get_int("Appearance", "MonoFontSize", 14);
  a.high_contrast = fld::lower(ini.get("Appearance", "Contrast", "normal")) == "high";
  a.text_scale = ini.get_double("Appearance", "TextScale", 1.0);
  if (a.text_scale < 1.0 || a.text_scale > 2.0) a.text_scale = 1.0;
  return a;
}

namespace {

struct Setting {
  const char *ns, *key;
};
const Setting kSettings[] = {
    {"org.freedesktop.appearance", "color-scheme"},
    {"org.freedesktop.appearance", "accent-color"},
    {"org.freedesktop.appearance", "contrast"},
    {"org.gnome.desktop.interface", "color-scheme"},
    {"org.gnome.desktop.interface", "gtk-theme"},
    {"org.gnome.desktop.interface", "icon-theme"},
    {"org.gnome.desktop.interface", "font-name"},
    {"org.gnome.desktop.interface", "monospace-font-name"},
    {"org.gnome.desktop.interface", "text-scaling-factor"},
    {"org.gnome.desktop.a11y.interface", "high-contrast"},
};

// FLTK sizes are pixels; GTK font names use points (96 dpi)
std::string font_name(const std::string &family, int px) {
  int pt = (int)std::lround(px * 0.75);
  return family + " " + std::to_string(std::max(6, pt));
}

bool ns_wanted(const std::string &ns, const std::vector<std::string> &want) {
  if (want.empty()) return true;
  for (auto &w : want) {
    if (w.empty() || w == ns) return true;
    if (fld::ends_with(w, "*") && fld::starts_with(ns, w.substr(0, w.size() - 1))) return true;
  }
  return false;
}

} // namespace

GVariant *settings_read(const Appearance &a, const std::string &ns, const std::string &key) {
  if (ns == "org.freedesktop.appearance") {
    if (key == "color-scheme") return g_variant_new_uint32(a.dark ? 1 : 0);
    if (key == "accent-color") return g_variant_new("(ddd)", a.accent_r, a.accent_g, a.accent_b);
    if (key == "contrast") return g_variant_new_uint32(a.high_contrast ? 1 : 0);
  } else if (ns == "org.gnome.desktop.interface") {
    if (key == "color-scheme") return g_variant_new_string(a.dark ? "prefer-dark" : "default");
    if (key == "gtk-theme")
      return g_variant_new_string(a.high_contrast ? (a.dark ? "HighContrastInverse" : "HighContrast")
                                                  : (a.dark ? "Adwaita-dark" : "Adwaita"));
    if (key == "icon-theme") return g_variant_new_string(a.icon_theme.c_str());
    if (key == "font-name") return g_variant_new_string(font_name(a.font, a.font_size).c_str());
    if (key == "monospace-font-name") return g_variant_new_string(font_name(a.mono_font, a.mono_size).c_str());
    if (key == "text-scaling-factor") return g_variant_new_double(a.text_scale);
  } else if (ns == "org.gnome.desktop.a11y.interface") {
    if (key == "high-contrast") return g_variant_new_boolean(a.high_contrast);
  }
  return nullptr;
}

GVariant *settings_read_all(const Appearance &a, const std::vector<std::string> &namespaces) {
  GVariantBuilder outer;
  g_variant_builder_init(&outer, G_VARIANT_TYPE("a{sa{sv}}"));
  std::string cur;
  GVariantBuilder *inner = nullptr;
  for (auto &s : kSettings) {
    if (!ns_wanted(s.ns, namespaces)) continue;
    if (cur != s.ns) {
      if (inner) {
        g_variant_builder_add(&outer, "{sa{sv}}", cur.c_str(), inner);
        g_variant_builder_unref(inner);
      }
      inner = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
      cur = s.ns;
    }
    g_variant_builder_add(inner, "{sv}", s.key, settings_read(a, s.ns, s.key));
  }
  if (inner) {
    g_variant_builder_add(&outer, "{sa{sv}}", cur.c_str(), inner);
    g_variant_builder_unref(inner);
  }
  return g_variant_builder_end(&outer);
}

} // namespace portal
