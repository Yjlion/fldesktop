#!/bin/sh
# fl-docs --info on generated OpenDocument text, EPUB and comic book files:
# page counts and sizes, titles, comic page order and search hits.
# usage: docs-formats.sh <fl-docs>
set -u
DOCS="$1"
command -v python3 >/dev/null 2>&1 || { echo "no python3"; exit 77; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; cat "$T/out"; exit 1; }
python3 -I "$(dirname "$0")/make-docs.py" "$T" || fail "make-docs"
# a PDF-only build says so
"$DOCS" --info "$T/test.cbz" >"$T/out" 2>&1
if grep -q "built for PDF only" "$T/out"; then echo "fl-docs built for PDF only"; exit 77; fi

"$DOCS" --info "$T/test.odt" --find fldesktop >"$T/out" 2>&1 || fail "odt"
grep -qx "pages 2" "$T/out" || fail "odt: page count (the page break)"
grep -qx "title fl-docs odt test" "$T/out" || fail "odt: title"
grep -qx "page 1 595x842" "$T/out" || fail "odt: A4 from the page layout"
[ "$(grep -c '^hit 1 ' "$T/out")" = 1 ] || fail "odt: one hit on page 1"
[ "$(grep -c '^hit 2 ' "$T/out")" = 1 ] || fail "odt: one hit on page 2"
# the hit on page 1 is inside the 2 cm margins, below the heading
set -- $(grep '^hit 1 ' "$T/out")
[ "$3" -ge 56 ] && [ "$4" -gt 60 ] && [ "$4" -lt 200 ] || fail "odt: hit at $3,$4"

"$DOCS" --info "$T/test.epub" --find FLDESKTOP >"$T/out" 2>&1 || fail "epub"
grep -qx "pages 2" "$T/out" || fail "epub: page count (a page per chapter)"
grep -qx "title fl-docs epub test" "$T/out" || fail "epub: title"
grep -qx "page 1 432x648" "$T/out" || fail "epub: page size"
[ "$(grep -c '^hit ' "$T/out")" = 2 ] || fail "epub: two hits"

"$DOCS" --info "$T/test.cbz" >"$T/out" 2>&1 || fail "cbz"
grep -qx "pages 3" "$T/out" || fail "cbz: page count"
grep -qx "title test" "$T/out" || fail "cbz: title from the file name"
# 1, 2, 10: 300, 320 and 340 pixels wide (225, 240, 255 points)
grep -qx "page 1 225x300" "$T/out" || fail "cbz: page 1"
grep -qx "page 2 240x300" "$T/out" || fail "cbz: page 2"
grep -qx "page 3 255x300" "$T/out" || fail "cbz: page 3"

"$DOCS" --info "$T/nobook.zip" >"$T/out" 2>&1 && fail "a zip of text files opened"

# printing: the PDFs fl-docs makes for the printer have the same pages
for f in odt epub cbz; do
	"$DOCS" --to-pdf "$T/test.$f" "$T/$f.pdf" >"$T/out" 2>&1 || fail "$f: --to-pdf"
	"$DOCS" --info "$T/$f.pdf" --find fldesktop >"$T/info" 2>&1 || { cat "$T/info" >>"$T/out"; fail "$f: the PDF does not open"; }
	"$DOCS" --info "$T/test.$f" --find fldesktop >"$T/orig" 2>&1
	grep '^page \|^pages ' "$T/orig" >"$T/a"
	grep '^page \|^pages ' "$T/info" >"$T/b"
	cmp -s "$T/a" "$T/b" || { diff "$T/a" "$T/b" >>"$T/out"; fail "$f: the PDF's pages differ"; }
	[ "$(grep '^title ' "$T/orig")" = "$(grep '^title ' "$T/info")" ] || fail "$f: the PDF's title"
	[ "$(grep -c '^hit ' "$T/orig")" = "$(grep -c '^hit ' "$T/info")" ] || fail "$f: the PDF's text differs"
done
echo "fl-docs format test passed"
exit 0
