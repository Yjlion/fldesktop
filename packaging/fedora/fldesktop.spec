# Fedora / RPM spec for fldesktop (one package; build from a checkout):
#   rpmbuild -bb packaging/fedora/fldesktop.spec --define "_sourcedir $PWD" \
#            --define "fld_version $(bash packaging/version.sh | sed -n 's/^FLD_PKGVER=//p')"
# Dependencies use pkgconfig() names so the spec follows the distribution's
# package names. fldesktop needs FLTK >= 1.4 and wlroots 0.18 - 0.20.
%{!?fld_version: %global fld_version 0.1.4}

Name:           fldesktop
Version:        %{fld_version}
Release:        1%{?dist}
Summary:        Lightweight Wayland desktop built with FLTK
License:        GPL-2.0-or-later
URL:            https://github.com/Yjlion/fldesktop

BuildRequires:  cmake ninja-build gcc-c++ pkgconf-pkg-config gettext
BuildRequires:  wayland-protocols-devel /usr/bin/wayland-scanner
BuildRequires:  fltk-devel >= 1.4
BuildRequires:  (pkgconfig(wlroots-0.19) or pkgconfig(wlroots-0.18) or pkgconfig(wlroots-0.20))
BuildRequires:  pkgconfig(wayland-server) pkgconfig(wayland-client) pkgconfig(xkbcommon) pkgconfig(libinput)
BuildRequires:  pkgconfig(pangocairo) pkgconfig(cairo) pkgconfig(librsvg-2.0) pkgconfig(libdrm) pkgconfig(pixman-1)
BuildRequires:  pkgconfig(vterm) pkgconfig(glib-2.0) pkgconfig(gio-2.0) pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(polkit-agent-1) pkgconfig(libnm) pkgconfig(fontconfig) pkgconfig(openssl)
BuildRequires:  pkgconfig(cups) pkgconfig(packagekit-glib2) pkgconfig(flatpak) pkgconfig(appstream)
BuildRequires:  pkgconfig(libjxl) pkgconfig(libarchive) pkgconfig(poppler-cpp) pkgconfig(libxml-2.0)
BuildRequires:  pkgconfig(wpe-webkit-2.0) pkgconfig(gstreamer-1.0) pkgconfig(gstreamer-video-1.0)
BuildRequires:  pkgconfig(gstreamer-app-1.0) pkgconfig(gstreamer-pbutils-1.0) pkgconfig(gstreamer-audio-1.0)
BuildRequires:  pkgconfig(egl) pkgconfig(glesv2) pam-devel
BuildRequires:  pkgconfig(avahi-client) pkgconfig(avahi-glib) pkgconfig(enchant-2)
BuildRequires:  pkgconfig(xcb) pkgconfig(xcb-icccm)
BuildRequires:  shared-mime-info xkeyboard-config

Requires:       xdg-desktop-portal shared-mime-info polkit
Recommends:     greetd xorg-x11-server-Xwayland pipewire wireplumber NetworkManager bluez cups
Recommends:     power-profiles-daemon gnome-keyring orca grim wl-clipboard

%description
fldesktop is a small desktop environment for Wayland made with the FLTK
toolkit: its own wlroots compositor (fl-wm), a panel, an application
launcher, wallpaper and desktop icons, a greetd greeter, a settings
center, system tools and a set of core applications.

%prep
# built from the checkout the spec lives in (_sourcedir)

%build
%cmake -S %{_sourcedir} -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFLD_PAM_STACK=system-auth \
  -DFLD_VERSION=%{version}
%cmake_build

%install
%cmake_install
# translations (po/), when there are any
%find_lang fldesktop || touch fldesktop.lang

%check
# the headless session tests need a seat and D-Bus; the unit tests run
%ctest -E "wm-smoke|session-keys|notify-smoke|portal-smoke|bt-mock|lock-smoke|xwayland-smoke"

%files -f fldesktop.lang
%license COPYING LICENSES/*
%doc README.md doc/ARCHITECTURE.md
%{_bindir}/*
%{_libexecdir}/fldesktop/
%{_libexecdir}/xdg-desktop-portal-fl
%{_datadir}/applications/*.desktop
%{_datadir}/wayland-sessions/fldesktop.desktop
%{_datadir}/fldesktop/
%{_datadir}/icons/*
%{_datadir}/pixmaps/faces/fldesktop/
%{_datadir}/xdg-desktop-portal/
%{_datadir}/dbus-1/services/*
%{_datadir}/polkit-1/actions/*
%{_datadir}/plymouth/themes/fldesktop/
%{_mandir}/man1/*
%{_includedir}/fldesktop/
%config(noreplace) %{_sysconfdir}/pam.d/*
%config(noreplace) %{_sysconfdir}/xdg/autostart/*

%changelog
* Wed Nov 25 2026 fldesktop contributors - 0.1.4-1
- First Fedora spec.
