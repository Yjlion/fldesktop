// fl-remote: VNC and RDP connections to other computers. Saved
// connections in a list; VNC is shown in a window of its own (rfb.cpp),
// RDP goes to the FreeRDP client (wlfreerdp, sdl-freerdp or xfreerdp).
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "remote_util.h"
#include "rfb.h"

namespace {

std::string ini_path() { return fld::fld_config_dir() + "/remote.ini"; }

std::vector<remote::Connection> g_conns;
Fl_Double_Window *g_win;
Fl_Hold_Browser *g_list;
Fl_Box *g_status;

void status(const std::string &s) {
  static std::string keep;
  keep = s;
  g_status->label(keep.c_str());
  g_status->redraw();
}

void load() {
  std::string text;
  fld::read_file(ini_path(), text);
  g_conns = remote::from_ini(text);
}

void save() { fld::write_file(ini_path(), remote::to_ini(g_conns)); }

void fill() {
  int sel = g_list->value();
  g_list->clear();
  for (const remote::Connection &c : g_conns)
    g_list->add((std::string("@b") + c.name + "\t" + (c.protocol == "rdp" ? "RDP" : "VNC") + "\t" + c.host +
                 (c.port ? ":" + std::to_string(c.port) : "") + (c.user.empty() ? "" : "  (" + c.user + ")"))
                    .c_str());
  if (g_conns.empty()) g_list->add("@iNo saved connections. Use Connect to make one.");
  if (sel && sel <= (int)g_conns.size()) g_list->select(sel);
}

// ---- the VNC window -----------------------------------------------------------------------

int connect_tcp(const std::string &host, int port, std::string &err) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
  if (rc != 0) {
    err = std::string("Cannot find ") + host + ": " + gai_strerror(rc);
    return -1;
  }
  int fd = -1;
  for (addrinfo *a = res; a; a = a->ai_next) {
    fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
    if (errno == EINPROGRESS) {
      // wait up to 10 s, keeping the windows alive
      bool ok = false;
      for (int i = 0; i < 100; i++) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, 100) > 0) {
          int e = 0;
          socklen_t l = sizeof e;
          getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
          ok = e == 0;
          if (!ok) err = strerror(e);
          break;
        }
        Fl::check();
      }
      if (ok) break;
      if (err.empty()) err = "No answer.";
    } else {
      err = strerror(errno);
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

class VncView;

class VncWindow : public Fl_Double_Window {
public:
  VncView *view;
  int fd = -1;
  rfb::Client client;
  std::string title;
  bool scale = true;
  VncWindow(const remote::Connection &c, int sock);
  ~VncWindow() override { disconnect(); }
  void disconnect() {
    if (fd >= 0) {
      Fl::remove_fd(fd);
      close(fd);
      fd = -1;
    }
  }
  void flush_output() {
    std::string o = client.take_output();
    size_t off = 0;
    while (fd >= 0 && off < o.size()) {
      ssize_t n = send(fd, o.data() + off, o.size() - off, MSG_NOSIGNAL);
      if (n > 0) off += (size_t)n;
      else if (errno == EAGAIN) {
        pollfd p{fd, POLLOUT, 0};
        poll(&p, 1, 200);
      } else break;
    }
  }
  int handle(int e) override;
};

class VncView : public Fl_Widget {
public:
  VncWindow *vw;
  std::vector<unsigned char> rgb;
  std::unique_ptr<Fl_RGB_Image> img;
  bool stale = true;
  std::map<int, uint32_t> down_keys; // FLTK key -> keysym sent when pressed
  int buttons = 0;
  VncView(VncWindow *w, int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H), vw(w) {}
  void geometry(int &dx, int &dy, int &dw, int &dh) {
    int fw = vw->client.width(), fh = vw->client.height();
    if (fw <= 0 || fh <= 0) {
      dx = x(), dy = y(), dw = dh = 0;
      return;
    }
    if (vw->scale) {
      double z = std::min((double)w() / fw, (double)h() / fh);
      dw = (int)(fw * z);
      dh = (int)(fh * z);
    } else {
      dw = fw;
      dh = fh;
    }
    dx = x() + (w() - dw) / 2;
    dy = y() + (h() - dh) / 2;
    if (!vw->scale) {
      dx = std::min(dx, x());
      dy = std::min(dy, y());
    }
  }
  void draw() override {
    fl_color(FL_BLACK);
    fl_rectf(x(), y(), w(), h());
    int fw = vw->client.width(), fh = vw->client.height();
    if (fw <= 0) {
      fl_color(FL_WHITE);
      fl_font(FL_HELVETICA, 14);
      fl_draw(vw->client.state() == rfb::Client::FAILED ? vw->client.error().c_str() : "Connecting…", x(), y(), w(), h(),
              FL_ALIGN_CENTER | FL_ALIGN_WRAP);
      return;
    }
    if (stale) {
      const std::vector<uint32_t> &px = vw->client.pixels();
      rgb.resize((size_t)fw * fh * 3);
      for (size_t i = 0; i < px.size(); i++) {
        rgb[i * 3] = (unsigned char)(px[i] >> 16);
        rgb[i * 3 + 1] = (unsigned char)(px[i] >> 8);
        rgb[i * 3 + 2] = (unsigned char)px[i];
      }
      img.reset(new Fl_RGB_Image(rgb.data(), fw, fh, 3));
      stale = false;
    }
    int dx, dy, dw, dh;
    geometry(dx, dy, dw, dh);
    img->scale(dw, dh, 0, 1);
    fl_push_clip(x(), y(), w(), h());
    img->draw(dx, dy);
    fl_pop_clip();
  }
  void to_remote(int &rx, int &ry) {
    int dx, dy, dw, dh;
    geometry(dx, dy, dw, dh);
    rx = dw > 0 ? (int)((Fl::event_x() - dx) * (double)vw->client.width() / dw) : 0;
    ry = dh > 0 ? (int)((Fl::event_y() - dy) * (double)vw->client.height() / dh) : 0;
  }
  int handle(int e) override {
    int rx, ry;
    switch (e) {
      case FL_FOCUS: case FL_UNFOCUS: return 1;
      case FL_ENTER:
        fl_cursor(FL_CURSOR_CROSS);
        return 1;
      case FL_LEAVE:
        fl_cursor(FL_CURSOR_DEFAULT);
        return 1;
      case FL_PUSH:
        take_focus();
        [[fallthrough]];
      case FL_RELEASE: case FL_DRAG: case FL_MOVE: {
        int st = Fl::event_buttons();
        buttons = (st & FL_BUTTON1 ? 1 : 0) | (st & FL_BUTTON2 ? 2 : 0) | (st & FL_BUTTON3 ? 4 : 0);
        to_remote(rx, ry);
        vw->client.pointer(rx, ry, buttons);
        vw->flush_output();
        return 1;
      }
      case FL_MOUSEWHEEL: {
        to_remote(rx, ry);
        int bit = Fl::event_dy() < 0 ? 8 : Fl::event_dy() > 0 ? 16 : Fl::event_dx() < 0 ? 32 : 64;
        vw->client.pointer(rx, ry, buttons | bit);
        vw->client.pointer(rx, ry, buttons);
        vw->flush_output();
        return 1;
      }
      case FL_KEYDOWN: case FL_SHORTCUT: {
        if (Fl::focus() != this) return 0;
        int k = Fl::event_key();
        uint32_t sym = rfb::keysym_for(k, Fl::event_text() ? Fl::event_text() : "");
        if (!sym) return 1;
        down_keys[k] = sym;
        vw->client.key(sym, true);
        vw->flush_output();
        return 1;
      }
      case FL_KEYUP: {
        int k = Fl::event_key();
        auto it = down_keys.find(k);
        uint32_t sym = it != down_keys.end() ? it->second : rfb::keysym_for(k, "");
        if (it != down_keys.end()) down_keys.erase(it);
        vw->client.key(sym, false);
        vw->flush_output();
        return 1;
      }
    }
    return Fl_Widget::handle(e);
  }
};

void vnc_data(int fd, void *d) {
  VncWindow *w = (VncWindow *)d;
  char buf[65536];
  ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
  if (n <= 0) {
    if (n < 0 && errno == EAGAIN) return;
    w->disconnect();
    w->copy_label((w->title + " (disconnected)").c_str());
    w->view->redraw();
    return;
  }
  if (!w->client.feed(buf, (size_t)n)) {
    w->disconnect();
    w->view->redraw();
    fl_alert("%s", w->client.error().c_str());
    return;
  }
  w->flush_output();
}

VncWindow::VncWindow(const remote::Connection &c, int sock)
    : Fl_Double_Window(1024, 700), fd(sock), client(c.password), title(c.name + " – Remote") {
  copy_label(title.c_str());
  view = new VncView(this, 0, 0, w(), h());
  end();
  resizable(view);
  fld::set_window_icon(this, "fl-remote");
  client.on_resize = [this] {
    view->stale = true;
    // the window fits the remote screen when it can
    int sw = Fl::w(), sh = Fl::h();
    size(std::min(client.width(), sw - 40), std::min(client.height(), sh - 80));
    view->redraw();
  };
  client.on_update = [this](int, int, int, int) {
    view->stale = true;
    view->redraw();
  };
  client.on_connected = [this] { copy_label((client.name().empty() ? title : client.name() + " – Remote").c_str()); };
  client.on_cut_text = [](const std::string &t) {
    Fl::copy(t.c_str(), (int)t.size(), 1);
    Fl::copy(t.c_str(), (int)t.size(), 0);
  };
  client.on_bell = [] { fl_beep(); };
  client.on_need_password = [this] {
    const char *p = fl_password("The remote computer asks for its VNC password:", "");
    if (!p) {
      disconnect();
      return;
    }
    client.set_password(p);
    client.password_given();
    flush_output();
  };
  Fl::add_fd(fd, FL_READ, vnc_data, this);
  callback([](Fl_Widget *w, void *) {
    ((VncWindow *)w)->disconnect();
    Fl::delete_widget(w);
  });
}

int VncWindow::handle(int e) {
  // F8: a menu (scale, send clipboard, Ctrl+Alt+Del, disconnect)
  if ((e == FL_KEYDOWN || e == FL_SHORTCUT) && Fl::event_key() == FL_F + 8) {
    Fl_Menu_Item m[] = {{"Scale to the Window", 0, nullptr, (void *)1, FL_MENU_TOGGLE | (scale ? FL_MENU_VALUE : 0)},
                        {"Send the Clipboard as Text", 0, nullptr, (void *)2, 0},
                        {"Send Ctrl+Alt+Delete", 0, nullptr, (void *)3, FL_MENU_DIVIDER},
                        {"Disconnect", 0, nullptr, (void *)4, 0},
                        {nullptr}};
    const Fl_Menu_Item *r = m->popup(w() / 2, h() / 2);
    if (r) switch ((long)r->user_data()) {
        case 1: scale = !scale; view->redraw(); break;
        case 2:
          Fl::paste(*this, 1);
          break;
        case 3:
          for (uint32_t s : {0xffe3u, 0xffe9u, 0xffffu}) client.key(s, true);
          for (uint32_t s : {0xffffu, 0xffe9u, 0xffe3u}) client.key(s, false);
          flush_output();
          break;
        case 4: do_callback(); return 1;
      }
    return 1;
  }
  if (e == FL_PASTE) {
    client.cut_text(remote::utf8_to_latin1(std::string(Fl::event_text(), Fl::event_length())));
    flush_output();
    return 1;
  }
  return Fl_Double_Window::handle(e);
}

// ---- connecting ---------------------------------------------------------------------------

std::string rdp_client() {
  for (const char *c : {"wlfreerdp3", "sdl-freerdp3", "wlfreerdp", "sdl-freerdp", "xfreerdp3", "xfreerdp"})
    if (!fld::which(c).empty()) return c;
  return "";
}

void open_connection(remote::Connection c) {
  if (c.protocol == "rdp") {
    std::string cl = rdp_client();
    if (cl.empty()) {
      fl_alert("No RDP client is installed. Install FreeRDP (freerdp) for RDP connections.");
      return;
    }
    std::vector<std::string> argv = remote::freerdp_args(cl, c);
    fld::spawn(argv);
    status("Started " + cl + " for " + c.name + ".");
    return;
  }
  status("Connecting to " + c.host + "…");
  Fl::flush();
  std::string err;
  int fd = connect_tcp(c.host, c.port ? c.port : 5900, err);
  if (fd < 0) {
    status("");
    fl_alert("Cannot connect to %s: %s", c.host.c_str(), err.c_str());
    return;
  }
  status("Connected to " + c.host + ". F8 opens the viewer's menu.");
  VncWindow *w = new VncWindow(c, fd);
  w->show();
  w->view->take_focus();
}

// a connection dialog; false when cancelled
bool edit(remote::Connection &c, const char *title, bool &save_it) {
  Fl_Double_Window d(440, 330, title);
  d.set_modal();
  Fl_Input name(120, 16, 300, 28, "Name:");
  name.value(c.name.c_str());
  Fl_Choice proto(120, 52, 160, 28, "Protocol:");
  proto.add("VNC|RDP (Windows)");
  proto.value(c.protocol == "rdp" ? 1 : 0);
  Fl_Input host(120, 88, 220, 28, "Computer:");
  host.value(c.host.c_str());
  host.tooltip("A host name or address; host:port works too");
  Fl_Int_Input port(380, 88, 40, 28, ":");
  if (c.port) port.value(std::to_string(c.port).c_str());
  Fl_Input user(120, 124, 300, 28, "User name:");
  user.value(c.user.c_str());
  Fl_Input domain(120, 160, 300, 28, "Domain:");
  domain.value(c.domain.c_str());
  Fl_Secret_Input pass(120, 196, 300, 28, "Password:");
  pass.tooltip("Leave empty to be asked each time (it is not saved)");
  Fl_Check_Button keep(120, 232, 300, 26, "Save this connection");
  keep.value(save_it);
  auto upd = [](Fl_Widget *, void *v) {
    Fl_Choice *p = (Fl_Choice *)v;
    Fl_Group *g = p->parent();
    for (int i = 0; i < g->children(); i++) {
      Fl_Widget *w = g->child(i);
      if (w->label() && (!strcmp(w->label(), "User name:") || !strcmp(w->label(), "Domain:")))
        p->value() == 1 ? w->activate() : w->deactivate();
    }
  };
  proto.callback(upd, &proto);
  upd(&proto, &proto);
  Fl_Button cancel(204, 284, 100, 30, "Cancel");
  Fl_Return_Button ok(312, 284, 108, 30, "Connect");
  int result = 0;
  cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 2; }, &result);
  ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
  d.end();
  d.show();
  while (!result && d.shown()) Fl::wait();
  if (result != 1) return false;
  c.protocol = proto.value() == 1 ? "rdp" : "vnc";
  remote::split_host(host.value(), c.host, c.port);
  if (*port.value()) c.port = atoi(port.value());
  c.user = user.value();
  c.domain = domain.value();
  c.password = pass.value();
  c.name = *name.value() ? name.value() : c.host;
  save_it = keep.value();
  return !c.host.empty();
}

