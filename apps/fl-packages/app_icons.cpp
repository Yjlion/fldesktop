#include "app_icons.h"

#include <appstream.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef HAVE_JXL
#include <jxl/decode.h>
#endif

namespace appicons {

std::map<std::string, AppInfo> load_catalog(std::string *err) {
  std::map<std::string, AppInfo> out;
  AsPool *pool = as_pool_new();
  GError *e = nullptr;
  if (!as_pool_load(pool, nullptr, &e)) {
    if (err) *err = e ? e->message : "AppStream data could not be loaded";
    if (e) g_error_free(e);
    g_object_unref(pool);
    return out;
  }
  AsComponentBox *box = as_pool_get_components(pool);
  for (guint i = 0; i < as_component_box_len(box); i++) {
    AsComponent *c = as_component_box_index(box, i);
    const gchar *pkg = as_component_get_pkgname(c);
    if (!pkg || !*pkg) continue;
    // desktop applications first: a package's other components (addons,
    // fonts) do not replace its application
    AsComponentKind kind = as_component_get_kind(c);
    auto it = out.find(pkg);
    bool is_app = kind == AS_COMPONENT_KIND_DESKTOP_APP || kind == AS_COMPONENT_KIND_CONSOLE_APP;
    if (it != out.end() && !is_app) continue;
    if (it != out.end() && !it->second.icon_file.empty()) continue;
    AppInfo info;
    if (const gchar *n = as_component_get_name(c)) info.name = n;
    GPtrArray *icons = as_component_get_icons(c);
    int best = -1;
    for (guint k = 0; icons && k < icons->len; k++) {
      AsIcon *ic = (AsIcon *)g_ptr_array_index(icons, k);
      AsIconKind ik = as_icon_get_kind(ic);
      if (ik == AS_ICON_KIND_STOCK) {
        if (info.stock.empty() && as_icon_get_name(ic)) info.stock = as_icon_get_name(ic);
      } else if (ik == AS_ICON_KIND_CACHED || ik == AS_ICON_KIND_LOCAL) {
        const gchar *f = as_icon_get_filename(ic);
        if (!f || f[0] != '/') continue;
        int w = (int)as_icon_get_width(ic) * (int)std::max(1u, as_icon_get_scale(ic));
        if (w <= 0) w = 64;
        // the size closest to 64, larger ones preferred
        int score = w >= 64 ? w - 64 : (64 - w) * 2;
        if (best < 0 || score < best) {
          best = score;
          info.icon_file = f;
        }
      }
    }
    out[pkg] = info;
  }
  g_object_unref(pool);
  return out;
}

#ifdef HAVE_JXL
bool have_jxl() { return true; }

bool decode_jxl(const std::string &path, int &w, int &h, std::vector<uint8_t> &rgba) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  std::vector<uint8_t> data;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
  fclose(f);
  JxlDecoder *dec = JxlDecoderCreate(nullptr);
  if (!dec) return false;
  bool ok = false;
  JxlPixelFormat fmt = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) == JXL_DEC_SUCCESS &&
      JxlDecoderSetInput(dec, data.data(), data.size()) == JXL_DEC_SUCCESS) {
    JxlDecoderCloseInput(dec);
    for (;;) {
      JxlDecoderStatus st = JxlDecoderProcessInput(dec);
      if (st == JXL_DEC_BASIC_INFO) {
        JxlBasicInfo info;
        if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) break;
        w = (int)info.xsize;
        h = (int)info.ysize;
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) break;
      } else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
        size_t size = 0;
        if (JxlDecoderImageOutBufferSize(dec, &fmt, &size) != JXL_DEC_SUCCESS) break;
        rgba.assign(size, 0);
        if (JxlDecoderSetImageOutBuffer(dec, &fmt, rgba.data(), rgba.size()) != JXL_DEC_SUCCESS) break;
      } else if (st == JXL_DEC_FULL_IMAGE) {
        ok = rgba.size() == (size_t)w * h * 4;
      } else if (st == JXL_DEC_SUCCESS) {
        break;
      } else {
        ok = false;
        break;
      }
    }
  }
  JxlDecoderDestroy(dec);
  return ok;
}
#else
bool have_jxl() { return false; }
bool decode_jxl(const std::string &, int &, int &, std::vector<uint8_t> &) { return false; }
#endif

} // namespace appicons
