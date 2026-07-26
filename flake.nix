# SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
{
  description = "wl-x11: a minimal wlroots Wayland compositor nested under X11, mapping one Wayland toplevel to one X11 window";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Same set nixpkgs uses for pkgs.wlroots, minus libinput/seatd/vulkan/
        # xwayland (disabled via mesonFlags). Attribute names match
        # pkgs/development/libraries/wlroots/default.nix on nixos-unstable.
        wlrootsBuildInputs = with pkgs; [
          wayland
          wayland-protocols
          libxkbcommon
          pixman
          libdrm
          libGL
          libgbm
          libx11
          libxcb
          # These may be named libxcb-* or xorg.xcbutil* depending on pin;
          # try the names from current nixpkgs wlroots package first.
          libxcb-wm
          libxcb-render-util
          libxcb-image
          libxcb-errors
          lcms2
        ];
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "wl-x11";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            wayland-scanner
          ];

          buildInputs = wlrootsBuildInputs;

          mesonFlags = [
            "-Dwlroots:backends=x11"
            "-Dwlroots:session=disabled"
            "-Dwlroots:examples=false"
            "-Dwlroots:tests=false"
            "-Dwlroots:xwayland=disabled"
            "-Dwlroots:renderers=gles2"
            "-Dwlroots:color-management=disabled"
          ];

          meta = with pkgs.lib; {
            description = "Minimal wlroots-based Wayland compositor nested in X11, one X11 window per Wayland toplevel";
            homepage = "https://example.invalid/wl-x11";
            license = licenses.gpl3Plus;
            platforms = platforms.linux;
            mainProgram = "wl-x11";
          };
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/wl-x11";
        };

        checks.reuse = pkgs.runCommand "wl-x11-reuse-lint" {
          nativeBuildInputs = [ pkgs.reuse ];
          src = ./.;
        } ''
          cp -r "$src" ./src
          chmod -R u+w ./src
          cd ./src
          reuse lint
          touch "$out"
        '';

        devShells.default = pkgs.mkShell {
          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            wayland-scanner
            gdb
            reuse
          ];
          buildInputs = wlrootsBuildInputs;

          shellHook = ''
            echo "wl-x11 dev shell (vendored subprojects/wlroots)."
            echo "See flake.nix mesonFlags for suggested -Dwlroots:* options."
          '';
        };
      });
}
