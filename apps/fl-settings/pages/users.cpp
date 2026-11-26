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
#include "users_dialogs.h"

#ifndef FLD_LIBEXECDIR
#define FLD_LIBEXECDIR "/usr/libexec/fldesktop"
#endif


namespace fld {
namespace {

using namespace users;
using namespace users_ui;

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
