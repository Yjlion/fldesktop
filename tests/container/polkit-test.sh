#!/bin/bash
# Manual integration test for fl-polkit-agent. Run as root in a Debian
# container that has the build in /build/out, polkitd, dbus, wtype and grim:
#   polkit-test.sh PASSWORD [SECOND_PASSWORD]   or   polkit-test.sh ESC
# It starts a system bus and polkitd, creates the user "tester" (password
# "secret"), registers the agent for one process and runs pkcheck with
# interaction; the result is printed as "pkcheck exit: N" (0 = authorized).
# runs inside the fldeb container as root
set -u
mkdir -p /run/dbus
[ -S /run/dbus/system_bus_socket ] || dbus-daemon --system --fork
pgrep -x polkitd >/dev/null || (/usr/lib/polkit-1/polkitd --no-debug >/tmp/polkitd.log 2>&1 &)
id tester >/dev/null 2>&1 || { useradd -m -G sudo tester; echo 'tester:secret' | chpasswd; }
sleep 1
cat > /tmp/as-tester.sh <<'IN'
#!/bin/bash
export XDG_RUNTIME_DIR=/tmp/rt-tester; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export XDG_DATA_DIRS=/build/out/share:/usr/share PATH=/build/out/bin:$PATH
sleep 600 & TARGET=$!
/build/out/bin/fl-wm -s "fl-polkit-agent --process $TARGET" > /tmp/wm.log 2>&1 &
WM=$!
sleep 3
export WAYLAND_DISPLAY=$(grep -o 'WAYLAND_DISPLAY=[a-z0-9-]*' /tmp/wm.log | head -1 | cut -d= -f2)
run_check() {
  pkcheck --action-id org.freedesktop.policykit.exec --process $TARGET --allow-user-interaction > /tmp/pkcheck.out 2>&1
  echo "pkcheck exit: $?" >> /tmp/pkcheck.out
}
run_check &
CHECK=$!
sleep 3
grim /tmp/polkit-dialog.png
if [ "$1" = ESC ]; then wtype -k Escape; else wtype "$1"; wtype -k Return; fi
sleep 4
grim /tmp/polkit-after.png
if [ -n "${2:-}" ]; then wtype "$2"; wtype -k Return; sleep 4; fi
wait $CHECK
kill $WM $TARGET 2>/dev/null
grep -a 'fl-polkit-agent' /tmp/wm.log
cat /tmp/pkcheck.out
IN
chmod +x /tmp/as-tester.sh
su tester -c "/tmp/as-tester.sh '$1' '${2:-}'"
