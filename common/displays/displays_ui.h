// The screen arrangement editor: a drawing of the screens, their list, and
// resolution, scale, rotation and position of the selected one. Apply asks
// to keep the new configuration and reverts after 15 seconds otherwise.
// In fl-settings (Displays) and fl-displays.
#pragma once
#include <FL/Fl_Group.H>
#include <functional>
#include <vector>

#include "outputs.h"

class Fl_Box;
class Fl_Button;
class Fl_Check_Button;
class Fl_Choice;
class Fl_Hold_Browser;
class Fl_Spinner;

namespace fld {

class DisplaysView : public Fl_Group {
public:
  // h is the height with several screens (kHeight); with one screen there
  // is nothing to arrange and the view is kSingleHeight high
  static const int kHeight = 440, kSingleHeight = 190;
  DisplaysView(int x, int y, int w);

  // after a configuration was kept: to remember it (fl-wm's wm.ini)
  std::function<void(const std::vector<outputs::HeadConfig> &)> on_saved;
  // after the height changed (one screen <-> several)
  std::function<void(int dy)> on_resize;

  void refresh();  // from outputs::on_changed

private:
  class Arrangement;
  Arrangement *arr_;
  Fl_Hold_Browser *list_;
  Fl_Check_Button *enabled_;
  Fl_Choice *mode_, *scale_, *rotation_;
  Fl_Spinner *px_, *py_;
  Fl_Box *pos_label_;
  Fl_Button *apply_btn_;
  Fl_Box *status_;
  int sel_ = 0;
  bool dirty_ = false, single_ = false;
  std::vector<outputs::HeadConfig> edited_;

  void set_single(bool single);
  void show_head();
  void edit();
  bool confirm();
  void do_apply();
};

}  // namespace fld
