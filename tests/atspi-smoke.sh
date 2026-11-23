#!/bin/sh
# The AT-SPI bridge end to end: a private D-Bus session stands for the
# accessibility bus, at-spi2-registryd runs on it, a test window runs in
# fl-wm (headless), and atspi-probe finds it through the registry and works
# its widgets. usage: atspi-smoke.sh <bindir> <atspi-app> <atspi-probe>
set -u
BIN="$1"
APP="$2"
PROBE="$3"
command -v dbus-run-session >/dev/null 2>&1 || { echo "no dbus-run-session"; exit 77; }
REG=""
for r in /usr/lib/at-spi2-registryd /usr/libexec/at-spi2-registryd /usr/lib/at-spi2-core/at-spi2-registryd; do
  [ -x "$r" ] && REG="$r"
done
[ -n "$REG" ] || { echo "no at-spi2-registryd"; exit 77; }
[ -x "$BIN/fl-wm" ] || { echo "no fl-wm"; exit 77; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/run" "$T/home"
chmod 700 "$T/run"
export XDG_RUNTIME_DIR="$T/run" HOME="$T/home"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
cat >"$T/run.sh" <<EOS
"$REG" >"$T/reg.log" 2>&1 &
sleep 0.5
FLD_ATSPI=1 "$APP" >"$T/app.log" 2>&1 &
"$PROBE" atspi-app >"$T/probe.log" 2>&1
echo \$? >"$T/status"
kill \$FLWM_PID
EOS
timeout 90 dbus-run-session -- sh -c "export AT_SPI_BUS_ADDRESS=\$DBUS_SESSION_BUS_ADDRESS; \"$BIN/fl-wm\" -s 'sh $T/run.sh'" >"$T/wm.log" 2>&1
st=$(cat "$T/status" 2>/dev/null || echo none)
cat "$T/probe.log"
[ "$st" = 0 ] || { echo "atspi-probe: status $st"; tail -n 5 "$T/app.log" "$T/reg.log"; exit 1; }
echo "atspi smoke test passed"
