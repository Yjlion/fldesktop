#!/bin/sh
# Memory of the fldesktop processes running now, from /proc/<pid>/smaps_rollup:
# PSS (the fair share of shared pages), RSS, and the private (unshared) part.
# usage: memstat.sh [user]   (default: the current user)
# Programs are matched by name: fl-* and xdg-desktop-portal-fl.
user=${1:-$(id -un)}
printf '%-24s %7s %9s %9s %9s\n' PROGRAM PID "PSS KiB" "RSS KiB" "PRIV KiB"
total_pss=0
total_rss=0
total_priv=0
for p in $(pgrep -u "$user" -f '(^|/)(fl-[a-z-]+|xdg-desktop-portal-fl)( |$)'); do
	f=/proc/$p/smaps_rollup
	[ -r "$f" ] || continue
	name=$(basename "$(readlink /proc/$p/exe 2>/dev/null || cat /proc/$p/comm)")
	set -- $(awk '/^Pss:/{pss=$2} /^Rss:/{rss=$2} /^Private_Clean:/{pc=$2} /^Private_Dirty:/{pd=$2}
		END{print pss+0, rss+0, pc+pd}' "$f")
	printf '%-24s %7d %9d %9d %9d\n' "$name" "$p" "$1" "$2" "$3"
	total_pss=$((total_pss + $1))
	total_rss=$((total_rss + $2))
	total_priv=$((total_priv + $3))
done
printf '%-24s %7s %9d %9d %9d\n' TOTAL "" "$total_pss" "$total_rss" "$total_priv"
