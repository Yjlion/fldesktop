// fl-keyring: passwords and keys. Browses and edits the passwords in the
// Secret Service (GNOME Keyring, KeePassXC), and the SSH keys in ~/.ssh and
// the GnuPG keys of the user.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_ask.H>

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "fld/menus.h"
#include "fld/mime.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "keyring_util.h"
#include "secret.h"

namespace {

Fl_Double_Window *g_win;
Fl_Tabs *g_tabs;
Fl_Box *g_status;

void status(const std::string &s) {
  static std::string keep;
  keep = fld::escape_at(s);
  g_status->label(keep.c_str());
}

void copy_text(const std::string &t) {
  Fl::copy(t.c_str(), (int)t.size(), 1);
  Fl::copy(t.c_str(), (int)t.size(), 0);
}

void clear_clipboard(void *) {
  copy_text("");
  status("The copied password was removed from the clipboard.");
}

std::string date(long long t) { return t > 0 ? fld::format_time((time_t)t, "%Y-%m-%d") : ""; }

// A small form dialog: labelled inputs, OK and Cancel. Returns false on cancel.
bool form(const char *title, std::vector<std::pair<const char *, std::string *>> fields, const char *ok_label,
          std::vector<bool> secret = {}) {
  int h = 70 + 38 * (int)fields.size();
  Fl_Double_Window d(460, h, title);
  d.set_modal();
  std::vector<Fl_Input *> ins;
  int y = 16;
  for (size_t i = 0; i < fields.size(); i++) {
    Fl_Input *in = i < secret.size() && secret[i] ? new Fl_Secret_Input(150, y, 290, 28, fields[i].first)
                                                  : new Fl_Input(150, y, 290, 28, fields[i].first);
    in->value(fields[i].second->c_str());
    ins.push_back(in);
    y += 38;
  }
  Fl_Button cancel(220, h - 44, 106, 30, "Cancel");
  Fl_Return_Button ok(334, h - 44, 106, 30, ok_label);
  int result = 0;
  cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 2; }, &result);
  ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
  d.callback([](Fl_Widget *, void *r) { *(int *)r = 2; }, &result);
  d.end();
  d.show();
  while (!result && d.shown()) Fl::wait();
  if (result != 1) return false;
  for (size_t i = 0; i < fields.size(); i++) *fields[i].second = ins[i]->value();
  return true;
}

// ---- passwords ----------------------------------------------------------------------------

class PasswordsPage : public Fl_Group {
public:
  std::vector<secret::Collection> cols;
  std::vector<const secret::Item *> shown; // the list's rows
  Fl_Choice *keyring;
  Fl_Button *lock_btn;
  Fl_Input *search;
  Fl_Hold_Browser *list;
  Fl_Input *label;
  Fl_Multiline_Output *attrs;
  Fl_Secret_Input *pass;
  Fl_Input *pass_plain;
  Fl_Button *show_btn, *copy_btn, *save_btn, *del_btn, *add_btn, *new_btn;
  bool connected = false, revealed = false;
  std::string error;

  PasswordsPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H, "Passwords") {
    int lw = 330;
    keyring = new Fl_Choice(X + 80, Y + 10, lw - 80 - 90, 28, "Keyring:");
    keyring->callback([](Fl_Widget *, void *d) { ((PasswordsPage *)d)->fill(); }, this);
    lock_btn = new Fl_Button(X + lw - 86, Y + 10, 84, 28, "Unlock");
    lock_btn->callback([](Fl_Widget *, void *d) { ((PasswordsPage *)d)->toggle_lock(); }, this);
    search = new Fl_Input(X + 80, Y + 46, lw - 82, 28, "Search:");
    search->when(FL_WHEN_CHANGED);
    search->callback([](Fl_Widget *, void *d) { ((PasswordsPage *)d)->fill(); }, this);
    list = new Fl_Hold_Browser(X + 8, Y + 82, lw - 6, H - 82 - 46);
    list->format_char(0);
    list->textsize(13);
    list->callback([](Fl_Widget *, void *d) { ((PasswordsPage *)d)->select(); }, this);
    add_btn = new Fl_Button(X + 8, Y + H - 40, 130, 30, "Add Password…");
    add_btn->callback([](Fl_Widget *, void *d) { ((PasswordsPage *)d)->add(); }, this);
    new_btn = new Fl_Button(X + 144, Y + H - 40, 130, 30, "New Keyring…");
    new_btn->callback([](Fl_Widget *, void *d) { ((PasswordsPage *)d)->new_keyring(); }, this);

    int dx = X + lw + 110, dw = W - lw - 130;
    label = new Fl_Input(dx, Y + 46, dw, 28, "Name:");
    attrs = new Fl_Multiline_Output(dx, Y + 82, dw, 110, "Details:");
    attrs->textsize(12);
    attrs->wrap(1);
    pass = new Fl_Secret_Input(dx, Y + 200, dw - 180, 28, "Password:");
    pass_plain = new Fl_Input(dx, Y + 200, dw - 180, 28);
    pass_plain->hide();
    show_btn = new Fl_Button(dx + dw - 174, Y + 200, 84, 28, "Show");
    show_btn->callback([](Fl_Widget *, void *d) { ((PasswordsPage *)d)->reveal(); }, this);
    copy_btn = new Fl_Button(dx + dw - 86, Y + 200, 86, 28, "Copy");
    copy_btn->callback([](Fl_Widget *, void *d) { ((PasswordsPage *)d)->copy(); }, this);
    save_btn = new Fl_Button(dx, Y + 244, 120, 30, "Save Changes");
    save_btn->callback([](Fl_Widget *, void *d) { ((PasswordsPage *)d)->save(); }, this);
    del_btn = new Fl_Button(dx + 126, Y + 244, 100, 30, "Delete…");
    del_btn->callback([](Fl_Widget *, void *d) { ((PasswordsPage *)d)->remove(); }, this);
    Fl_Box *hint = new Fl_Box(dx, Y + 290, dw, 60,
                              "Passwords that programs keep for you (web sites, Wi-Fi, mail, SSH key "
                              "passphrases). A locked keyring asks for its password first.");
    hint->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
    hint->labelsize(12);
    hint->labelcolor(fld::palette().fg_dim);
    end();
    resizable(list);
  }

  void load() {
    connected = secret::connect(error);
    if (!connected) {
      list->clear();
      list->add("No password store is running.");
      status(error);
      for (Fl_Widget *w : {(Fl_Widget *)keyring, (Fl_Widget *)lock_btn, (Fl_Widget *)add_btn, (Fl_Widget *)new_btn})
        w->deactivate();
      select();
      return;
    }
    std::string cur = keyring->value() >= 0 && keyring->value() < (int)cols.size() ? cols[(size_t)keyring->value()].path : "";
    cols = secret::collections();
    keyring->clear();
    int pick = 0;
    for (size_t i = 0; i < cols.size(); i++) {
      std::string n = cols[i].label.empty() ? fld::basename(cols[i].path) : cols[i].label;
      if (cols[i].alias == "default") n += " (default)";
      keyring->add(fld::Menu::text(n).c_str());
      if (cols[i].path == cur || (cur.empty() && cols[i].alias == "default")) pick = (int)i;
    }
    if (!cols.empty()) keyring->value(pick);
    fill();
  }

  secret::Collection *current() {
    int i = keyring->value();
    return i >= 0 && i < (int)cols.size() ? &cols[(size_t)i] : nullptr;
  }

  void fill() {
    list->clear();
    shown.clear();
    secret::Collection *c = current();
    if (!c) {
      select();
      return;
    }
    lock_btn->label(c->locked ? "Unlock" : "Lock");
    std::string q = fld::lower(search->value());
    for (const secret::Item &it : c->items) {
      std::string text = it.label + " " + keyring::attributes_text(it.attributes);
      if (!q.empty() && fld::lower(text).find(q) == std::string::npos) continue;
      shown.push_back(&it);
      list->add(fld::escape_at(it.label.empty() ? std::string("(no name)") : it.label).c_str());
      if (c->locked)
        if (Fl_Image *lock = fld::symbolic_icon("changes-prevent", 16, fld::palette().fg_dim)) list->icon(list->size(), lock);
    }
    if (c->items.empty()) list->add(c->locked ? "Locked: unlock it to see the passwords." : "No passwords yet.");
    status(std::to_string(c->items.size()) + " passwords in " + (c->label.empty() ? "this keyring" : c->label));
    select();
  }

  const secret::Item *item() {
    int i = list->value() - 1;
    return i >= 0 && i < (int)shown.size() ? shown[(size_t)i] : nullptr;
  }

  void hide_secret() {
    revealed = false;
    pass->value("");
    pass_plain->value("");
    pass_plain->hide();
    pass->show();
    show_btn->label("Show");
  }

  void select() {
    const secret::Item *it = item();
    hide_secret();
    for (Fl_Widget *w : {(Fl_Widget *)label, (Fl_Widget *)attrs, (Fl_Widget *)pass, (Fl_Widget *)show_btn,
                         (Fl_Widget *)copy_btn, (Fl_Widget *)save_btn, (Fl_Widget *)del_btn})
      it ? w->activate() : w->deactivate();
    if (!it) {
      label->value("");
      attrs->value("");
      return;
    }
    label->value(it->label.c_str());
    std::string d;
    for (auto &a : it->attributes) d += a.first + ": " + a.second + "\n";
    if (it->created) d += "Created: " + date(it->created) + "\n";
    if (it->modified) d += "Changed: " + date(it->modified) + "\n";
    attrs->value(d.c_str());
  }

  // the secret, unlocking the keyring if needed
  bool fetch(std::string &value) {
    const secret::Item *it = item();
    secret::Collection *c = current();
    if (!it || !c) return false;
    std::string err;
    if (c->locked || it->locked) {
      if (!secret::unlock(it->path, err)) {
        status("Not unlocked: " + err);
        return false;
      }
      std::string path = it->path;
      load();
      // find it again after reloading
      for (size_t i = 0; i < shown.size(); i++)
        if (shown[i]->path == path) list->select((int)i + 1);
      select();
    }
    if (!secret::get_secret(item()->path, value, err)) {
      status("Cannot read the password: " + err);
      return false;
    }
    return true;
  }

  void reveal() {
    if (revealed) {
      hide_secret();
      return;
    }
    std::string v;
    if (!fetch(v)) return;
    revealed = true;
    pass_plain->value(v.c_str());
    pass->value(v.c_str());
    pass->hide();
    pass_plain->show();
    show_btn->label("Hide");
  }

  void copy() {
    std::string v;
    if (!fetch(v)) return;
    copy_text(v);
    status("Password copied; it is removed from the clipboard in 30 seconds.");
    // forget it again after a while
    Fl::remove_timeout(clear_clipboard);
    Fl::add_timeout(30, clear_clipboard);
  }

  void save() {
    const secret::Item *it = item();
    if (!it) return;
    std::string err, path = it->path;
    if (label->value() != it->label && !secret::set_label(path, label->value(), err)) {
      fl_alert("Cannot rename: %s", err.c_str());
      return;
    }
    if (revealed && !secret::set_secret(path, pass_plain->value(), err)) {
      fl_alert("Cannot change the password: %s", err.c_str());
      return;
    }
    status("Saved.");
    load();
  }

  void remove() {
    const secret::Item *it = item();
    if (!it) return;
    if (fl_choice("Delete the password \"%s\"? Programs that use it will ask for it again.", "Cancel", "Delete",
                  nullptr, it->label.c_str()) != 1)
      return;
    std::string err;
    if (!secret::delete_item(it->path, err)) fl_alert("Cannot delete: %s", err.c_str());
    load();
  }

  void add() {
    secret::Collection *c = current();
    if (!c) return;
    std::string name, user, site, pw;
    if (!form("Add Password", {{"Name:", &name}, {"User name:", &user}, {"Web site or server:", &site},
                               {"Password:", &pw}},
              "Add", {false, false, false, true}))
      return;
    if (name.empty()) name = site.empty() ? user : site;
    std::vector<std::pair<std::string, std::string>> a = {{"xdg:schema", "org.fldesktop.Password"}};
    if (!user.empty()) a.push_back({"username", user});
    if (!site.empty()) a.push_back({"server", site});
    std::string err;
    if (c->locked && !secret::unlock(c->path, err)) {
      status("Not unlocked: " + err);
      return;
    }
    if (!secret::create_item(c->path, name, a, pw, err)) fl_alert("Cannot add the password: %s", err.c_str());
    load();
  }

  void new_keyring() {
    std::string name;
    if (!form("New Keyring", {{"Name:", &name}}, "Create")) return;
    std::string err;
    if (!secret::create_collection(name, err)) fl_alert("Cannot create the keyring: %s", err.c_str());
    load();
  }

  void toggle_lock() {
    secret::Collection *c = current();
    if (!c) return;
    std::string err;
    bool ok = c->locked ? secret::unlock(c->path, err) : secret::lock(c->path, err);
    if (!ok) status(err);
    load();
  }
};

