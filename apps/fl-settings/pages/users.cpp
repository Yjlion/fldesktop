// Users: add, remove and change the accounts of the people who use the
// computer (full name, account type, password, picture, groups, automatic
// login). Changes are made as root by pkexec fl-users-helper.
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

#ifndef FLD_LIBEXECDIR
#define FLD_LIBEXECDIR "/usr/libexec/fldesktop"
#endif

namespace fld {
namespace {

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

// One helper call; input goes to its stdin (passwords).
struct Job {
  std::vector<std::string> args;
  std::string input;
};

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

struct NewUser {
  std::string name, full, password;
  bool admin = false;
};

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

class UsersPage : public Fl_Group {
public:
  std::vector<Group> groups;
  std::vector<Account> accounts;
  std::vector<std::string> editable;
  std::string admin, autologin, selected;
  bool greetd = false, busy = false;
  Fl_Hold_Browser *list;
  Fl_Button *add_b, *remove_b, *pic_b, *pic_rm, *pw_b, *apply, *revert;
  Fl_Button *fp_b = nullptr;
  Picture *pic;
  Fl_Input *full;
  Fl_Box *uname, *status;
  Fl_Choice *type;
  Fl_Check_Button *auto_b;
  Fl_Check_Browser *grp;

  UsersPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Users"), cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    auto changed = [](Fl_Widget *, void *d) { ((UsersPage *)d)->set_changed(); };

    f.row("", 150, wx, wy, ww);
    list = new Fl_Hold_Browser(wx - 200, wy, ww + 200 - 130, 150);
    list->callback([](Fl_Widget *, void *d) { ((UsersPage *)d)->pick(); }, this);
    add_b = new Fl_Button(wx + ww - 120, wy, 120, 28, _("Add User…"));
    add_b->callback([](Fl_Widget *, void *d) { ((UsersPage *)d)->add_user(); }, this);
    remove_b = new Fl_Button(wx + ww - 120, wy + 36, 120, 28, _("Remove…"));
    remove_b->callback([](Fl_Widget *, void *d) { ((UsersPage *)d)->remove_user(); }, this);

    f.heading(_("Account"));
    f.row(_("Picture:"), 72, wx, wy, ww);
    pic = new Picture(wx, wy, 72, 72);
    pic_b = new Fl_Button(wx + 84, wy + 4, 120, 28, _("Choose…"));
    pic_b->callback([](Fl_Widget *, void *d) { ((UsersPage *)d)->choose_picture(); }, this);
    pic_rm = new Fl_Button(wx + 84, wy + 40, 120, 28, _("No Picture"));
    pic_rm->callback([](Fl_Widget *, void *d) { ((UsersPage *)d)->remove_picture(); }, this);
    f.row(_("User name:"), 28, wx, wy, ww);
    uname = new Fl_Box(wx, wy, ww, 28);
    uname->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    f.row(_("Full name:"), 28, wx, wy, ww);
    full = new Fl_Input(wx, wy, ww, 28);
    full->when(FL_WHEN_CHANGED);
    full->callback(changed, this);
    f.row(_("Account type:"), 28, wx, wy, ww);
    type = new Fl_Choice(wx, wy, 180, 28);
    type->add(_("Standard"));
    type->add(_("Administrator"));
    type->callback(changed, this);
    f.note(_("Administrators can install software, change system settings and manage users."), 2);
    f.row(_("Password:"), 28, wx, wy, ww);
    pw_b = new Fl_Button(wx, wy, 180, 28, _("Change Password…"));
    pw_b->callback([](Fl_Widget *, void *d) { ((UsersPage *)d)->change_password(); }, this);
#ifdef FLD_HAVE_FPRINT
    // fingerprints: when there is a reader
    if (fprint::available()) {
      fp_b = new Fl_Button(wx + 190, wy, 150, 28, _("Fingerprints…"));
      fp_b->callback([](Fl_Widget *, void *d) {
        UsersPage *p = (UsersPage *)d;
        if (const Account *a = p->current()) fingerprint_dialog(a->name);
      }, this);
    }
#endif
    f.row("", 26, wx, wy, ww);
    auto_b = new fld::CheckButton(wx, wy, ww, 26, _("Log in automatically when the computer starts"));
    auto_b->callback(changed, this);
    // the list is as tall as its groups: a list that scrolls inside the
    // page, which scrolls itself, is hard to use with the wheel
    int grp_h = 120;
    {
      std::string pw, gt;
      read_file("/etc/passwd", pw);
      read_file("/etc/group", gt);
      std::vector<Group> gs = parse_group(gt);
      std::vector<Account> as = parse_passwd(pw);
      std::string adm = admin_group(gs);
      attach_groups(as, gs, adm);
      grp_h = std::max(48, std::min(480, (int)editable_groups(gs, as, adm).size() * 20 + 6));
    }
    f.row(_("Groups:"), grp_h, wx, wy, ww);
    grp = new Fl_Check_Browser(wx, wy, ww, grp_h);
    grp->when(FL_WHEN_CHANGED);
    grp->callback(changed, this);
    f.note(_("Groups give access to devices and services, e.g. printers (lp), serial ports (uucp, dialout) or docker."), 2);
    f.space(4);
    f.row("", 30, wx, wy, ww);
    apply = new Fl_Button(wx, wy, 120, 30, _("Apply…"));
    apply->tooltip(_("Changing a user account needs an administrator password"));
    apply->callback([](Fl_Widget *, void *d) { ((UsersPage *)d)->save(); }, this);
    revert = new Fl_Button(wx + 130, wy, 120, 30, _("Revert"));
    revert->callback([](Fl_Widget *, void *d) { ((UsersPage *)d)->show_selected(); }, this);
    f.row("", 40, wx, wy, ww);
    status = new Fl_Box(wx - 200, wy, ww + 200, 40);
    status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_TOP);
    status->labelsize(13);
    g->end();
    end();
    reload();
  }

