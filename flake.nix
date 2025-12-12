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
          pkg-config
          gcc
          sdl3
          vulkan-headers
          vulkan-loader
          shaderc
          imgui
        ];
      in
      rec {
        devShell = pkgs.mkShell {
          buildInputs = devShellPackages;

          shellHook = ''
            export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath devShellPackages}:$LD_LIBRARY_PATH
          '';
        };
      }
    );
}


