#include "launcher_providers.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#include "calc_engine.h"
#include "fld/util.h"

namespace launcher {

static bool has_digit(const std::string &s) {
  for (char c : s)
    if (std::isdigit((unsigned char)c)) return true;
  return false;
}

bool calculate(const std::string &query, Extra &out) {
  std::string q = fld::trim(query);
  if (!q.empty() && q[0] == '=') q = fld::trim(q.substr(1));
  if (q.empty() || !has_digit(q)) return false;
  // a plain number is not a calculation
  bool op = false;
  for (size_t i = 0; i < q.size(); i++) {
    char c = q[i];
    if (std::strchr("+-*/^%!()×÷−", c) && !(i == 0 && (c == '-' || c == '+'))) op = true;
    if (std::isalpha((unsigned char)c)) op = true; // sqrt(2), 2pi
  }
  if ((unsigned char)q[0] >= 0x80) op = true; // √
  if (!op) return false;
  calc::Result r = calc::evaluate(q);
  if (!r.ok || !std::isfinite(r.value)) return false;
  out = Extra{Extra::Calc, "= " + calc::format(r.value), q, "accessories-calculator", calc::format(r.value), {}};
  return true;
}

namespace {
struct Unit {
  const char *dim;
  double factor; // to the base unit
  double offset; // temperature: base = value * factor + offset
};

const std::map<std::string, Unit> &units() {
  static const std::map<std::string, Unit> u = {
      // length (m)
      {"mm", {"length", 0.001, 0}}, {"cm", {"length", 0.01, 0}}, {"m", {"length", 1, 0}},
      {"km", {"length", 1000, 0}}, {"in", {"length", 0.0254, 0}}, {"inch", {"length", 0.0254, 0}},
      {"ft", {"length", 0.3048, 0}}, {"foot", {"length", 0.3048, 0}}, {"feet", {"length", 0.3048, 0}},
      {"yd", {"length", 0.9144, 0}}, {"mi", {"length", 1609.344, 0}}, {"mile", {"length", 1609.344, 0}},
      {"miles", {"length", 1609.344, 0}}, {"nmi", {"length", 1852, 0}},
      // mass (kg)
      {"mg", {"mass", 1e-6, 0}}, {"g", {"mass", 0.001, 0}}, {"kg", {"mass", 1, 0}}, {"t", {"mass", 1000, 0}},
      {"oz", {"mass", 0.028349523125, 0}}, {"lb", {"mass", 0.45359237, 0}}, {"lbs", {"mass", 0.45359237, 0}},
      {"st", {"mass", 6.35029318, 0}},
      // volume (l)
      {"ml", {"volume", 0.001, 0}}, {"cl", {"volume", 0.01, 0}}, {"l", {"volume", 1, 0}},
      {"floz", {"volume", 0.0295735295625, 0}}, {"cup", {"volume", 0.2365882365, 0}},
      {"pt", {"volume", 0.473176473, 0}}, {"qt", {"volume", 0.946352946, 0}}, {"gal", {"volume", 3.785411784, 0}},
      // temperature (K)
      {"c", {"temperature", 1, 273.15}}, {"°c", {"temperature", 1, 273.15}},
      {"f", {"temperature", 5.0 / 9, 273.15 - 32 * 5.0 / 9}}, {"°f", {"temperature", 5.0 / 9, 273.15 - 32 * 5.0 / 9}},
      {"k", {"temperature", 1, 0}},
      // data (bytes)
      {"b", {"data", 1, 0}}, {"kb", {"data", 1e3, 0}}, {"mb", {"data", 1e6, 0}}, {"gb", {"data", 1e9, 0}},
      {"tb", {"data", 1e12, 0}}, {"kib", {"data", 1024, 0}}, {"mib", {"data", 1048576, 0}},
      {"gib", {"data", 1073741824, 0}}, {"tib", {"data", 1099511627776.0, 0}},
      // time (s)
      {"ms", {"time", 0.001, 0}}, {"s", {"time", 1, 0}}, {"sec", {"time", 1, 0}}, {"min", {"time", 60, 0}},
      {"h", {"time", 3600, 0}}, {"hr", {"time", 3600, 0}}, {"day", {"time", 86400, 0}},
      {"days", {"time", 86400, 0}}, {"week", {"time", 604800, 0}}, {"weeks", {"time", 604800, 0}},
      // speed (m/s)
      {"m/s", {"speed", 1, 0}}, {"km/h", {"speed", 1 / 3.6, 0}}, {"kmh", {"speed", 1 / 3.6, 0}},
      {"mph", {"speed", 0.44704, 0}}, {"kn", {"speed", 0.514444, 0}}, {"knots", {"speed", 0.514444, 0}},
  };
  return u;
}
} // namespace

bool convert(const std::string &query, Extra &out) {
  std::istringstream in(fld::lower(fld::trim(query)));
  std::string num, from, word, to;
  if (!(in >> num >> from >> word >> to) || (word != "in" && word != "to")) return false;
  std::string rest;
  if (in >> rest) return false;
  char *end;
  double v = std::strtod(num.c_str(), &end);
  if (*end || !std::isfinite(v)) return false;
  auto &u = units();
  auto a = u.find(from), b = u.find(to);
  if (a == u.end() || b == u.end() || std::string(a->second.dim) != b->second.dim) return false;
  double base = v * a->second.factor + a->second.offset;
  double r = (base - b->second.offset) / b->second.factor;
  std::string text = calc::format(r);
  out = Extra{Extra::Convert, "= " + text + " " + to, num + " " + from + " " + word + " " + to,
              "accessories-calculator", text, {}};
  return true;
}

bool command(const std::string &query, const std::vector<std::string> &terminal, Extra &out) {
  std::string q = fld::trim(query);
  if (q.size() < 2 || q[0] != '>') return false;
  std::string cmd = fld::trim(q.substr(1));
  if (cmd.empty()) return false;
  std::vector<std::string> argv = terminal;
  // the terminal runs it through the shell and keeps the window open after it
  argv.insert(argv.end(), {"sh", "-c", cmd + "; echo; printf '[%s]' \"$?\"; read -r _"});
  out = Extra{Extra::Command, cmd, "", "utilities-terminal", "", argv};
  return true;
}

std::vector<std::string> locate_command(const std::string &query, const std::string &locate_path) {
  std::string q = fld::trim(query);
  if (locate_path.empty() || q.size() < 3 || q[0] == '>' || q[0] == '-') return {};
  return {locate_path, "-i", "-b", "-l", "40", "--", q};
}

std::vector<Extra> files_from_locate(const std::string &output, size_t max) {
  std::vector<Extra> out;
  std::string home = fld::home_dir();
  for (auto &line : fld::split(output, '\n')) {
    std::string p = fld::trim(line);
    if (p.empty() || p[0] != '/') continue;
    // the user's files first: leave out system and hidden paths
    if (!home.empty() && !fld::starts_with(p, home + "/")) continue;
    if (p.find("/.") != std::string::npos) continue;
    std::string shown = "~" + p.substr(home.size());
    out.push_back(Extra{Extra::File, fld::basename(p), fld::dirname(shown), fld::is_dir(p) ? "folder" : "text-x-generic",
                        "", {"xdg-open", p}});
    if (out.size() >= max) break;
  }
  return out;
}

std::vector<Extra> quick(const std::string &query, const std::vector<std::string> &terminal) {
  std::vector<Extra> out;
  Extra e;
  if (command(query, terminal, e)) out.push_back(e);
  else if (convert(query, e)) out.push_back(e);
  else if (calculate(query, e)) out.push_back(e);
  return out;
}

} // namespace launcher
