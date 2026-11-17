// Pickers shared by Settings pages: accent color swatches, wallpaper gallery
#pragma once
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Widget.H>
#include <memory>
#include <string>
#include <vector>

class Fl_Image;

namespace fld {

// A row of round color swatches for the accent presets plus a "Custom"
// swatch that opens the color chooser. do_callback() runs when the user
// picks a color; value() is "#rrggbb".
class AccentSwatches : public Fl_Widget {
public:
  AccentSwatches(int x, int y, int w, int h);
  void value(const std::string &hex);
  const std::string &value() const { return value_; }
  // index of the selected preset or -1 for a custom color
  int preset() const;
  int handle(int e) override;
  static int preferred_width(int h);
protected:
  void draw() override;
private:
  std::string value_ = "#3584e4", custom_;
  int hover_ = -1;
  int swatch_at(int mx, int my) const;
  void swatch_box(int i, int &cx, int &cy, int &r) const;
};

// A horizontally scrolling row of wallpaper thumbnails. do_callback() runs
// when one is clicked; value() is its path.
class WallpaperGallery : public Fl_Scroll {
public:
  WallpaperGallery(int x, int y, int w, int h);
  ~WallpaperGallery() override;
  // the image files of these directories (and their own files) in name order
  void load(const std::vector<std::string> &dirs);
  void value(const std::string &path);
  const std::string &value() const { return value_; }
  // adds a file that is not in the directories (the current custom choice)
  void add_file(const std::string &path);
  static std::vector<std::string> default_dirs();
private:
  class Thumb;
  std::string value_;
  std::vector<Thumb *> thumbs_;
  void relayout();
  friend class Thumb;
};

} // namespace fld
