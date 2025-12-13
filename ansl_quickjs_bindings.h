#pragma once

#include <string>

struct JSContext;

// Registers a global `ANSL` object matching ansl/src/index.js (DOM-free subset).
// Returns false and sets `error` on failure.
bool RegisterAnslNativeQuickJS(JSContext* ctx, std::string& error);


