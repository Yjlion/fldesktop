{
  # Nix flake for fldesktop:
  #   nix build            the desktop (result/bin/fl-wm, fl-session, ...)
  #   nix develop          a shell with the build dependencies
  # A display manager starts result/share/wayland-sessions/fldesktop.desktop;
  # on NixOS add the package to environment.systemPackages and
  # services.displayManager.sessionPackages.
  description = "Lightweight Wayland desktop built with FLTK";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in {
      packages = forAll (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "fldesktop";
          version = "0.1.4";
          src = self;
          nativeBuildInputs = with pkgs; [ cmake ninja pkg-config gettext wayland-scanner python3 wrapGAppsHook3 ];
          buildInputs = with pkgs; [
            fltk14 wlroots_0_19 wayland wayland-protocols libxkbcommon libinput pango cairo librsvg libdrm
            pixman libvterm-neovim glib pipewire polkit networkmanager fontconfig openssl cups packagekit
            flatpak appstream libjxl libarchive poppler libxml2 wpewebkit gst_all_1.gstreamer
            gst_all_1.gst-plugins-base gst_all_1.gst-plugins-good libGL libglvnd pam avahi enchant
            xorg.libxcb xorg.xcbutilwm shared-mime-info xkeyboard_config
          ];
          cmakeFlags = [
            "-DFLD_VERSION=0.1.4"
            "-DFLD_PAM_STACK=login"
          ];
          # the session tests need a seat and D-Bus
          doCheck = false;
          passthru.providedSessions = [ "fldesktop" ];
          meta = with pkgs.lib; {
            description = "Lightweight Wayland desktop built with FLTK";
            homepage = "https://github.com/Yjlion/fldesktop";
            license = licenses.gpl2Plus;
            platforms = platforms.linux;
            mainProgram = "fl-session";
          };
        };
      });
      devShells = forAll (pkgs: {
        default = pkgs.mkShell { inputsFrom = [ self.packages.${pkgs.system}.default ]; };
      });
    };
}
