#include "prefs.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_ask.H>
#include <memory>
#include <vector>

#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fpclient.h"

namespace fp {

static std::string settings_path() { return fld::fld_config_dir() + "/flatpak.ini"; }

Settings load_settings() {
  fld::Ini ini;
  ini.load(settings_path());
  Settings s;
  s.target = ini.get("General", "Target", "system") == "user" ? "user" : "system";
  s.refresh_on_start = ini.get_bool("General", "RefreshOnStart", false);
  return s;
}

void save_settings(const Settings &s) {
  fld::Ini ini;
  ini.load(settings_path());
  ini.set("General", "Target", s.target);
  ini.set_bool("General", "RefreshOnStart", s.refresh_on_start);
  fld::mkdir_p(fld::fld_config_dir());
  ini.save(settings_path());
}

} // namespace fp

namespace {

// Add / Edit a source. Returns false when canceled.
struct SourceForm {
  std::string installation = "system", name, title, url, key; // key: file, "" or "-"
  int prio = 1;
  bool enabled = true;
};

bool source_dialog(SourceForm &f, bool editing) {
  Fl_Double_Window w(520, editing ? 250 : 300, editing ? "Edit Source" : "Add Source");
  w.set_modal();
  int y = 16, lx = 130, fw = 520 - lx - 16;
  Fl_Choice *inst = new Fl_Choice(lx, y, 200, 28, "Installation:");
  inst->add("All users");
  inst->add("Only me");
  inst->value(f.installation == "user" ? 1 : 0);
  if (editing) inst->deactivate();
  y += 36;
  Fl_Input *url = new Fl_Input(lx, y, fw - 34, 28, "Address:");
  url->value(f.url.c_str());
  url->tooltip("The repository URL, or a .flatpakrepo file (a web address or a local file)");
  Fl_Button *pick = new Fl_Button(lx + fw - 30, y, 30, 28, "…");
  pick->tooltip("Choose a .flatpakrepo file");
  pick->callback([](Fl_Widget *, void *d) {
    Fl_Native_File_Chooser fc;
    fc.title("Choose a Flatpak source file");
    fc.filter("Flatpak sources\t*.flatpakrepo");
    if (fc.show() == 0) ((Fl_Input *)d)->value(fc.filename());
  }, url);
  if (editing) {
    url->deactivate();
    pick->deactivate();
  }
  y += 36;
  Fl_Input *name = new Fl_Input(lx, y, 200, 28, "Name:");
  name->value(f.name.c_str());
  name->tooltip("A short name for the command line (flatpak install NAME ...); made from the address if empty");
  if (editing) name->deactivate();
  y += 36;
  Fl_Input *title = new Fl_Input(lx, y, fw, 28, "Title:");
  title->value(f.title.c_str());
  y += 36;
  Fl_Choice *key = nullptr;
  Fl_Input *keyfile = nullptr;
  Fl_Spinner *prio = nullptr;
  Fl_Check_Button *en = nullptr;
  if (!editing) {
    key = new Fl_Choice(lx, y, 200, 28, "Signing key:");
    key->add("From the .flatpakrepo");
    key->add("From a key file…");
    key->add("None (not verified)");
    key->value(0);
    keyfile = new Fl_Input(lx + 206, y, fw - 206, 28);
    keyfile->hide();
    key->callback([](Fl_Widget *w, void *d) {
      Fl_Input *kf = (Fl_Input *)d;
      int v = ((Fl_Choice *)w)->value();
      if (v == 1) {
        Fl_Native_File_Chooser fc;
        fc.title("Choose the GPG key of the source");
        if (fc.show() == 0) kf->value(fc.filename());
      }
      if (v == 1) kf->show();
      else kf->hide();
    }, keyfile);
    y += 36;
  } else {
    prio = new Fl_Spinner(lx, y, 80, 28, "Priority:");
    prio->range(0, 100);
    prio->step(1);
    prio->value(f.prio);
    prio->tooltip("When several sources have an app, the one with the higher priority is used");
    en = new fld::CheckButton(lx + 100, y, 200, 28, "Enabled");
    en->value(f.enabled);
    y += 36;
  }
  Fl_Return_Button *ok = new Fl_Return_Button(520 - 16 - 100, w.h() - 46, 100, 30, editing ? "Save" : "Add");
  Fl_Button *cancel = new Fl_Button(520 - 16 - 206, w.h() - 46, 100, 30, "Cancel");
  w.end();
  int result = -1;
  ok->callback([](Fl_Widget *, void *d) { *(int *)d = 1; }, &result);
  cancel->callback([](Fl_Widget *, void *d) { *(int *)d = 0; }, &result);
  w.callback([](Fl_Widget *, void *d) { *(int *)d = 0; }, &result);
  w.show();
  while (result < 0) {
    Fl::wait();
    if (result == 1 && !editing && fld::trim(url->value()).empty()) {
      fl_alert("Enter the address of the source.");
      result = -1;
    }
  }
  if (result != 1) return false;
  f.title = fld::trim(title->value());
  if (!editing) {
    f.installation = inst->value() == 1 ? "user" : "system";
    f.url = fld::trim(url->value());
    if (f.url.find("://") == std::string::npos && fld::extension(f.url) == "flatpakrepo")
      f.url = fld::abs_path(f.url);
    f.name = fld::trim(name->value());
    if (f.name.empty()) f.name = fp::remote_name_for(f.title.empty() ? f.url : f.title);
    int k = key->value();
    f.key = k == 2 ? "-" : k == 1 ? fld::trim(keyfile->value()) : "";
  } else {
    f.prio = (int)prio->value();
    f.enabled = en->value() != 0;
  }
  return true;
}

class PrefsWindow : public Fl_Double_Window {
public:
  std::function<void()> changed;
  std::vector<fp::Remote> remotes;
  Fl_Hold_Browser *list;
  Fl_Button *add, *edit, *remove, *toggle;
  Fl_Box *status;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
  bool busy = false;

