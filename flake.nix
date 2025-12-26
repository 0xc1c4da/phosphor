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

        simplep2p = pkgs.callPackage ./nix/simplep2p.nix { };

        baseVersion = pkgs.lib.strings.removeSuffix "\n" (builtins.readFile ./VERSION);

        # Deterministic build identifier for reproducible builds:
        # - Prefer git revision when available in flake metadata.
        # - Otherwise fall back to the source narHash (always present).
        shortRev =
          if self ? sourceInfo && self.sourceInfo ? shortRev && self.sourceInfo.shortRev != null
          then self.sourceInfo.shortRev
          else if self ? shortRev && self.shortRev != null
          then self.shortRev
          else null;
        narHash =
          if self ? sourceInfo && self.sourceInfo ? narHash && self.sourceInfo.narHash != null
          then self.sourceInfo.narHash
          else null;
        narId =
          if narHash != null
          then pkgs.lib.strings.removePrefix "sha256-" narHash
          else "unknown";
        buildId =
          if shortRev != null
          then "g${shortRev}"
          else "nar${builtins.substring 0 8 narId}";
        phosphorVersionStr = "${baseVersion}+${buildId}";

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
          icu67
          icu67.dev
          luajit
          zstd
          libnoise
          md4c
          libblake3
          libsixel
        ];

        phosphor = pkgs.stdenv.mkDerivation {
          pname = "phosphor";
          version = baseVersion;

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
            "PHOSPHOR_VERSION_STR=${phosphorVersionStr}"
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
          python3
          simplep2p
        ] ++ phosphorBuildInputs;
      in
      {
        packages = {
          phosphor = phosphor;
          simplep2p = simplep2p;
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


