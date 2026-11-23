#!/bin/sh
# Regenerates the fldesktop artwork that is committed to the repository:
# icon themes and cursor themes (data/icons), wallpapers (data/backgrounds),
# the Plymouth theme and the stock user pictures (data/faces).
# Needs python3 and rsvg-convert (librsvg). The output is deterministic, so
# re-running it only changes files whose source changed.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
command -v rsvg-convert >/dev/null || { echo "rsvg-convert (librsvg) is needed" >&2; exit 1; }
python3 "$here/icons.py" "$top/data/icons"
python3 "$here/cursors.py" "$top/data/icons"
python3 "$here/backgrounds.py" "$top/data/backgrounds"
python3 "$here/plymouth.py" "$top/data/plymouth/fldesktop"
python3 "$here/faces.py" "$top/data/faces"
