#include "fld/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fld {

const Json &Json::operator[](const std::string &key) const {
  static const Json null;
  auto it = o_.find(key);
  return it == o_.end() ? null : it->second;
}

Json &Json::set(const std::string &key, const Json &v) {
  type_ = Object;
  o_[key] = v;
  return *this;
}

Json &Json::push(const Json &v) {
  type_ = Array;
  a_.push_back(v);
  return *this;
}

static void escape(const std::string &s, std::string &out) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else out += (char)c;
    }
  }
  out += '"';
}

void Json::dump_to(std::string &out) const {
  switch (type_) {
    case Null: out += "null"; break;
    case Bool: out += b_ ? "true" : "false"; break;
    case Number: {
      char buf[32];
      if (std::floor(n_) == n_ && std::fabs(n_) < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)n_);
      else snprintf(buf, sizeof buf, "%.17g", n_);
      out += buf;
      break;
    }
    case String: escape(s_, out); break;
    case Array:
      out += '[';
      for (size_t i = 0; i < a_.size(); i++) {
        if (i) out += ',';
        a_[i].dump_to(out);
      }
      out += ']';
      break;
    case Object: {
      out += '{';
      bool first = true;
      for (auto &kv : o_) {
        if (!first) out += ',';
        first = false;
        escape(kv.first, out);
        out += ':';
        kv.second.dump_to(out);
      }
      out += '}';
      break;
    }
  }
}

std::string Json::dump() const {
  std::string s;
  dump_to(s);
  return s;
}

namespace {

struct Parser {
  const char *p, *end;
  std::string err;

  void ws() { while (p < end && strchr(" \t\r\n", *p)) p++; }
  bool fail(const char *m) { if (err.empty()) err = m; return false; }

  static void utf8(unsigned cp, std::string &o) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
      o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    } else {
      o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
      o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(unsigned &v) {
    if (end - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; i++) {
      char c = *p++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    return true;
  }

  bool str(std::string &o) {
    if (p >= end || *p != '"') return fail("expected string");
    p++;
    while (p < end && *p != '"') {
      if (*p == '\\') {
        p++;
        if (p >= end) return fail("bad escape");
        char c = *p++;
        switch (c) {
          case '"': o += '"'; break;
          case '\\': o += '\\'; break;
          case '/': o += '/'; break;
          case 'b': o += '\b'; break;
          case 'f': o += '\f'; break;
          case 'n': o += '\n'; break;
          case 'r': o += '\r'; break;
          case 't': o += '\t'; break;
          case 'u': {
            unsigned cp;
            if (!hex4(cp)) return fail("bad \\u escape");
            if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
              p += 2;
              unsigned lo;
              if (!hex4(lo)) return fail("bad surrogate");
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            utf8(cp, o);
            break;
          }
          default: return fail("bad escape");
        }
      } else o += *p++;
    }
    if (p >= end) return fail("unterminated string");
    p++;
    return true;
  }

  bool value(Json &out, int depth) {
    if (depth > 64) return fail("nesting too deep");
    ws();
    if (p >= end) return fail("unexpected end");
    if (*p == '{') {
      p++;
      out = Json::object();
      ws();
      if (p < end && *p == '}') { p++; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!str(k)) return false;
        ws();
        if (p >= end || *p != ':') return fail("expected ':'");
        p++;
        Json v;
        if (!value(v, depth + 1)) return false;
        out.set(k, v);
        ws();
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == '}') { p++; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (*p == '[') {
      p++;
      out = Json::array();
      ws();
      if (p < end && *p == ']') { p++; return true; }
      for (;;) {
        Json v;
        if (!value(v, depth + 1)) return false;
        out.push(v);
        ws();
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == ']') { p++; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (*p == '"') {
      std::string s;
      if (!str(s)) return false;
      out = Json(s);
      return true;
    }
    if (end - p >= 4 && !strncmp(p, "true", 4)) { p += 4; out = Json(true); return true; }
    if (end - p >= 5 && !strncmp(p, "false", 5)) { p += 5; out = Json(false); return true; }
    if (end - p >= 4 && !strncmp(p, "null", 4)) { p += 4; out = Json(); return true; }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
      std::string num;
      while (p < end && strchr("+-0123456789.eE", *p)) num += *p++;
      char *e;
      double d = strtod(num.c_str(), &e);
      if (*e) return fail("bad number");
      out = Json(d);
      return true;
    }
    return fail("unexpected character");
  }
};

} // namespace

bool Json::parse(const std::string &text, Json &out, std::string *err) {
  Parser ps{text.data(), text.data() + text.size(), {}};
  bool ok = ps.value(out, 0);
  if (ok) {
    ps.ws();
    if (ps.p != ps.end) ok = ps.fail("trailing data");
  }
  if (!ok && err) *err = ps.err;
  return ok;
}

} // namespace fld
