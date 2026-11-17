// CUPS access for fldesktop (libcups, local scheduler).
//
// The calls are synchronous and quick for a local cupsd, except discover()
// and add_printer() (device probing), which callers run in a worker thread.
// When CUPS asks for a password, the callback set with set_password_prompt()
// runs (from any thread).
#pragma once
#include <ctime>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace prn {

struct Printer {
  std::string name, info, location, make_model, uri, device_uri, state_message;
  int state = 3;                     // 3 idle, 4 processing, 5 stopped
  std::vector<std::string> reasons;  // printer-state-reasons without "none"
  bool accepting = true, is_default = false, shared = false, remote = false;
  int active_jobs = 0;
};

struct Job {
  int id = 0;
  std::string printer, title, user;
  int state = 3;                     // 3 pending, 4 held, 5 processing, 6 stopped, 7 canceled, 8 aborted, 9 completed
  time_t created = 0, completed = 0;
  int size_kb = 0;
  bool active() const { return state >= 3 && state <= 6; }
};

struct Device {
  std::string uri, info, make_model, device_class, device_id, location;
};

struct Driver {
  std::string name, make_model, make; // name = ppd-name ("everywhere", "drv:///...", "lsb/...")
};

// a printer option with its choices (media, sides, print-color-mode)
struct Option {
  std::string key, value;                                  // e.g. "media", "iso_a4_210x297mm"
  std::vector<std::pair<std::string, std::string>> choices; // value, label
};

using Error = std::string; // "" = success

bool available();
std::vector<Printer> printers(Error *err = nullptr);
// jobs of all printers (printer "") or one; mine = only the user's jobs
std::vector<Job> jobs(bool active_only, const std::string &printer = "", bool mine = true);
Error cancel_job(int id);
Error set_default(const std::string &name);
Error set_enabled(const std::string &name, bool on);   // pause / resume
Error set_accepting(const std::string &name, bool on); // accept / reject jobs
Error set_shared(const std::string &name, bool on);
Error set_info(const std::string &name, const std::string &info, const std::string &location);
Error delete_printer(const std::string &name);
// ppd "everywhere" sets up a driverless IPP printer
Error add_printer(const std::string &name, const std::string &uri, const std::string &ppd, const std::string &info,
                  const std::string &location);
std::vector<Device> discover(int timeout_s, Error *err = nullptr);
// drivers matching an IEEE 1284 device id or a make (both may be empty)
std::vector<Driver> drivers(const std::string &device_id, const std::string &make, Error *err = nullptr);
// returns the job id or 0
int print_test_page(const std::string &name, Error *err = nullptr);
std::vector<Option> options(const std::string &name);
Error set_option(const std::string &name, const std::string &key, const std::string &value);

// Asked for (user, password) when CUPS needs authentication; return false to cancel.
void set_password_prompt(std::function<bool(const std::string &resource, std::string &user, std::string &password)> fn);

// ---- helpers (unit tested) --------------------------------------------------
std::string printer_state_text(const Printer &p);
std::string job_state_text(int state);
// a valid CUPS queue name from a description: letters, digits, '-', '_'
std::string queue_name(const std::string &description);
// "iso_a4_210x297mm" -> "A4", "na_letter_8.5x11in" -> "Letter"
std::string media_label(const std::string &media);
// device class / scheme can use driverless IPP
bool is_driverless_uri(const std::string &uri);
// the MFG / MDL fields of an IEEE 1284 device id
std::string device_id_field(const std::string &device_id, const std::string &key);

} // namespace prn
