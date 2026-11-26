# Packaging

| Directory | Distribution | How it is built | Tested in CI |
|-----------|--------------|-----------------|--------------|
| `arch/` | Arch Linux (split packages) | `makepkg` from the checkout | yes (packages.yml) |
| `debian/` | Debian trixie and later | `dpkg-buildpackage` | yes (packages.yml) |
| `fedora/fldesktop.spec` | Fedora (one package) | `rpmbuild -bb` with `_sourcedir` = the checkout | no |
| `alpine/APKBUILD` | Alpine Linux (OpenRC) | `abuild -r` in `packaging/alpine` | no |
| `../flake.nix` | Nix / NixOS | `nix build` | no |
| `live/` | a live image (Debian based) | `live/build.sh` | on release |

The Fedora, Alpine and Nix files follow the Debian and Arch package lists but have not
been built by the project yet; reports and fixes are welcome. All need FLTK 1.4 and
wlroots 0.18 – 0.20.

`version.sh` derives the version from the git release tags (`v*`); packages pass it in
with `-DFLD_VERSION=...`.
