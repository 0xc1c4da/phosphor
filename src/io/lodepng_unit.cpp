// Single translation unit that compiles LodePNG.
//
// LodePNG is provided by our Nix flake input (exported as $LDEPNG_DIR and passed
// to the compiler via -I$(LDEPNG_DIR) in the Makefile).
//
// We compile the upstream implementation as C++ by including it here, so we
// don't need a separate build rule for external source paths.
//
// Do not include "lodepng.cpp" from anywhere else.
#include "lodepng.cpp"


