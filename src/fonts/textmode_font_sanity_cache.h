#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace textmode_font
{
// Persistent cache of known-broken textmode fonts (FIGlet + TDF bundle entries).
//
// Goal:
// - First run (or after font pack changes): optionally do an expensive validation pass
//   (render "test" for every font) and record broken ids.
// - Subsequent runs: skip the validation pass entirely by reusing the cached results,
//   making startup/scan fast.
//
// Cache invalidation is based on `fonts_fingerprint` (a hash of filenames + metadata in
// assets/fonts/{flf,tdf}).
struct SanityCache
{
    // Bump if the cache schema or fingerprinting strategy changes.
    int schema_version = 1;

    // 64-bit fingerprint of the assets/fonts/{flf,tdf} directories (paths + stat metadata).
    std::uint64_t fonts_fingerprint = 0;

    // True if a full validation pass was completed for this fingerprint.
    // If false, callers may choose to re-validate.
    bool complete = false;

    // Stable ids of fonts that failed validation.
    // - FIGlet: "flf:<relative_path_without_ext>"
    // - TDF:    "tdf:<relative_path_without_ext>#<bundle_index>"
    std::vector<std::string> broken_ids;
};
} // namespace textmode_font


