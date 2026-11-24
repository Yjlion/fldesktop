// fl-secretd --ask unlock|new|change <keyring label> [<message>]
// The password dialogs, in their own process (the daemon has no window).
// Prints the password (for "change": old and new on two lines); exit 1 when
// cancelled.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_ask.H>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "prompt.h"

int ask_main(const std::string &mode, const std::string &label, const std::string &message) {
  fld::init_app("fl-secretd");
  const fld::Palette &pal = fld::palette();
  bool fresh = mode == "new", change = mode == "change";
  const int W = 460;
  std::string title = fresh ? "New Keyring" : change ? "Change Keyring Password" : "Unlock Keyring";
  Fl_Double_Window w(W, 220);
  w.copy_label(title.c_str());
  int y = 16;
  Fl_Box *icon = new Fl_Box(16, y, 48, 48);
  icon->image(fld::icon_first({"dialog-password", "fl-keyring", "security-high"}, 48));
  std::string text =
      fresh ? "Choose a password for the keyring \"" + label + "\". It protects the passwords programs keep in it."
      : change ? "Enter the current password of \"" + label + "\" and the new one."
               : "A program wants to use the keyring \"" + label + "\". Enter its password to unlock it.";
  Fl_Box *msg = new Fl_Box(76, y, W - 92, 56);
  msg->copy_label(fld::escape_at(text).c_str());
  msg->labelsize(13);
  msg->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  y += 64;
  Fl_Box *err = new Fl_Box(76, y, W - 92, 20);
  err->labelcolor(pal.danger);
  err->labelsize(13);
  err->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  if (!message.empty()) err->copy_label(fld::escape_at(message).c_str());
  y += 24;
  Fl_Secret_Input *old_pw = nullptr, *pw, *again = nullptr;
  if (change) {
    old_pw = new Fl_Secret_Input(170, y, W - 186, 28, "Current password:");
    y += 36;
  }
  pw = new Fl_Secret_Input(170, y, W - 186, 28, fresh || change ? "New password:" : "Password:");
  y += 36;
  if (fresh || change) {
    again = new Fl_Secret_Input(170, y, W - 186, 28, "Again:");
    y += 36;
  }
  y += 8;
  Fl_Return_Button *ok = new Fl_Return_Button(W - 116, y, 100, 30, fresh ? "Create" : change ? "Change" : "Unlock");
  Fl_Button *cancel = new Fl_Button(W - 222, y, 100, 30, "Cancel");
  w.size(W, y + 46);
  w.end();
  int result = -1;
  ok->callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
  cancel->callback([](Fl_Widget *, void *r) { *(int *)r = 0; }, &result);
  w.callback([](Fl_Widget *, void *r) { *(int *)r = 0; }, &result);
  w.show();
  (old_pw ? old_pw : pw)->take_focus();
  for (;;) {
    result = -1;
    while (result < 0 && w.shown()) Fl::wait();
    if (result != 1) return 1;
    if (again && strcmp(pw->value(), again->value()) != 0) {
      err->label("The two new passwords are not the same.");
      w.redraw();
      again->value("");
      pw->take_focus();
      continue;
    }
    if (fresh && !*pw->value() &&
        fl_choice("Without a password the keyring is not protected: anyone using this account can read it.",
                  "Cancel", "Use No Password", nullptr) != 1) {
      continue;
    }
    break;
  }
  std::string out = change ? std::string(old_pw->value()) + "\n" + pw->value() : pw->value();
  fwrite(out.data(), 1, out.size(), stdout);
  fputc('\n', stdout);
  fflush(stdout);
  std::fill(out.begin(), out.end(), '\0');
  return 0;
}
