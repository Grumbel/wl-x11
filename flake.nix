{
  description = "wc-x11: a minimal wlroots Wayland compositor nested under X11, mapping one Wayland toplevel to one X11 window";

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
          pname = "wc-x11";
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
            xcb-util-cursor
          ];

          meta = with pkgs.lib; {
            description = "Minimal wlroots-based Wayland compositor nested in X11, one X11 window per Wayland toplevel";
            homepage = "https://example.invalid/wc-x11";
            license = licenses.mit;
            platforms = platforms.linux;
            mainProgram = "wc-x11";
          };
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/wc-x11";
        };

        devShells.default = pkgs.mkShell {
          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            gdb
          ];
          buildInputs = with pkgs; [
            wlroots
            wayland
            wayland-protocols
            libxkbcommon
            pixman
            libxcb
            xcb-util-cursor
          ];

          shellHook = ''
            echo "wc-x11 dev shell. Build with: meson setup build && ninja -C build"
            echo "Run with:   DISPLAY=:0 ./build/wc-x11"
          '';
        };
      });
}
