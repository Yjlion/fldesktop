// fl-files: the dialogs of network locations (GVfs)
#include "gvfs_ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Radio_Round_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_draw.H>

#include <algorithm>

#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/widgets.h"

namespace {

// runs a modal window until it closes; result is set by its buttons
int run_modal(Fl_Double_Window *w, int &result) {
  w->end();
  w->set_modal();
  w->show();
  while (w->shown()) Fl::wait();
  int r = result;
  Fl::delete_widget(w);
  return r;
}

Fl_Box *message_box(int x, int y, int w, int h, const std::string &text) {
  Fl_Box *b = new Fl_Box(x, y, w, h);
  b->copy_label(fld::escape_at(text).c_str());
  b->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  return b;
}

} // namespace

gv::PasswordReply gvfs_password_dialog(const gv::PasswordRequest &q) {
  const int W = 440, LX = 130, FW = W - LX - 20;
  int rows = (q.need_user ? 1 : 0) + (q.need_domain ? 1 : 0) + (q.need_password ? 1 : 0);
  int H = 92 + (q.anonymous_ok ? 56 : 0) + rows * 34 + (q.can_save ? 40 : 0) + 50;
  Fl_Double_Window *w = new Fl_Double_Window(W, H, "Connect");
  Fl_Box *ic = new Fl_Box(16, 16, 48, 48);
  ic->image(fld::icon_first({"dialog-password", "network-server"}, 48));
  message_box(76, 16, W - 92, 64, q.message.empty() ? "Enter your password" : q.message);
  int y = 92;
  Fl_Radio_Round_Button *anon = nullptr, *named = nullptr;
  if (q.anonymous_ok) {
    anon = new Fl_Radio_Round_Button(20, y, W - 40, 24, "Connect anonymously");
    named = new Fl_Radio_Round_Button(20, y + 26, W - 40, 24, "Connect as this user:");
    named->value(1);
    y += 56;
  }
  Fl_Input *user = nullptr, *domain = nullptr;
  Fl_Secret_Input *pw = nullptr;
  if (q.need_user) {
    user = new Fl_Input(LX, y, FW, 28, "User name:");
    user->value(q.user.c_str());
    y += 34;
  }
  if (q.need_domain) {
    domain = new Fl_Input(LX, y, FW, 28, "Domain:");
    domain->value(q.domain.c_str());
    y += 34;
  }
  if (q.need_password) {
    pw = new Fl_Secret_Input(LX, y, FW, 28, "Password:");
    y += 34;
  }
  Fl_Choice *remember = nullptr;
  if (q.can_save) {
    y += 4;
    remember = new Fl_Choice(LX, y, FW, 28, "Remember:");
    remember->add("Forget it right away");
    remember->add("Until I log out");
    remember->add("Always");
    remember->value(1);
    y += 36;
  }
  static int result;
  result = 0;
  Fl_Button *cancel = new Fl_Button(W - 232, H - 44, 100, 30, "Cancel");
  cancel->callback([](Fl_Widget *b, void *) { result = 0; b->window()->hide(); });
  Fl_Return_Button *ok = new Fl_Return_Button(W - 120, H - 44, 100, 30, "Connect");
  ok->callback([](Fl_Widget *b, void *) { result = 1; b->window()->hide(); });
  if (anon) {
    // the fields are for a named login only
    auto toggle = [](Fl_Widget *b, void *d) {
      Fl_Group *g = b->window();
      bool on = ((Fl_Radio_Round_Button *)d)->value();
      for (int i = 0; i < g->children(); i++) {
        Fl_Widget *c = g->child(i);
        if (dynamic_cast<Fl_Input_ *>(c) || dynamic_cast<Fl_Choice *>(c)) on ? c->activate() : c->deactivate();
      }
    };
    anon->callback(toggle, named);
    named->callback(toggle, named);
  }
  if (user && q.user.empty()) user->take_focus();
  else if (pw) pw->take_focus();
  w->callback([](Fl_Widget *win, void *) { result = 0; win->hide(); });
  gv::PasswordReply r;
  // read the fields before the window goes
  w->end();
  w->set_modal();
  w->show();
  while (w->shown()) Fl::wait();
  r.ok = result == 1;
  if (r.ok) {
    r.anonymous = anon && anon->value();
    if (user) r.user = user->value();
    else r.user = q.user;
    if (domain) r.domain = domain->value();
    else r.domain = q.domain;
    if (pw) r.password = pw->value();
    if (remember)
      r.remember = remember->value() == 2   ? gv::PasswordReply::FOREVER
                   : remember->value() == 1 ? gv::PasswordReply::SESSION
                                            : gv::PasswordReply::NEVER;
  }
  if (pw) pw->value(""); // not left in freed memory longer than needed
  Fl::delete_widget(w);
  return r;
}

