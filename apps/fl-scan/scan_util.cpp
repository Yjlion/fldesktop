#include "scan_util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "fld/util.h"

namespace scan {

std::vector<Device> parse_devices(const std::string &out) {
  std::vector<Device> v;
  for (const std::string &line : fld::split(out, '\n')) {
    size_t a = line.find("device `");
    if (a == std::string::npos) continue;
    a += 8;
    size_t b = line.find("' is a", a);
    if (b == std::string::npos) continue;
    Device d;
    d.name = line.substr(a, b - a);
    d.label = fld::trim(line.substr(b + 6));
    // "eSCL Canon X ip=..." / "Brother MFC scanner" -> keep it short
    if (fld::starts_with(d.label, "eSCL ") || fld::starts_with(d.label, "WSD ")) d.label = d.label.substr(d.label.find(' ') + 1);
    size_t ip = d.label.find(" ip=");
    if (ip != std::string::npos) d.label = d.label.substr(0, ip) + " (network)";
    v.push_back(d);
  }
  return v;
}

namespace {

// "    --mode Color|Gray|Lineart [Color]" -> name, choices, default
bool option_line(const std::string &line, std::string &name, std::string &choices, std::string &def) {
  std::string t = fld::trim(line);
  if (!fld::starts_with(t, "-")) return false;
  size_t sp = t.find(' ');
  if (sp == std::string::npos) return false;
  name = t.substr(0, sp);
  std::string rest = fld::trim(t.substr(sp + 1));
  size_t lb = rest.rfind('[');
  if (lb != std::string::npos && rest.back() == ']') {
    def = rest.substr(lb + 1, rest.size() - lb - 2);
    rest = fld::trim(rest.substr(0, lb));
  }
  choices = rest;
  return true;
}

double range_max(const std::string &choices) {
  // "0..215.9mm (in steps of 0.1)" -> 215.9
  size_t dots = choices.find("..");
  if (dots == std::string::npos) return 0;
  return atof(choices.c_str() + dots + 2);
}

} // namespace

Options parse_options(const std::string &out) {
  Options o;
  for (const std::string &line : fld::split(out, '\n')) {
    std::string name, choices, def;
    if (!option_line(line, name, choices, def)) continue;
    if (name == "--mode") {
      for (const std::string &m : fld::split(choices, '|')) o.modes.push_back(fld::trim(m));
      o.mode = def;
    } else if (name == "--source") {
      // sources may contain spaces: "Flatbed|ADF Front|ADF Duplex"
      for (const std::string &m : fld::split(choices, '|')) o.sources.push_back(fld::trim(m));
      o.source = def;
    } else if (name == "--resolution") {
      std::string c = choices;
      if (fld::ends_with(c, "dpi")) c = c.substr(0, c.size() - 3);
      if (c.find("..") != std::string::npos) {
        // a range: the usual steps within it
        int lo = atoi(c.c_str()), hi = atoi(c.c_str() + c.find("..") + 2);
        for (int r : {75, 100, 150, 200, 300, 600, 1200, 2400})
          if (r >= lo && r <= hi) o.resolutions.push_back(r);
      } else {
        for (const std::string &r : fld::split(c, '|')) {
          int v = atoi(r.c_str());
          if (v > 0) o.resolutions.push_back(v);
        }
      }
      o.resolution = atoi(def.c_str());
    } else if (name == "-x") {
      o.max_w = range_max(choices);
    } else if (name == "-y") {
      o.max_h = range_max(choices);
    }
  }
  std::sort(o.resolutions.begin(), o.resolutions.end());
  o.resolutions.erase(std::unique(o.resolutions.begin(), o.resolutions.end()), o.resolutions.end());
  return o;
}

std::vector<std::string> scanimage_args(const Request &r) {
  std::vector<std::string> a = {"scanimage", "-d", r.device, "--format=png"};
  if (!r.mode.empty()) a.push_back("--mode=" + r.mode);
  if (!r.source.empty()) a.push_back("--source=" + r.source);
  a.push_back("--resolution=" + std::to_string(r.resolution));
  if (r.w > 0 && r.h > 0) {
    char b[64];
    snprintf(b, sizeof b, "%.1f", r.x);
    a.push_back(std::string("-l") + b);
    snprintf(b, sizeof b, "%.1f", r.y);
    a.push_back(std::string("-t") + b);
    snprintf(b, sizeof b, "%.1f", r.w);
    a.push_back(std::string("-x") + b);
    snprintf(b, sizeof b, "%.1f", r.h);
    a.push_back(std::string("-y") + b);
  }
  if (r.batch) a.push_back("--batch=" + r.output);
  else a.push_back("--output-file=" + r.output);
  a.push_back("-p"); // progress on stderr
  return a;
}

int mm_to_px(double mm, int dpi) { return (int)std::lround(mm / 25.4 * dpi); }
double px_to_pt(int px, int dpi) { return dpi > 0 ? px * 72.0 / dpi : px; }

std::string error_text(const std::string &out) {
  std::string last;
  for (const std::string &line : fld::split(out, '\n')) {
    std::string t = fld::trim(line);
    if (t.empty() || fld::starts_with(t, "Progress:")) continue;
    last = t;
  }
  size_t c = last.rfind(": ");
  return c == std::string::npos ? last : last.substr(c + 2);
}

} // namespace scan
