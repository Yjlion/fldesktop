#!/bin/sh
# Runs fl-wm from the build tree, nested in the current Wayland/X11 session
# (or on a VT). Extra arguments are passed to fl-wm.
#   tools/run-dev.sh            # full session (desktop, panel, autostart)
#   tools/run-dev.sh -s fl-term # just a terminal
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build}
export XDG_DATA_DIRS="$BUILD/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export FLD_DATADIR="$BUILD/share/fldesktop"
export PATH="$BUILD/bin:$PATH"
exec "$BUILD/bin/fl-wm" "$@"
