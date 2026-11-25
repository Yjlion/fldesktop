#include "calc_engine.h"
#include "test.h"

static double ev(const char *s, bool deg = false) {
  calc::Options o;
  o.degrees = deg;
  o.ans = 42;
  calc::Result r = calc::evaluate(s, o);
  if (!r.ok) fprintf(stderr, "  '%s' -> error %s\n", s, r.error.c_str());
  return r.ok ? r.value : NAN;
}

static bool fails(const char *s) { return !calc::evaluate(s).ok; }

int main() {
  // √ and ∛ keys apply to what is already there
  CHECK_EQ(calc::apply_function("9", "√"), std::string("√(9)"));
  CHECK_EQ(calc::apply_function("12×3+9", "√"), std::string("12×3+√(9)"));
  CHECK_EQ(calc::apply_function("(2+7)", "√"), std::string("√((2+7))"));
  CHECK_EQ(calc::apply_function("1+sin(30)", "√"), std::string("1+√(sin(30))"));
  CHECK_EQ(calc::apply_function("", "√"), std::string("√("));
  CHECK_EQ(calc::apply_function("2+", "√"), std::string("2+√("));
  CHECK_EQ(calc::apply_function("ans", "∛"), std::string("∛(ans)"));
  CHECK_NEAR(ev("√(9)"), 3);
  CHECK_NEAR(ev("12×3+√(16)"), 40);
  CHECK_NEAR(ev("∛(27)"), 3);
  CHECK_NEAR(ev("2√(9)"), 6);
  CHECK_NEAR(ev("1+2*3"), 7);
  CHECK_NEAR(ev("(1+2)*3"), 9);
  CHECK_NEAR(ev("2^3^2"), 512);
  CHECK_NEAR(ev("-2^2"), -4);
  CHECK_NEAR(ev("2^-1"), 0.5);
  CHECK_NEAR(ev("2*-3"), -6);
  CHECK_NEAR(ev("10/4"), 2.5);
  CHECK_NEAR(ev("7−2"), 5);
  CHECK_NEAR(ev("3×4÷6"), 2);
  CHECK_NEAR(ev("2pi"), 2 * M_PI);
  CHECK_NEAR(ev("2(3+4)"), 14);
  CHECK_NEAR(ev("(1+1)(2+2)"), 8);
  CHECK_NEAR(ev("5!"), 120);
  CHECK_NEAR(ev("50%"), 0.5);
  CHECK_NEAR(ev("200*10%"), 20);
  CHECK_NEAR(ev("sqrt(16)+√9"), 7);
  CHECK_NEAR(ev("sin(90)", true), 1);
  CHECK_NEAR(ev("sin(pi)"), 0);
  CHECK_NEAR(ev("cos(0)"), 1);
  CHECK_NEAR(ev("asin(1)", true), 90);
  CHECK_NEAR(ev("ln(e)"), 1);
  CHECK_NEAR(ev("log(1000)"), 3);
  CHECK_NEAR(ev("exp(0)"), 1);
  CHECK_NEAR(ev("abs(-3)"), 3);
  CHECK_NEAR(ev("1.5e3"), 1500);
  CHECK_NEAR(ev("2e"), 2 * M_E);
  CHECK_NEAR(ev("ans+1"), 43);
  CHECK_NEAR(ev("3²"), 9);
  CHECK_NEAR(ev("(2+3"), 5); // forgiving missing ')'
  CHECK(fails("1/0"));
  CHECK(fails("sqrt(-1)"));
  CHECK(fails("1+"));
  CHECK(fails("foo(2)"));
  CHECK(fails("1..2"));
  CHECK(fails("3.5!"));
  CHECK(fails(""));
  CHECK_EQ(calc::format(0.1 + 0.2), std::string("0.3"));
  CHECK_EQ(calc::format(-0.0), std::string("0"));
  CHECK_EQ(calc::format(1e20), std::string("1e+20"));
  CHECK_EQ(calc::format(123456), std::string("123456"));
  return test_result("calc");
}
