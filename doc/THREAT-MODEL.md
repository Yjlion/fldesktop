# fldesktop threat model

What fldesktop protects, from whom, and where the boundaries are. Reviewed for v0.1.4 (roadmap item 1.4).

## Boundaries

| Boundary | Attacker | Protected |
|----------|----------|-----------|
| Lock screen | someone at the keyboard of a locked session | the session |
| pkexec helpers | a program running as the user | root, other users, the system configuration |
| Flatpak sandbox (security-context-v1) | a sandboxed app | screen contents, input, the clipboard, other windows |
| Parsers of files and network data | a crafted file, playlist, VPN config or VNC server | the user's session (memory safety) |

**Not a boundary:** unsandboxed programs of the same user. They can already ptrace the user's processes,
change `~/.config`, and use every Wayland global. fl-wm's checks against them are against accidents and
look-alike windows, not against malware.

## 1. Lock screen

fl-wm starts `fl-lock --locker` itself (`compositor/lock.c`, fork + exec, no shell) and unlocks when that
process **exits with status 0**. Any other end (a signal, another status) starts the locker again, with a
back-off if it keeps failing. The lock role is given only to a window of the process fl-wm started
(`lock_client_is_locker()`), and while locked only lock surfaces get input.

**Fixed in this review:** the locker returned `Fl::run()`'s result. FLTK's default window callback closes a
window on Escape, after which `Fl::run()` returns 0, so **pressing Escape (also on the lock screen's on-screen
keyboard) unlocked the session**. Now the lock window ignores close requests and `run_locker()` returns 3
when the event loop ends; only a successful PAM or fingerprint check exits with 0
(`apps/fl-lock/main.cpp`, `finish_auth()`).

Remaining notes:
- Any path to `exit(0)` in fl-lock is an unlock. New code in the locker must not call `exit(0)` or return
  0 from `main` except after authentication. `tests/lock-smoke.sh` locks a headless session, presses
  Escape and Alt+F4, and fails if the session unlocks (it did before the fix).
- `lock_cmd` in wm.ini is split on spaces, not by a shell. A user who can edit wm.ini can replace the
  locker; that is the same user (not a boundary).
- With an external ext-session-lock client (swaylock) the protocol's own rules apply.

## 2. Shell roles in fl-wm

`detect_role()` (`compositor/view.c`) gives a role when the app id or title matches **and**
`/proc/<pid>/exe` (pid from the Wayland socket credentials) is the program of that name in fl-wm's own
directory or `FLD_BINDIR`.

- Same-user processes can still get a role by running the real binary with `LD_PRELOAD`, or by ptrace.
  Accepted: not a boundary (see above). Sandboxed clients cannot (no access to the host binaries' process,
  and the privileged globals are filtered).
- `FLWM_TRUST_SHELL` in fl-wm's environment turns the check off (for development). It is read from fl-wm's
  own environment only; `fl-session` does not set it.
- Roadmap 4 (layer-shell through FLTK) would let the shell use layer-shell, so fewer roles need the
  executable check at all.

## 3. Privileged helpers (pkexec)

| Helper | Polkit action | Commands |
|--------|---------------|----------|
| `fl-settings-helper system …` | `org.fldesktop.settings.login-screen` | greeter.ini and background, time zone, time, NTP, host name, Avahi, published services |
| `fl-settings-helper users …` | `org.fldesktop.settings.users` | add, delete, modify, password, picture, automatic login |
| `fl-disks-helper` | `org.fldesktop.disks.modify` | partition tables, partitions, format, resize, move, disk images |
| `fl-services-helper` | `org.fldesktop.services` | enable and disable OpenRC services (names checked by `svc::valid_name`, no leading `-`) |
| `fl-backlight-helper` | `org.fldesktop.backlight` (`yes` for the active session) | brightness |

