// fl-settings, Users: the page's dialogs (users_dialogs.h)
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <set>
#include <memory>
#include <thread>
#include <unistd.h>

#include "fld/avatar.h"
#include "fld/file_dialog.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"
#ifdef FLD_HAVE_FPRINT
#include <FL/Fl_Progress.H>

#include <memory>

#include "fprint.h"
#endif
#include "users_model.h"
#include "users_dialogs.h"

#ifndef FLD_LIBEXECDIR
#define FLD_LIBEXECDIR "/usr/libexec/fldesktop"
#endif


namespace fld {
namespace users_ui {

using namespace users;

std::string users_helper_path() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) {
    buf[n] = 0;
    std::string local = dirname(buf) + "/fl-users-helper";
    if (file_exists(local) && !starts_with(local, "/usr/")) return local;
  }
  return std::string(FLD_LIBEXECDIR) + "/fl-users-helper";
}


// ---- dialogs ----------------------------------------------------------------

// Runs a modal window until one of its buttons sets *done.
void run_modal(Fl_Double_Window *w, const bool *done) {
  w->set_modal();
  w->show();
  while (w->shown() && !*done) Fl::wait();
  w->hide();
}

class StrengthBox : public Fl_Box {
public:
  StrengthBox(int x, int y, int w, int h) : Fl_Box(x, y, w, h) {
    align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    labelsize(12);
  }
  void update(const std::string &pw, const std::string &again) {
    if (pw.empty()) label("");
    else if (!again.empty() && again != pw) {
      label("The passwords are not the same");
      labelcolor(palette().danger);
    } else {
      int s = password_strength(pw);
      label(strength_text(s));
      labelcolor(s == 0 ? palette().danger : palette().fg_dim);
    }
    redraw();
  }
};

// Asks for a new password twice; returns false when cancelled.
bool ask_password(const std::string &title, std::string &out) {
  bool done = false, ok = false;
  Fl_Double_Window w(420, 190);
  w.copy_label(title.c_str());
  Fl_Box *l1 = new Fl_Box(20, 20, 130, 28, _("New password:"));
  l1->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  Fl_Secret_Input *p1 = new Fl_Secret_Input(160, 20, 240, 28);
  Fl_Box *l2 = new Fl_Box(20, 56, 130, 28, _("Again:"));
  l2->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  Fl_Secret_Input *p2 = new Fl_Secret_Input(160, 56, 240, 28);
  StrengthBox *st = new StrengthBox(160, 88, 240, 22);
  Fl_Button *cancel = new Fl_Button(200, 140, 95, 30, _("Cancel"));
  Fl_Return_Button *okb = new Fl_Return_Button(305, 140, 95, 30, _("Change"));
  struct Ctx {
    Fl_Secret_Input *p1, *p2;
    StrengthBox *st;
    Fl_Button *ok;
    bool *done, *accepted;
  } c{p1, p2, st, okb, &done, &ok};
  auto update = [](Fl_Widget *, void *d) {
    Ctx *c = (Ctx *)d;
    std::string a = c->p1->value(), b = c->p2->value(), err;
    c->st->update(a, b);
    valid_password(a, err) && a == b ? c->ok->activate() : c->ok->deactivate();
  };
  p1->when(FL_WHEN_CHANGED);
  p2->when(FL_WHEN_CHANGED);
  p1->callback(update, &c);
  p2->callback(update, &c);
  okb->deactivate();
  okb->callback([](Fl_Widget *, void *d) { *((Ctx *)d)->accepted = *((Ctx *)d)->done = true; }, &c);
  cancel->callback([](Fl_Widget *, void *d) { *((Ctx *)d)->done = true; }, &c);
  w.end();
  run_modal(&w, &done);
  if (ok) out = p1->value();
  return ok;
}


struct NewUserDialog {
  Fl_Input *full, *name;
  Fl_Secret_Input *p1, *p2;
  StrengthBox *st;
  Fl_Box *msg;
  Fl_Button *add;
  const std::vector<Account> *existing;
  bool done = false, ok = false, name_edited = false;
  std::string msg_text;

