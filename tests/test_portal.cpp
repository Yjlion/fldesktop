// xdg-desktop-portal-fl helpers: option parsing and settings values
#include <glib.h>

#include "portal.h"
#include "test.h"

static GVariant *parse(const char *type, const char *text) {
  GError *err = nullptr;
  GVariant *v = g_variant_parse(type ? G_VARIANT_TYPE(type) : nullptr, text, nullptr, nullptr, &err);
  if (!v) fprintf(stderr, "parse %s: %s\n", text, err->message);
  return v ? g_variant_ref_sink(v) : nullptr;
}

int main() {
  using namespace portal;
  // filters
  GVariant *f = parse("a(sa(us))", "[('Images', [(0, '*.png'), (1, 'image/jpeg')]), ('All', [(0, '*')])]");
  auto filters = parse_filters(f);
  CHECK_EQ(filters.size(), (size_t)2);
  CHECK_EQ(filters[0].name, std::string("Images"));
  CHECK_EQ(filters[0].globs.size(), (size_t)1);
  CHECK_EQ(filters[0].mimes[0], std::string("image/jpeg"));
  CHECK(filters[0].matches("photo.PNG"));
  CHECK(!filters[0].matches("notes.txt"));
  CHECK(filters[1].matches("anything"));
  GVariant *back = g_variant_ref_sink(filter_to_variant(filters[0]));
  gchar *s = g_variant_print(back, FALSE);
  CHECK_EQ(std::string(s), std::string("('Images', [(0, '*.png'), (1, 'image/jpeg')])"));
  g_free(s);
  g_variant_unref(back);
  g_variant_unref(f);
  // choices
  GVariant *c = parse("a(ssa(ss)s)", "[('enc', 'Encoding', [('utf8', 'UTF-8'), ('l1', 'Latin-1')], 'l1'), "
                                     "('ro', 'Read only', [], '')]");
  auto ch = parse_choices(c);
  CHECK_EQ(ch.size(), (size_t)2);
  CHECK_EQ(ch[0].options.size(), (size_t)2);
  CHECK_EQ(ch[0].selected, std::string("l1"));
  CHECK(ch[1].options.empty());
  CHECK_EQ(ch[1].selected, std::string("false"));
  g_variant_unref(c);
  // byte strings (NUL terminated, as portals send them)
  GVariant *opts = parse("a{sv}", "{'current_folder': <b'/tmp/x y'>, 'accept_label': <'_Open'>, 'multiple': <true>}");
  GVariant *bs = g_variant_lookup_value(opts, "current_folder", G_VARIANT_TYPE_BYTESTRING);
  CHECK(bs != nullptr);
  CHECK_EQ(bytestring(bs), std::string("/tmp/x y"));
  if (bs) g_variant_unref(bs);
  CHECK_EQ(opt_string(opts, "accept_label"), std::string("_Open"));
  CHECK(opt_bool(opts, "multiple"));
  CHECK(!opt_bool(opts, "directory"));
  g_variant_unref(opts);
  CHECK_EQ(path_to_uri("/tmp/a b#c"), std::string("file:///tmp/a%20b%23c"));
  // settings
  fld::Ini ini;
  ini.parse("[Appearance]\nTheme=dark\nAccent=#ff0000\nFont=Cantarell\nFontSize=16\n[General]\nIconTheme=Papirus\n");
  Appearance a = read_appearance(ini);
  CHECK(a.dark);
  CHECK_NEAR(a.accent_r, 1.0);
  CHECK_NEAR(a.accent_g, 0.0);
  GVariant *all = g_variant_ref_sink(settings_read_all(a, {"org.freedesktop.appearance"}));
  s = g_variant_print(all, FALSE);
  CHECK_EQ(std::string(s), std::string("{'org.freedesktop.appearance': {'color-scheme': <uint32 1>, "
                                       "'accent-color': <(1.0, 0.0, 0.0)>, 'contrast': <uint32 0>}}"));
  g_free(s);
  g_variant_unref(all);
  all = g_variant_ref_sink(settings_read_all(a, {"org.gnome.*"}));
  CHECK(g_variant_n_children(all) == 2); // desktop.interface and desktop.a11y.interface
  g_variant_unref(all);
  all = g_variant_ref_sink(settings_read_all(a, {}));
  CHECK(g_variant_n_children(all) == 3);
  g_variant_unref(all);
  GVariant *font = g_variant_ref_sink(settings_read(a, "org.gnome.desktop.interface", "font-name"));
  CHECK_EQ(std::string(g_variant_get_string(font, nullptr)), std::string("Cantarell 12"));
  g_variant_unref(font);
  CHECK(settings_read(a, "org.example", "nothing") == nullptr);
  // accessibility: contrast and text size
  {
    fld::Ini hc;
    hc.parse("[Appearance]\nContrast=high\nTextScale=1.5\n");
    Appearance h = read_appearance(hc);
    CHECK(h.high_contrast);
    GVariant *c = g_variant_ref_sink(settings_read(h, "org.freedesktop.appearance", "contrast"));
    CHECK_EQ(g_variant_get_uint32(c), 1u);
    g_variant_unref(c);
    GVariant *t = g_variant_ref_sink(settings_read(h, "org.gnome.desktop.interface", "text-scaling-factor"));
    CHECK(std::fabs(g_variant_get_double(t) - 1.5) < 1e-9);
    g_variant_unref(t);
    GVariant *b = g_variant_ref_sink(settings_read(h, "org.gnome.desktop.a11y.interface", "high-contrast"));
    CHECK(g_variant_get_boolean(b));
    g_variant_unref(b);
    GVariant *g = g_variant_ref_sink(settings_read(h, "org.gnome.desktop.interface", "gtk-theme"));
    CHECK_EQ(std::string(g_variant_get_string(g, nullptr)), std::string("HighContrast"));
    g_variant_unref(g);
    fld::Ini bad;
    bad.parse("[Appearance]\nTextScale=9\n");
    CHECK(read_appearance(bad).text_scale == 1.0);
  }
  return test_result("portal");
}
