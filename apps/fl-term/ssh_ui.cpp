#include "ssh_ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <functional>
#include <unistd.h>
#include <fstream>
#include <sstream>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

namespace ssh_ui {

std::string profiles_path() { return fld::fld_config_dir() + "/ssh.ini"; }
std::string ssh_config_path() { return fld::home_dir() + "/.ssh/config"; }

std::vector<ssh::Profile> profiles() {
  return ssh::all_profiles(profiles_path(), ssh_config_path(), fld::home_dir() + "/.ssh");
}

ssh::Profile resolve(const std::string &arg) {
  auto all = profiles();
  if (const ssh::Profile *p = ssh::find(all, arg)) return *p;
  return ssh::parse_target(arg);
}

static std::string self_exe() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  return n > 0 ? std::string(buf, (size_t)n) : "fl-term";
}

void (*open_in_tab)(const std::vector<std::string> &, const std::string &, const std::string &, bool,
                    bool) = nullptr;

void connect(const ssh::Profile &p) {
  if (p.host.empty()) return;
  if (open_in_tab) {
    auto all = profiles();
    const ssh::Profile *saved = ssh::find(all, p.name);
    const ssh::Profile &use = saved && saved->host == p.host && saved->user == p.user && saved->port == p.port ? *saved : p;
    open_in_tab(ssh::build_argv(use), use.target(), use.scheme, false, true);
    return;
  }
  // saved profiles are looked up by name so per-profile settings apply
  auto all = profiles();
  const ssh::Profile *saved = ssh::find(all, p.name);
  std::vector<std::string> argv{self_exe(), "--ssh"};
  if (saved && saved->host == p.host && saved->user == p.user && saved->port == p.port) {
    argv.push_back(p.name);
  } else {
    // an unsaved connection: pass the full ssh command line
    argv = {self_exe(), "--hold-on-error", "-T", p.target()};
    if (!p.scheme.empty()) { argv.push_back("--scheme"); argv.push_back(p.scheme); }
    argv.push_back("-e");
    for (auto &a : ssh::build_argv(p)) argv.push_back(a);
  }
  fld::spawn(argv);
}

// ---- connect / edit dialog --------------------------------------------------------

namespace {

struct ConnectDialog {
  Fl_Double_Window *win;
  Fl_Input *name, *host, *user, *identity, *jump, *forwards, *options, *command;
  Fl_Int_Input *port;
  Fl_Choice *scheme;
  std::string original_name;
  int result = 0; // 1 connect, 2 save, 3 save and connect

  ssh::Profile profile() const {
    ssh::Profile p;
    p.name = fld::trim(name->value());
    p.host = fld::trim(host->value());
    p.user = fld::trim(user->value());
    p.port = atoi(port->value());
    p.identity = fld::trim(identity->value());
    p.jump = fld::trim(jump->value());
    for (auto &f : fld::split(forwards->value(), ';'))
      if (!fld::trim(f).empty()) p.forwards.push_back(fld::trim(f));
    p.options = fld::trim(options->value());
    p.command = fld::trim(command->value());
    static const char *schemes[] = {"", "dark", "light", "red", "green", "blue"};
    int s = scheme->value();
    p.scheme = s > 0 && s < 6 ? schemes[s] : "";
    if (p.name.empty()) p.name = p.target();
    return p;
  }
};

Fl_Input *field(int y, const char *label, const char *tip, const std::string &value, int w = 300) {
  Fl_Input *in = new Fl_Input(130, y, w, 28, label);
  in->tooltip(tip);
  in->value(value.c_str());
  return in;
}

void save_profile(const ssh::Profile &p, const std::string &replace_name) {
  auto saved = ssh::load_profiles(profiles_path());
  saved.erase(std::remove_if(saved.begin(), saved.end(),
                             [&](const ssh::Profile &q) { return q.name == p.name || q.name == replace_name; }),
              saved.end());
  saved.push_back(p);
  std::sort(saved.begin(), saved.end(), [](auto &a, auto &b) { return fld::lower(a.name) < fld::lower(b.name); });
  if (!ssh::save_profiles(profiles_path(), saved)) fl_alert("Could not save %s", profiles_path().c_str());
}

} // namespace

