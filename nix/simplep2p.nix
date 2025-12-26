{ pkgs
, lib ? pkgs.lib
, simplep2pSrc ? (pkgs.fetchFromGitHub {
    owner = "joshuadahlunr";
    repo = "simpleP2P";
    # Matches the vendored copy currently present in this repo under references/p2p/simpleP2P
    # (refs/heads/master in that checkout).
    rev = "5f384830a99789bc2ea97523051992a5e114575b";
    # Placeholder; first build will fail and print the correct SRI hash.
    hash = "sha256-cby4MiVt/CMgpThplRoqmKZE71ndHm+CxIQejVBSqJk=";
    fetchSubmodules = true;
  })
, version ? "0.0.0"

  # simpleP2P upstream generates go.mod/go.sum and runs `go get ...` during the CMake build.
  # In Nix, we want this to be deterministic and offline, so we precompute a vendor/ tree in
  # a fixed-output derivation and inject it before configuring CMake.
, libp2pVersion ? null
, vendorHash ? "sha256-nug62JkqnuG/s0VQrIUTyFsFJZBRMCLUApprQvpDQUs="
}:

let
  libp2pVersionStr = if libp2pVersion == null then "" else libp2pVersion;

  simplep2pGoVendor = pkgs.stdenv.mkDerivation {
    pname = "simplep2p-go-vendor";
    src = simplep2pSrc;
    inherit version;

    nativeBuildInputs = [
      pkgs.go
      pkgs.git
    ];

    # Fixed-output derivation. First build will fail with the correct hash.
    outputHashMode = "recursive";
    outputHashAlgo = "sha256";
    outputHash = vendorHash;

    buildPhase = ''
      runHook preBuild

      export HOME="$TMPDIR/home"
      mkdir -p "$HOME"

      # Work on a writable copy of just the Go sources.
      cp -r "${simplep2pSrc}/src" ./mod
      chmod -R u+rwX ./mod
      cd ./mod

      # Create a module (upstream normally does this in CMake).
      if [ ! -f go.mod ]; then
        go mod init simplep2p
      fi

      if [ -n "${libp2pVersionStr}" ]; then
        go get "github.com/libp2p/go-libp2p@${libp2pVersionStr}"
      else
        go get github.com/libp2p/go-libp2p
      fi

      go mod tidy -e
      go mod vendor

      # Ensure subsequent phases start from the derivation build root.
      cd ..

      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      mkdir -p "$out"
      cp -r ./mod/vendor "$out/vendor"
      cp ./mod/go.mod ./mod/go.sum "$out/"
      runHook postInstall
    '';
  };
in
pkgs.stdenv.mkDerivation {
  pname = "simplep2p";
  src = simplep2pSrc;
  inherit version;

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.go
  ];

  # The upstream build defines a custom target that always runs `go get ...` (network access).
  # Replace it with a no-op; we inject vendor/ and set -mod=vendor.
  postPatch = ''
    substituteInPlace cmake/golang.cmake \
      --replace 'add_custom_target(${"$"}{TARG} ${"$"}{GO_ENV} ${"$"}{CMAKE_Go_COMPILER} get ${"$"}{ARGN})' \
                'add_custom_target(${"$"}{TARG} COMMAND ${"$"}{CMAKE_COMMAND} -E echo "nix: skipping go get, deps vendored")'

    # Upstream also generates go.mod/go.sum as build steps. In Nix we pre-provide these
    # (and vendor/), so make those steps no-ops to avoid mutating the module or failing
    # when go.mod already exists.
    substituteInPlace cmake/golang.cmake \
      --replace 'COMMAND  ${"$"}{GO_ENV} ${"$"}{CMAKE_Go_COMPILER} mod init ${"$"}{SOURCE_FILE_NO_PATH}' \
                'COMMAND  ${"$"}{CMAKE_COMMAND} -E echo "nix: go.mod provided"'

    substituteInPlace cmake/golang.cmake \
      --replace 'COMMAND  ${"$"}{GO_ENV} ${"$"}{CMAKE_Go_COMPILER} mod tidy -e' \
                'COMMAND  ${"$"}{CMAKE_COMMAND} -E echo "nix: go.sum provided"'
  '';

  preConfigure = ''
    export HOME="$TMPDIR/home"
    mkdir -p "$HOME"

    # Inject go.mod/go.sum + vendor/ at the repository root before CMake config.
    # Upstream's CMake generates go.mod/go.sum in the top-level source dir and runs `go build`
    # from there, so vendor/ must also live at the top-level.
    cp -f "${simplep2pGoVendor}/go.mod" ./go.mod
    cp -f "${simplep2pGoVendor}/go.sum" ./go.sum
    rm -rf ./vendor
    cp -r "${simplep2pGoVendor}/vendor" ./vendor

    export GOFLAGS="-mod=vendor -trimpath"
    export GOPROXY=off
    export GOSUMDB=off
    export GONOSUMDB="*"
    export CGO_ENABLED=1
  '';

  cmakeFlags = [
    "-GNinja"
    "-DCMAKE_BUILD_TYPE=Release"
    "-DBUILD_EXAMPLES=OFF"

    # Used by cmake/golang.cmake when invoking go build
    "-DCMAKE_GO_FLAGS=-mod=vendor"
  ];

  installPhase = ''
    runHook preInstall

    mkdir -p "$out/lib" "$out/include" "$out/lib/pkgconfig"

    # Libraries (names come from upstream CMake targets)
    if [ -f "libsimplep2p.a" ]; then
      cp -v "libsimplep2p.a" "$out/lib/"
    fi
    if [ -f "libsimplep2p_golib.a" ]; then
      cp -v "libsimplep2p_golib.a" "$out/lib/"
    fi

    # Go c-archive typically produces a header next to the archive.
    if [ -f "libsimplep2p_golib.h" ]; then
      cp -v "libsimplep2p_golib.h" "$out/include/"
    fi

    # Public headers
    cp -v ../src/simplep2p.h ../src/simplep2p.hpp ../src/delegate.hpp "$out/include/"

    # pkg-config metadata (so downstream can do: pkg-config --cflags --libs simplep2p)
    cat > "$out/lib/pkgconfig/simplep2p.pc" <<'EOF'
prefix=@out@
exec_prefix=${"$"}{prefix}
libdir=${"$"}{exec_prefix}/lib
includedir=${"$"}{prefix}/include

Name: simplep2p
Description: simpleP2P C/C++ wrapper around go-libp2p (includes go c-archive)
Version: @version@
Cflags: -I${"$"}{includedir}
Libs: -L${"$"}{libdir} -lsimplep2p -lsimplep2p_golib -pthread -ldl
EOF
    sed -i \
      -e "s|@out@|$out|g" \
      -e "s|@version@|${version}|g" \
      "$out/lib/pkgconfig/simplep2p.pc"

    runHook postInstall
  '';

  meta = with lib; {
    description = "simpleP2P: minimalistic C/C++ wrapper around the Go implementation of libp2p";
    homepage = "https://github.com/joshuadahlunr/simpleP2P";
    license = licenses.mit;
    platforms = platforms.unix;
  };
}