  const Account *current() const { return find_account(accounts, selected); }

  void reload() {
    std::string p, gtext, toml;
    read_file("/etc/passwd", p);
    read_file("/etc/group", gtext);
    greetd = read_file("/etc/greetd/config.toml", toml);
    groups = parse_group(gtext);
    accounts = parse_passwd(p);
    admin = admin_group(groups);
    attach_groups(accounts, groups, admin);
    editable = editable_groups(groups, accounts, admin);
    autologin = greetd ? autologin_user(toml) : "";
    list->clear();
    int sel = 0;
    uid_t me = getuid();
    for (size_t i = 0; i < accounts.size(); i++) {
      const Account &a = accounts[i];
      std::string line = a.real_name.empty() ? a.name : a.real_name + "  (" + a.name + ")";
      if (a.admin) line += "  —  Administrator";
      if (a.uid == me) line += "  —  you";
      list->add(escape_at(line).c_str());
      if (a.name == selected || (selected.empty() && a.uid == me)) sel = (int)i + 1;
    }
    if (!accounts.empty()) {
      list->value(sel ? sel : 1);
      selected = accounts[list->value() - 1].name;
    } else selected.clear();
    show_selected();
  }

  void pick() {
    int i = list->value();
    if (i < 1 || i > (int)accounts.size()) return;
    selected = accounts[i - 1].name;
    show_selected();
  }

  void show_selected() {
    const Account *a = current();
    Fl_Widget *ws[] = {pic_b, pic_rm, full, type, pw_b, auto_b, grp, remove_b};
    for (auto w : ws) a && !busy ? w->activate() : w->deactivate();
    busy ? add_b->deactivate() : add_b->activate();
    grp->clear();
    if (!a) {
      uname->label("");
      full->value("");
      pic->set("", "", "");
      apply->deactivate();
      revert->deactivate();
      return;
    }
    uname->copy_label(escape_at(a->name).c_str());
    full->value(a->real_name.c_str());
    type->value(a->admin ? 1 : 0);
    if (admin.empty()) type->deactivate();
    auto_b->value(autologin == a->name);
    if (!greetd) {
      auto_b->deactivate();
      auto_b->tooltip(_("Automatic login needs greetd (/etc/greetd/config.toml)"));
    }
    for (auto &gname : editable) grp->add(gname.c_str(), a->in(gname) ? 1 : 0);
    pic->set(find_picture(*a), a->name, a->real_name);
    if (a->uid == getuid()) remove_b->deactivate();
    // others' fingers need an administrator (fprintd asks polkit)
    if (fp_b) a->uid == getuid() || !admin.empty() ? fp_b->activate() : fp_b->deactivate();
    apply->deactivate();
    revert->deactivate();
    redraw();
  }

  void set_changed() {
    if (busy) return;
    apply->activate();
    revert->activate();
  }

  void set_status(const std::string &s) {
    status->copy_label(escape_at(s).c_str());
    status->redraw();
  }

  // Runs the jobs one after another through pkexec (polkit keeps the
  // authorization for a few minutes, so it asks once), then reloads.
  void run(std::vector<Job> jobs, const std::string &doing, const std::string &done_text) {
    if (jobs.empty()) return;
    if (which("pkexec").empty()) {
      fl_alert("%s", _("pkexec (polkit) is needed to change user accounts."));
      return;
    }
    busy = true;
    show_selected();
    set_status(doing + " (polkit asks for an administrator password)");
    std::string helper = users_helper_path();
    for (auto &j : jobs) {
      j.args.insert(j.args.begin(), helper);
      j.args.insert(j.args.begin(), "pkexec");
    }
    struct Res {
      UsersPage *page;
      int rc = 0;
      std::string out, done_text;
    };
    std::thread([this, jobs, done_text]() {
      Res *r = new Res{this, 0, "", done_text};
      for (auto &j : jobs) {
        r->rc = run_capture_input(j.args, j.input, r->out);
        if (r->rc != 0) break;
      }
      Fl::awake([](void *p) {
        std::unique_ptr<Res> r((Res *)p);
        r->page->finished(r->rc, r->out, r->done_text);
      }, r);
    }).detach();
  }