void connect_dialog(const ssh::Profile *initial) {
  ssh::Profile p0;
  if (initial) p0 = *initial;
  ConnectDialog d;
  const int W = 460;
  d.win = new Fl_Double_Window(W, 446, initial && !initial->from_ssh_config ? "Edit SSH Connection" : "SSH Connection");
  int y = 12;
  d.host = field(y, "Host:", "Host name, address or a Host alias from ~/.ssh/config", p0.host);
  y += 34;
  d.user = field(y, "User:", "Leave empty for your user name or the ssh_config default", p0.user, 180);
  d.port = new Fl_Int_Input(370, y, 60, 28, "Port:");
  d.port->tooltip("Default: 22");
  if (p0.port > 0) d.port->value(std::to_string(p0.port).c_str());
  y += 34;
  d.identity = field(y, "Identity file:", "Private key, e.g. ~/.ssh/id_ed25519", p0.identity, 214);
  Fl_Button *browse = new Fl_Button(350, y, 80, 28, "Browse…");
  browse->callback([](Fl_Widget *, void *v) {
    Fl_Input *in = (Fl_Input *)v;
    std::string start = fld::home_dir() + "/.ssh/";
    const char *f = fl_file_chooser("Identity File", "*", start.c_str(), 0);
    if (f) in->value(fld::tilde(f).c_str());
  }, d.identity);
  y += 34;
  d.jump = field(y, "Jump host:", "Connect through this host (ssh -J), e.g. user@bastion", p0.jump);
  y += 34;
  d.forwards = field(y, "Forwards:",
                     "Port forwards separated by ';':\n  L 8080:localhost:80   local port to remote\n"
                     "  R 9000:localhost:9000   remote port to local\n  D 1080   SOCKS proxy",
                     fld::join(p0.forwards, "; "));
  y += 34;
  d.options = field(y, "Extra options:", "More ssh arguments, e.g. -C -o ServerAliveInterval=30", p0.options);
  y += 34;
  d.command = field(y, "Remote command:", "Run this instead of a login shell, e.g. tmux new -A -s main", p0.command);
  y += 34;
  d.scheme = new Fl_Choice(130, y, 180, 28, "Colors:");
  d.scheme->tooltip("Terminal colors for this connection, to tell remote sessions apart");
  for (const char *s : {"Default", "Dark", "Light", "Red tint", "Green tint", "Blue tint"}) d.scheme->add(s);
  const char *ids[] = {"", "dark", "light", "red", "green", "blue"};
  d.scheme->value(0);
  for (int i = 0; i < 6; i++)
    if (p0.scheme == ids[i]) d.scheme->value(i);
  y += 46;
  d.name = field(y, "Save as:", "Name of the saved connection", initial ? p0.name : "");
  d.original_name = initial && !initial->from_ssh_config ? initial->name : "";
  y += 34;
  Fl_Box *note = new Fl_Box(130, y, 300, 22, "Passwords are asked in the terminal, never stored.");
  note->labelsize(11);
  note->labelcolor(fld::palette().fg_dim);
  note->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  y += 34;
  Fl_Button *cancel = new Fl_Button(W - 100, y, 88, 30, "Cancel");
  cancel->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
  Fl_Button *save = new Fl_Button(W - 200, y, 92, 30, "Save");
  save->callback([](Fl_Widget *w, void *v) {
    ConnectDialog *dd = (ConnectDialog *)v;
    if (fld::trim(dd->host->value()).empty()) { fl_alert("Enter a host name or address."); return; }
    dd->result = 2;
    w->window()->hide();
  }, &d);
  Fl_Return_Button *go = new Fl_Return_Button(W - 316, y, 108, 30, "Connect");
  go->callback([](Fl_Widget *w, void *v) {
    ConnectDialog *dd = (ConnectDialog *)v;
    if (fld::trim(dd->host->value()).empty()) { fl_alert("Enter a host name or address."); return; }
    dd->result = fld::trim(dd->name->value()).empty() ? 1 : 3;
    w->window()->hide();
  }, &d);
  d.win->end();
  d.win->set_modal();
  d.win->size(W, y + 42);
  d.win->show();
  d.host->take_focus();
  while (d.win->shown()) Fl::wait();
  ssh::Profile p = d.profile();
  if (d.result == 2 || d.result == 3) save_profile(p, d.original_name);
  if (d.result == 1 || d.result == 3) connect(p);
  Fl::delete_widget(d.win);
}

