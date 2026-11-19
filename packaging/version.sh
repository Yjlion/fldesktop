#!/bin/bash
# Works out the package version from git. Release tags define the version:
#
#   v<version>          release <version>            (e.g. v0.2.0, v1.0, v1.2.3.4)
#   v<version>-<n>      rebuild <n> of <version>     (Debian <version>+r<n>, Arch pkgrel=<n>)
#
# Anything else is a development build of the latest release tag reachable from
# HEAD: <version>+git<count>.g<hash> (Debian) or <version>.r<count>.g<hash> (Arch).
# With no release tag at all, the version in CMakeLists.txt is the base.
#
#   version.sh [stable|devel]    prints shell assignments:
#     FLD_VERSION     the version shown by the programs
#     FLD_DEB_VERSION the Debian package version
#     FLD_PKGVER      the Arch pkgver
#     FLD_PKGREL      the Arch pkgrel
#
# The channel defaults to stable when HEAD (or $GITHUB_REF_NAME, for a tag
# build on CI) is a release tag, else devel. A stable build of a ref that is not a
# valid release tag is an error.
set -euo pipefail

src=$(cd "$(dirname "$(realpath "$0")")/.." && pwd)
channel=${1:-}
tag_re='^v([0-9]+(\.[0-9]+)+)(-([0-9]+))?$'

base_from_cmake() {
  sed -n 's/^project(fldesktop VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt"
}

# the release tag being built, if any
tag=
if [[ "${GITHUB_REF_TYPE:-}" == tag ]]; then
  tag=${GITHUB_REF_NAME:-}
else
  for t in $(git -C "$src" tag --points-at HEAD 2>/dev/null); do
    if [[ "$t" =~ $tag_re ]]; then tag=$t; break; fi
  done
fi

if [[ -z "$channel" ]]; then
  if [[ -n "$tag" && "$tag" =~ $tag_re ]]; then channel=stable; else channel=devel; fi
fi

case "$channel" in
  stable)
    if [[ ! "$tag" =~ $tag_re ]]; then
      echo "version.sh: '${tag:-<no tag>}' is not a release tag (expected v<version> or v<version>-<number>, e.g. v0.2.0 or v0.2.0-2)" >&2
      exit 1
    fi
    ver=${BASH_REMATCH[1]} rel=${BASH_REMATCH[4]:-}
    if [[ -n "$rel" ]]; then deb="$ver+r$rel"; else deb=$ver; rel=1; fi
    printf 'FLD_VERSION=%s\nFLD_DEB_VERSION=%s\nFLD_PKGVER=%s\nFLD_PKGREL=%s\n' "$deb" "$deb" "$ver" "$rel"
    ;;
  devel)
    base=
    # the highest release version reachable from HEAD
    while read -r t; do
      if [[ "$t" =~ $tag_re ]]; then base=${BASH_REMATCH[1]}; fi
    done < <(git -C "$src" tag --merged HEAD --list 'v[0-9]*' 2>/dev/null | sed 's/^v//' | sort -V | sed 's/^/v/')
    [[ -n "$base" ]] || base=$(base_from_cmake)
    if n=$(git -C "$src" rev-list --count HEAD 2>/dev/null) && h=$(git -C "$src" rev-parse --short=7 HEAD 2>/dev/null); then
      deb="$base+git$n.g$h" arch="$base.r$n.g$h"
    else
      deb=$base arch=$base
    fi
    printf 'FLD_VERSION=%s\nFLD_DEB_VERSION=%s\nFLD_PKGVER=%s\nFLD_PKGREL=%s\n' "$deb" "$deb" "$arch" 1
    ;;
  *) echo "version.sh: unknown channel '$channel' (stable or devel)" >&2; exit 2 ;;
esac
