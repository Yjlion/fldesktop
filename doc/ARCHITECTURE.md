# fldesktop architecture

## Source tree

| Directory | What it holds |
|-----------|---------------|
| `compositor/` | **fl-wm**, the wlroots compositor (C11) |
| `shell/` | fl-panel, fl-launcher, fl-desktop, fl-cliphist, fl-nightlight, fl-autostart, the `fl-session` script |
| `greeter/` | fl-greet, the greetd greeter |
| `apps/` | the 38 applications (one directory each; testable logic in a `*-util` static library) |
| `portal/` | xdg-desktop-portal-fl |
| `common/libfld` | the shared FLTK library: theme and styles, widgets, menus, INI, JSON, desktop entries, MIME, processes, file operations, AT-SPI |
| `common/*` | client libraries shared by an app, the panel and Settings: `audio` (PipeWire), `nm` (NetworkManager), `bt` (BlueZ), `printers` (CUPS), `firewall`, `power` (UPower), `udisks`, `gvfs`, `mpris`, `backlight`, `fprint`, `secret`, `mdns`, `spell`, `gst` |
| `protocols/` | Wayland protocol XML not shipped by wayland-protocols |
| `data/` | desktop entries, icons, wallpapers, D-Bus services, the session file, example configs |
| `tests/` | unit tests, headless smoke tests, fuzz targets (`tests/fuzz`) |
| `packaging/` | Arch, Debian, live image and package repository |

A page of fl-settings that also exists as an app (Network, Bluetooth, Printers, Firewall, Sound) is a thin
wrapper around that app's `*-ui` library in `common/`.

## Processes in a session

```
greetd ─ fl-greet ─(login)─ fl-session (sh)
                              └ dbus-run-session (if no session bus)
                                  └ fl-wm ──────────────── Wayland socket
                                      ├ fl-panel          (role: panel, OSD, popups)
                                      ├ fl-desktop        (role: desktop)
                                      ├ fl-autostart      (~/.config/autostart, XDG autostart)
                                      ├ fl-lock --locker  (started by fl-wm when locking)
                                      └ xdg-desktop-portal-fl (activated by D-Bus)
              D-Bus activated: fl-notifyd, fl-polkit-agent (autostart), apps
```

`fl-session` sets the session environment (`~/.config/fldesktop/environment`, Flatpak data dirs, libdecor)
and runs fl-wm. fl-wm starts the programs listed in `wm.ini [autostart]` (fl-panel and fl-autostart by default)
and exports its environment to D-Bus activation and systemd.

## Configuration

- `~/.config/fldesktop/wm.ini`: fl-wm (keys, outputs, workspaces, snapping, hot corners, environment,
  autostart). fl-wm reloads it on `SIGHUP` (`fld::reload_wm()`).
- `~/.config/fldesktop/*.ini`: the shell and apps (panel, theme, launcher, …), read with `fld::Ini` and
  watched with `fld::ConfigWatch` so changes in Settings apply at once.
- `/etc/fldesktop/greeter.ini`: the greeter, written by `fl-settings-helper` through pkexec.

## Communication

| Channel | Who | What |
|---------|-----|------|
| Wayland, standard protocols | all clients | xdg-shell, layer-shell, foreign-toplevel, ext-workspace, screencopy, data-control, virtual keyboard, … |
| `fldesktop-window-v1` | FLTK apps | start a window move from the app's own widgets |
| Signals to `$FLWM_PID` | shell, Settings | `SIGHUP` reload wm.ini, `SIGUSR1` lock, `SIGUSR2` dump windows to the log |
| `org.freedesktop.Notifications` | fl-notifyd | notifications; `org.fldesktop.Notifications` adds history and Do Not Disturb for the panel |
| `org.freedesktop.impl.portal.desktop.fl` | xdg-desktop-portal-fl | portals; `org.fldesktop.GlobalShortcuts` passes shortcuts from fl-wm |
| `org.fldesktop.LockDaemon` | fl-lock daemon | lock before suspend, idle locking |
| `org.fldesktop.Password` | fl-keyring | password prompts for other apps |
| `org.fldesktop.Web.Login` | fl-web | saved logins |
| system services | apps | NetworkManager, BlueZ, UPower, UDisks2, logind, PackageKit, firewalld, CUPS, PipeWire |

## Trust model (summary)

fl-wm gives special roles (panel, desktop, launcher, OSD, notifications, OSK, snipping, lock) to clients
that ask for them by app id or title **and** whose executable is the fldesktop program of that name in fl-wm's
own directory or the install `bindir`. The lock role also needs the process to be the locker fl-wm started
itself. Clients sandboxed with security-context-v1 (Flatpak) do not see the privileged globals.
Root actions go through pkexec helpers with polkit actions. Details and open issues: `doc/THREAT-MODEL.md`.
