#include "cupsclient.h"

#include <cups/cups.h>
#include <cups/ipp.h>
#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace prn {

namespace {

std::mutex g_pw_mutex;
std::function<bool(const std::string &, std::string &, std::string &)> g_prompt;

const char *password_cb(const char *prompt, http_t *, const char *method, const char *resource, void *) {
  std::function<bool(const std::string &, std::string &, std::string &)> fn;
  {
    std::lock_guard<std::mutex> l(g_pw_mutex);
    fn = g_prompt;
  }
  if (!fn) return nullptr;
  std::string user = cupsUser(), pw;
  if (!fn(resource ? resource : "", user, pw)) return nullptr;
  cupsSetUser(user.c_str());
  static thread_local std::string keep;
  keep = pw;
  return keep.c_str();
}

void init_once() {
  static std::once_flag f;
  std::call_once(f, [] { cupsSetPasswordCB2(password_cb, nullptr); });
}

http_t *connect() {
  init_once();
  return httpConnect2(cupsServer(), ippPort(), nullptr, AF_UNSPEC, cupsEncryption(), 1, 5000, nullptr);
}

std::string printer_uri(const std::string &name) {
  char uri[HTTP_MAX_URI];
  httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof uri, "ipp", nullptr, "localhost", ippPort(), "/printers/%s",
                   name.c_str());
  return uri;
}

std::string last_error() {
  const char *m = cupsLastErrorString();
  return m && *m ? m : "CUPS error";
}

// sends a request to the admin resource (or another), returns "" on success
Error do_admin(ipp_t *req, const char *resource = "/admin/") {
  http_t *http = connect();
  if (!http) {
    ippDelete(req);
    return "Cannot connect to the printing service (CUPS).";
  }
  ipp_t *resp = cupsDoRequest(http, req, resource);
  ipp_status_t st = cupsLastError();
  ippDelete(resp);
  httpClose(http);
  if (st > IPP_STATUS_OK_EVENTS_COMPLETE) return last_error();
  return "";
}

ipp_t *printer_request(ipp_op_t op, const std::string &name) {
  ipp_t *req = ippNewRequest(op);
  ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", nullptr, printer_uri(name).c_str());
  ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", nullptr, cupsUser());
  return req;
}

std::string attr_string(ipp_attribute_t *a, int i = 0) {
  if (!a) return "";
  const char *s = ippGetString(a, i, nullptr);
  return s ? s : "";
}

} // namespace

void set_password_prompt(std::function<bool(const std::string &, std::string &, std::string &)> fn) {
  std::lock_guard<std::mutex> l(g_pw_mutex);
  g_prompt = std::move(fn);
}

bool available() {
  http_t *http = connect();
  if (!http) return false;
  httpClose(http);
  return true;
}

std::vector<Printer> printers(Error *err) {
  std::vector<Printer> out;
  http_t *http = connect();
  if (!http) {
    if (err) *err = "The printing service (CUPS) is not running.";
    return out;
  }
  ipp_t *req = ippNewRequest(IPP_OP_CUPS_GET_PRINTERS);
  static const char *const attrs[] = {"printer-name", "printer-info", "printer-location", "printer-make-and-model",
                                      "printer-uri-supported", "device-uri", "printer-state",
                                      "printer-state-message", "printer-state-reasons", "printer-is-accepting-jobs",
                                      "printer-is-shared", "printer-type", "queued-job-count"};
  ippAddStrings(req, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", sizeof attrs / sizeof attrs[0],
                nullptr, attrs);
  ipp_t *resp = cupsDoRequest(http, req, "/");
  cups_dest_t *dests = nullptr;
  int nd = cupsGetDests2(http, &dests);
  std::string def;
  for (int i = 0; i < nd; i++)
    if (dests[i].is_default && !dests[i].instance) def = dests[i].name;
  cupsFreeDests(nd, dests);
  if (resp) {
    Printer p;
    bool have = false;
    for (ipp_attribute_t *a = ippFirstAttribute(resp); a; a = ippNextAttribute(resp)) {
      const char *n = ippGetName(a);
      if (!n) { // group separator
        if (have) out.push_back(p);
        p = Printer();
        have = false;
        continue;
      }
      std::string k = n;
      have = true;
      if (k == "printer-name") p.name = attr_string(a);
      else if (k == "printer-info") p.info = attr_string(a);
      else if (k == "printer-location") p.location = attr_string(a);
      else if (k == "printer-make-and-model") p.make_model = attr_string(a);
      else if (k == "printer-uri-supported") p.uri = attr_string(a);
      else if (k == "device-uri") p.device_uri = attr_string(a);
      else if (k == "printer-state") p.state = ippGetInteger(a, 0);
      else if (k == "printer-state-message") p.state_message = attr_string(a);
      else if (k == "printer-is-accepting-jobs") p.accepting = ippGetBoolean(a, 0);
      else if (k == "printer-is-shared") p.shared = ippGetBoolean(a, 0);
      else if (k == "printer-type") p.remote = (ippGetInteger(a, 0) & CUPS_PRINTER_REMOTE) != 0;
      else if (k == "queued-job-count") p.active_jobs = ippGetInteger(a, 0);
      else if (k == "printer-state-reasons") {
        for (int i = 0; i < ippGetCount(a); i++) {
          std::string r = attr_string(a, i);
          if (r != "none") p.reasons.push_back(r);
        }
      }
    }
    if (have) out.push_back(p);
    ippDelete(resp);
  } else if (cupsLastError() != IPP_STATUS_ERROR_NOT_FOUND && err) {
    *err = last_error();
  }
  httpClose(http);
  for (auto &p : out) p.is_default = p.name == def;
  out.erase(std::remove_if(out.begin(), out.end(), [](const Printer &p) { return p.name.empty(); }), out.end());
  std::sort(out.begin(), out.end(), [](const Printer &a, const Printer &b) { return a.name < b.name; });
  return out;
}

