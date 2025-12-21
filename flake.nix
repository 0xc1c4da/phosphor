{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
    imgui = {
      url = "github:ocornut/imgui/dc48a7c88eef4a347000d55062d35cc94e00ef70";
      flake = false;
    };
    lodepng = {
      url = "github:lvandeve/lodepng/d41d4aa8c63dea277e25c94ad85046b6c5335ccc";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, imgui, lodepng }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config = {
            allowUnfree = true;
          };
        };

        phosphorBuildInputs = with pkgs; [
          sdl3
          vulkan-headers
          vulkan-loader
          shaderc
          nlohmann_json
          chafa
          stb
          curl
          glib
          icu67.dev
          luajit
          zstd
          libnoise
          md4c
        ];

        phosphor = pkgs.stdenv.mkDerivation {
          pname = "phosphor";
          version = "0.0.0";

          src = self;

          nativeBuildInputs = with pkgs; [
            binutils
            gnumake
            pkg-config
            gnutar
            zstd
          ];

          buildInputs = phosphorBuildInputs;

          # libnoise in nixpkgs is static-only; the Makefile expects these env vars.
          preBuild = ''
            export LIBNOISE_CFLAGS="-I${pkgs.libnoise}/include"
            export LIBNOISE_LIBS="-L${pkgs.libnoise}/lib -lnoise-static -lnoiseutils-static"
          '';

          makeFlags = [
            "CXX=${pkgs.stdenv.cc}/bin/c++"
            "IMGUI_DIR=${imgui}"
            "LDEPNG_DIR=${lodepng}"
          ];

          installPhase = ''
            runHook preInstall

            mkdir -p $out/bin
            install -m755 phosphor $out/bin/phosphor

            runHook postInstall
          '';

          meta = {
            mainProgram = "phosphor";
          };
        };

        devShellPackages = with pkgs; [
          gnumake
          gdb
          pkg-config
          gcc
        ] ++ phosphorBuildInputs;
      in
      {
        packages = {
          phosphor = phosphor;
          default = phosphor;
        };

        apps = {
          phosphor = {
            type = "app";
            program = "${phosphor}/bin/phosphor";
          };
          default = {
            type = "app";
            program = "${phosphor}/bin/phosphor";
          };
        };

        devShells.default = pkgs.mkShell {
          buildInputs = devShellPackages;

          shellHook = ''
            export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath devShellPackages}:$LD_LIBRARY_PATH
            export IMGUI_DIR=${imgui}
            export LDEPNG_DIR=${lodepng}

            # Provide libnoise
            export LIBNOISE_CFLAGS="-I${pkgs.libnoise}/include"
            export LIBNOISE_LIBS="-L${pkgs.libnoise}/lib -lnoise-static -lnoiseutils-static"

            # Project helpers
            export PHOSPHOR_PROJECT_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
            alias nuke_assets='(cd "$PHOSPHOR_PROJECT_ROOT" && rm -rf ~/.config/phosphor/assets/ && cp -R assets/ ~/.config/phosphor/assets)'
          '';
        };
      }
    );
}


