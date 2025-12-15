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
          zstd
          libnoise
        ];
      in
      {
        devShells.default = pkgs.mkShell {
          buildInputs = devShellPackages;

          shellHook = ''
            export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath devShellPackages}:$LD_LIBRARY_PATH

            # Provide libnoise
            export LIBNOISE_CFLAGS="-I${pkgs.libnoise}/include"
            export LIBNOISE_LIBS="-L${pkgs.libnoise}/lib -lnoise-static -lnoiseutils-static"
          '';
        };
      }
    );
}