// ---- manage ------------------------------------------------------------------------

void manage_dialog() {
  Fl_Double_Window *win = new Fl_Double_Window(520, 360, "SSH Connections");
  Fl_Hold_Browser *list = new Fl_Hold_Browser(12, 12, 380, 336);
  static std::vector<ssh::Profile> all;
  auto fill = [list]() {
    all = profiles();
    list->clear();
    static int widths[] = {170, 0};
    list->column_widths(widths);
    list->column_char('\t');
    for (auto &p : all) {
      std::string line = fld::escape_at(p.name) + "\t" + fld::escape_at(p.target());
      if (p.port > 0) line += ":" + std::to_string(p.port);
      if (p.from_ssh_config) line = "@i" + line + "  (ssh_config)";
      list->add(line.c_str());
    }
    if (!all.empty()) list->value(1);
  };
  fill();
  static std::function<void()> refill;
  refill = fill;
  auto sel = [list]() -> const ssh::Profile * {
    int v = list->value();
    return v > 0 && v <= (int)all.size() ? &all[(size_t)v - 1] : nullptr;
  };
  static std::function<const ssh::Profile *()> selected;
  selected = sel;
  int bx = 404, bw = 104, by = 12;
  Fl_Return_Button *c = new Fl_Return_Button(bx, by, bw, 30, "Connect");
  c->callback([](Fl_Widget *, void *) { if (auto p = selected()) connect(*p); });
  list->callback([](Fl_Widget *, void *) {
    if (Fl::event_clicks() && selected()) connect(*selected());
  });
  by += 40;
  Fl_Button *n = new Fl_Button(bx, by, bw, 30, "New…");
  n->callback([](Fl_Widget *, void *) { connect_dialog(nullptr); refill(); });
  by += 36;
  Fl_Button *e = new Fl_Button(bx, by, bw, 30, "Edit…");
  e->callback([](Fl_Widget *, void *) {
    if (auto p = selected()) {
      ssh::Profile copy = *p;
      connect_dialog(&copy);
      refill();
    }
  });
  by += 36;
  Fl_Button *del = new Fl_Button(bx, by, bw, 30, "Delete");
  del->callback([](Fl_Widget *, void *) {
    const ssh::Profile *p = selected();
    if (!p) return;
    if (p->from_ssh_config) {
      fl_message("\"%s\" comes from ~/.ssh/config; edit that file to remove it.", fld::escape_at(p->name).c_str());
      return;
    }
    if (fl_choice("Delete the connection \"%s\"?", "Cancel", "Delete", nullptr, fld::escape_at(p->name).c_str()) != 1)
      return;
    auto saved = ssh::load_profiles(profiles_path());
    std::string name = p->name;
    saved.erase(std::remove_if(saved.begin(), saved.end(), [&](auto &q) { return q.name == name; }), saved.end());
    ssh::save_profiles(profiles_path(), saved);
    refill();
  });
  by += 46;
  if (have_sshfs()) {
    Fl_Button *b = new Fl_Button(bx, by, bw, 30, "Browse Files");
    b->tooltip("Mount the host with sshfs and open it in Files");
    b->callback([](Fl_Widget *, void *) { if (auto p = selected()) browse_files(*p); });
  }
  Fl_Button *close = new Fl_Button(bx, 318, bw, 30, "Close");
  close->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
  win->resizable(list);
  win->end();
  win->show();
  while (win->shown()) Fl::wait();
  Fl::delete_widget(win);
}

// ---- keys ----------------------------------------------------------------------------

static void run_in_terminal(const std::string &title, const std::vector<std::string> &cmd) {
  if (open_in_tab) {
    open_in_tab(cmd, title, "", true, false);
    return;
  }
  std::vector<std::string> argv{self_exe(), "--hold", "-T", title, "-e"};
  argv.insert(argv.end(), cmd.begin(), cmd.end());
  fld::spawn(argv);
}

