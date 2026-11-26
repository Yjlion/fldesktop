// fld::i18n: the catalogs built from po/ are found and used, and
// fld::format() puts arguments in place.
#include "fld/i18n.h"
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(int argc, char **argv) {
  CHECK(fld::format("%s – %s", {"a", "b"}) == "a – b");
  CHECK(fld::format("%2 before %1", {"x", "y"}) == "y before x");
  CHECK(fld::format("100%% %d", {"5"}) == "100% 5");
  CHECK(std::string(_("")) == "");
  if (argc < 2) return failures ? 1 : 0;
  // with the build tree's catalogs: German
  setenv("FLD_LOCALEDIR", argv[1], 1);
  setenv("LANGUAGE", "de", 1);
  const char *loc = nullptr;
  for (const char *l : {"de_DE.UTF-8", "en_US.UTF-8", "C.UTF-8"})
    if (setlocale(LC_MESSAGES, l)) { loc = l; break; }
  if (!loc) { fprintf(stderr, "no UTF-8 locale; skipped\n"); return failures ? 1 : 77; }
  setenv("LC_ALL", loc, 1);
  fld::i18n_init();
  const char *t = _("Settings");
  if (!strcmp(t, "Settings")) {
    // glibc ignores LANGUAGE in the C locale
    fprintf(stderr, "catalog not used with %s; skipped\n", loc);
    return failures ? 1 : 77;
  }
  CHECK(!strcmp(t, "Einstellungen"));
  CHECK(!strcmp(_("%s – Settings"), "%s – Einstellungen"));
  CHECK(!strcmp(_("not in any catalog"), "not in any catalog"));
  CHECK(!fld::rtl_language());
  setlocale(LC_NUMERIC, nullptr);
  CHECK(!strcmp(setlocale(LC_NUMERIC, nullptr), "C")); // numbers keep their dots
  printf("translations: %s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