  void check() {
    std::string err;
    std::string n = name->value(), a = p1->value(), b = p2->value();
    bool good = valid_full_name(full->value(), err) && valid_user_name(n, err);
    if (good && find_account(*existing, n)) {
      err = "There is already a user called " + n + ".";
      good = false;
    }
    if (good && !valid_password(a, err)) good = false;
    if (good && a != b) good = false;
    // complain about the names once something was typed, about passwords below them
    bool names_bad = !good && (*name->value() || *full->value()) && err != "Enter a password." && !(a != b && err.empty());
    msg_text = names_bad ? err : "";
    msg->label(msg_text.c_str());
    msg->redraw();
    st->update(a, b);
    good ? add->activate() : add->deactivate();
  }
};

bool ask_new_user(const std::vector<Account> &existing, NewUser &out) {
  Fl_Double_Window w(460, 330, "Add User");
  auto label = [](int y, const char *t) {
    Fl_Box *b = new Fl_Box(20, y, 140, 28, t);
    b->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  };
  NewUserDialog c;
  c.existing = &existing;
  label(20, "Full name:");
  c.full = new Fl_Input(170, 20, 270, 28);
  label(56, "User name:");
  c.name = new Fl_Input(170, 56, 270, 28);
  c.name->tooltip(_("Lower case letters, digits, - and _. It cannot be changed later."));
  label(92, "Account type:");
  Fl_Choice *type = new Fl_Choice(170, 92, 180, 28);
  type->add(_("Standard"));
  type->add(_("Administrator"));
  type->value(0);
  label(128, "Password:");
  c.p1 = new Fl_Secret_Input(170, 128, 270, 28);
  label(164, "Again:");
  c.p2 = new Fl_Secret_Input(170, 164, 270, 28);
  c.st = new StrengthBox(170, 196, 270, 22);
  c.msg = new Fl_Box(20, 222, 420, 40);
  c.msg->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  c.msg->labelsize(12);
  c.msg->labelcolor(palette().danger);
  Fl_Button *cancel = new Fl_Button(240, 280, 95, 30, _("Cancel"));
  c.add = new Fl_Return_Button(345, 280, 95, 30, _("Add"));
  c.full->when(FL_WHEN_CHANGED);
  c.full->callback([](Fl_Widget *, void *d) {
    NewUserDialog *c = (NewUserDialog *)d;
    if (!c->name_edited) c->name->value(suggest_user_name(c->full->value()).c_str());
    c->check();
  }, &c);
  c.name->when(FL_WHEN_CHANGED);
  c.name->callback([](Fl_Widget *, void *d) {
    NewUserDialog *c = (NewUserDialog *)d;
    c->name_edited = true;
    c->check();
  }, &c);
  auto pcb = [](Fl_Widget *, void *d) { ((NewUserDialog *)d)->check(); };
  c.p1->when(FL_WHEN_CHANGED);
  c.p2->when(FL_WHEN_CHANGED);
  c.p1->callback(pcb, &c);
  c.p2->callback(pcb, &c);
  c.add->deactivate();
  c.add->callback([](Fl_Widget *, void *d) { ((NewUserDialog *)d)->ok = ((NewUserDialog *)d)->done = true; }, &c);
  cancel->callback([](Fl_Widget *, void *d) { ((NewUserDialog *)d)->done = true; }, &c);
  w.end();
  run_modal(&w, &c.done);
  if (!c.ok) return false;
  out.full = trim(c.full->value());
  out.name = c.name->value();
  out.password = c.p1->value();
  out.admin = type->value() == 1;
  return true;
}

// Confirms removing a user; *remove_home is whether to delete their files.
bool ask_remove(const Account &a, bool &remove_home) {
  bool done = false, ok = false;
  Fl_Double_Window w(460, 200, "Remove User");
  std::string who = a.real_name.empty() ? a.name : a.real_name + " (" + a.name + ")";
  std::string text = "Remove " + who + "?\n\nThey will no longer be able to log in.";
  Fl_Box *b = new Fl_Box(20, 16, 420, 70);
  b->copy_label(escape_at(text).c_str());
  b->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  Fl_Check_Button *home = new fld::CheckButton(20, 96, 420, 26, nullptr);
  std::string hl = "Also delete the home folder " + a.home + " and all the files in it";
  home->copy_label(escape_at(hl).c_str());
  home->value(0);
  Fl_Button *cancel = new Fl_Button(240, 150, 95, 30, _("Cancel"));
  Fl_Button *rm = new Fl_Button(345, 150, 95, 30, _("Remove"));
  rm->color(palette().danger);
  rm->labelcolor(FL_WHITE);
  struct Ctx {
    bool *done, *ok;
  } c{&done, &ok};
  rm->callback([](Fl_Widget *, void *d) { *((Ctx *)d)->ok = *((Ctx *)d)->done = true; }, &c);
  cancel->callback([](Fl_Widget *, void *d) { *((Ctx *)d)->done = true; }, &c);
  w.end();
  run_modal(&w, &done);
  remove_home = home->value() != 0;
  return ok;
}

