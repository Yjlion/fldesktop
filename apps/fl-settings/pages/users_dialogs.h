// fl-settings, Users: the dialogs of the page (new user, password with a
// strength meter, removing, the picture picker, fingerprints) and the helper
// that runs the changes as root.
#pragma once
#include <FL/Fl_Box.H>
#include <FL/Fl_Image.H>
#include <FL/fl_draw.H>
#include <memory>
#include <string>
#include <vector>

#include "fld/avatar.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "users_model.h"

namespace fld {
namespace users_ui {


// One helper call; input goes to its stdin (passwords).
struct Job {
  std::vector<std::string> args;
  std::string input;
};

struct NewUser {
  std::string name, full, password;
  bool admin = false;
};

// A user's picture (or the initial on a colour of its own when there is none)
class Picture : public Fl_Box {
public:
  std::unique_ptr<Fl_Image> img;
  std::string user, real_name;
  Picture(int x, int y, int w, int h) : Fl_Box(x, y, w, h) {}
  void set(const std::string &path, const std::string &u, const std::string &real) {
    img.reset();
    user = u;
    real_name = real;
    if (!path.empty()) {
      std::unique_ptr<Fl_Image> full(load_image(path));
      if (full) img.reset(avatar_image(full.get(), w()));
    }
    redraw();
  }
  void draw() override {
    fl_color(palette().bg2);
    fl_rectf(x(), y(), w(), h());
    if (img) img->draw(x(), y());
    else if (!user.empty()) draw_initial_avatar(x(), y(), w(), user, real_name);
  }
};

// asks for a new password twice, with a strength meter; false when cancelled
bool ask_password(const std::string &title, std::string &out);
bool ask_new_user(const std::vector<users::Account> &existing, NewUser &out);
// asks before removing an account; remove_home: also its files
bool ask_remove(const users::Account &a, bool &remove_home);
// the account's picture file ("" when none)
std::string find_picture(const users::Account &a);
// a stock picture or an image file of your own; "" when cancelled
std::string pick_picture();
void fingerprint_dialog(const std::string &user);

} // namespace users_ui
} // namespace fld
