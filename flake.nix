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
        inherit (pkgs) lib;

        # Shared deps for building our patched wlroots (X11 backend only).
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
          libxcb-wm
          libxcb-render-util
          libxcb-image
          libxcb-errors
        ];

        # Separate derivation so edits to compositor src/*.c do not rebuild
        # the whole of wlroots. Only changes under subprojects/wlroots/ do.
        wlroots = pkgs.stdenv.mkDerivation {
          pname = "wlroots-wl-x11";
          version = "0.21.0-dev";
          src = ./subprojects/wlroots;

          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            wayland-scanner
          ];

          buildInputs = wlrootsBuildInputs;

          mesonFlags = [
            "-Dbackends=x11"
            "-Dsession=disabled"
            "-Dexamples=false"
            "-Dtests=false"
            "-Dxwayland=disabled"
            "-Drenderers=gles2"
            "-Dallocators=gbm"
            "-Dcolor-management=disabled"
            # Do not set auto_features=disabled: that empties allocators=auto
            # and leaves no buffer allocator at runtime.
          ];

          # Consumers need the .pc and headers at build time.
          # meson installs libwlroots-0.21.so + wlroots-0.21.pc by default.
        };

        # Compositor sources without the heavy subproject tree so the
        # derivation hash only changes when compositor files change.
        wlX11Src = lib.cleanSourceWith {
          src = ./.;
          filter = path: type:
            let base = baseNameOf path; in
            # Drop the vendored tree (built as packages.wlroots) and VCS noise.
            !(lib.hasInfix "/subprojects/" path)
            && base != ".git"
            && base != "result"
            && base != "build";
        };

        wl-x11 = pkgs.stdenv.mkDerivation {
          pname = "wl-x11";
          version = "0.1.0";
          src = wlX11Src;

          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
          ];

          buildInputs = wlrootsBuildInputs ++ [ wlroots ];

          meta = with lib; {
            description = "Minimal wlroots-based Wayland compositor nested in X11, one X11 window per Wayland toplevel";
            homepage = "https://example.invalid/wl-x11";
            license = licenses.gpl3Plus;
            platforms = platforms.linux;
            mainProgram = "wl-x11";
          };
        };
      in
      {
        packages = {
          default = wl-x11;
          inherit wl-x11 wlroots;
        };

        apps.default = {
          type = "app";
          program = "${wl-x11}/bin/wl-x11";
        };

        checks.reuse = pkgs.runCommand "wl-x11-reuse-lint" {
          nativeBuildInputs = [ pkgs.reuse ];
          src = wlX11Src;
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
          # Provide the prebuilt patched wlroots so local meson setup
          # picks it up via pkg-config instead of rebuilding the subproject.
          buildInputs = wlrootsBuildInputs ++ [ wlroots ];

          shellHook = ''
            echo "wl-x11 dev shell."
            echo "  packages.wlroots = patched X11-only wlroots (cached separately)"
            echo "  packages.wl-x11   = compositor only"
            echo "Build compositor: meson setup build && ninja -C build"
            echo "Rebuild wlroots:  nix build .#wlroots"
          '';
        };
      });
}
