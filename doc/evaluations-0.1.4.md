# Evaluations for 0.1.4

## Memory use

### How it was measured

`tools/memstat.sh` sums PSS (each process's fair share of shared pages), RSS
and private memory for every running `fl-*` process from
`/proc/<pid>/smaps_rollup`.

The session was headless `fl-wm` (pixman renderer, one 1280×720 output)
running `fl-desktop`, `fl-panel`, `fl-notifyd` and `fl-cliphist`, with six
files on the desktop and a 3840×2160 PNG as the wallpaper. All binaries were
RelWithDebInfo builds from the build tree.

### Findings

| Process | PSS before | PSS after |
|---|---:|---:|
| fl-wm | 12.9 MB | 12.8 MB |
| fl-desktop | **52.0 MB** | **27.8 MB** |
| fl-panel | 21.8 MB | 22.0 MB |
| fl-notifyd | 12.2 MB | 12.2 MB |
| fl-cliphist | 2.4 MB | 2.4 MB |
| **Total** | **102.3 MB** | **77.2 MB** |

**The wallpaper was the one large, avoidable cost.** `fl-desktop` kept the
decoded file (about 33 MB for a 4K photo) as well as the copy scaled to the
screen. It now keeps only the scaled copy, and decodes the file again when
the screen size or the wallpaper changes. With the default SVG wallpaper this
makes no difference: the SVG source was never drawn at full size.

These were measured and not changed, because the gain is too small for the
risk:

- **A shared libfld.** libfld and the `common/*` helpers are linked
  statically into every program, which is why the files look large:
  `fl-panel` is 21 MB and `fl-settings` 29 MB. That is mostly debug info,
  which is never loaded. The code itself is small: all of libfld is 780 KB of
  text, each program links only the parts it uses, and the resident code of
  each program is 0.1–1.1 MB (2.3 MB for the whole session, libfld included).
  A shared library would save well under 1 MB per session. It would also cost
  relocation pages in every process, a library package and an ABI to keep.
- **gio only when a screen reader is on.** Every FLTK program loads
  glib/gio, for the AT-SPI bridge and the D-Bus clients. That costs 72 KB of
  private pages per process, plus a 110–330 KB share of the library pages.
  Loading the bridge with `dlopen` would save less than 0.3 MB per program,
  and fl-wm loads glib anyway, through Pango.
- **`malloc_trim` after loading the wallpaper.** No change: the 8 MB heap
  left in `fl-desktop` is data still in use (icons, fonts, the window's
  buffers).
- **`MALLOC_ARENA_MAX=2`.** 0.2 MB in total.
- **Smaller icon caches.** `fld::icon()` and `fld::symbolic_icon()` promise
  never to free an image, and widgets keep those pointers (the desktop's
  items, the panel's buttons). An LRU cache would have to change that
  promise in every caller. Programs already restart when the icon theme
  changes, so images from an old theme do not pile up.

### What remains

- `fl-desktop` peaks at about 62 MB while it decodes a 4K photo. Decoding
  JPEGs at a reduced scale (libjpeg can do this) would lower that peak, but
  FLTK's `Fl_JPEG_Image` does not offer it.
- `fl-panel` (22 MB) and `fl-notifyd` (12 MB) are FLTK plus fontconfig and
  cairo for each process. Making them smaller means doing without those, or
  merging processes, for example running notifications inside the panel. That
  is a design decision, not a quick win.

## A Debian live image

### The choice: live-build

- **live-build** is what Debian uses for its own live images. It works from
  a plain directory of package lists, hooks and files, in a `debian:trixie`
  container on a GitHub runner, with its chroots in a privileged container.
  Its output is a hybrid ISO that boots on BIOS and UEFI, from USB or DVD.
- **mkosi** is newer and builds without root. But it targets disk images and
  systemd-boot more than live media, and it is not what Debian's own live
  images are made with.

### What is in the repository

- `packaging/live/build.sh [stable|devel] [outdir]` runs `lb config` and
  `lb build`. It adds the fldesktop apt repository (trixie or trixie-devel)
  with the key from `packaging/repo/fldesktop-archive-keyring.asc`.
- `packaging/live/config` holds:
  - the package list: fldesktop, the greeter, Plymouth, NetworkManager,
    PipeWire;
  - the greetd config: the live user `user` (password `live`) is logged in
    directly, and fl-greet shows after logging out;
  - a hook that sets the fldesktop Plymouth theme and enables greetd.
- The **Live image** workflow (`.github/workflows/live.yml`) is started by
  hand. It builds the amd64 ISO and keeps it as a build artifact for 14 days.
  It does not publish to Pages, because the files are large.

### Not done yet

- **An installer.** Calamares is the usual choice for Debian live images.
- **arm64 images.**
- **Building from the packages of the same CI run.** `build.sh` already
  takes `.deb` files placed in `config/packages.chroot/`.
- **Verification.** The workflow has not been run yet, and the ISO has not
  been booted in QEMU.

## libproxy

Not evaluated (dropped from 0.1.4).