int gvfs_question_dialog(const std::string &message, const std::vector<std::string> &choices) {
  const int W = 460;
  // gvfs messages: a first line as the title, details after it
  Fl_Double_Window *w = new Fl_Double_Window(W, 170, "Files");
  Fl_Box *ic = new Fl_Box(16, 16, 48, 48);
  ic->image(fld::icon_first({"dialog-question", "dialog-information"}, 48));
  message_box(76, 16, W - 92, 100, message);
  static int result;
  result = -1;
  int x = W - 16;
  // the first choice is the default: on the right
  for (int i = (int)choices.size() - 1; i >= 0; i--) {
    fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
    int bw = std::max(90, (int)fl_width(choices[(size_t)i].c_str()) + 30);
    x -= bw;
    Fl_Button *b = i == 0 ? new Fl_Return_Button(x, 126, bw, 30) : new Fl_Button(x, 126, bw, 30);
    b->copy_label(fld::escape_at(choices[(size_t)i]).c_str());
    b->callback([](Fl_Widget *b, void *d) { result = (int)(long)d; b->window()->hide(); }, (void *)(long)i);
    x -= 8;
  }
  w->callback([](Fl_Widget *win, void *) { result = -1; win->hide(); });
  return run_modal(w, result);
}

std::string connect_dialog(std::vector<std::string> &recent, bool &bookmark) {
  const int W = 480, H = 380;
  Fl_Double_Window *w = new Fl_Double_Window(W, H, "Connect to Server");
  Fl_Box *t = new Fl_Box(16, 12, W - 32, 22, "Server address:");
  t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  Fl_Input *addr = new Fl_Input(16, 36, W - 32, 30);
  Fl_Box *hint = new Fl_Box(16, 68, W - 32, 40,
                            "For example smb://server/share, sftp://user@@host/, ftp://host, "
                            "davs://host/path, nfs://host/export or afp://host");
  hint->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  hint->labelsize(12);
  hint->labelcolor(fld::palette().fg_dim);
  Fl_Box *rl = new Fl_Box(16, 112, W - 32, 22, "Recent servers:");
  rl->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  Fl_Hold_Browser *list = new Fl_Hold_Browser(16, 136, W - 32, 150);
  for (auto &r : recent) {
    list->add(fld::escape_at(r).c_str());
    list->icon(list->size(), fld::icon_first({"network-server", "folder-remote"}, 16));
  }
  static Fl_Input *s_addr;
  static std::vector<std::string> *s_recent;
  static int result;
  s_addr = addr;
  s_recent = &recent;
  result = 0;
  list->callback([](Fl_Widget *b, void *) {
    int line = ((Fl_Hold_Browser *)b)->value();
    if (line <= 0 || line > (int)s_recent->size()) return;
    s_addr->value((*s_recent)[(size_t)line - 1].c_str());
    if (Fl::event_clicks()) { result = 1; b->window()->hide(); }
  });
  Fl_Button *clear = new Fl_Button(16, 292, 120, 28, "Clear Recent");
  clear->callback([](Fl_Widget *b, void *d) {
    s_recent->clear();
    ((Fl_Hold_Browser *)d)->clear();
    b->deactivate();
  }, list);
  if (recent.empty()) clear->deactivate();
  Fl_Check_Button *bm = new fld::CheckButton(150, 292, W - 166, 28, "Add a bookmark");
  bm->value(bookmark);
  Fl_Button *cancel = new Fl_Button(W - 232, H - 44, 100, 30, "Cancel");
  cancel->callback([](Fl_Widget *b, void *) { result = 0; b->window()->hide(); });
  Fl_Return_Button *ok = new Fl_Return_Button(W - 120, H - 44, 100, 30, "Connect");
  ok->callback([](Fl_Widget *b, void *) { result = 1; b->window()->hide(); });
  w->callback([](Fl_Widget *win, void *) { result = 0; win->hide(); });
  addr->take_focus();
  w->end();
  w->set_modal();
  w->show();
  while (w->shown()) Fl::wait();
  std::string uri = result == 1 ? addr->value() : "";
  bookmark = bm->value();
  Fl::delete_widget(w);
  size_t a = uri.find_first_not_of(" \t"), b = uri.find_last_not_of(" \t");
  return a == std::string::npos ? "" : uri.substr(a, b - a + 1);
}
