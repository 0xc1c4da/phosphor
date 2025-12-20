#pragma once

#include "fonts/textmode_font.h"
#include "fonts/textmode_font_sanity_cache.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace textmode_font
{
// Registry of FIGlet/TDF fonts discovered under the Phosphor assets directory.
//
// This is intended to back the Lua tool API (ansl.font.*):
// - `Scan()` loads and indexes all fonts from disk (assets/fonts/{flf,tdf})
// - `List()` returns metadata suitable for UI drop-downs
// - `Render()` renders UTF-8 text by stable id
//
// Note: ids are stable strings:
//  - FIGlet: "flf:<relative_path_without_ext>"
//  - TDF:    "tdf:<relative_path_without_ext>#<bundle_index>"
struct RegistryEntry
{
    std::string id;
    std::string label;
    FontMeta meta;
};

class Registry
{
public:
    // Scans assets/fonts/{flf,tdf}. Returns true if at least one font was loaded.
    // On partial failures, still returns true but records errors (see Errors()).
    bool Scan(const std::string& assets_dir, std::string& out_error);

    struct ScanOptions
    {
        // If true, perform an expensive validation pass when the cache is missing/stale:
        // render `validate_text` for every discovered font and record broken ids.
        //
        // If the cache is valid for the current assets fingerprint, validation is skipped.
        bool validate_if_cache_miss = false;

        // If true and a valid cache is available, omit cached-broken fonts from List()/Render().
        bool filter_broken_fonts = false;

        // Text used for validation renders.
        std::string validate_text = "test";
    };

    // Scan with optional persistent cache:
    // - Computes a fingerprint of assets/fonts/{flf,tdf}
    // - If `cache` is valid, can skip validation and/or filter broken fonts
    // - If cache is missing/stale and validate_if_cache_miss is true, rebuild cache
    bool Scan(const std::string& assets_dir, std::string& out_error, const ScanOptions& options, SanityCache* cache);

    const std::vector<RegistryEntry>& List() const { return entries_; }
    const std::vector<std::string>&  Errors() const { return errors_; }
    const std::vector<std::string>&  BrokenIds() const { return broken_ids_; }

    bool Render(std::string_view id,
                std::string_view utf8_text,
                const RenderOptions& options,
                Bitmap& out,
                std::string& err) const;

private:
    std::vector<RegistryEntry> entries_;
    std::unordered_map<std::string, Font> fonts_by_id_;
    std::vector<std::string> errors_;
    std::vector<std::string> broken_ids_;
};
} // namespace textmode_font


