{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
    imgui = {
      url = "github:ocornut/imgui/v1.91.4";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, imgui }:
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
          glib
          icu67.dev
          luajit
          zstd
          libnoise
        ];

        phosphorSrc = pkgs.lib.cleanSourceWith {
          src = ./.;
          filter = path: type:
            let
              root = toString ./. + "/";
              rel = pkgs.lib.removePrefix root (toString path);
            in
              # Always keep the flake metadata / build files
              rel == "flake.nix" ||
              rel == "flake.lock" ||
              rel == "Makefile" ||
              rel == "README.md" ||
              # Keep source + vendored deps + runtime assets (embedded at build time)
              pkgs.lib.hasPrefix "src/" rel ||
              pkgs.lib.hasPrefix "vendor/" rel ||
              pkgs.lib.hasPrefix "assets/" rel ||
              # Exclude build outputs / large reference material / nix result symlinks
              !(pkgs.lib.hasPrefix "build/" rel) &&
              !(pkgs.lib.hasPrefix "result" rel) &&
              !(pkgs.lib.hasPrefix "references/" rel) &&
              rel != "phosphor";
        };

        phosphor = pkgs.stdenv.mkDerivation {
          pname = "phosphor";
          version = "0.0.0";

          src = phosphorSrc;

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

            # Flakes don't automatically include git submodule contents in the source tree.
            # Ensure Dear ImGui sources are present at vendor/imgui for the Makefile build.
            rm -rf vendor/imgui
            mkdir -p vendor
            cp -r ${imgui} vendor/imgui
          '';

          makeFlags = [
            "CXX=${pkgs.stdenv.cc}/bin/c++"
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

            # Provide libnoise
            export LIBNOISE_CFLAGS="-I${pkgs.libnoise}/include"
            export LIBNOISE_LIBS="-L${pkgs.libnoise}/lib -lnoise-static -lnoiseutils-static"
          '';
        };
      }
    );
}


