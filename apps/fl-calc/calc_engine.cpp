#include "calc_engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace calc {

namespace {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Parser {
public:
  Parser(const std::string &s, const Options &o) : src_(normalize(s)), p_(0), opt_(o) {}

  double parse() {
    double v = expr();
    skip();
    if (p_ < src_.size()) throw Error(std::string("unexpected '") + src_[p_] + "'");
    return v;
  }

private:
  std::string src_;
  size_t p_;
  Options opt_;

  // Replace the UTF-8 operator glyphs with ASCII.
  static std::string normalize(const std::string &in) {
    std::string s = in;
    auto rep = [&](const char *from, const char *to) {
      size_t pos;
      while ((pos = s.find(from)) != std::string::npos) s.replace(pos, strlen(from), to);
    };
    rep("×", "*");
    rep("÷", "/");
    rep("−", "-");
    rep("π", "pi");
    // a space after the name: "√9" is sqrt of 9, not a name "sqrt9"
    rep("√", "sqrt ");
    rep("∛", "cbrt ");
    rep("²", "^2");
    return s;
  }

  void skip() {
    while (p_ < src_.size() && isspace((unsigned char)src_[p_])) p_++;
  }
  bool eat(char c) {
    skip();
    if (p_ < src_.size() && src_[p_] == c) {
      p_++;
      return true;
    }
    return false;
  }
  bool at_end() {
    skip();
    return p_ >= src_.size();
  }
  // Can the next token start a factor (for implicit multiplication)?
  bool starts_factor() {
    skip();
    if (p_ >= src_.size()) return false;
    char c = src_[p_];
    return c == '(' || isalpha((unsigned char)c) || isdigit((unsigned char)c) || c == '.';
  }

  double expr() {
    double v = term();
    for (;;) {
      if (eat('+')) v += term();
      else if (eat('-')) v -= term();
      else return v;
    }
  }

  double term() {
    double v = power();
    for (;;) {
      if (eat('*')) v *= power();
      else if (eat('/')) {
        double d = power();
        if (d == 0) throw Error("division by zero");
        v /= d;
      } else if (starts_factor()) {
        v *= power(); // implicit multiplication: 2pi, 3(4), (1)(2)
      } else return v;
    }
  }

  double power() {
    double base = unary();
    if (eat('^')) {
      double e = power_rhs();
      double r = std::pow(base, e);
      if (std::isnan(r)) throw Error("invalid power");
      return r;
    }
    return base;
  }
  // right-associative and allows a sign: 2^-3, 2^3^2
  double power_rhs() {
    if (eat('-')) return -power_rhs();
    if (eat('+')) return power_rhs();
    double b = postfix(primary());
    if (eat('^')) return std::pow(b, power_rhs());
    return b;
  }

  double unary() {
    if (eat('-')) return -power();
    if (eat('+')) return power();
    return postfix(primary());
  }

  double postfix(double v) {
    for (;;) {
      if (eat('!')) v = factorial(v);
      else if (eat('%')) v /= 100.0;
      else return v;
    }
  }

  static double factorial(double v) {
    if (v < 0 || std::floor(v) != v) throw Error("factorial needs a whole number >= 0");
    if (v > 170) throw Error("number too large");
    double r = 1;
    for (int i = 2; i <= (int)v; i++) r *= i;
    return r;
  }

  double number() {
    skip();
    size_t start = p_;
    while (p_ < src_.size() && (isdigit((unsigned char)src_[p_]) || src_[p_] == '.')) p_++;
    if (p_ < src_.size() && (src_[p_] == 'e' || src_[p_] == 'E')) {
      // exponent only if followed by digits (so "2e" stays 2*e)
      size_t q = p_ + 1;
      if (q < src_.size() && (src_[q] == '+' || src_[q] == '-')) q++;
      if (q < src_.size() && isdigit((unsigned char)src_[q])) {
        p_ = q;
        while (p_ < src_.size() && isdigit((unsigned char)src_[p_])) p_++;
      }
    }
    std::string tok = src_.substr(start, p_ - start);
    if (std::count(tok.begin(), tok.end(), '.') > 1) throw Error("malformed number");
    char *end;
    double v = strtod(tok.c_str(), &end);
    if (*end) throw Error("malformed number");
    return v;
  }

  double to_rad(double x) const { return opt_.degrees ? x * M_PI / 180.0 : x; }
  double from_rad(double x) const { return opt_.degrees ? x * 180.0 / M_PI : x; }