// ---- SSH keys -----------------------------------------------------------------------------

class SshPage : public Fl_Group {
public:
  std::vector<keyring::SshKey> keys;
  Fl_Hold_Browser *list;

  SshPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H, "SSH Keys") {
    list = new Fl_Hold_Browser(X + 8, Y + 10, W - 16, H - 56);
    list->textsize(13);
    static int cols[] = {170, 90, 360, 0};
    list->column_widths(cols);
    list->column_char('\t');
    int x = X + 8;
    auto btn = [&](const char *l, int w, Fl_Callback *cb) {
      Fl_Button *b = new Fl_Button(x, Y + H - 40, w, 30, l);
      b->callback(cb, this);
      x += w + 6;
      return b;
    };
    btn("Generate…", 110, [](Fl_Widget *, void *d) { ((SshPage *)d)->generate(); });
    btn("Copy Public Key", 130, [](Fl_Widget *, void *d) { ((SshPage *)d)->copy(); });
    btn("Change Passphrase…", 150, [](Fl_Widget *, void *d) { ((SshPage *)d)->passphrase(); });
    btn("Delete…", 90, [](Fl_Widget *, void *d) { ((SshPage *)d)->remove(); });
    btn("Open Folder", 110, [](Fl_Widget *, void *) { fld::open_path(fld::home_dir() + "/.ssh"); });
    end();
    resizable(list);
  }

  std::string dir() { return fld::home_dir() + "/.ssh"; }

  void load() {
    keys = keyring::find_ssh_keys(dir());
    list->clear();
    list->add("@bName\t@bType\t@bFingerprint\t@bComment");
    for (keyring::SshKey &k : keys) {
      std::string out;
      if (!fld::which("ssh-keygen").empty() && fld::run_capture({"ssh-keygen", "-l", "-f", k.pub_path}, out) == 0)
        keyring::parse_ssh_fingerprint(out, k);
      std::string name = fld::basename(k.path.empty() ? k.pub_path : k.path);
      std::string type = k.type + (k.bits && k.type == "RSA" ? " " + std::to_string(k.bits) : "");
      list->add((name + (k.path.empty() ? " (public only)" : "") + "\t" + type + "\t" + k.fingerprint + "\t" +
                 k.comment).c_str());
    }
    if (keys.empty()) list->add("No SSH keys yet. Generate one to log in to servers without a password.");
    if (fld::which("ssh-keygen").empty()) status("ssh-keygen is not installed (OpenSSH): keys cannot be made here.");
  }

  keyring::SshKey *key() {
    int i = list->value() - 2;
    return i >= 0 && i < (int)keys.size() ? &keys[(size_t)i] : nullptr;
  }

  void copy() {
    keyring::SshKey *k = key();
    if (!k) return;
    std::string text;
    if (!fld::read_file(k->pub_path, text, 64 * 1024)) return;
    copy_text(fld::trim(text));
    status("Public key copied: paste it into the server's ~/.ssh/authorized_keys.");
  }

  void generate() {
    std::string name = "id_ed25519", comment, pw, pw2;
    char host[256] = "";
    gethostname(host, sizeof host - 1);
    comment = std::string(getenv("USER") ? getenv("USER") : "me") + "@" + host;
    if (!form("Generate SSH Key", {{"File name:", &name}, {"Comment:", &comment}, {"Passphrase:", &pw},
                                   {"Again:", &pw2}},
              "Generate", {false, false, true, true}))
      return;
    if (!keyring::valid_key_name(name)) {
      fl_alert("Use letters, digits, dots, dashes and underscores for the file name.");
      return;
    }
    if (pw != pw2) {
      fl_alert("The passphrases differ.");
      return;
    }
    std::string path = dir() + "/" + name;
    if (fld::file_exists(path) || fld::file_exists(path + ".pub")) {
      fl_alert("%s exists already.", path.c_str());
      return;
    }
    fld::mkdir_p(dir(), 0700);
    chmod(dir().c_str(), 0700);
    std::string out;
    if (fld::run_capture({"ssh-keygen", "-q", "-t", "ed25519", "-C", comment, "-N", pw, "-f", path}, out) != 0)
      fl_alert("ssh-keygen failed: %s", out.c_str());
    else status("Made " + fld::tilde(path) + " and " + fld::tilde(path) + ".pub.");
    load();
  }

  void passphrase() {
    keyring::SshKey *k = key();
    if (!k || k->path.empty()) return;
    std::string old, pw, pw2;
    if (!form("Change Passphrase", {{"Current:", &old}, {"New:", &pw}, {"Again:", &pw2}}, "Change",
              {true, true, true}))
      return;
    if (pw != pw2) {
      fl_alert("The passphrases differ.");
      return;
    }
    std::string out;
    if (fld::run_capture({"ssh-keygen", "-q", "-p", "-P", old, "-N", pw, "-f", k->path}, out) != 0)
      fl_alert("The passphrase was not changed: %s", out.c_str());
    else status("Passphrase changed.");
  }

  void remove() {
    keyring::SshKey *k = key();
    if (!k) return;
    std::string n = fld::basename(k->path.empty() ? k->pub_path : k->path);
    if (fl_choice("Delete the SSH key %s? Servers that know only this key will not let you in any more.", "Cancel",
                  "Delete", nullptr, n.c_str()) != 1)
      return;
    if (!k->path.empty()) unlink(k->path.c_str());
    unlink(k->pub_path.c_str());
    load();
  }
};

