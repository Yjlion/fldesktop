// FLTK views for NetworkManager, shared by fl-network and fl-settings.
#pragma once
#include <FL/Fl_Group.H>
#include <string>
#include <vector>

class Fl_Hold_Browser;
class Fl_Check_Button;
class Fl_Box;
class Fl_Button;

namespace nm {

class ApList;

class NetworkView : public Fl_Group {
public:
  NetworkView(int x, int y, int w, int h);
  void refresh();        // re-reads Client::get().state()
  void resize(int x, int y, int w, int h) override;
  // the device path or "" for "All connections"
  const std::string &page() const { return page_; }

private:
  Fl_Check_Button *networking_, *wireless_;
  Fl_Box *summary_;
  Fl_Hold_Browser *sidebar_;
  Fl_Group *dev_page_, *conn_page_;
  // device page
  Fl_Box *dev_icon_, *dev_title_, *dev_state_, *dev_info_;
  Fl_Button *dev_toggle_, *dev_edit_;
  ApList *aps_;
  Fl_Button *wifi_connect_, *wifi_hidden_, *wifi_scan_;
  // connections page
  Fl_Hold_Browser *conns_;
  Fl_Button *c_add_wired_, *c_add_wifi_, *c_edit_, *c_delete_, *c_toggle_;
  std::string page_;          // device path, or "*" for all connections
  bool user_chose_ = false;   // the user picked a page (otherwise follow the first device)
  std::string title_s_, state_s_, info_s_, summary_s_;
  std::vector<std::string> sidebar_keys_, conn_uuids_;

  void layout_children();
  void show_page(const std::string &key);
  void fill_device_page();
  void fill_connections_page();
  std::string selected_uuid() const;

  static void sidebar_cb(Fl_Widget *, void *);
  static void toggle_device_cb(Fl_Widget *, void *);
  static void edit_device_cb(Fl_Widget *, void *);
  static void wifi_connect_cb(Fl_Widget *, void *);
  static void wifi_hidden_cb(Fl_Widget *, void *);
  static void conn_add_cb(Fl_Widget *, void *);
  static void conn_edit_cb(Fl_Widget *, void *);
  static void conn_delete_cb(Fl_Widget *, void *);
  static void conn_toggle_cb(Fl_Widget *, void *);
};

// Shows the connection editor for uuid (or a new connection of `type` when uuid
// is empty: "802-3-ethernet" or "802-11-wireless").
void edit_connection(const std::string &uuid, const std::string &type = "");
// Asks for a Wi-Fi password; returns "" when cancelled.
std::string ask_wifi_password(const std::string &ssid, bool retry = false);

} // namespace nm