  double call(const std::string &f, double a) {
    if (f == "sin") return clean(std::sin(to_rad(a)));
    if (f == "cos") return clean(std::cos(to_rad(a)));
    if (f == "tan") {
      double c = std::cos(to_rad(a));
      if (std::fabs(c) < 1e-15) throw Error("tan undefined");
      return clean(std::tan(to_rad(a)));
    }
    if (f == "asin" || f == "acos") {
      if (a < -1 || a > 1) throw Error(f + " needs -1..1");
      return from_rad(f == "asin" ? std::asin(a) : std::acos(a));
    }
    if (f == "atan") return from_rad(std::atan(a));
    if (f == "sinh") return std::sinh(a);
    if (f == "cosh") return std::cosh(a);
    if (f == "tanh") return std::tanh(a);
    if (f == "sqrt") {
      if (a < 0) throw Error("square root of a negative number");
      return std::sqrt(a);
    }
    if (f == "cbrt") return std::cbrt(a);
    if (f == "ln" || f == "log" || f == "log2") {
      if (a <= 0) throw Error("logarithm of a non-positive number");
      return f == "ln" ? std::log(a) : f == "log" ? std::log10(a) : std::log2(a);
    }
    if (f == "exp") return std::exp(a);
    if (f == "abs") return std::fabs(a);
    if (f == "round") return std::round(a);
    if (f == "floor") return std::floor(a);
    if (f == "ceil") return std::ceil(a);
    throw Error("unknown function " + f);
  }

  // sin(pi) should be 0, not 1.2e-16
  static double clean(double v) { return std::fabs(v) < 1e-14 ? 0.0 : v; }

  double primary() {
    skip();
    if (p_ >= src_.size()) throw Error("incomplete expression");
    char c = src_[p_];
    if (c == '(') {
      p_++;
      double v = expr();
      if (!eat(')')) {
        if (at_end()) return v; // forgive missing closing parentheses
        throw Error("missing ')'");
      }
      return v;
    }
    if (isdigit((unsigned char)c) || c == '.') return number();
    if (isalpha((unsigned char)c)) {
      size_t start = p_;
      while (p_ < src_.size() && (isalnum((unsigned char)src_[p_]) || src_[p_] == '_')) p_++;
      std::string id = src_.substr(start, p_ - start);
      for (auto &ch : id) ch = (char)tolower((unsigned char)ch);
      if (id == "pi") return M_PI;
      if (id == "e") return M_E;
      if (id == "ans") return opt_.ans;
      if (id == "tau") return 2 * M_PI;
      // function: allow "sin(x)" and "sin x"
      double arg;
      if (eat('(')) {
        arg = expr();
        if (!eat(')') && !at_end()) throw Error("missing ')'");
      } else {
        arg = power();
      }
      return call(id, arg);
    }
    throw Error(std::string("unexpected '") + c + "'");
  }
};

} // namespace

Result evaluate(const std::string &expr, const Options &opt) {
  Result r;
  try {
    Parser p(expr, opt);
    r.value = p.parse();
    if (std::isinf(r.value)) throw Error("overflow");
    if (std::isnan(r.value)) throw Error("not a number");
    r.ok = true;
  } catch (const std::exception &e) {
    r.error = e.what();
  }
  return r;
}

std::string format(double v) {
  if (v == 0) return "0";
  char buf[64];
  double a = std::fabs(v);
  if (a >= 1e15 || a < 1e-9) snprintf(buf, sizeof buf, "%.10g", v);
  else snprintf(buf, sizeof buf, "%.12g", v);
  std::string s = buf;
  // "-0" -> "0"
  if (s == "-0") s = "0";
  return s;
}

std::string apply_function(const std::string &e, const std::string &fn) {
  size_t i = e.size();
  if (i == 0) return fn + "(";
  if (e[i - 1] == ')') {
    int depth = 0;
    while (i > 0) {
      char c = e[--i];
      if (c == ')') depth++;
      else if (c == '(' && --depth == 0) break;
    }
    if (depth != 0) return e + fn + "(";
    // the function name in front of the bracket (sin, √, ...) goes with it
    while (i > 0 && (isalpha((unsigned char)e[i - 1]) || ((unsigned char)e[i - 1] & 0x80))) i--;
  } else {
    unsigned char last = (unsigned char)e[i - 1];
    if (!isalnum(last) && last != '.') return e + fn + "(";
    while (i > 0 && (isalnum((unsigned char)e[i - 1]) || e[i - 1] == '.')) i--;
  }
  return e.substr(0, i) + fn + "(" + e.substr(i) + ")";
}

} // namespace calc