Reviewed properties:
- Settings has a single helper. Its first argument (`system` or `users`) selects the polkit action through
  `org.freedesktop.policykit.exec.argv1`. Before anything runs as root, `helper_args.cpp` checks the whole
  command line against a fixed list of commands: the number of arguments and the form of each one (absolute
  paths without control characters, names, `on`/`off`, `0`/`1`). `tests/test_settings_helper.cpp` covers it.
  With a polkit too old for `exec.argv1`, pkexec takes the first action for the path; both actions are
  `auth_admin_keep`.
- All run tools with argv vectors (`fld::run_capture*`), never a shell. Passwords go on stdin
  (`chpasswd`), never in argv, and `valid_password()` rejects line breaks and NUL, so a password cannot add a
  second `user:password` line.
- Files from the user (new greeter.ini, pictures, disk images) are opened with `O_NOFOLLOW`, must be regular
  files **owned by `PKEXEC_UID`**, and have a size limit (`fld::read_user_file()`). Disk image files are
  opened with the caller's rights.
- Output files under `/etc` are written with `fld::write_atomic()` (`O_NOFOLLOW` temp file, `fsync`, rename).
- Arguments are validated before use: time zones (`dtsys::valid_zone`: no `..`, `//` or leading `/` `.`,
  must exist), times, host names, user and full names (`:` `,` `=` `\` and control characters rejected),
  Avahi service ids from a fixed list, greeter keys from an allowlist (`validate_greeter_config`).
  fl-disks only touches whole disks named `/dev/<name>` that are not mounted, swap or held by another device.
- The users helper only changes accounts of people (uid 1000–59999 with a login shell) and refuses to delete
  the caller's own account.

Open items:
- The actions use `auth_admin_keep`: for about five minutes after one authentication, any program of the
  user can run the helper again without a password (for example to add an administrator account). Consider
  `auth_admin` for the users helper and for `fl-disks-helper` format and image-write.
- `PKEXEC_UID` is trusted as the caller; this holds for pkexec, which clears the environment. Running the
  helpers through `sudo` keeps the caller's environment; they are only installed for pkexec.

## 4. Running other programs

Programs are started with `fld::spawn()` and `fld::run_capture()` (argv, `execvp`). The places that still
use a shell:
- `fld::spawn_shell()` for user-defined commands (custom shortcuts, launcher commands, Exec lines with a
  shell): the user's own command, by design.
- `displays.cpp` starts Night Light with `sh -c "pgrep … || exec fl-nightlight"` (a constant string; to be
  replaced by a libfld helper, roadmap 2).
- **Fixed in this review:** fl-sysinfo ran `eglinfo`, `glxinfo` and `vulkaninfo` through `popen()` with a
  built shell string. It now uses `fld::run_capture()` with argv.

## 5. Parsers of untrusted input

Fuzz targets (`tests/fuzz`, see CONTRIBUTING.md) cover: `fld::Ini`, `fld::Json`, desktop entries and MIME
sniffing, Markdown (fl-docs), M3U/PLS playlists, wg-quick files, the RFB (VNC) client, and fl-wm's wm.ini.
ctest replays their seed corpora on every build; CI also runs the whole test suite with ASan and UBSan.

**Found by the fuzzer in this review:** the RFB client allocated the screen a server asked for, up to
65535 × 65535 pixels (17 GB), and waited for names and clipboard texts of up to 4 GB, so a hostile VNC
server could make fl-remote use all memory. Screens are now limited to 8192 × 8192, texts to 1 MB, and
pictures must lie on the screen (`apps/fl-remote/rfb.cpp`, tests in `test_remote.cpp`).

Not yet fuzzed: the EPUB and ODT readers of fl-docs (they live in the fl-docs executable; they need to move
into `fl-docs-util` first). `.ovpn` files are parsed by NetworkManager's plugins, not by fldesktop.

## 6. Build hardening

Builds use `-D_FORTIFY_SOURCE=3` (optimized builds), `-fstack-protector-strong`, full RELRO
(`-z relro -z now`) and PIE (`FLD_HARDENING`, on by default).