// ---- GPG keys -----------------------------------------------------------------------------

class GpgPage : public Fl_Group {
public:
  std::vector<keyring::GpgKey> keys;
  Fl_Hold_Browser *list;

  GpgPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H, "GPG Keys") {
    list = new Fl_Hold_Browser(X + 8, Y + 10, W - 16, H - 56);
    list->textsize(13);
    static int cols[] = {300, 150, 130, 100, 100, 0};
    list->column_widths(cols);
    list->column_char('\t');
    int x = X + 8;
    auto btn = [&](const char *l, int w, Fl_Callback *cb) {
      Fl_Button *b = new Fl_Button(x, Y + H - 40, w, 30, l);
      b->callback(cb, this);
      x += w + 6;
      return b;
    };
    btn("Generate…", 110, [](Fl_Widget *, void *d) { ((GpgPage *)d)->generate(); });
    btn("Copy Public Key", 130, [](Fl_Widget *, void *d) { ((GpgPage *)d)->copy(); });
    btn("Import…", 90, [](Fl_Widget *, void *d) { ((GpgPage *)d)->import(); });
    btn("Delete…", 90, [](Fl_Widget *, void *d) { ((GpgPage *)d)->remove(); });
    end();
    resizable(list);
  }

  void load() {
    list->clear();
    if (fld::which("gpg").empty()) {
      list->add("GnuPG (gpg) is not installed.");
      return;
    }
    std::string pub, sec;
    fld::run_capture({"gpg", "--batch", "--list-keys", "--with-colons", "--fixed-list-mode"}, pub);
    fld::run_capture({"gpg", "--batch", "--list-secret-keys", "--with-colons", "--fixed-list-mode"}, sec);
    keys = keyring::parse_gpg_colons(pub);
    keyring::mark_secret(keys, keyring::parse_gpg_colons(sec));
    list->add("@bName\t@bKey\t@bType\t@bCreated\t@bExpires\t@bYours");
    for (const keyring::GpgKey &k : keys) {
      std::string uid = k.uids.empty() ? "(no name)" : k.uids[0];
      std::string state = k.revoked ? " (revoked)" : k.expired ? " (expired)" : "";
      list->add((uid + state + "\t" + k.key_id + "\t" + k.algorithm + "\t" + date(k.created) + "\t" +
                 (k.expires ? date(k.expires) : "never") + "\t" + (k.secret ? "yes" : ""))
                    .c_str());
    }
    if (keys.empty()) list->add("No GPG keys yet.");
  }

  keyring::GpgKey *key() {
    int i = list->value() - 2;
    return i >= 0 && i < (int)keys.size() ? &keys[(size_t)i] : nullptr;
  }

  void copy() {
    keyring::GpgKey *k = key();
    if (!k) return;
    std::string out;
    if (fld::run_capture({"gpg", "--batch", "--armor", "--export", k->fingerprint}, out) == 0 && !out.empty()) {
      copy_text(out);
      status("Public key copied.");
    }
  }

  void generate() {
    std::string name = getenv("USER") ? getenv("USER") : "", mail;
    if (!form("Generate GPG Key", {{"Name:", &name}, {"E-mail:", &mail}}, "Generate")) return;
    if (name.empty() || mail.find('@') == std::string::npos) {
      fl_alert("Give a name and an e-mail address.");
      return;
    }
    status("Making the key (GnuPG asks for a passphrase)…");
    Fl::flush();
    std::string out;
    // the passphrase prompt is GnuPG's pinentry
    if (fld::run_capture({"gpg", "--quick-generate-key", name + " <" + mail + ">", "default", "default", "3y"}, out) != 0)
      fl_alert("gpg failed: %s", out.c_str());
    else status("Key made.");
    load();
  }

  void import() {
    const char *f = fl_file_chooser("Import Keys", "Keys (*.{asc,gpg,pgp,key})\tAll Files (*)", fld::home_dir().c_str());
    if (!f) return;
    std::string out;
    if (fld::run_capture({"gpg", "--batch", "--import", f}, out) != 0) fl_alert("Import failed: %s", out.c_str());
    load();
  }

  void remove() {
    keyring::GpgKey *k = key();
    if (!k) return;
    std::string uid = k->uids.empty() ? k->key_id : k->uids[0];
    if (fl_choice("Delete the key \"%s\"%s?", "Cancel", "Delete", nullptr, uid.c_str(),
                  k->secret ? " and its secret key (you cannot read what was encrypted for it any more)" : "") != 1)
      return;
    std::string out;
    int rc = fld::run_capture({"gpg", "--batch", "--yes", k->secret ? "--delete-secret-and-public-key" : "--delete-keys",
                               k->fingerprint},
                              out);
    if (rc != 0) fl_alert("Not deleted: %s", out.c_str());
    load();
  }
};

