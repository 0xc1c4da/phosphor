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
          icu67.dev
          luajit
        ];
      in
      {
        devShells.default = pkgs.mkShell {
          buildInputs = devShellPackages;

          shellHook = ''
            export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath devShellPackages}:$LD_LIBRARY_PATH
          '';
        };
      }
    );
}


