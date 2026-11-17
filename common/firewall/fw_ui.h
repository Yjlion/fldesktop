// FLTK view for firewalld, shared by fl-firewall and fl-settings.
#pragma once
#include <FL/Fl_Group.H>
#include <string>
#include <vector>

#include "fwclient.h"

class Fl_Box;
class Fl_Browser;
class Fl_Button;
class Fl_Check_Browser;
class Fl_Check_Button;
class Fl_Choice;
class Fl_Hold_Browser;
class Fl_Tabs;

namespace fw {

class FirewallView : public Fl_Group {
public:
  FirewallView(int x, int y, int w, int h);
  void refresh(); // re-reads Client::get().state()

private:
  Fl_Box *status_, *empty_, *zone_info_;
  Fl_Choice *default_zone_;
  Fl_Check_Button *panic_, *permanent_, *masq_;
  Fl_Button *reload_, *to_perm_;
  Fl_Hold_Browser *zones_;
  Fl_Tabs *tabs_;
  Fl_Check_Browser *services_;
  Fl_Hold_Browser *ports_, *ifaces_, *sources_, *forwards_;
  Fl_Choice *iface_zone_;
  std::string zone_, status_s_, info_s_;
  std::vector<std::string> zone_names_, service_names_;
  bool filling_ = false;

  const Zone *zone() const;
  bool permanent() const;
  void fill_zone();
  static void done_cb(const std::string &err);
};

} // namespace fw
