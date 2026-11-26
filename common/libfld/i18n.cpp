#include "fld/i18n.h"

#include <clocale>
#include <cstdlib>
#include <cstring>
#include <libintl.h>
#include <vector>

#ifndef FLD_LOCALEDIR
#define FLD_LOCALEDIR "/usr/share/locale"
#endif

namespace fld {

namespace {
const char *const kDomain = "fldesktop";
bool g_done = false;
} // namespace

void i18n_init() {
  if (g_done) return;
  g_done = true;
  setlocale(LC_MESSAGES, "");
  setlocale(LC_CTYPE, "");
  // a build tree run (tools/run-dev.sh) finds the catalogs it built
  const char *dir = getenv("FLD_LOCALEDIR");
  bindtextdomain(kDomain, dir && *dir ? dir : FLD_LOCALEDIR);
  bind_textdomain_codeset(kDomain, "UTF-8");
  textdomain(kDomain);
}

const char *tr(const char *msgid) {
  if (!msgid || !*msgid) return msgid ? msgid : "";
  return dgettext(kDomain, msgid);
}

const char *tr_ctx(const char *ctx_msgid, const char *msgid) {
  const char *t = dgettext(kDomain, ctx_msgid);
  return t == ctx_msgid ? msgid : t;
}

const char *tr_plural(const char *msgid, const char *plural, unsigned long n) {
  return dngettext(kDomain, msgid, plural, n);
}

bool rtl_language() {
  // the translators say so (msgid "default:LTR" as in GTK), else by language
  const char *dir = dgettext(kDomain, "default:LTR");
  if (!strcmp(dir, "default:RTL")) return true;
  const char *l = getenv("LANGUAGE");
  if (!l || !*l) l = setlocale(LC_MESSAGES, nullptr);
  if (!l) return false;
  for (const char *rtl : {"ar", "he", "fa", "ur", "yi", "ps", "sd", "ug", "dv", "ckb"}) {
    size_t n = strlen(rtl);
    if (!strncmp(l, rtl, n) && (l[n] == 0 || l[n] == '_' || l[n] == '.' || l[n] == '@' || l[n] == ':')) return true;
  }
  return false;
}

std::string format(const char *fmt, std::initializer_list<std::string> args) {
  std::vector<std::string> a(args);
  std::string out;
  size_t next = 0;
  for (const char *p = fmt; *p; p++) {
    if (*p != '%') { out += *p; continue; }
    char c = p[1];
    if (c == '%') { out += '%'; p++; }
    else if (c == 's' || c == 'd') { if (next < a.size()) out += a[next]; next++; p++; }
    else if (c >= '1' && c <= '9') { size_t i = c - '1'; if (i < a.size()) out += a[i]; p++; }
    else out += *p;
  }
  return out;
}

} // namespace fld
