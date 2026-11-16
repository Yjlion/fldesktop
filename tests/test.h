// Minimal test helpers
#pragma once
#include <cmath>
#include <cstdio>
#include <string>

static int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                                \
  do {                                                                             \
    g_checks++;                                                                    \
    if (!(cond)) {                                                                 \
      g_failures++;                                                                \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                             \
  do {                                                                             \
    g_checks++;                                                                    \
    auto _a = (a);                                                                 \
    auto _b = (b);                                                                 \
    if (!(_a == _b)) {                                                             \
      g_failures++;                                                                \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s != %s\n", __FILE__, __LINE__, #a, #b); \
    }                                                                              \
  } while (0)

#define CHECK_NEAR(a, b)                                                           \
  do {                                                                             \
    g_checks++;                                                                    \
    double _a = (a), _b = (b);                                                     \
    if (std::fabs(_a - _b) > 1e-9 * (1 + std::fabs(_b))) {                         \
      g_failures++;                                                                \
      fprintf(stderr, "%s:%d: %s = %.15g, expected %.15g\n", __FILE__, __LINE__, #a, _a, _b); \
    }                                                                              \
  } while (0)

static int test_result(const char *name) {
  printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
  return g_failures ? 1 : 0;
}
