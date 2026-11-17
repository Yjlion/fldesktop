#!/bin/sh
# Sends a few example notifications (for tools/screenshots.sh).
N() { gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications \
	-m org.freedesktop.Notifications.Notify "$@" >/dev/null; }
N "fl-term" 0 "utilities-terminal" "Build finished" "fldesktop compiled in <b>42 s</b> &amp; all tests passed." \
	'["default","Open","log","Show log"]' '{}' 0
N "Printers" 0 "printer" "Printed" '"report.pdf" was printed on Office_Laser.' '[]' '{}' 0
N "Battery" 0 "battery-caution" "Battery low" "5% left. Plug in the charger." '[]' '{"urgency": <byte 2>}' 0