std::vector<Job> jobs(bool active_only, const std::string &printer, bool mine) {
  std::vector<Job> out;
  http_t *http = connect();
  if (!http) return out;
  cups_job_t *js = nullptr;
  int n = cupsGetJobs2(http, &js, printer.empty() ? nullptr : printer.c_str(), mine ? 1 : 0,
                       active_only ? CUPS_WHICHJOBS_ACTIVE : CUPS_WHICHJOBS_ALL);
  for (int i = 0; i < n; i++) {
    Job j;
    j.id = js[i].id;
    j.printer = js[i].dest ? js[i].dest : "";
    j.title = js[i].title ? js[i].title : "";
    j.user = js[i].user ? js[i].user : "";
    j.state = js[i].state;
    j.created = js[i].creation_time;
    j.completed = js[i].completed_time;
    j.size_kb = js[i].size;
    out.push_back(j);
  }
  cupsFreeJobs(n, js);
  httpClose(http);
  std::sort(out.begin(), out.end(), [](const Job &a, const Job &b) { return a.id > b.id; });
  return out;
}

Error cancel_job(int id) {
  http_t *http = connect();
  if (!http) return "Cannot connect to the printing service (CUPS).";
  int ok = cupsCancelJob2(http, nullptr, id, 0);
  httpClose(http);
  return ok == IPP_STATUS_OK || cupsLastError() <= IPP_STATUS_OK_EVENTS_COMPLETE ? "" : last_error();
}

Error set_default(const std::string &name) { return do_admin(printer_request(IPP_OP_CUPS_SET_DEFAULT, name)); }

Error set_enabled(const std::string &name, bool on) {
  return do_admin(printer_request(on ? IPP_OP_RESUME_PRINTER : IPP_OP_PAUSE_PRINTER, name));
}

Error set_accepting(const std::string &name, bool on) {
  return do_admin(printer_request(on ? IPP_OP_CUPS_ACCEPT_JOBS : IPP_OP_CUPS_REJECT_JOBS, name));
}

Error set_shared(const std::string &name, bool on) {
  ipp_t *req = printer_request(IPP_OP_CUPS_ADD_MODIFY_PRINTER, name);
  ippAddBoolean(req, IPP_TAG_PRINTER, "printer-is-shared", on);
  return do_admin(req);
}

