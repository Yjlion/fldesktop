// User pictures: the same look in Settings > Users, the greeter and the lock
// screen. A picture is shown as a circle; an account without one gets a
// circle in a color of its own with the first letter of its name.
#pragma once

#include <FL/Enumerations.H>
#include <string>

class Fl_Image;
class Fl_RGB_Image;

namespace fld {

// One of the accent colors, picked from the user name: the same user always
// gets the same color.
Fl_Color avatar_color(const std::string &user);
std::string avatar_color_hex(const std::string &user);
// The first letter of the real name, else of the user name, in upper case
// (UTF-8); "?" when both are empty.
std::string avatar_initial(const std::string &user, const std::string &real_name);
// Draws the circle with the initial (FLTK drawing, in the current window).
void draw_initial_avatar(int x, int y, int size, const std::string &user, const std::string &real_name);

// A size x size copy of img with straight-alpha RGBA pixels: scaled to cover
// the square, center-cropped and, with `round`, clipped to a circle with a
// smooth edge. The caller owns it; nullptr if img has no pixels.
Fl_RGB_Image *avatar_image(Fl_Image *img, int size, bool round = true);

// img scaled to cover a size x size square and center-cropped, as PNG data.
bool avatar_png(Fl_Image *img, int size, std::string &png);
// The initial on its color in a circle (transparent corners), as PNG data:
// the picture new accounts start with.
bool initial_avatar_png(const std::string &user, const std::string &real_name, int size, std::string &png);

} // namespace fld
