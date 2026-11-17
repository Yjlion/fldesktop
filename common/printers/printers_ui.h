// FLTK views for CUPS printers, shared by fl-printers and fl-settings.
#pragma once
#include <FL/Fl_Group.H>
#include <string>
#include <vector>

#include "cupsclient.h"

class Fl_Hold_Browser;
class Fl_Browser;
class Fl_Box;
class Fl_Button;
class Fl_Check_Button;
class Fl_Input;
class Fl_Choice;

namespace prn {

class PrintersView : public Fl_Group {
public:
  PrintersView(int x, int y, int w, int h);
  ~PrintersView();
  void refresh();                           // re-reads printers and jobs
  void select(const std::string &name);
  void resize(int x, int y, int w, int h) override;

private:
  std::vector<Printer> printers_;
  std::vector<Job> jobs_;
  std::string current_;
  Fl_Hold_Browser *list_;
  Fl_Button *add_, *remove_;
  Fl_Group *detail_;
  Fl_Box *icon_, *title_, *state_, *model_, *conn_, *empty_;
  Fl_Input *info_, *location_;
  Fl_Check_Button *enabled_, *accepting_, *shared_, *show_done_;
  Fl_Button *default_, *test_, *cancel_job_;
  Fl_Choice *opt_[3];
  std::vector<Option> options_;
  Fl_Browser *jobs_list_;
  std::string title_s_, state_s_, model_s_, conn_s_;

  void layout_children();
  void fill_list();
  void fill_detail();
  void fill_jobs();
  const Printer *printer() const;
  static void timer(void *);
};

// Opens the "Add Printer" dialog; returns the new queue name or "".
std::string add_printer_dialog();
// Installs an FLTK password dialog for CUPS authentication (any thread).
void install_password_prompt();
// FLTK helper: shows an error (if any) and returns true when there was one.
bool show_error(const std::string &what, const std::string &err);

} // namespace prn
