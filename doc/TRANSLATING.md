# Translating fldesktop

All programs share one gettext domain, `fldesktop`. The template is `po/fldesktop.pot`, the catalogs
are `po/<lang>.po`, and `po/LINGUAS` lists the languages that are built and installed
(`<localedir>/<lang>/LC_MESSAGES/fldesktop.mo`).

## Adding or updating a language

```sh
tools/update-po.py                       # refresh POTFILES.in, the template and every .po
msginit -i po/fldesktop.pot -o po/fr.po -l fr_FR.UTF-8 --no-translator   # a new language
echo fr >> po/LINGUAS
cmake --build build                      # compiles the catalogs into build/locale
LANGUAGE=fr FLD_LOCALEDIR=build/locale build/bin/fl-settings
```

Settings > Language & Region sets `LANG` for the next login. Strings that are not translated yet show
in English.

Notes for translators:
- `Default|Large|Larger` is a list of choices: keep the `|` between them and the same number of items.
- `&` marks the shortcut letter of a menu item; `@` starts an FLTK symbol (`@-3>`): keep those.
- `%s`, `%d` are filled in by the program. Where the word order has to change, use `%1`, `%2`, … in
  the translation (`fld::format()` accepts both).
- Plural forms come from the catalog's `Plural-Forms` header.

## Marking text in the code

```cpp
#include "fld/i18n.h"
button->label(_("Apply"));
static const char *kinds[] = {N_("Folder"), N_("File")};   // translated later: _(kinds[i])
C_("verb", "Open");                                         // a context for the translators
fld::format(P_("%d file", "%d files", n), {std::to_string(n)});
```

`fld::init_app()` sets up translations (`fld::i18n_init()`); a program that shows text before calling it
calls `fld::i18n_init()` first. Only `LC_MESSAGES` and `LC_CTYPE` follow the user's locale: numbers in
configuration files keep their dots.

`tools/i18n-mark.py <file.cpp>…` wraps the obvious user-visible literals (widget labels, tooltips, menu
items, dialogs, the Settings form helpers) in `_()`. Review its diff: text that the program also compares
(`w->label() == "…"`) must use `_()` on both sides, and text built with `+` needs `fld::format()`.

ctest's `translation-template` test fails when `po/fldesktop.pot` or `po/POTFILES.in` is out of date: run
`tools/update-po.py` and commit the result.

So far libfld, the shell (panel, launcher, desktop) and fl-settings are marked; the other programs follow.

## Checking layouts

```sh
tools/update-po.py --pseudo build/locale
LANGUAGE=en@pseudo FLD_LOCALEDIR=build/locale build/bin/fl-settings
```

The pseudo language makes every text about 40 % longer and accented (`[Šéttîñgš~~~~]`), which shows
labels that are cut off and text that was not marked.

Right-to-left languages: `fld::rtl_language()` says when one is in use (from the catalog's
`default:RTL` entry or the language code). FLTK 1.4 has no bidirectional text layout, so Arabic or Hebrew
text shows in logical order and widgets are not mirrored. Mirroring the layouts needs support in FLTK
or in libfld's widgets first.