  PrefsWindow(std::function<void()> ch) : Fl_Double_Window(720, 460, "Flatpak Preferences"), changed(std::move(ch)) {
    fld::Tabs *tabs = new fld::Tabs(8, 8, w() - 16, h() - 56);
    {
      Fl_Group *g = new Fl_Group(8, 36, w() - 16, h() - 84, "Sources");
      list = new Fl_Hold_Browser(20, 48, w() - 40, h() - 150);
      static int cols[] = {110, 150, 230, 0};
      list->column_widths(cols);
      list->column_char('\t');
      list->textsize(13);
      list->callback([](Fl_Widget *, void *d) {
        PrefsWindow *p = (PrefsWindow *)d;
        p->update_buttons();
        if (Fl::event_clicks()) p->do_edit();
      }, this);
      int by = h() - 94;
      add = new Fl_Button(20, by, 100, 30, "Add…");
      add->callback([](Fl_Widget *, void *d) { ((PrefsWindow *)d)->do_add(); }, this);
      edit = new Fl_Button(126, by, 100, 30, "Edit…");
      edit->callback([](Fl_Widget *, void *d) { ((PrefsWindow *)d)->do_edit(); }, this);
      toggle = new Fl_Button(232, by, 100, 30, "Disable");
      toggle->callback([](Fl_Widget *, void *d) { ((PrefsWindow *)d)->do_toggle(); }, this);
      remove = new Fl_Button(338, by, 100, 30, "Remove");
      remove->callback([](Fl_Widget *, void *d) { ((PrefsWindow *)d)->do_remove(); }, this);
      Fl_Button *flathub = new Fl_Button(w() - 20 - 130, by, 130, 30, "Add Flathub");
      flathub->tooltip("The main source of Flatpak apps");
      flathub->callback([](Fl_Widget *, void *d) {
        PrefsWindow *p = (PrefsWindow *)d;
        fp::Settings s = fp::load_settings();
        p->run("Adding Flathub…", [s](fp::Done d) { fp::add_flathub(s.target == "user", d); });
      }, this);
      g->end();
    }
    {
      Fl_Group *g = new Fl_Group(8, 36, w() - 16, h() - 84, "General");
      fp::Settings s = fp::load_settings();
      int y = 56;
      Fl_Choice *target = new Fl_Choice(220, y, 200, 28, "Install apps for:");
      target->add("All users");
      target->add("Only me");
      target->value(s.target == "user" ? 1 : 0);
      target->tooltip("The choice the main window starts with");
      target->callback([](Fl_Widget *w, void *) {
        fp::Settings s = fp::load_settings();
        s.target = ((Fl_Choice *)w)->value() == 1 ? "user" : "system";
        fp::save_settings(s);
      });
      y += 40;
      Fl_Check_Button *refresh = new fld::CheckButton(220, y, 440, 28, "Download the catalogs when the program starts");
      refresh->value(s.refresh_on_start);
      refresh->tooltip("Off: the catalogs downloaded before are used; Update All and adding a source refresh them");
      refresh->callback([](Fl_Widget *w, void *) {
        fp::Settings s = fp::load_settings();
        s.refresh_on_start = ((Fl_Check_Button *)w)->value() != 0;
        fp::save_settings(s);
      });
      y += 56;
      Fl_Box *maint = new Fl_Box(20, y, 400, 24, "Maintenance");
      maint->labelfont(FL_HELVETICA_BOLD);
      maint->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
      y += 32;
      Fl_Button *unused = new Fl_Button(20, y, 220, 30, "Remove Unused Runtimes");
      unused->tooltip("Runtimes and extensions that no installed app needs any more");
      unused->callback([](Fl_Widget *, void *d) { ((PrefsWindow *)d)->do_unused(); }, this);
      Fl_Button *repair = new Fl_Button(250, y, 180, 30, "Repair…");
      repair->tooltip("Checks the installed files and downloads damaged or missing ones again (flatpak repair)");
      repair->callback([](Fl_Widget *, void *d) { ((PrefsWindow *)d)->do_repair(); }, this);
      g->end();
    }
    tabs->end();
    status = new Fl_Box(16, h() - 42, w() - 140, 30);
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    status->labelcolor(fld::palette().fg_dim);
    Fl_Button *close = new Fl_Button(w() - 116, h() - 42, 100, 30, "Close");
    close->callback([](Fl_Widget *, void *d) { ((PrefsWindow *)d)->close(); }, this);
    callback([](Fl_Widget *w, void *) { ((PrefsWindow *)w)->close(); });
    end();
    set_modal();
    reload();
  }

