#!/bin/sh
# A mocked MPRIS player on a private bus; test_mpris sends it PlayPause and
# Next (fld-mpris' command_sync) and the script checks that they arrived.
# usage: mpris-mock.sh <test_mpris>
set -u
PY=${FLD_TEST_PYTHON:-python3}
command -v dbus-run-session >/dev/null 2>&1 || { echo "no dbus-run-session"; exit 77; }
"$PY" -c "import dbusmock" 2>/dev/null || { echo "python-dbusmock not installed"; exit 77; }
if [ -z "${FLD_IN_BUS:-}" ]; then
	FLD_IN_BUS=1 exec dbus-run-session -- "$0" "$@"
fi
T=$(mktemp -d)
trap 'kill $MOCK 2>/dev/null; rm -rf "$T"' EXIT
"$PY" -m dbusmock --session -l "$T/calls" org.mpris.MediaPlayer2.mock /org/mpris/MediaPlayer2 \
	org.mpris.MediaPlayer2.Player >/dev/null 2>&1 &
MOCK=$!
OBJ="-d org.mpris.MediaPlayer2.mock -o /org/mpris/MediaPlayer2"
for i in $(seq 1 50); do
	gdbus call --session $OBJ -m org.freedesktop.DBus.Mock.AddProperty \
		org.mpris.MediaPlayer2.Player PlaybackStatus '<"Playing">' >/dev/null 2>&1 && break
	sleep 0.1
done
gdbus call --session $OBJ -m org.freedesktop.DBus.Mock.AddMethods org.mpris.MediaPlayer2.Player \
	"[('PlayPause', '', '', ''), ('Next', '', '', '')]" >/dev/null || exit 1
FLD_MPRIS_MOCK=1 "$1" || exit 1
grep -q PlayPause "$T/calls" || { echo "PlayPause did not arrive"; exit 1; }
grep -q Next "$T/calls" || { echo "Next did not arrive"; exit 1; }
echo "mpris mock test passed"
