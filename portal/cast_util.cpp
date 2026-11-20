// xdg-desktop-portal-fl: ScreenCast helpers without I/O (unit tested)
#include <cstring>

#include "portal.h"

namespace portal {

// restore_data is (suv): vendor "fl", version 1 and a(usss) with one entry
// per shared source: kind, output name, app_id, title.
GVariant *cast_restore_to_variant(const std::vector<CastSource> &chosen) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a(usss)"));
  for (const CastSource &s : chosen)
    g_variant_builder_add(&b, "(usss)", (guint32)s.kind, s.kind == CAST_MONITOR ? s.id.c_str() : "",
                          s.app_id.c_str(), s.title.c_str());
  return g_variant_new("(suv)", "fl", (guint32)1, g_variant_builder_end(&b));
}

std::vector<CastSource> cast_parse_restore(GVariant *restore_data) {
  std::vector<CastSource> out;
  if (!restore_data || !g_variant_is_of_type(restore_data, G_VARIANT_TYPE("(suv)"))) return out;
  const char *vendor;
  guint32 version;
  GVariant *data;
  g_variant_get(restore_data, "(&suv)", &vendor, &version, &data);
  if (strcmp(vendor, "fl") == 0 && version == 1 && g_variant_is_of_type(data, G_VARIANT_TYPE("a(usss)"))) {
    GVariantIter it;
    g_variant_iter_init(&it, data);
    guint32 kind;
    const char *id, *app_id, *title;
    while (g_variant_iter_next(&it, "(u&s&s&s)", &kind, &id, &app_id, &title)) {
      if (kind != CAST_MONITOR && kind != CAST_WINDOW) {
        out.clear();
        break;
      }
      out.push_back({(int)kind, id, title, app_id});
    }
  }
  g_variant_unref(data);
  return out;
}

std::vector<int> cast_match_restore(const std::vector<CastSource> &wanted, const std::vector<CastSource> &available) {
  std::vector<int> picked;
  std::vector<bool> used(available.size(), false);
  for (const CastSource &w : wanted) {
    int best = -1;
    for (size_t i = 0; i < available.size(); i++) {
      const CastSource &a = available[i];
      if (used[i] || a.kind != w.kind) continue;
      if (w.kind == CAST_MONITOR) {
        if (a.id == w.id) best = (int)i;
      } else if (a.app_id == w.app_id && !w.app_id.empty()) {
        // a window of the same program; the same title wins
        if (best < 0 || a.title == w.title) best = (int)i;
        if (a.title == w.title) break;
      }
      if (best >= 0 && w.kind == CAST_MONITOR) break;
    }
    if (best < 0) return {};
    used[best] = true;
    picked.push_back(best);
  }
  return picked;
}

bool cast_embed_cursor(guint32 cursor_mode) { return (cursor_mode & (CAST_CURSOR_EMBEDDED | CAST_CURSOR_METADATA)) != 0; }

} // namespace portal
