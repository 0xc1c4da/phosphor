{ pkgs
, lib ? pkgs.lib
, automergeSrc ? (pkgs.fetchgit {
    url = "https://github.com/automerge/automerge.git";
    # Matches the vendored copy currently present in this repo under references/p2p/automerge
    # (refs/heads/main in that checkout).
    rev = "7861cb7f16f61a75c8ccef623ac53a9451dddaec";
    hash = "sha256-KoZtypzYXoqnPBfu+Oty1UVbiMBff+Mx+bAk6BbXPoo=";
    fetchSubmodules = false;
  })
, version ? "0.3.0"
, cargoVendorHash ? "sha256-WI3Se3z1mvnR9dLxbx+a8sx3+AYgQOv0ZWn2SRtle0w="
}:

let
  # Cargo workspace is rooted at ./rust. The automerge-c build (CMake) runs cargo
  # from rust/automerge-c but it still uses the workspace lockfile and config.
  #
  # We vendor dependencies in a fixed-output derivation (like buildRustPackage does)
  # and inject rust/Cargo.lock + rust/vendor + rust/.cargo/config.toml before building,
  # so the real build can run fully offline.
  automergeCargoVendor = pkgs.stdenv.mkDerivation {
    pname = "automerge-cargo-vendor";
    inherit version;
    src = automergeSrc;

    nativeBuildInputs = [
      pkgs.cargo
      pkgs.rustc
      pkgs.git
      pkgs.cacert
    ];

    outputHashMode = "recursive";
    outputHashAlgo = "sha256";
    outputHash = cargoVendorHash;

    # Fixed-output derivations must not be rewritten to reference store paths.
    # The default fixupPhase patches shebangs inside vendored crates, which would
    # introduce /nix/store references and break fixed-output requirements.
    dontFixup = true;
    dontPatchShebangs = true;

    buildPhase = ''
      runHook preBuild

      export HOME="$TMPDIR/home"
      mkdir -p "$HOME"

      export CARGO_HOME="$TMPDIR/cargo-home"
      mkdir -p "$CARGO_HOME"

      # Help cargo/curl find CA certs inside the Nix sandbox.
      export SSL_CERT_FILE="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
      export CARGO_HTTP_CAINFO="$SSL_CERT_FILE"

      # Generate a workspace lockfile and vendor all crates.
      cargo generate-lockfile --manifest-path rust/Cargo.toml
      mkdir -p rust/.cargo
      rm -rf rust/vendor
      cargo vendor --manifest-path rust/Cargo.toml rust/vendor > rust/.cargo/config.toml

      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall

      mkdir -p "$out/rust"
      cp -v rust/Cargo.lock "$out/rust/Cargo.lock"
      mkdir -p "$out/rust/.cargo"
      cp -v rust/.cargo/config.toml "$out/rust/.cargo/config.toml"
      cp -r rust/vendor "$out/rust/vendor"

      runHook postInstall
    '';
  };
in
pkgs.stdenv.mkDerivation {
  pname = "automerge-c";
  inherit version;
  src = automergeSrc;

  # CMake project lives in a subdir of the repo.
  # cmakeConfigurePhase runs from an out-of-source build dir, so use a path relative to it.
  cmakeDir = "../rust/automerge-c";

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.cargo
    pkgs.rustc
    pkgs.git
  ];

  # Needed for libdl, pthreads, libm; CMake finds these itself but downstream links need them.
  buildInputs = [ ];

  postPatch = ''
    # Avoid requiring rustup/nightly and unstable -Z build-std flags.
    # Keep release builds, but make them work on stable cargo/rustc.
    #
    # NOTE: Avoid writing the antiquotation opener in this snippet; Nix would try to evaluate it inside strings.
    substituteInPlace rust/automerge-c/CMakeLists.txt \
      --replace 'set(RUSTUP_TOOLCHAIN nightly)' 'set(RUSTUP_TOOLCHAIN "")'

    substituteInPlace rust/automerge-c/CMakeLists.txt \
      --replace '-Z build-std=std,panic_abort --release' '--release'

    # Ensure cargo respects our generated lockfile.
    substituteInPlace rust/automerge-c/CMakeLists.txt \
      --replace '${"$"}{CARGO_CMD} build --locked ' '${"$"}{CARGO_CMD} build --offline '
  '';

  preConfigure = ''
    export HOME="$TMPDIR/home"
    mkdir -p "$HOME"

    export CARGO_HOME="$TMPDIR/cargo-home"
    mkdir -p "$CARGO_HOME"

    # Force offline build (deps are vendored).
    export CARGO_NET_OFFLINE=true

    # Inject the vendored workspace deps into rust/ (workspace root).
    cp -f "${automergeCargoVendor}/rust/Cargo.lock" rust/Cargo.lock
    rm -rf rust/vendor rust/.cargo
    cp -r "${automergeCargoVendor}/rust/vendor" rust/vendor
    mkdir -p rust/.cargo
    cp -f "${automergeCargoVendor}/rust/.cargo/config.toml" rust/.cargo/config.toml

    # The generated config refers to "rust/vendor" (relative to the workspace root),
    # but Cargo resolves it relative to the workspace root already, so it should just be "vendor".
    substituteInPlace rust/.cargo/config.toml \
      --replace 'directory = "rust/vendor"' 'directory = "vendor"'

    # Nix sources may be unpacked read-only; Cargo needs to update Cargo.lock in-place.
    chmod -R u+rwX rust
  '';

  cmakeFlags = [
    "-GNinja"
    "-DCMAKE_BUILD_TYPE=Release"
    "-DBUILD_TESTING=OFF"
    # Keep static by default; downstream Phosphor just links the archive.
    "-DBUILD_SHARED_LIBS=OFF"

    # Upstream's CMakeLists expects GNUInstallDirs-style relative install dirs
    # (e.g. include/, lib/). Nix's cmake hooks pass absolute *DIR values which
    # breaks their header FILE_SET paths at configure time, so override to relative.
    "-DCMAKE_INSTALL_INCLUDEDIR=include"
    "-DCMAKE_INSTALL_LIBDIR=lib"
  ];

  # Use upstream install() rules, then add pkg-config metadata.
  postInstall = ''
    mkdir -p "$out/lib/pkgconfig"

    cat > "$out/lib/pkgconfig/automerge-c.pc" <<'EOF'
prefix=@out@
exec_prefix=${"$"}{prefix}
libdir=${"$"}{exec_prefix}/lib
includedir=${"$"}{prefix}/include

Name: automerge-c
Description: Automerge C API (automerge-c)
Version: @version@
Cflags: -I${"$"}{includedir}
Libs: -L${"$"}{libdir} -lautomerge -lm -pthread -ldl
EOF
    sed -i \
      -e "s|@out@|$out|g" \
      -e "s|@version@|${version}|g" \
      "$out/lib/pkgconfig/automerge-c.pc"
  '';

  meta = with lib; {
    description = "automerge-c: C API bindings for the Automerge Rust library";
    homepage = "https://github.com/automerge/automerge";
    license = licenses.mit;
    platforms = platforms.unix;
  };
}


