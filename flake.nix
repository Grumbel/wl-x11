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

        # Source of truth: top-level VERSION (e.g. "0.1.0-dev"). Append short
        # git rev when building from a flake checkout; see AGENTS.md.
        versionBase = lib.strings.removeSuffix "\n" (builtins.readFile ./VERSION);
        gitRev = "${self.shortRev or self.dirtyShortRev or "dirty"}";
        version = "${versionBase}+g${gitRev}";

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
          inherit version;
          src = wlX11Src;

          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
          ];

          buildInputs = wlrootsBuildInputs ++ [ wlroots ];

          # Source is filtered without subprojects/; meson falls back to the
          # patched packages.wlroots via pkg-config (see meson.build).
          # version_full embeds VERSION+g<rev> into the binary (--version).
          mesonFlags = [
            "-Duse_system_wlroots=true"
            "-Dversion_full=${version}"
          ];

          meta = with lib; {
            description = "Minimal wlroots-based Wayland compositor nested in X11, one X11 window per Wayland toplevel";
            homepage = "https://example.invalid/wl-x11";
            license = licenses.gpl3Plus;
            platforms = platforms.linux;
            mainProgram = "wl-x11";
          };
        };
        resize-torture = pkgs.stdenv.mkDerivation {
          pname = "wl-x11-resize-torture";
          inherit version;
          src = lib.cleanSourceWith {
            src = ./tools/resize-torture;
            filter = path: type:
              let base = baseNameOf path; in
              base != ".git" && base != "build" && base != "result";
          };
          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            wayland-scanner
          ];
          buildInputs = with pkgs; [
            wayland
            wayland-protocols
          ];
          meta = with lib; {
            description = "xdg-shell resize stress client for wl-x11";
            license = licenses.gpl3Plus;
            platforms = platforms.linux;
            mainProgram = "resize-torture";
          };
        };
      in
      {
        packages = {
          default = wl-x11;
          inherit wl-x11 wlroots resize-torture;
        };

        apps.default = {
          type = "app";
          program = "${wl-x11}/bin/wl-x11";
        };

        apps.resize-torture = {
          type = "app";
          program = "${resize-torture}/bin/resize-torture";
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
            echo "  packages.resize-torture = xdg-shell resize stress client"
            echo "Build compositor against the Nix wlroots package:"
            echo "  meson setup build -Duse_system_wlroots=true && ninja -C build"
            echo "Or use the in-tree subproject (no flag) if present:"
            echo "  meson setup build && ninja -C build"
            echo "Rebuild wlroots:  nix build .#wlroots"
          '';
        };
      });
}