PasswordsPage *g_pw;
SshPage *g_ssh;
GpgPage *g_gpg;

} // namespace

int main(int argc, char **argv) {
  fld::init_app("fl-keyring", argc, argv);
  std::string page;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-keyring [--page passwords|ssh|gpg]\n");
      return 0;
    }
    if (a == "--page" && i + 1 < argc) page = argv[++i];
  }
  const int W = 900, H = 580;
  g_win = new Fl_Double_Window(W, H, "Passwords and Keys");
  g_tabs = new fld::Tabs(0, 0, W, H - 26);
  g_pw = new PasswordsPage(0, 30, W, H - 56);
  g_ssh = new SshPage(0, 30, W, H - 56);
  g_ssh->hide();
  g_gpg = new GpgPage(0, 30, W, H - 56);
  g_gpg->hide();
  g_tabs->end();
  g_tabs->callback([](Fl_Widget *, void *) {
    Fl_Widget *v = g_tabs->value();
    if (v == g_ssh) g_ssh->load();
    else if (v == g_gpg) g_gpg->load();
    else g_pw->load();
  });
  g_status = new Fl_Box(8, H - 26, W - 16, 24);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  g_status->labelcolor(fld::palette().fg_dim);
  g_win->end();
  g_win->resizable(g_tabs);
  g_win->size_range(640, 400);
  fld::set_window_icon(g_win, "fl-keyring");
  g_win->show();
  if (page == "ssh") g_tabs->value(g_ssh);
  else if (page == "gpg") g_tabs->value(g_gpg);
  g_tabs->do_callback();
  return Fl::run();
}