  void close() {
    *alive = false;
    hide();
    Fl::delete_widget(this);
  }

  void set_status(const std::string &s) {
    status->copy_label(fld::escape_at(s).c_str());
    redraw();
  }

  const fp::Remote *selected() const {
    int i = list->value() - 1;
    return i >= 0 && i < (int)remotes.size() ? &remotes[(size_t)i] : nullptr;
  }

  void update_buttons() {
    const fp::Remote *r = selected();
    for (Fl_Button *b : {edit, remove, toggle}) {
      if (r && !busy) b->activate();
      else b->deactivate();
    }
    if (busy) add->deactivate();
    else add->activate();
    toggle->label(r && r->disabled ? "Enable" : "Disable");
  }

  void reload(const std::string &select = "") {
    set_status("Reading the sources…");
    auto a = alive;
    fp::list_remotes([this, a, select](std::vector<fp::Remote> &r, const std::string &err) {
      if (!*a) return;
      std::string keep = select;
      if (keep.empty() && selected()) keep = selected()->installation + "/" + selected()->name;
      remotes = r;
      list->clear();
      for (auto &x : remotes) {
        std::string state = x.disabled ? "disabled" : "priority " + std::to_string(x.prio);
        if (!x.gpg_verify) state += ", not verified";
        if (x.installed_refs) state += ", " + std::to_string(x.installed_refs) + " installed";
        std::string line = fld::escape_at(x.name) + "\t" + fld::escape_at(x.title.empty() ? "" : x.title) + "\t" +
                           fld::escape_at(x.url) + "\t" + (x.installation == "user" ? "only me, " : "all users, ") +
                           state;
        if (x.disabled) line = "@i" + line;
        list->add(line.c_str());
        if (x.installation + "/" + x.name == keep) list->value(list->size());
      }
      set_status(!err.empty() ? err : remotes.empty() ? "No sources: add Flathub or another one."
                                                      : std::to_string(remotes.size()) + " sources");
      update_buttons();
    });
  }