remote::Connection *selected() {
  int i = g_list->value() - 1;
  return i >= 0 && i < (int)g_conns.size() ? &g_conns[(size_t)i] : nullptr;
}

} // namespace

int main(int argc, char **argv) {
  fld::init_app("fl-remote", argc, argv);
  load();
  std::string target;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-remote [vnc://host[:port] | rdp://[user@]host[:port] | host[:port]]\n");
      return 0;
    }
    if (a[0] != '-') target = a;
  }
  const int W = 640, H = 420;
  g_win = new Fl_Double_Window(W, H, "Remote Desktop");
  g_list = new Fl_Hold_Browser(8, 8, W - 16, H - 80);
  g_list->format_char('@');
  g_list->textsize(14);
  static int cols[] = {200, 60, 0};
  g_list->column_widths(cols);
  g_list->column_char('\t');
  g_list->callback([](Fl_Widget *, void *) {
    if (Fl::event_clicks() && Fl::event() == FL_RELEASE && selected()) open_connection(*selected());
  });
  int x = 8;
  auto btn = [&](const char *l, int w, Fl_Callback *cb) {
    Fl_Button *b = new Fl_Button(x, H - 64, w, 30, l);
    b->callback(cb);
    x += w + 6;
  };
  btn("Connect…", 100, [](Fl_Widget *, void *) {
    remote::Connection c;
    c.protocol = "vnc";
    bool keep = true;
    if (!edit(c, "New Connection", keep)) return;
    if (keep) {
      remote::Connection stored = c;
      stored.password.clear();
      g_conns.push_back(stored);
      save();
      fill();
    }
    open_connection(c);
  });
  btn("Open", 80, [](Fl_Widget *, void *) {
    if (selected()) open_connection(*selected());
  });
  btn("Edit…", 80, [](Fl_Widget *, void *) {
    remote::Connection *c = selected();
    if (!c) return;
    remote::Connection e = *c;
    bool keep = true;
    if (!edit(e, "Edit Connection", keep)) return;
    std::string pw = e.password;
    e.password.clear();
    *c = e;
    save();
    fill();
    e.password = pw;
    open_connection(e);
  });
  btn("Delete", 80, [](Fl_Widget *, void *) {
    remote::Connection *c = selected();
    if (!c) return;
    if (fl_choice("Delete the connection \"%s\"?", "Cancel", "Delete", nullptr, c->name.c_str()) != 1) return;
    g_conns.erase(g_conns.begin() + (c - g_conns.data()));
    save();
    fill();
  });
  g_status = new Fl_Box(8, H - 28, W - 16, 24);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  g_status->labelcolor(fld::palette().fg_dim);
  g_win->end();
  g_win->resizable(g_list);
  fld::set_window_icon(g_win, "fl-remote");
  fill();
  g_win->show();
  if (!target.empty()) {
    remote::Connection c;
    if (remote::parse_url(target, c)) open_connection(c);
    else fl_alert("Not a remote address: %s", target.c_str());
  }
  return Fl::run();
}
