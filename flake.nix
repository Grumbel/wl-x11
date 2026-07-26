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
          ];

          # nixpkgs' `wlroots` currently tracks the 0.18.x series, which is
          # what this compositor is written against. If your nixpkgs pin
          # ships a different wlroots major version, the wlr_output_state /
          # wlr_scene_* calls in src/main.c may need small adjustments -- see
          # README.md.
          buildInputs = with pkgs; [
            wlroots
            wayland
            wayland-protocols
            libxkbcommon
            pixman
            libxcb
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
          # runCommand sandbox is read-only on $src; copy so reuse can
          # walk the tree the same way a checkout would.
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
            gdb
            reuse
          ];
          buildInputs = with pkgs; [
            wlroots
            wayland
            wayland-protocols
            libxkbcommon
            pixman
            libxcb
          ];

          shellHook = ''
            echo "wl-x11 dev shell. Build with: meson setup build && ninja -C build"
            echo "Run with:   DISPLAY=:0 ./build/wl-x11"
            echo "REUSE lint: reuse lint"
          '';
        };
      });
}