// ---- the page ---------------------------------------------------------------


std::string find_picture(const Account &a) {
  for (std::string p : {picture_path(a.name), a.home + "/.face", a.home + "/.face.icon"})
    if (file_exists(p) && !is_dir(p) && access(p.c_str(), R_OK) == 0) return p;
  return "";
}

// The stock pictures: fldesktop's (/usr/share/pixmaps/faces/fldesktop), then
// any others in /usr/share/pixmaps/faces (other desktops put theirs there).
std::vector<std::string> stock_pictures() {
  std::vector<std::string> dirs, out;
  for (auto &d : data_dirs()) dirs.push_back(path_join(d, "pixmaps/faces/fldesktop"));
  for (auto &d : data_dirs()) dirs.push_back(path_join(d, "pixmaps/faces"));
  std::set<std::string> seen_dirs, seen;
  for (auto &d : dirs) {
    if (!is_dir(d) || !seen_dirs.insert(abs_path(d)).second) continue;
    std::vector<std::string> names = list_dir(d);
    std::sort(names.begin(), names.end());
    for (auto &n : names) {
      std::string p = path_join(d, n), e = extension(n);
      if (is_dir(p) || (e != "png" && e != "jpg" && e != "jpeg" && e != "svg")) continue;
      if (seen.insert(d.substr(d.rfind('/') + 1) + "/" + n).second) out.push_back(p);
    }
  }
  return out;
}

std::string browse_picture() {
  FileDialogOptions o;
  o.title = "User Picture";
  o.folder = user_dir("PICTURES");
  o.filters.push_back({"Images", {"*.png", "*.jpg", "*.jpeg", "*.svg"}, {}});
  FileDialogResult r = run_file_dialog(o);
  return r.ok && !r.paths.empty() ? r.paths[0] : "";
}

// A grid of the stock pictures and "Browse…" for a photo of your own.
// Returns the chosen file, "" when cancelled.
std::string pick_picture() {
  std::vector<std::string> faces = stock_pictures();
  if (faces.empty()) return browse_picture();
  struct Ctx {
    bool done = false, browse = false;
    int chosen = -1;
  } c;
  struct Slot {
    Ctx *c;
    int i;
  };
  const int cols = 5, cell = 84, pic = 72, pad = 12;
  int rows = ((int)faces.size() + cols - 1) / cols, shown = std::min(rows, 4);
  int sb = rows > shown ? Fl::scrollbar_size() : 0;
  int W = 2 * pad + cols * cell + sb, H = pad + 28 + shown * cell + 12 + 28 + pad;
  Fl_Double_Window w(W, H, "User Picture");
  Fl_Box head(pad, pad, W - 2 * pad, 24, "Choose a picture, or a photo of your own:");
  head.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  Fl_Scroll sc(pad, pad + 28, cols * cell + sb, shown * cell);
  sc.type(Fl_Scroll::VERTICAL);
  sc.box(FL_NO_BOX);
  std::vector<std::unique_ptr<Fl_Image>> imgs;
  std::vector<Slot> slots(faces.size());
  for (size_t i = 0; i < faces.size(); i++) {
    int bx = sc.x() + (int)(i % cols) * cell, by = sc.y() + (int)(i / cols) * cell;
    Fl_Button *b = new Fl_Button(bx, by, cell - 4, cell - 4);
    b->box(FL_FLAT_BOX);
    b->down_box(FL_FLAT_BOX);
    b->color(palette().bg);
    b->selection_color(palette().accent);
    std::string name = basename(faces[i]);
    b->copy_tooltip(name.substr(0, name.rfind('.')).c_str());
    std::unique_ptr<Fl_Image> full(load_image(faces[i]));
    if (full) {
      imgs.emplace_back(avatar_image(full.get(), pic));
      b->image(imgs.back().get());
    }
    slots[i] = {&c, (int)i};
    b->callback([](Fl_Widget *, void *d) {
      Slot *s = (Slot *)d;
      s->c->chosen = s->i;
      s->c->done = true;
    }, &slots[i]);
  }
  sc.end();
  Fl_Button browse(pad, H - pad - 28, 120, 28, "Browse…");
  browse.callback([](Fl_Widget *, void *d) {
    ((Ctx *)d)->browse = true;
    ((Ctx *)d)->done = true;
  }, &c);
  Fl_Button cancel(W - pad - 100, H - pad - 28, 100, 28, "Cancel");
  cancel.callback([](Fl_Widget *, void *d) { ((Ctx *)d)->done = true; }, &c);
  w.end();
  run_modal(&w, &c.done);
  if (c.browse) return browse_picture();
  return c.chosen >= 0 ? faces[c.chosen] : "";
}

