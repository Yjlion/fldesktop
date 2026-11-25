// Expression evaluator for fl-calc (no FLTK dependency, unit tested)
#pragma once
#include <string>

namespace calc {

struct Result {
  bool ok = false;
  double value = 0;
  std::string error;
};

struct Options {
  bool degrees = false; // trigonometric functions use degrees
  double ans = 0;       // value of "ans"
};

// Evaluates an expression such as "2(3+4)^2 - sqrt(16) + 5% + sin(pi/2) + 3!".
// Accepts × ÷ − as operators and implicit multiplication.
Result evaluate(const std::string &expr, const Options &opt = {});
// A function key that works on what is already there (√ after "9" gives
// "√(9)", after "(2+7)" gives "√((2+7))"): wraps the number, constant or
// bracket at the end of expr in fn( ). With nothing to wrap it appends "fn(".
std::string apply_function(const std::string &expr, const std::string &fn);
// Formats a number for display (up to 12 significant digits, no trailing zeros).
std::string format(double v);

} // namespace calc