  // runs a change; then reads the sources again and tells the main window
  void run(const std::string &doing, std::function<void(fp::Done)> op, const std::string &select = "") {
    busy = true;
    update_buttons();
    set_status(doing);
    auto a = alive;
    auto ch = changed;
    op([this, a, ch, select](const std::string &err) {
      if (ch) ch();
      if (!*a) return;
      busy = false;
      if (!err.empty()) fl_alert("%s", err.c_str());
      reload(select);
    });
  }

  void do_add() {
    SourceForm f;
    f.installation = fp::load_settings().target;
    if (!source_dialog(f, false)) return;
    run("Adding " + f.name + "…", [f](fp::Done d) { fp::add_remote(f.installation, f.name, f.url, f.title, f.key, d); },
        f.installation + "/" + f.name);
  }

  void do_edit() {
    const fp::Remote *r = selected();
    if (!r || busy) return;
    SourceForm f;
    f.installation = r->installation;
    f.name = r->name;
    f.title = r->title;
    f.url = r->url;
    f.prio = r->prio;
    f.enabled = !r->disabled;
    if (!source_dialog(f, true)) return;
    fp::Remote m = *r;
    m.title = f.title;
    m.prio = f.prio;
    m.disabled = !f.enabled;
    run("Changing " + m.name + "…", [m](fp::Done d) { fp::modify_remote(m, d); }, m.installation + "/" + m.name);
  }

  void do_toggle() {
    const fp::Remote *r = selected();
    if (!r || busy) return;
    fp::Remote m = *r;
    m.disabled = !m.disabled;
    run((m.disabled ? "Disabling " : "Enabling ") + m.name + "…", [m](fp::Done d) { fp::modify_remote(m, d); },
        m.installation + "/" + m.name);
  }

  void do_remove() {
    const fp::Remote *r = selected();
    if (!r || busy) return;
    fp::Remote m = *r;
    bool force = false;
    if (m.installed_refs > 0) {
      if (fl_choice("%d installed apps and runtimes come from %s. Without it they get no more updates.\n\n"
                    "Remove the source anyway?",
                    "Cancel", "Remove", nullptr, m.installed_refs, m.name.c_str()) != 1)
        return;
      force = true;
    } else if (fl_choice("Remove the source %s?", "Cancel", "Remove", nullptr, m.name.c_str()) != 1) {
      return;
    }
    run("Removing " + m.name + "…", [m, force](fp::Done d) { fp::remove_remote(m, force, d); });
  }

  void do_unused() {
    if (busy) return;
    busy = true;
    update_buttons();
    set_status("Removing unused runtimes…");
    auto a = alive;
    auto left = std::make_shared<int>(2);
    auto ch = changed;
    auto errs = std::make_shared<std::string>();
    for (const char *inst : {"system", "user"}) {
      std::string which = inst;
      fp::remove_unused(which, [this, a](const fp::Progress &p) {
        if (*a && !p.status.empty()) set_status(p.status);
      }, [this, a, left, errs, which, ch](const std::string &e) {
        if (!e.empty() && e != "Canceled.") *errs += (errs->empty() ? "" : "\n") + which + ": " + e;
        if (--*left > 0) return;
        if (ch) ch();
        if (!*a) return;
        busy = false;
        update_buttons();
        set_status(errs->empty() ? "Unused runtimes removed." : "Some runtimes could not be removed.");
        if (!errs->empty()) fl_alert("%s", errs->c_str());
      });
    }
  }

  void do_repair() {
    if (busy) return;
    int c = fl_choice("Repair which installation? Damaged or missing files are downloaded again.", "Cancel",
                      "Only Mine", "All Users");
    if (c == 0) return;
    std::string inst = c == 1 ? "user" : "system";
    run("Repairing (this can take a while)…", [inst](fp::Done d) { fp::repair(inst, d); });
  }
};

} // namespace

void preferences_dialog(std::function<void()> changed) {
  PrefsWindow *w = new PrefsWindow(std::move(changed));
  w->show();
}
