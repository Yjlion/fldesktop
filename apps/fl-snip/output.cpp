// fl-snip: saving and copying snips
#include "fld/i18n.h"
#include "snip_app.h"

// ---------------------------------------------------------------------------
// Saving and copying

bool write_png(const Image &img, const std::string &path) {
  // drop the alpha channel when nothing is transparent
  bool alpha = false;
  for (size_t i = 3; i < img.px.size(); i += 4)
    if (img.px[i] != 255) {
      alpha = true;
      break;
    }
  if (alpha) return fl_write_png(path.c_str(), img.px.data(), img.w, img.h, 4) == 0;
  std::vector<uint8_t> rgb((size_t)img.w * img.h * 3);
  for (size_t i = 0, j = 0; i < img.px.size(); i += 4, j += 3) {
    rgb[j] = img.px[i];
    rgb[j + 1] = img.px[i + 1];
    rgb[j + 2] = img.px[i + 2];
  }
  return fl_write_png(path.c_str(), rgb.data(), img.w, img.h, 3) == 0;
}

std::string auto_save(const Image &img) {
  std::string dir = screenshots_dir();
  fld::mkdir_p(dir);
  std::string name = snip::file_name(time(nullptr));
  std::string stem = name.substr(0, name.size() - 4); // without ".png"
  std::string path = dir + "/" + name;
  for (int i = 2; fld::file_exists(path); i++) path = dir + "/" + stem + "-" + std::to_string(i) + ".png";
  return write_png(img, path) ? path : "";
}

// wl-copy keeps the picture on the clipboard after fl-snip exits; FLTK's
// clipboard only lasts while it runs
void copy_image(const Image &img, bool lasting) {
  if (lasting && !fld::which("wl-copy").empty()) {
    std::string tmp = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-snip-copy.png";
    if (write_png(img, tmp)) {
      fld::spawn_shell("wl-copy --type image/png < " + fld::shell_quote(tmp) + "; rm -f " + fld::shell_quote(tmp));
      return;
    }
  }
  std::vector<uint8_t> rgb((size_t)img.w * img.h * 3);
  for (size_t i = 0, j = 0; i < img.px.size(); i += 4, j += 3) {
    uint8_t a = img.px[i + 3];
    // transparent parts become white
    for (int c = 0; c < 3; c++) rgb[j + c] = (uint8_t)((img.px[i + c] * a + 255 * (255 - a)) / 255);
  }
  // what is drawn on a copy surface goes to the clipboard when it is deleted
  Fl_RGB_Image pic(rgb.data(), img.w, img.h, 3);
  auto *cs = new Fl_Copy_Surface(img.w, img.h);
  Fl_Surface_Device::push_current(cs);
  pic.draw(0, 0);
  Fl_Surface_Device::pop_current();
  delete cs;
}
