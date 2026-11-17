// FLTK views for Bluetooth, shared by fl-bluetooth, fl-settings and fl-panel.
#pragma once
#include <FL/Fl_Group.H>
#include <functional>
#include <string>
#include <vector>

#include "btclient.h"

class Fl_Hold_Browser;
class Fl_Box;
class Fl_Button;
class Fl_Check_Button;
class Fl_Input;

namespace bt {

class BluetoothView : public Fl_Group {
public:
  BluetoothView(int x, int y, int w, int h);
  ~BluetoothView();
  void refresh(); // re-reads Client::get().state()

private:
  Fl_Check_Button *power_, *visible_;
  Fl_Box *summary_, *empty_;
  Fl_Hold_Browser *mine_, *nearby_;
  Fl_Box *mine_label_, *nearby_label_;
  Fl_Button *connect_, *pair_, *forget_;
  Fl_Check_Button *trusted_;
  std::vector<std::string> mine_paths_, nearby_paths_;
  std::string selected_;
  std::string summary_s_;
  bool discovering_ = false;

  void select(const std::string &path);
  void update_buttons();
  void set_discovery(bool on);
  static void list_cb(Fl_Widget *, void *);
};

// The pairing agent's dialogs (FLTK thread); use with Client::start_agent.
void ask_agent(const AgentRequest &r, std::function<void(bool, const std::string &)> reply);
// Text for a device's state line ("Connected · battery 80%").
std::string device_status(const Device &d);
// Shows an error in a dialog (if any).
void show_error(const std::string &what, const std::string &err);

} // namespace bt
