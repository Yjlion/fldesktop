#include "assuan.h"

#include <cctype>
#include <cstdio>
#include <set>
#include <unistd.h>

namespace assuan {

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string unescape(const std::string &s) {
  std::string r;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int a = hexval(s[i + 1]), b = hexval(s[i + 2]);
      if (a >= 0 && b >= 0) {
        r += (char)(a * 16 + b);
        i += 2;
        continue;
      }
    }
    r += s[i];
  }
  return r;
}

std::string escape(const std::string &s) {
  std::string r;
  for (unsigned char c : s) {
    if (c == '%' || c == '\r' || c == '\n' || c < 0x20) {
      char b[4];
      snprintf(b, sizeof b, "%%%02X", c);
      r += b;
    } else {
      r += (char)c;
    }
  }
  return r;
}

static std::string err(unsigned code, const std::string &text) {
  return "ERR " + std::to_string(code) + " " + text + " <Pinentry>";
}

std::vector<std::string> Session::handle(const std::string &raw) {
  std::string line = raw;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (line.empty() || line[0] == '#') return {};
  size_t sp = line.find(' ');
  std::string cmd = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
  for (char &c : cmd) c = (char)toupper((unsigned char)c);
  std::string val = unescape(arg);

  // the texts of the dialog
  static const std::set<std::string> setters = {"SETDESC", "SETPROMPT", "SETTITLE", "SETOK", "SETCANCEL",
                                                "SETNOTOK", "SETERROR", "SETREPEAT", "SETREPEATERROR",
                                                "SETREPEATOK", "SETQUALITYBAR", "SETQUALITYBAR_TT", "SETGENPIN",
                                                "SETGENPIN_TT", "SETKEYINFO", "SETTIMEOUT"};
  if (setters.count(cmd)) {
    if (cmd == "SETDESC") req_.description = val;
    else if (cmd == "SETPROMPT") req_.prompt = val;
    else if (cmd == "SETTITLE") req_.title = val;
    else if (cmd == "SETOK") req_.ok = val;
    else if (cmd == "SETCANCEL") req_.cancel = val;
    else if (cmd == "SETNOTOK") req_.notok = val;
    else if (cmd == "SETERROR") req_.error = val;
    else if (cmd == "SETREPEAT") req_.repeat = val.empty() ? "Repeat:" : val;
    else if (cmd == "SETREPEATERROR") req_.repeat_error = val;
    else if (cmd == "SETREPEATOK") req_.repeat_ok = val;
    else if (cmd == "SETQUALITYBAR") req_.quality_bar = val.empty() ? "Quality:" : val;
    else if (cmd == "SETQUALITYBAR_TT") req_.quality_tooltip = val;
    else if (cmd == "SETGENPIN") req_.genpin = val.empty() ? "Suggest" : val;
    else if (cmd == "SETGENPIN_TT") req_.genpin_tooltip = val;
    else if (cmd == "SETKEYINFO") req_.keyinfo = val == "--clear" ? "" : val;
    else if (cmd == "SETTIMEOUT") req_.timeout = atoi(val.c_str());
    return {"OK"};
  }
  if (cmd == "OPTION") {
    std::string k = arg, v;
    size_t eq = arg.find_first_of("= ");
    if (eq != std::string::npos) {
      k = arg.substr(0, eq);
      v = unescape(arg.substr(eq + 1));
    }
    if (k.size() > 2 && k.compare(0, 2, "--") == 0) k = k.substr(2);
    req_.options[k] = v;
    if (k == "allow-external-password-cache") req_.allow_external_cache = true;
    return {"OK"};
  }
  if (cmd == "GETINFO") {
    if (arg == "flavor") return {"D fldesktop", "OK"};
    if (arg == "version") return {"D 1.0.0", "OK"};
    if (arg == "pid") return {"D " + std::to_string(getpid()), "OK"};
    if (arg == "ttyinfo") return {"D - - - - " + std::to_string(getuid()) + "/" + std::to_string(getgid()) + " -", "OK"};
    return {err(kParameter, "Invalid parameter")};
  }
  if (cmd == "GETPIN") {
    if (!ui_.get_pin) return {err(kCanceled, "Operation cancelled")};
    Answer a = ui_.get_pin(req_);
    req_.error.clear(); // shown once
    if (a.kind == Answer::Timeout) return {err(kTimeout, "Timeout")};
    if (a.kind != Answer::Ok) return {err(kCanceled, "Operation cancelled")};
    std::vector<std::string> out;
    if (a.from_cache) out.push_back("S PASSWORD_FROM_CACHE");
    if (a.repeated) out.push_back("S PIN_REPEATED");
    if (!a.pin.empty()) out.push_back("D " + escape(a.pin));
    out.push_back("OK");
    return out;
  }
  if (cmd == "CONFIRM" || cmd == "MESSAGE") {
    Request r = req_;
    r.one_button = cmd == "MESSAGE" || arg == "--one-button";
    if (!ui_.confirm) return {err(kCanceled, "Operation cancelled")};
    Answer a = ui_.confirm(r);
    req_.error.clear();
    if (a.kind == Answer::Ok) return {"OK"};
    if (a.kind == Answer::Timeout) return {err(kTimeout, "Timeout")};
    if (a.kind == Answer::NotOk) return {err(kNotConfirmed, "Not confirmed")};
    return {err(kCanceled, "Operation cancelled")};
  }
  if (cmd == "RESET") {
    req_ = Request();
    return {"OK"};
  }
  if (cmd == "BYE") {
    bye_ = true;
    return {"OK closing connection"};
  }
  if (cmd == "NOP" || cmd == "CANCEL" || cmd == "AUTH" || cmd == "CLEARPASSPHRASE") return {"OK"};
  return {err(kUnknownCommand, "Unknown IPC command")};
}

int quality(const std::string &pin) {
  if (pin.empty()) return 0;
  bool lower = false, upper = false, digit = false, other = false;
  std::set<char> distinct;
  for (unsigned char c : pin) {
    distinct.insert((char)c);
    if (islower(c)) lower = true;
    else if (isupper(c)) upper = true;
    else if (isdigit(c)) digit = true;
    else other = true;
  }
  int pool = (lower ? 26 : 0) + (upper ? 26 : 0) + (digit ? 10 : 0) + (other ? 33 : 0);
  // bits of entropy, as if random; repeats count less
  double bits = 0, per = 0;
  for (int p = pool; p > 1; p /= 2) per += 1;
  bits = per * (double)distinct.size() + 1.0 * (double)(pin.size() - distinct.size());
  int q = (int)(bits * 100 / 80); // 80 bits: a strong passphrase
  return q > 100 ? 100 : q;
}

} // namespace assuan
