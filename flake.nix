{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config = {
            allowUnfree = true;
          };
        };

        devShellPackages = with pkgs; [
          gnumake
          gdb
          pkg-config
          gcc
          nodejs
          esbuild
          sdl3
          vulkan-headers
          vulkan-loader
          shaderc
          imgui
          nlohmann_json
          chafa
          stb
          glib
          quickjs
          icu67.dev
          luajit
        ];
      in
      {
        devShells.default = pkgs.mkShell {
          buildInputs = devShellPackages;

          shellHook = ''
            export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath devShellPackages}:$LD_LIBRARY_PATH
            export NIX_LDFLAGS="-L${pkgs.quickjs}/lib/quickjs $NIX_LDFLAGS"
            export NIX_CFLAGS_COMPILE="-I${pkgs.quickjs}/include -I${pkgs.quickjs}/include/quickjs $NIX_CFLAGS_COMPILE"
          '';
        };
      }
    );
}