#ifdef FLD_HAVE_FPRINT
// Fingerprints of a user (fprintd): the enrolled fingers, enrolling one more
// with the reader's stages, deleting them all.
void fingerprint_dialog(const std::string &user) {
  Fl_Double_Window d(440, 380, "Fingerprints");
  d.set_modal();
  Fl_Box head(16, 10, 408, 26, "Fingers that unlock the screen and log in:");
  head.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  Fl_Hold_Browser list(16, 40, 408, 120);
  Fl_Choice finger(110, 172, 200, 28, "New finger:");
  for (auto &f : fprint::all_fingers()) finger.add(fprint::finger_label(f).c_str());
  finger.value(0);
  Fl_Button enroll(318, 172, 106, 28, "Enroll…");
  Fl_Progress progress(16, 212, 408, 20);
  progress.minimum(0);
  progress.maximum(1);
  progress.selection_color(palette().accent);
  Fl_Box status(16, 238, 408, 44, "");
  status.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  Fl_Box note(16, 284, 408, 40, "Logging in with a finger needs pam_fprintd in the PAM setup of greetd and polkit.");
  note.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  note.labelsize(11);
  note.labelcolor(palette().fg_dim);
  Fl_Button del(16, 336, 120, 30, "Delete All…");
  Fl_Button close(324, 336, 100, 30, "Close");
  struct Ctx {
    std::string user;
    Fl_Hold_Browser *list;
    Fl_Choice *finger;
    Fl_Progress *progress;
    Fl_Box *status;
    std::string status_text;
    std::unique_ptr<fprint::Enroll> enroll;
    void fill() {
      list->clear();
      for (auto &f : fprint::enrolled(user)) list->add(fprint::finger_label(f).c_str());
      if (list->size() == 0) list->add(_("@iNone yet"));
    }
    void say(const std::string &t) {
      status_text = t;
      status->label(status_text.c_str());
      status->redraw();
    }
  } ctx{user, &list, &finger, &progress, &status, "", nullptr};
  ctx.fill();
  enroll.callback([](Fl_Widget *, void *v) {
    Ctx *c = (Ctx *)v;
    if (c->enroll) return;
    std::string f = fprint::all_fingers()[(size_t)std::max(0, c->finger->value())];
    c->progress->value(0);
    c->say("Starting…");
    c->enroll.reset(new fprint::Enroll(c->user, f, [c](int stage, int stages, const std::string &t, bool done, bool ok) {
      c->progress->value(stages > 0 ? (float)stage / stages : 0);
      c->say(t);
      if (done) {
        // the enroller ends itself; drop it after this callback
        Fl::add_timeout(0, [](void *v) { ((Ctx *)v)->enroll.reset(); }, c);
        if (ok) c->fill();
      }
    }));
  }, &ctx);
  del.callback([](Fl_Widget *, void *v) {
    Ctx *c = (Ctx *)v;
    if (fl_choice(_("Delete all fingerprints of %s?"), _("Cancel"), _("Delete"), nullptr, c->user.c_str()) != 1) return;
    std::string err;
    if (!fprint::delete_all(c->user, err)) fl_alert(_("The fingerprints were not deleted: %s"), err.c_str());
    c->fill();
  }, &ctx);
  close.callback([](Fl_Widget *w, void *) { w->window()->hide(); });
  d.end();
  d.show();
  while (d.shown()) Fl::wait();
  ctx.enroll.reset();
}
#endif

} // namespace users_ui
} // namespace fld