static std::vector<std::string> public_keys() {
  std::vector<std::string> v;
  for (auto &n : fld::list_dir(fld::home_dir() + "/.ssh", true))
    if (fld::ends_with(n, ".pub")) v.push_back(fld::home_dir() + "/.ssh/" + n);
  std::sort(v.begin(), v.end());
  return v;
}

void copy_key_dialog() {
  auto keys = public_keys();
  if (keys.empty()) {
    if (fl_choice("You have no SSH key yet. Create one now?", "Cancel", "Create Key", nullptr) == 1) generate_key();
    return;
  }
  const char *target = fl_input("Copy your public key to (user@host[:port]):", "");
  if (!target || !*target) return;
  ssh::Profile p = resolve(target);
  std::vector<std::string> cmd{"ssh-copy-id", "-i", keys[0]};
  for (auto &k : keys)
    if (fld::ends_with(k, "id_ed25519.pub")) cmd[2] = k;
  if (p.port > 0) { cmd.push_back("-p"); cmd.push_back(std::to_string(p.port)); }
  if (!p.jump.empty()) { cmd.push_back("-o"); cmd.push_back("ProxyJump=" + p.jump); }
  cmd.push_back(p.target());
  run_in_terminal("Copy SSH key to " + p.target(), cmd);
}

void generate_key() {
  std::string key = fld::home_dir() + "/.ssh/id_ed25519";
  if (fld::file_exists(key)) {
    fl_message("%s already exists.", fld::escape_at(fld::tilde(key)).c_str());
    return;
  }
  fld::mkdir_p(fld::home_dir() + "/.ssh", 0700);
  std::string comment = fld::getenv_or("USER", "user") + "@" + fld::trim([] {
                          std::string h;
                          fld::read_file("/etc/hostname", h);
                          return h;
                        }());
  run_in_terminal("Create SSH key", {"ssh-keygen", "-t", "ed25519", "-f", key, "-C", comment});
}

// ---- sshfs ----------------------------------------------------------------------------

bool have_sshfs() { return !fld::which("sshfs").empty(); }

static std::string mount_root() {
  return fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-sshfs";
}

std::vector<std::string> sshfs_mounts() {
  std::vector<std::string> v;
  std::string text;
  if (!fld::read_file("/proc/self/mounts", text)) return v;
  std::string root = mount_root() + "/";
  for (auto &line : fld::split(text, '\n')) {
    auto f = fld::split(line, ' ');
    if (f.size() > 2 && fld::starts_with(f[1], root) && f[2].find("sshfs") != std::string::npos) v.push_back(f[1]);
  }
  return v;
}

void unmount(const std::string &mp) {
  std::string out;
  std::string tool = !fld::which("fusermount3").empty() ? "fusermount3" : "fusermount";
  if (fld::run_capture({tool, "-u", mp}, out) != 0) fl_alert("Could not unmount %s:\n%s", mp.c_str(), out.c_str());
  else rmdir(mp.c_str());
}

void browse_files(const ssh::Profile &p) {
  std::string safe;
  for (char c : p.name) safe += isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' || c == '@' ? c : '_';
  std::string mp = mount_root() + "/" + safe;
  for (auto &m : sshfs_mounts())
    if (m == mp) {
      fld::spawn({"fl-files", mp});
      return;
    }
  std::string cmd = "mkdir -p " + fld::shell_quote(mp) + " && sshfs";
  if (p.port > 0) cmd += " -p " + std::to_string(p.port);
  if (!p.identity.empty()) cmd += " -o " + fld::shell_quote("IdentityFile=" + fld::expand_tilde(p.identity));
  if (!p.jump.empty()) cmd += " -o " + fld::shell_quote("ProxyJump=" + p.jump);
  cmd += " -o reconnect " + fld::shell_quote(p.target() + ":") + " " + fld::shell_quote(mp);
  cmd += " && fl-files " + fld::shell_quote(mp) + " && echo 'Mounted at " + mp + "'";
  run_in_terminal("Mount " + p.target(), {"/bin/sh", "-c", cmd});
}

void browse_files_dialog() {
  const char *target = fl_input("Browse files on (profile or user@host[:port]):", "");
  if (target && *target) browse_files(resolve(target));
}

} // namespace ssh_ui
