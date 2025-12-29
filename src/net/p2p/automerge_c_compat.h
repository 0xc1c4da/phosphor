// C++ compat wrapper for automerge-c headers.
//
// Some automerge-c headers (notably utils/stack.h) are plain C headers without
// `extern "C"` guards. When included from C++, that gives the declarations C++
// linkage (mangled names) which then fails to link against the automerge-c
// static library (which exports C symbols).
//
// Include this header instead of including automerge-c headers directly from
// C++.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <automerge-c/automerge.h>
#include <automerge-c/utils/stack.h>

#ifdef __cplusplus
} // extern "C"
#endif


