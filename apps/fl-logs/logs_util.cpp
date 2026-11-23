#include "logs_util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "fld/json.h"
#include "fld/util.h"

namespace logs {

const char *priority_name(int p) {
  static const char *n[] = {"Emergency", "Alert", "Critical", "Error", "Warning", "Notice", "Info", "Debug"};
  return p >= 0 && p <= 7 ? n[p] : "Info";
}

namespace {

std::string value_text(const fld::Json &v) {
  if (v.is_string()) return v.as_string();
  // binary fields come as arrays of byte values
  if (v.is_array()) {
    std::string s;
    for (const fld::Json &b : v.items())
      if (b.type() == fld::Json::Number) s += (char)(int)b.as_number();
    return s;
  }
  if (v.type() == fld::Json::Number) return std::to_string((long long)v.as_number());
  return "";
}

} // namespace

bool parse_journal_json(const std::string &line, Entry &e) {
  fld::Json j;
  if (!fld::Json::parse(line, j) || !j.is_object()) return false;
  for (auto &m : j.members()) e.fields[m.first] = value_text(m.second);
  auto get = [&](const char *k) {
    auto it = e.fields.find(k);
    return it == e.fields.end() ? std::string() : it->second;
  };
  std::string ts = get("__REALTIME_TIMESTAMP");
  e.time = ts.empty() ? 0 : atof(ts.c_str()) / 1e6;
  std::string p = get("PRIORITY");
  e.priority = p.empty() ? INFO : std::max(0, std::min(7, atoi(p.c_str())));
  e.source = get("SYSLOG_IDENTIFIER");
  if (e.source.empty()) e.source = get("_COMM");
  e.unit = get("_SYSTEMD_UNIT");
  if (e.unit.empty()) e.unit = get("_SYSTEMD_USER_UNIT");
  if (e.source.empty()) e.source = e.unit;
  if (get("_TRANSPORT") == "kernel" && e.source.empty()) e.source = "kernel";
  e.pid = atoi(get("_PID").c_str());
  e.host = get("_HOSTNAME");
  e.message = get("MESSAGE");
  return true;
}

int guess_priority(const std::string &text, int def) {
  std::string l = fld::lower(text);
  auto has = [&](const char *w) {
    size_t p = l.find(w);
    while (p != std::string::npos) {
      bool start = p == 0 || !isalpha((unsigned char)l[p - 1]);
      if (start) return true;
      p = l.find(w, p + 1);
    }
    return false;
  };
  if (has("panic") || has("emerg")) return EMERG;
  if (has("critical") || has("crit:") || has("fatal")) return CRIT;
  if (has("error") || has("failed") || has("failure") || has("segfault")) return ERR;
  if (has("warning") || has("warn")) return WARNING;
  return def;
}

Entry parse_syslog_line(const std::string &line, int year) {
  Entry e;
  e.message = line;
  std::string rest;
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  struct tm tm {};
  bool ok = false;
  if (line.size() > 16 && isalpha((unsigned char)line[0]) && line[3] == ' ') {
    for (int m = 0; m < 12; m++)
      if (line.compare(0, 3, months[m]) == 0) {
        int d, H, M, S;
        if (sscanf(line.c_str() + 4, "%d %d:%d:%d", &d, &H, &M, &S) == 4) {
          tm.tm_year = year - 1900;
          tm.tm_mon = m;
          tm.tm_mday = d;
          tm.tm_hour = H;
          tm.tm_min = M;
          tm.tm_sec = S;
          tm.tm_isdst = -1;
          e.time = (double)mktime(&tm);
          rest = line.substr(15);
          ok = true;
        }
      }
  } else if (line.size() > 19 && isdigit((unsigned char)line[0]) && line[4] == '-' && line[10] == 'T') {
    int Y, mo, d, H, M;
    double S;
    if (sscanf(line.c_str(), "%d-%d-%dT%d:%d:%lf", &Y, &mo, &d, &H, &M, &S) == 6) {
      tm.tm_year = Y - 1900;
      tm.tm_mon = mo - 1;
      tm.tm_mday = d;
      tm.tm_hour = H;
      tm.tm_min = M;
      tm.tm_sec = (int)S;
      tm.tm_isdst = -1;
      e.time = (double)mktime(&tm) + (S - (int)S);
      size_t sp = line.find(' ');
      rest = sp == std::string::npos ? "" : line.substr(sp);
      ok = true;
    }
  }
  if (ok) {
    // " host prog[pid]: message"
    rest = fld::trim(rest);
    size_t sp = rest.find(' ');
    std::string host = sp == std::string::npos ? "" : rest.substr(0, sp);
    std::string tail = sp == std::string::npos ? rest : rest.substr(sp + 1);
    size_t colon = tail.find(": ");
    std::string tag = colon == std::string::npos ? "" : tail.substr(0, colon);
    if (!tag.empty() && tag.find(' ') == std::string::npos) {
      e.host = host;
      size_t br = tag.find('[');
      e.source = br == std::string::npos ? tag : tag.substr(0, br);
      if (br != std::string::npos) e.pid = atoi(tag.c_str() + br + 1);
      e.message = tail.substr(colon + 2);
    } else {
      e.host = host;
      e.message = tail;
    }
  }
  e.priority = guess_priority(e.message);
  return e;
}

bool matches(const Entry &e, const Filter &f) {
  if (e.priority > f.max_priority) return false;
  if (!f.source.empty() && e.source != f.source && e.unit != f.source) return false;
  if (!f.text.empty()) {
    std::string q = fld::lower(f.text);
    if (fld::lower(e.message).find(q) == std::string::npos && fld::lower(e.source).find(q) == std::string::npos &&
        fld::lower(e.unit).find(q) == std::string::npos)
      return false;
  }
  return true;
}

std::string format_time(double t, time_t now) {
  if (t <= 0) return "";
  time_t tt = (time_t)t;
  struct tm a, b;
  localtime_r(&tt, &a);
  localtime_r(&now, &b);
  char buf[64];
  if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) strftime(buf, sizeof buf, "%H:%M:%S", &a);
  else if (a.tm_year == b.tm_year) strftime(buf, sizeof buf, "%b %d %H:%M:%S", &a);
  else strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &a);
  return buf;
}

std::vector<std::string> log_files(const std::string &dir) {
  std::vector<std::string> out;
  for (const std::string &n : fld::list_dir(dir)) {
    std::string p = dir + "/" + n;
    if (fld::is_dir(p)) continue;
    // rotated and compressed copies
    if (fld::ends_with(n, ".gz") || fld::ends_with(n, ".xz") || fld::ends_with(n, ".bz2") || fld::ends_with(n, ".zst") ||
        fld::ends_with(n, ".old"))
      continue;
    size_t dot = n.rfind('.');
    if (dot != std::string::npos && n.find_first_not_of("0123456789", dot + 1) == std::string::npos) continue;
    // binary logs
    if (n == "wtmp" || n == "btmp" || n == "lastlog" || n == "faillog") continue;
    bool known = n == "syslog" || n == "messages" || n == "kern.log" || n == "auth.log" || n == "daemon.log" ||
                 n == "user.log" || n == "dmesg" || n == "debug" || n == "secure" || fld::ends_with(n, ".log");
    if (known) out.push_back(p);
  }
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace logs