Error set_info(const std::string &name, const std::string &info, const std::string &location) {
  ipp_t *req = printer_request(IPP_OP_CUPS_ADD_MODIFY_PRINTER, name);
  ippAddString(req, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-info", nullptr, info.c_str());
  ippAddString(req, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-location", nullptr, location.c_str());
  return do_admin(req);
}

Error delete_printer(const std::string &name) { return do_admin(printer_request(IPP_OP_CUPS_DELETE_PRINTER, name)); }

Error add_printer(const std::string &name, const std::string &uri, const std::string &ppd, const std::string &info,
                  const std::string &location) {
  if (name.empty() || queue_name(name) != name) return "The printer name may only use letters, digits, '-' and '_'.";
  ipp_t *req = printer_request(IPP_OP_CUPS_ADD_MODIFY_PRINTER, name);
  if (!ppd.empty()) ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_NAME, "ppd-name", nullptr, ppd.c_str());
  ippAddString(req, IPP_TAG_PRINTER, IPP_TAG_URI, "device-uri", nullptr, uri.c_str());
  ippAddString(req, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-info", nullptr, (info.empty() ? name : info).c_str());
  ippAddString(req, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-location", nullptr, location.c_str());
  ippAddInteger(req, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", IPP_PSTATE_IDLE);
  ippAddBoolean(req, IPP_TAG_PRINTER, "printer-is-accepting-jobs", 1);
  Error e = do_admin(req);
  if (!e.empty()) return e;
  // a new queue starts paused on some systems: enable it
  set_enabled(name, true);
  set_accepting(name, true);
  return "";
}

std::vector<Device> discover(int timeout_s, Error *err) {
  std::vector<Device> out;
  http_t *http = connect();
  if (!http) {
    if (err) *err = "The printing service (CUPS) is not running.";
    return out;
  }
  ipp_t *req = ippNewRequest(IPP_OP_CUPS_GET_DEVICES);
  ippAddInteger(req, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "timeout", timeout_s);
  httpSetTimeout(http, timeout_s + 30, nullptr, nullptr);
  ipp_t *resp = cupsDoRequest(http, req, "/");
  if (resp) {
    Device d;
    for (ipp_attribute_t *a = ippFirstAttribute(resp); a; a = ippNextAttribute(resp)) {
      const char *n = ippGetName(a);
      if (!n) {
        if (!d.uri.empty()) out.push_back(d);
        d = Device();
        continue;
      }
      std::string k = n;
      if (k == "device-uri") d.uri = attr_string(a);
      else if (k == "device-info") d.info = attr_string(a);
      else if (k == "device-make-and-model") d.make_model = attr_string(a);
      else if (k == "device-class") d.device_class = attr_string(a);
      else if (k == "device-id") d.device_id = attr_string(a);
      else if (k == "device-location") d.location = attr_string(a);
    }
    if (!d.uri.empty()) out.push_back(d);
    ippDelete(resp);
  } else if (err) {
    *err = last_error();
  }
  httpClose(http);
  // backends such as "socket" or "lpd" without a host are templates for manual entry
  out.erase(std::remove_if(out.begin(), out.end(),
                           [](const Device &d) { return d.uri.find(':') == std::string::npos || d.uri.back() == ':'; }),
            out.end());
  return out;
}

std::vector<Driver> drivers(const std::string &device_id, const std::string &make, Error *err) {
  std::vector<Driver> out;
  http_t *http = connect();
  if (!http) {
    if (err) *err = "The printing service (CUPS) is not running.";
    return out;
  }
  ipp_t *req = ippNewRequest(IPP_OP_CUPS_GET_PPDS);
  if (!device_id.empty())
    ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_TEXT, "ppd-device-id", nullptr, device_id.c_str());
  else if (!make.empty())
    ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_TEXT, "ppd-make", nullptr, make.c_str());
  static const char *const attrs[] = {"ppd-name", "ppd-make-and-model", "ppd-make"};
  ippAddStrings(req, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", 3, nullptr, attrs);
  ipp_t *resp = cupsDoRequest(http, req, "/");
  if (resp) {
    Driver d;
    for (ipp_attribute_t *a = ippFirstAttribute(resp); a; a = ippNextAttribute(resp)) {
      const char *n = ippGetName(a);
      if (!n) {
        if (!d.name.empty()) out.push_back(d);
        d = Driver();
        continue;
      }
      std::string k = n;
      if (k == "ppd-name") d.name = attr_string(a);
      else if (k == "ppd-make-and-model") d.make_model = attr_string(a);
      else if (k == "ppd-make") d.make = attr_string(a);
    }
    if (!d.name.empty()) out.push_back(d);
    ippDelete(resp);
  } else if (err && cupsLastError() != IPP_STATUS_ERROR_NOT_FOUND) {
    *err = last_error();
  }
  httpClose(http);
  std::sort(out.begin(), out.end(), [](const Driver &a, const Driver &b) { return a.make_model < b.make_model; });
  return out;
}

int print_test_page(const std::string &name, Error *err) {
  http_t *http = connect();
  if (!http) {
    if (err) *err = "Cannot connect to the printing service (CUPS).";
    return 0;
  }
  // CUPS renders its own test page from the "testprint" banner
  const char *candidates[] = {"/usr/share/cups/data/testprint", "/usr/local/share/cups/data/testprint"};
  const char *file = nullptr;
  for (auto c : candidates)
    if (access(c, R_OK) == 0) file = c;
  int id = 0;
  if (file) {
    cups_option_t *opts = nullptr;
    int n = cupsAddOption("document-format", "application/vnd.cups-banner", 0, &opts);
    id = cupsPrintFile2(http, name.c_str(), file, "Test Page", n, opts);
    cupsFreeOptions(n, opts);
  } else {
    // no banner file: print a short text page
    char tmpl[] = "/tmp/fl-printers-testXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
      std::string text = "fldesktop printer test page\n\nPrinter: " + name + "\n\nIf you can read this, printing works.\n";
      if (write(fd, text.data(), text.size()) < 0) {}
      close(fd);
      id = cupsPrintFile2(http, name.c_str(), tmpl, "Test Page", 0, nullptr);
      unlink(tmpl);
    }
  }
  if (!id && err) *err = last_error();
  httpClose(http);
  return id;
}

int print_file(const std::string &printer, const std::string &file, const std::string &title,
               const std::vector<std::pair<std::string, std::string>> &options, Error *err) {
  http_t *http = connect();
  if (!http) {
    if (err) *err = "Cannot connect to the printing service (CUPS).";
    return 0;
  }
  cups_option_t *opts = nullptr;
  int n = 0;
  for (const auto &o : options) n = cupsAddOption(o.first.c_str(), o.second.c_str(), n, &opts);
  int id = cupsPrintFile2(http, printer.c_str(), file.c_str(), title.c_str(), n, opts);
  cupsFreeOptions(n, opts);
  if (!id && err) *err = last_error();
  httpClose(http);
  return id;
}

std::vector<Option> options(const std::string &name) {
  std::vector<Option> out;
  http_t *http = connect();
  if (!http) return out;
  ipp_t *req = printer_request(IPP_OP_GET_PRINTER_ATTRIBUTES, name);
  static const char *const attrs[] = {"media-default", "media-supported", "sides-default", "sides-supported",
                                      "print-color-mode-default", "print-color-mode-supported"};
  ippAddStrings(req, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", 6, nullptr, attrs);
  ipp_t *resp = cupsDoRequest(http, req, ("/printers/" + name).c_str());
  httpClose(http);
  if (!resp) return out;
  struct Spec {
    const char *key;
  } specs[] = {{"media"}, {"sides"}, {"print-color-mode"}};
  for (auto &s : specs) {
    std::string k = s.key;
    ipp_attribute_t *def = ippFindAttribute(resp, (k + "-default").c_str(), IPP_TAG_ZERO);
    ipp_attribute_t *sup = ippFindAttribute(resp, (k + "-supported").c_str(), IPP_TAG_ZERO);
    if (!sup || ippGetCount(sup) < 2) continue;
    Option o;
    o.key = k;
    o.value = attr_string(def);
    for (int i = 0; i < ippGetCount(sup); i++) {
      std::string v = attr_string(sup, i);
      if (v.empty()) continue;
      std::string label = v;
      if (k == "media") label = media_label(v);
      else if (v == "one-sided") label = "One-sided";
      else if (v == "two-sided-long-edge") label = "Two-sided (long edge)";
      else if (v == "two-sided-short-edge") label = "Two-sided (short edge)";
      else if (v == "color") label = "Color";
      else if (v == "monochrome") label = "Black and white";
      else if (v == "auto") label = "Automatic";
      o.choices.push_back({v, label});
    }
    out.push_back(o);
  }
  ippDelete(resp);
  return out;
}

Error set_option(const std::string &name, const std::string &key, const std::string &value) {
  // lpadmin -o key=value: a "<key>-default" attribute in the printer group
  ipp_t *req = printer_request(IPP_OP_CUPS_ADD_MODIFY_PRINTER, name);
  ippAddString(req, IPP_TAG_PRINTER, IPP_TAG_NAME, (key + "-default").c_str(), nullptr, value.c_str());
  return do_admin(req);
}

// ---- helpers ----------------------------------------------------------------

std::string printer_state_text(const Printer &p) {
  std::string s;
  if (p.state == 5) s = "Paused";
  else if (p.state == 4) s = "Printing";
  else s = "Ready";
  if (!p.accepting) s += ", not accepting jobs";
  for (auto &r : p.reasons) {
    std::string base = r;
    for (const char *suf : {"-error", "-warning", "-report"})
      if (base.size() > strlen(suf) && base.compare(base.size() - strlen(suf), strlen(suf), suf) == 0)
        base = base.substr(0, base.size() - strlen(suf));
    const char *t = nullptr;
    if (base == "media-empty" || base == "media-needed") t = "out of paper";
    else if (base == "media-jam") t = "paper jam";
    else if (base == "toner-low" || base == "marker-supply-low") t = "toner low";
    else if (base == "toner-empty" || base == "marker-supply-empty") t = "toner empty";
    else if (base == "door-open" || base == "cover-open") t = "cover open";
    else if (base == "offline" || base == "offline-report" || base == "connecting-to-device") t = "offline";
    else if (base == "paused") continue;
    if (t) s += std::string(", ") + t;
  }
  return s;
}

std::string job_state_text(int state) {
  switch (state) {
  case 3: return "Waiting";
  case 4: return "Held";
  case 5: return "Printing";
  case 6: return "Stopped";
  case 7: return "Canceled";
  case 8: return "Failed";
  case 9: return "Completed";
  }
  return "Unknown";
}

std::string queue_name(const std::string &description) {
  std::string out;
  for (unsigned char c : description) {
    if (isalnum(c) || c == '-' || c == '_') out += (char)c;
    else if ((c == ' ' || c == '.' || c == '/') && !out.empty() && out.back() != '_') out += '_';
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  if (out.size() > 127) out.resize(127);
  return out;
}

std::string media_label(const std::string &media) {
  static const std::pair<const char *, const char *> known[] = {
      {"iso_a3_", "A3"},           {"iso_a4_", "A4"},        {"iso_a5_", "A5"},       {"iso_a6_", "A6"},
      {"iso_b5_", "B5"},           {"jis_b5_", "B5 (JIS)"},  {"na_letter_", "Letter"}, {"na_legal_", "Legal"},
      {"na_executive_", "Executive"}, {"na_ledger_", "Tabloid"}, {"na_number-10_", "Envelope #10"},
      {"iso_dl_", "Envelope DL"},  {"iso_c5_", "Envelope C5"}, {"na_index-4x6_", "4×6 in"}, {"na_5x7_", "5×7 in"},
  };
  for (auto &k : known)
    if (media.rfind(k.first, 0) == 0) return k.second;
  // "custom_..." or unknown: the size part
  size_t u = media.rfind('_');
  return u == std::string::npos ? media : media.substr(u + 1);
}

bool is_driverless_uri(const std::string &uri) {
  return uri.rfind("ipp://", 0) == 0 || uri.rfind("ipps://", 0) == 0 ||
         (uri.rfind("dnssd://", 0) == 0 && (uri.find("._ipp.") != std::string::npos ||
                                            uri.find("._ipps.") != std::string::npos)) ||
         uri.rfind("implicitclass://", 0) == 0;
}

std::string device_id_field(const std::string &device_id, const std::string &key) {
  // fields are "KEY:value;" (with long and short key names)
  std::vector<std::string> keys = {key};
  if (key == "MFG") keys.push_back("MANUFACTURER");
  if (key == "MDL") keys.push_back("MODEL");
  if (key == "CMD") keys.push_back("COMMAND SET");
  size_t pos = 0;
  while (pos < device_id.size()) {
    size_t semi = device_id.find(';', pos);
    std::string field = device_id.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
    size_t colon = field.find(':');
    if (colon != std::string::npos) {
      std::string k = field.substr(0, colon);
      while (!k.empty() && k.front() == ' ') k.erase(0, 1);
      for (auto &want : keys)
        if (strcasecmp(k.c_str(), want.c_str()) == 0) return field.substr(colon + 1);
    }
    if (semi == std::string::npos) break;
    pos = semi + 1;
  }
  return "";
}

} // namespace prn
