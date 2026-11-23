// org.freedesktop.impl.portal.Print: printing for sandboxed applications.
//
// PreparePrint asks for the printer, copies, pages and sides and answers
// with GTK print settings and a token; Print sends the document (a PDF or
// PostScript file descriptor) to CUPS with those settings, or asks first
// when there is no token.
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Spinner.H>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "cupsclient.h"
#include "fld/glib_bridge.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "portal.h"

namespace portal {

namespace {

struct Job {
  std::string printer;
  int copies = 1;
  std::string ranges; // "1-3,5" or ""
  int sides = 0;      // one-sided, long edge, short edge
};

std::mutex g_mutex;
std::map<guint32, Job> g_tokens;
guint32 g_next_token = 1;

const char *kSides[] = {"one-sided", "two-sided-long-edge", "two-sided-short-edge"};
const char *kDuplex[] = {"simplex", "horizontal", "vertical"};

// "1-3, 5" -> "1-3,5" (the page count is not known here); ok=false if invalid
std::string ranges(const std::string &text, bool *ok) {
  std::string r = fld::trim(text);
  *ok = true;
  if (r.empty() || fld::lower(r) == "all") return "";
  std::string o;
  for (char c : r) {
    if (c == ' ') continue;
    if (!isdigit((unsigned char)c) && c != ',' && c != '-') {
      *ok = false;
      return "";
    }
    o += c;
  }
  return o;
}

class PrintWindow : public Fl_Double_Window {
public:
  std::vector<prn::Printer> printers;
  Fl_Choice *printer;
  Fl_Spinner *copies;
  Fl_Input *pages;
  Fl_Choice *sides;
  Fl_Box *msg;
  std::function<void(bool, const Job &)> done;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
  std::string handle;
  bool finished = false;

  PrintWindow(const std::string &hdl, const std::string &app, const std::string &title, const Job &preset)
      : Fl_Double_Window(440, 270, "Print"), handle(hdl) {
    printers = prn::printers();
    Fl_Box *t = new Fl_Box(16, 10, 408, 26);
    t->copy_label(fld::escape_at(title.empty() ? app + " wants to print" : app + ": " + title).c_str());
    t->labelfont(FL_HELVETICA_BOLD);
    t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    int y = 46;
    printer = new Fl_Choice(130, y, 290, 28, "Printer:");
    int def = 0;
    for (size_t i = 0; i < printers.size(); i++) {
      printer->add(fld::escape_at(printers[i].info.empty() ? printers[i].name : printers[i].info).c_str());
      if (printers[i].is_default) def = (int)i;
      if (!preset.printer.empty() && printers[i].name == preset.printer) def = (int)i;
    }
    if (!printers.empty()) printer->value(def);
    y += 38;
    copies = new Fl_Spinner(130, y, 70, 28, "Copies:");
    copies->range(1, 99);
    copies->value(std::max(1, preset.copies));
    y += 38;
    pages = new Fl_Input(130, y, 290, 28, "Pages:");
    pages->value(preset.ranges.empty() ? "All" : preset.ranges.c_str());
    pages->tooltip("For example 1-3, 5, 8-");
    y += 38;
    sides = new Fl_Choice(130, y, 290, 28, "Sides:");
    sides->add("One side|Both sides (long edge)|Both sides (short edge)");
    sides->value(std::max(0, std::min(2, preset.sides)));
    msg = new Fl_Box(16, 210, 200, 40);
    msg->labelsize(12);
    msg->labelcolor(fld::palette().fg_dim);
    msg->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
    Fl_Button *c = new Fl_Button(204, 222, 106, 30, "Cancel");
    c->callback([](Fl_Widget *b, void *) { ((PrintWindow *)b->window())->finish(false); });
    Fl_Return_Button *ok = new Fl_Return_Button(318, 222, 106, 30, "Print");
    ok->callback([](Fl_Widget *b, void *) { ((PrintWindow *)b->window())->finish(true); });
    if (printers.empty()) {
      msg->label("No printer is set up. Add one in Printers.");
      ok->deactivate();
    }
    end();
    callback([](Fl_Widget *w, void *) { ((PrintWindow *)w)->finish(false); });
    fld::set_window_icon(this, "printer");
  }

