// Central version string for Phosphor.
//
// Build systems are expected to define PHOSPHOR_VERSION_STR (e.g. via -D).
// We provide a safe fallback for local builds and IDE indexing.
#pragma once

#ifndef PHOSPHOR_VERSION_STR
#define PHOSPHOR_VERSION_STR "0.0.0+unknown"
#endif


