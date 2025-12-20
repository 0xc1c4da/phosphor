#pragma once

#include "fonts/textmode_font.h"

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

    const std::vector<RegistryEntry>& List() const { return entries_; }
    const std::vector<std::string>&  Errors() const { return errors_; }

    bool Render(std::string_view id,
                std::string_view utf8_text,
                const RenderOptions& options,
                Bitmap& out,
                std::string& err) const;

private:
    std::vector<RegistryEntry> entries_;
    std::unordered_map<std::string, Font> fonts_by_id_;
    std::vector<std::string> errors_;
};
} // namespace textmode_font


