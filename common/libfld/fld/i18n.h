// Translations (gettext). All of fldesktop shares one text domain,
// "fldesktop", with the catalogs in po/. fld::init_app() calls i18n_init().
//
//   _("Text")              translated text (a pointer that stays valid)
//   N_("Text")             marks text for translation, translated later with _()
//   C_("context", "Text")  translated text with a context for the translators
//   P_("%d file", "%d files", n)   plural forms
#pragma once
#include <string>

#define _(s) ::fld::tr(s)
#define N_(s) s
#define C_(ctx, s) ::fld::tr_ctx(ctx "\004" s, s)
#define P_(s, p, n) ::fld::tr_plural(s, p, n)

namespace fld {

// Binds the text domain and sets LC_MESSAGES and LC_CTYPE from the
// environment. LC_NUMERIC stays "C" so numbers in config files keep their
// dots. Safe to call more than once.
void i18n_init();
const char *tr(const char *msgid);
const char *tr_ctx(const char *ctx_msgid, const char *msgid);
const char *tr_plural(const char *msgid, const char *plural, unsigned long n);
// Whether the language reads right to left (Arabic, Hebrew, Persian, Urdu, ...)
bool rtl_language();
// The text with each %s / %d / %1... replaced in order by args (for
// translations that need a different word order use %1, %2, ...).
std::string format(const char *fmt, std::initializer_list<std::string> args);

} // namespace fld
