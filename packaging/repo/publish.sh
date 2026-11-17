#!/bin/bash
# Adds freshly built packages to the fldesktop apt and pacman repositories.
#
#   publish.sh <channel> <debs-dir> <pkgs-dir> <site-dir>
#
# <channel>  stable | devel
# <site-dir> the repository tree (the gh-pages branch). Layout:
#   debian/dists, debian/pool        apt repository (suites trixie, trixie-devel)
#   arch/<channel>/x86_64/           pacman repository "fldesktop"
#   state/apt                        reprepro database (not published)
#   fldesktop-archive-keyring.asc    the signing key, index.html
#
# Needs reprepro, repo-add, bsdtar, gpg with the signing key, and FLD_REPO_KEY set to
# its fingerprint.
set -euo pipefail

channel=$1 debs=$(realpath "$2") pkgs=$(realpath "$3") site=$(realpath "$4")
here=$(dirname "$(realpath "$0")")
: "${FLD_REPO_KEY:?set FLD_REPO_KEY to the signing key fingerprint}"

case "$channel" in
  stable) suite=trixie ;;
  devel)  suite=trixie-devel ;;
  *) echo "unknown channel $channel" >&2; exit 2 ;;
esac

# ---- apt -------------------------------------------------------------------
conf="$site/state/apt/conf"
mkdir -p "$conf" "$site/debian"
cat > "$conf/distributions" <<EOF
Origin: fldesktop
Label: fldesktop
Suite: trixie
Codename: trixie
Architectures: amd64 arm64
Components: main
Description: fldesktop, a Wayland desktop built with FLTK (releases)
SignWith: $FLD_REPO_KEY

Origin: fldesktop
Label: fldesktop
Suite: trixie-devel
Codename: trixie-devel
Architectures: amd64 arm64
Components: main
Description: fldesktop, a Wayland desktop built with FLTK (development builds of main)
SignWith: $FLD_REPO_KEY
EOF
cat > "$conf/options" <<EOF
outdir $site/debian
dbdir $site/state/apt/db
EOF
rr=(reprepro -b "$site/state/apt" --export=silent-never)

shopt -s nullglob
debfiles=()
for f in "$debs"/*.deb; do
  case "$(basename "$f")" in *-dbgsym_*) ;; *) debfiles+=("$f") ;; esac   # no debug symbols
done
if ((${#debfiles[@]})); then
  # one version per suite: drop the previous build before adding the new one
  "${rr[@]}" removesrc "$suite" fldesktop || true
  "${rr[@]}" includedeb "$suite" "${debfiles[@]}"
else
  echo "no .deb files in $debs" >&2
fi
"${rr[@]}" --export=force export trixie trixie-devel
"${rr[@]}" deleteunreferenced

# ---- pacman ----------------------------------------------------------------
adir="$site/arch/$channel/x86_64"
mkdir -p "$adir"
new=()
for f in "$pkgs"/*.pkg.tar.zst; do
  case "$(basename "$f")" in *-debug-*) continue ;; esac
  name=$(bsdtar -xOf "$f" .PKGINFO | sed -n 's/^pkgname = //p')
  # remove older builds of this package (repo-add only drops them from the db)
  for old in "$adir"/"$name"-*.pkg.tar.zst; do
    oname=$(bsdtar -xOf "$old" .PKGINFO | sed -n 's/^pkgname = //p')
    if [[ "$oname" == "$name" ]]; then rm -f "$old" "$old.sig"; fi
  done
  cp "$f" "$adir/"
  gpg --batch --yes --local-user "$FLD_REPO_KEY" --detach-sign --no-armor "$adir/$(basename "$f")"
  new+=("$adir/$(basename "$f")")
done
if ((${#new[@]})); then
  repo-add --verify --sign --key "$FLD_REPO_KEY" "$adir/fldesktop.db.tar.gz" "${new[@]}"
else
  echo "no .pkg.tar.zst files in $pkgs" >&2
fi
rm -f "$adir"/*.old "$adir"/*.old.sig
# GitHub Pages serves no symlinks: make fldesktop.db / .files real files
for l in fldesktop.db fldesktop.files fldesktop.db.sig fldesktop.files.sig; do
  if [[ -L "$adir/$l" ]]; then
    cp --remove-destination "$(readlink -f "$adir/$l")" "$adir/$l"
  fi
done

# ---- site files ------------------------------------------------------------
gpg --armor --export "$FLD_REPO_KEY" > "$site/fldesktop-archive-keyring.asc"
cp "$here/index.html" "$site/index.html"
touch "$site/.nojekyll"