  void finish(bool accepted) {
    if (finished) return;
    Job j;
    if (accepted) {
      bool ok;
      j.ranges = ranges(pages->value(), &ok);
      if (!ok) {
        msg->label("The pages are not valid: use numbers and ranges such as 1-3, 5.");
        return;
      }
      j.printer = printers[(size_t)std::max(0, printer->value())].name;
      j.copies = (int)copies->value();
      j.sides = std::max(0, sides->value());
    }
    finished = true;
    *alive = false;
    request_end(handle);
    hide();
    done(accepted, j);
    Fl::delete_widget(this);
  }
};

// GTK print settings for a job (what PreparePrint answers)
GVariant *settings_variant(const Job &j) {
  GVariantBuilder s;
  g_variant_builder_init(&s, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&s, "{sv}", "printer", g_variant_new_string(j.printer.c_str()));
  g_variant_builder_add(&s, "{sv}", "n-copies", g_variant_new_string(std::to_string(j.copies).c_str()));
  g_variant_builder_add(&s, "{sv}", "duplex", g_variant_new_string(kDuplex[j.sides]));
  if (!j.ranges.empty()) {
    g_variant_builder_add(&s, "{sv}", "print-pages", g_variant_new_string("ranges"));
    g_variant_builder_add(&s, "{sv}", "page-ranges", g_variant_new_string(j.ranges.c_str()));
  } else {
    g_variant_builder_add(&s, "{sv}", "print-pages", g_variant_new_string("all"));
  }
  return g_variant_builder_end(&s);
}

// the application's settings as a starting point
Job job_from_settings(GVariant *settings) {
  Job j;
  j.printer = opt_string(settings, "printer");
  j.copies = std::max(1, atoi(opt_string(settings, "n-copies", "1").c_str()));
  std::string d = opt_string(settings, "duplex");
  j.sides = d == "horizontal" ? 1 : d == "vertical" ? 2 : 0;
  if (opt_string(settings, "print-pages") == "ranges") j.ranges = opt_string(settings, "page-ranges");
  return j;
}

// worker thread: the document to a file, then to CUPS
void send(GDBusMethodInvocation *inv, int fd, const std::string &title, const Job &j) {
  const char *rt = getenv("XDG_RUNTIME_DIR");
  std::string path = std::string(rt && *rt ? rt : "/tmp") + "/fl-portal-print-" + std::to_string(getpid()) + "-" +
                     std::to_string(fd) + ".job";
  std::string data;
  char buf[65536];
  ssize_t n;
  lseek(fd, 0, SEEK_SET);
  while ((n = read(fd, buf, sizeof buf)) > 0) data.append(buf, (size_t)n);
  close(fd);
  if (data.empty() || !fld::write_file(path, data)) {
    reply(inv, OTHER);
    return;
  }
  std::vector<std::pair<std::string, std::string>> opts;
  if (j.copies > 1) opts.push_back({"copies", std::to_string(j.copies)});
  if (!j.ranges.empty()) opts.push_back({"page-ranges", j.ranges});
  opts.push_back({"sides", kSides[j.sides]});
  prn::Error err;
  int id = prn::print_file(j.printer, path, title.empty() ? "Document" : title, opts, &err);
  unlink(path.c_str());
  if (id <= 0) g_printerr("xdg-desktop-portal-fl: printing failed: %s\n", err.c_str());
  reply(inv, id > 0 ? SUCCESS : OTHER);
}

} // namespace

void print_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv, GUnixFDList *fds) {
  if (method == "PreparePrint") {
    const char *handle, *app_id, *parent, *title;
    GVariant *settings, *setup, *options;
    g_variant_get(params, "(&o&s&s&s@a{sv}@a{sv}@a{sv})", &handle, &app_id, &parent, &title, &settings, &setup,
                  &options);
    Job preset = job_from_settings(settings);
    g_variant_unref(settings);
    GVariant *page_setup = setup; // handed back as it came
    g_variant_unref(options);
    std::string h = handle, app = app_name(app_id), t = title;
    fld::ui_call([inv, h, app, t, preset, page_setup] {
      PrintWindow *w = new PrintWindow(h, app, t, preset);
      w->done = [inv, page_setup](bool ok, const Job &j) {
        if (!ok) {
          g_variant_unref(page_setup);
          reply(inv, CANCELLED);
          return;
        }
        guint32 token;
        {
          std::lock_guard<std::mutex> l(g_mutex);
          token = g_next_token++;
          g_tokens[token] = j;
        }
        GVariantBuilder r;
        g_variant_builder_init(&r, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&r, "{sv}", "settings", settings_variant(j));
        g_variant_builder_add(&r, "{sv}", "page-setup", page_setup);
        g_variant_builder_add(&r, "{sv}", "token", g_variant_new_uint32(token));
        g_variant_unref(page_setup);
        reply(inv, SUCCESS, g_variant_builder_end(&r));
      };
      auto alive = w->alive;
      w->show();
      request_begin(h, [w, alive] {
        if (*alive) w->finish(false);
      });
    });
    return;
  }
  if (method == "Print") {
    const char *handle, *app_id, *parent, *title;
    gint32 fd_index;
    GVariant *options;
    g_variant_get(params, "(&o&s&s&sh@a{sv})", &handle, &app_id, &parent, &title, &fd_index, &options);
    guint32 token = 0;
    g_variant_lookup(options, "token", "u", &token);
    g_variant_unref(options);
    int fd = fds ? g_unix_fd_list_get(fds, fd_index, nullptr) : -1;
    if (fd < 0) {
      reply(inv, OTHER);
      return;
    }
    std::string t = title;
    Job j;
    bool have = false;
    {
      std::lock_guard<std::mutex> l(g_mutex);
      auto it = g_tokens.find(token);
      if (token && it != g_tokens.end()) {
        j = it->second;
        g_tokens.erase(it);
        have = true;
      }
    }
    if (have) {
      std::thread(send, inv, fd, t, j).detach();
      return;
    }
    // no prepared settings: ask now
    std::string h = handle, app = app_name(app_id);
    fld::ui_call([inv, h, app, t, fd] {
      PrintWindow *w = new PrintWindow(h, app, t, Job());
      w->done = [inv, fd, t](bool ok, const Job &job) {
        if (!ok) {
          close(fd);
          reply(inv, CANCELLED);
          return;
        }
        std::thread(send, inv, fd, t, job).detach();
      };
      auto alive = w->alive;
      w->show();
      request_begin(h, [w, alive] {
        if (*alive) w->finish(false);
      });
    });
    return;
  }
  g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method.c_str());
}

} // namespace portal