  void finished(int rc, const std::string &out, const std::string &done_text) {
    busy = false;
    reload();
    if (rc == 0) set_status(done_text);
    else if (rc == 126 || rc == 127) set_status("Not changed: the authentication was cancelled or failed.");
    else set_status("Not changed: " + trim(out));
  }

  void save() {
    const Account *a = current();
    if (!a) return;
    std::string err, fn = trim(full->value());
    if (!valid_full_name(fn, err)) {
      fl_alert("%s", err.c_str());
      return;
    }
    bool want_admin = type->value() == 1;
    if (a->admin && !want_admin && admin_count(accounts) <= 1) {
      fl_alert(_("%s is the only administrator. Make someone else an administrator first."), a->name.c_str());
      return;
    }
    std::vector<std::string> want;
    bool groups_changed = false;
    for (int i = 1; i <= grp->nitems(); i++) {
      bool on = grp->checked(i);
      if (on) want.push_back(editable[i - 1]);
      if (on != a->in(editable[i - 1])) groups_changed = true;
    }
    std::vector<Job> jobs;
    if (fn != a->real_name || want_admin != a->admin || groups_changed)
      jobs.push_back({{"modify", a->name, fn, want_admin ? "1" : "0", groups_changed ? join(want, ",") : "-"}, ""});
    bool want_auto = auto_b->value() != 0;
    if (greetd && want_auto != (autologin == a->name)) {
      if (want_auto && fl_choice(_("Log %s in automatically, without a password, when the computer starts?\n\n"
                                 "Anyone who switches the computer on can then use the account."),
                                 _("Cancel"), _("Log In Automatically"), nullptr, a->name.c_str()) != 1)
        return;
      jobs.push_back({{"autologin", want_auto ? a->name : "-"}, ""});
    }
    if (jobs.empty()) {
      show_selected();
      return;
    }
    run(jobs, "Saving…", "Saved.");
  }

  void add_user() {
    NewUser n;
    if (!ask_new_user(accounts, n)) return;
    selected = n.name;
    run({{{"add", n.name, n.full, n.admin ? "1" : "0"}, n.password + "\n"}}, "Adding " + n.name + "…",
        "Added " + n.name + ".");
  }

  void remove_user() {
    const Account *a = current();
    if (!a) return;
    if (a->uid == getuid()) return;
    if (a->admin && admin_count(accounts) <= 1) {
      fl_alert(_("%s is the only administrator and cannot be removed."), a->name.c_str());
      return;
    }
    bool remove_home = false;
    if (!ask_remove(*a, remove_home)) return;
    std::string name = a->name;
    selected.clear();
    run({{{"delete", name, remove_home ? "remove-home" : "keep"}, ""}}, "Removing " + name + "…",
        "Removed " + name + ".");
  }

  void change_password() {
    const Account *a = current();
    if (!a) return;
    std::string pw;
    if (!ask_password("Password of " + a->name, pw)) return;
    run({{{"password", a->name}, pw + "\n"}}, "Changing the password…", "The password of " + a->name + " was changed.");
  }

  void choose_picture() {
    const Account *cur = current();
    if (!cur) return;
    // a copy: the account list may be read again while the dialogs are open
    const Account acc = *cur;
    const Account *a = &acc;
    std::string path = pick_picture();
    if (path.empty()) return;
    // the helper gets a 256 px square PNG: small, and the same in every place
    // it is shown (the greeter and the lock screen clip it to a circle)
    std::unique_ptr<Fl_Image> img(load_image(path));
    std::string data;
    if (!img || !avatar_png(img.get(), 256, data)) {
      fl_alert(_("Cannot read %s"), path.c_str());
      return;
    }
    std::string tmp = getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-settings-picture";
    unlink(tmp.c_str());
    if (!write_file(tmp, data)) {
      fl_alert(_("Cannot write %s"), tmp.c_str());
      return;
    }
    // your own picture also goes to ~/.face (no root needed for that)
    if (a->uid == getuid()) write_file(a->home + "/.face", data);
    run({{{"picture", a->name, tmp}, ""}}, "Setting the picture…", "The picture was changed.");
  }

  void remove_picture() {
    const Account *a = current();
    if (!a) return;
    if (a->uid == getuid()) {
      unlink((a->home + "/.face").c_str());
      unlink((a->home + "/.face.icon").c_str());
    }
    run({{{"picture", a->name, "-"}, ""}}, "Removing the picture…", "The picture was removed.");
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new UsersPage(x, y, w, h); }

}  // namespace

PageInfo page_users() {
  return {"users", _("Users"), "system-users", "System",
          _("users accounts user password administrator admin add remove delete groups picture avatar face "
          "autologin automatic login"),
          create};
}

}  // namespace fld
