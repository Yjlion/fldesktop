#!/bin/sh
# Builds a Debian trixie live ISO with fldesktop from the fldesktop apt
# repository (https://yjlion.github.io/fldesktop/debian).
# usage (as root, on Debian with live-build): build.sh [stable|devel] [outdir]
# Packages not released yet can be added as .deb files in
# config/packages.chroot/ before running it.
set -eu
channel=${1:-stable}
out=${2:-$PWD}
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
case "$channel" in
	stable) suite=trixie ;;
	devel) suite=trixie-devel ;;
	*) echo "channel: stable or devel" >&2; exit 2 ;;
esac
command -v lb >/dev/null || { echo "live-build is needed (apt install live-build)" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
lb config \
	--distribution trixie \
	--archive-areas "main contrib non-free-firmware" \
	--debian-installer none \
	--binary-images iso-hybrid \
	--iso-application "fldesktop live" \
	--iso-volume "fldesktop-live" \
	--bootappend-live "boot=live components quiet splash username=user hostname=fldesktop" \
	--apt-recommends true
cp -a "$here/config/." config/
# the fldesktop repository, signed with the key fldesktop-archive-keyring has
mkdir -p config/archives
echo "deb https://yjlion.github.io/fldesktop/debian $suite main" >config/archives/fldesktop.list.chroot
cp "$top/packaging/repo/fldesktop-archive-keyring.asc" config/archives/fldesktop.key.chroot
lb build
mkdir -p "$out"
for f in *.iso; do
	cp "$f" "$out/fldesktop-live-$channel-$(date +%Y%m%d)-amd64.iso"
done
ls -l "$out"/fldesktop-live-*.iso
