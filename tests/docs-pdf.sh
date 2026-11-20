#!/bin/sh
# fl-docs --info on a generated two page PDF: pages, sizes, title and search
# hits (top-left coordinates, both pages, case-insensitive).
# usage: docs-pdf.sh <fl-docs>
set -u
DOCS="$1"
command -v python3 >/dev/null 2>&1 || { echo "no python3"; exit 77; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; cat "$T/out"; exit 1; }
python3 -I "$(dirname "$0")/make-pdf.py" "$T/test.pdf" || fail "make-pdf"
"$DOCS" --info "$T/test.pdf" --find FLDESKTOP >"$T/out" 2>&1 || fail "fl-docs --info failed"
grep -qx "pages 2" "$T/out" || fail "page count"
grep -qx "title fl-docs test" "$T/out" || fail "title"
grep -qx "page 1 612x792" "$T/out" || fail "page 1 size"
grep -qx "page 2 792x612" "$T/out" || fail "page 2 size"
[ "$(grep -c '^hit ' "$T/out")" = 2 ] || fail "two hits expected"
# "Hello fldesktop" sits 92 pt from the top: the hit's y must be near the top
set -- $(grep '^hit 1 ' "$T/out")
[ "$4" -gt 60 ] && [ "$4" -lt 110 ] || fail "hit on page 1 at y=$4, expected near the top (top-left origin)"
set -- $(grep '^hit 2 ' "$T/out")
[ "$4" -gt 280 ] && [ "$4" -lt 310 ] || fail "hit on page 2 at y=$4"
# not a PDF
echo "plain text" >"$T/x.pdf"
"$DOCS" --info "$T/x.pdf" >/dev/null 2>&1 && fail "a text file opened as a PDF"
echo "fl-docs pdf test passed"
exit 0
