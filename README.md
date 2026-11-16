# fldesktop

A lightweight Wayland desktop built with the [FLTK](https://www.fltk.org) 1.4 toolkit.

| Component | What it is |
|-----------|------------|
| **fl-wm** | Stacking Wayland compositor on wlroots (0.18 – 0.20) with server-side decorations, key bindings, Alt+Tab, window snapping and layer-shell |
| **fl-panel** | Taskbar: application button, quick launch, window list, volume, battery, calendar clock, power menu |
| **fl-launcher** | Searchable application menu with categories and recently used apps (Super+Space) |
| **fl-desktop** | Wallpaper and desktop icons for the XDG desktop folder |
| **fl-greet** | Graphical greeter for [greetd](https://sr.ht/~kennylevinsen/greetd/) |
| **fl-term** | Terminal emulator (libvterm): scrollback, true colour, mouse, bracketed paste |
| **fl-files** | File manager: icon/list views, thumbnails, trash, background copy/move, drag and drop |
| **fl-edit** | Tabbed text editor with syntax highlighting, find/replace and auto-indent |
| **fl-calc** | Basic and scientific calculator with history and memory |
| **fl-view** | Image viewer: zoom, rotate, slideshow, animated GIFs |

![fldesktop](screenshots/desktop.png)

| | |
|---|---|
| ![launcher](screenshots/launcher.png) | ![files](screenshots/files.png) |
| ![editor](screenshots/editor.png) | ![terminal](screenshots/terminal.png) |
| ![calculator](screenshots/calculator.png) | ![greeter](screenshots/greeter.png) |

Screenshots are made in a headless session by `tools/screenshots.sh`.

All programs share `libfld` (XDG directories, desktop entries, icon themes,
mime types, trash, theme) and read their settings from `~/.config/fldesktop/`.

## Building

Dependencies: CMake ≥ 3.19, a C11/C++17 compiler, FLTK ≥ 1.4 (built with
Wayland support), wlroots 0.18/0.19/0.20, wayland, wayland-protocols,
xkbcommon, libinput, pango/cairo, libdrm, pixman and libvterm.

```sh
# Arch Linux
sudo pacman -S cmake ninja fltk wlroots0.20 libvterm wayland-protocols pango libinput
# Debian 13 (trixie)
sudo apt install cmake ninja-build pkgconf libfltk1.4-dev libwlroots-0.18-dev libvterm-dev \
  wayland-protocols libwayland-dev libxkbcommon-dev libinput-dev libpango1.0-dev libdrm-dev

cmake -B build -G Ninja
cmake --build build
ctest --test-dir build          # unit tests + a headless compositor smoke test
sudo cmake --install build
```

Each component can be switched off, e.g. `-DFLD_BUILD_WM=OFF` to build only
the FLTK applications for use under another compositor.

### Packages

* **Arch:** `cd packaging/arch && makepkg -si` builds split packages
  (`fldesktop`, `fldesktop-wm`, `fldesktop-shell`, `fldesktop-greeter`,
  `fldesktop-term`, …) from the checkout.
* **Debian/Ubuntu:** `cp -r packaging/debian debian && dpkg-buildpackage -b -us -uc`
  produces the same set of `.deb` packages.

CI (`.github/workflows/ci.yml`) builds and tests on Arch (wlroots 0.20) and
Debian trixie (wlroots 0.18) and attaches the packages to `v*` releases.

## Running

* From a display manager: choose the **fldesktop** session (installed to
  `/usr/share/wayland-sessions`).
* From a text console: run `fl-session`.
* Nested inside another Wayland session for testing: run `fl-wm`.
* With greetd: install `fldesktop-greeter` and set `/etc/greetd/config.toml` to
  `command = "cage -s -d -- fl-greet"` (see `/usr/share/fldesktop/config.toml`).
  `fl-greet --demo` shows the greeter without greetd.

Running uninstalled from the build tree (nested in your current session):

```sh
tools/run-dev.sh            # whole desktop
tools/run-dev.sh -s fl-term # only a terminal
```

### Default shortcuts

| Keys | Action |
|------|--------|
| Super+Space | Application launcher |
| Super+Enter / Super+T | Terminal |
| Super+E | File manager |
| Super+D | Show desktop |
| Super+Q, Alt+F4 | Close window |
| Super+Up / Down | Maximize / minimize |
| Super+Left / Right | Snap to the left / right half |
| F11, Super+F | Fullscreen |
| Alt+Tab, Alt+Shift+Tab | Switch windows |
| Super+drag, Super+right-drag | Move / resize any window |
| Print | Screenshot (needs grim) |
| Super+Shift+R | Reload configuration |
| Ctrl+Alt+Backspace | Quit the session |

Bindings, keyboard layout, pointer settings, outputs and autostart programs
are configured in `~/.config/fldesktop/wm.ini`; an annotated example is
installed as `/usr/share/fldesktop/wm.ini.example`.

## Configuration files

| File | Used by |
|------|---------|
| `~/.config/fldesktop/wm.ini` | compositor: appearance, keys, input, outputs, autostart |
| `~/.config/fldesktop/desktop.ini` | `[Appearance]` theme (light/dark), accent, fonts; `[Desktop]` wallpaper; `[General]` icon theme, terminal |
| `~/.config/fldesktop/panel.ini` | `[Panel] QuickLaunch=`, `[Clock] Format=` |
| `~/.config/fldesktop/term.ini` | `[Terminal] ColorScheme=dark|light`, `FontSize`, `Scrollback` |
| `/etc/fldesktop/greeter.ini` | `[Greeter] Background`, `MinUid`, `HideUsers` |

## How the shell works

FLTK cannot create layer-shell surfaces, so the panel, desktop and launcher are
ordinary borderless toplevels. fl-wm recognises them by their app id/title
(`fldesktop.panel`, `fldesktop.desktop`, `fldesktop.launcher`) **and** checks
that the client executable is the matching program installed next to `fl-wm`,
then pins them to the right layer and position and reserves the panel's space.
The taskbar uses the standard wlr-foreign-toplevel-management protocol, and
third-party layer-shell clients (bars, notification daemons, wallpaper tools)
work as well.

## License

GPL-2.0-or-later, see [COPYING](COPYING). The wlr protocol XML files in
`protocols/` keep their own MIT-style license.
