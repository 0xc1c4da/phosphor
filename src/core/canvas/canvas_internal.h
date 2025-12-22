// Internal helpers shared across AnsiCanvas implementation units.
// Not part of the public API.
#pragma once

#include "core/canvas.h"
#include "core/fonts.h"

#include "imgui.h"
#include "io/formats/sauce.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <limits>

static inline std::uint16_t ClampU16FromInt(int v)
{
    if (v < 0) return 0;
    if (v > 65535) return 65535;
    return (std::uint16_t)v;
}

// Coordinate helper: convert canvas-space (row/col) into layer-local (row/col) given an integer offset.
//
// This mirrors AnsiCanvas::CanvasToLayerLocalForWrite/Read semantics, but is header-only so
// hot loops in other translation units (e.g. selection/clipboard) can avoid cross-TU calls.
//
// Important behaviors preserved:
// - Reject negative results and out-of-range columns.
// - For "write": do NOT check the row upper bound (doc can grow on demand).
// - For "read": additionally require row within [0, rows).
static inline bool CanvasToLayerLocalForWriteFast(int canvas_row,
                                                 int canvas_col,
                                                 int offset_x,
                                                 int offset_y,
                                                 int columns,
                                                 int& out_local_row,
                                                 int& out_local_col)
{
    if (columns <= 0)
        return false;

    // Fast path: common case (no offset).
    if (offset_x == 0 && offset_y == 0)
    {
        if (canvas_row < 0 || canvas_col < 0)
            return false;
        if (canvas_col >= columns)
            return false;
        out_local_row = canvas_row;
        out_local_col = canvas_col;
        return true;
    }

    const long long lr = (long long)canvas_row - (long long)offset_y;
    const long long lc = (long long)canvas_col - (long long)offset_x;
    if (lr < 0 || lc < 0)
        return false;
    if (lc >= (long long)columns)
        return false;
    if (lr > (long long)std::numeric_limits<int>::max() || lc > (long long)std::numeric_limits<int>::max())
        return false;
    out_local_row = (int)lr;
    out_local_col = (int)lc;
    return true;
}

static inline bool CanvasToLayerLocalForReadFast(int canvas_row,
                                                int canvas_col,
                                                int offset_x,
                                                int offset_y,
                                                int columns,
                                                int rows,
                                                int& out_local_row,
                                                int& out_local_col)
{
    if (!CanvasToLayerLocalForWriteFast(canvas_row, canvas_col, offset_x, offset_y, columns, out_local_row, out_local_col))
        return false;
    if (out_local_row < 0 || out_local_row >= rows)
        return false;
    return true;
}

static inline void EnsureSauceDefaultsAndSyncGeometry(AnsiCanvas::ProjectState::SauceMeta& s,
                                                     int cols,
                                                     int rows)
{
    // Defaults: for our editor, treat canvases as Character/ANSi unless the user explicitly
    // chose a different datatype in the SAUCE editor.
    if (s.data_type == 0)
        s.data_type = 1; // Character
    if (s.data_type == 1 && s.file_type == 0)
        s.file_type = 1; // ANSi

    // Ensure a sane creation date for new canvases.
    if (s.date.empty())
        s.date = sauce::TodayYYYYMMDD();

    // Best-effort font name hint (SAUCE TInfoS). Keep it short and ASCII.
    if (s.tinfos.empty())
    {
        const std::string_view def = fonts::ToSauceName(fonts::DefaultCanvasFont());
        s.tinfos = def.empty() ? "unscii-16-full" : std::string(def);
    }

    // Keep geometry in sync when SAUCE is describing character-based content.
    if (s.data_type == 1 /* Character */ || s.data_type == 6 /* XBin */ || s.data_type == 0)
    {
        s.tinfo1 = ClampU16FromInt(cols);
        s.tinfo2 = ClampU16FromInt(rows);
    }

    // If we have any meaningful auto-filled fields, ensure the record is treated as present.
    // (Important for future exporters and for UI expectations.)
    if (!s.present)
    {
        if (s.tinfo1 != 0 || s.tinfo2 != 0 || !s.date.empty() || !s.tinfos.empty())
            s.present = true;
    }
}

// IMPORTANT:
// Many parts of this app implement per-window opacity via PushImGuiWindowChromeAlpha(),
// which multiplies ImGuiStyleVar_Alpha. ImDrawList primitives that use raw IM_COL32 /
// raw ImU32 colors bypass that multiplication unless we apply it manually.
static inline ImU32 ApplyCurrentStyleAlpha(ImU32 col)
{
    // Convert to float4 (includes original alpha), then let ImGui re-pack while applying style.Alpha.
    const ImVec4 v = ImGui::ColorConvertU32ToFloat4(col);
    return ImGui::GetColorU32(v);
}

// Utility: encode a single UTF-32 codepoint into UTF-8.
static inline int EncodeUtf8(char32_t cp, char out[5])
{
    // Based on UTF-8 encoding rules, up to 4 bytes.
    if (cp <= 0x7F)
    {
        out[0] = static_cast<char>(cp);
        out[1] = '\0';
        return 1;
    }
    else if (cp <= 0x7FF)
    {
        out[0] = static_cast<char>(0xC0 | ((cp >> 6) & 0x1F));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        out[2] = '\0';
        return 2;
    }
    else if (cp <= 0xFFFF)
    {
        out[0] = static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        out[3] = '\0';
        return 3;
    }
    else
    {
        out[0] = static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        out[4] = '\0';
        return 4;
    }
}

// Utility: decode UTF-8 bytes into Unicode codepoints.
// We keep this intentionally simple for now:
//  - malformed sequences are skipped
//  - no overlong/surrogate validation yet (fine for editor bootstrap)
static inline void DecodeUtf8(const std::string& bytes, std::vector<char32_t>& out_codepoints)
{
    out_codepoints.clear();

    const unsigned char* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t len = bytes.size();
    size_t i = 0;
    while (i < len)
    {
        unsigned char c = data[i];

        char32_t cp = 0;
        size_t remaining = 0;
        if ((c & 0x80) == 0)
        {
            cp = c;
            remaining = 0;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            cp = c & 0x1F;
            remaining = 1;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            cp = c & 0x0F;
            remaining = 2;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            cp = c & 0x07;
            remaining = 3;
        }
        else
        {
            ++i;
            continue;
        }

        if (i + remaining >= len)
            break;

        bool malformed = false;
        for (size_t j = 0; j < remaining; ++j)
        {
            unsigned char cc = data[i + 1 + j];
            if ((cc & 0xC0) != 0x80)
            {
                malformed = true;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (malformed)
        {
            ++i;
            continue;
        }

        i += 1 + remaining;
        out_codepoints.push_back(cp);
    }
}

// Common helper: interpret layer_index=-1 as "active layer".
static inline int NormalizeLayerIndex(const AnsiCanvas& c, int layer_index)
{
    if (layer_index < 0)
        return c.GetActiveLayerIndex();
    return layer_index;
}

static inline bool IsTransparentCellValue(char32_t cp, AnsiCanvas::Color32 fg, AnsiCanvas::Color32 bg, AnsiCanvas::Attrs attrs)
{
    (void)attrs;
    // In this editor, a cell is considered "transparent" (no contribution) iff:
    // - glyph is space
    // - fg is unset (0)
    // - bg is unset (0)
    // Note: space with a non-zero bg is visually opaque (background fill).
    //
    // IMPORTANT: attributes alone do NOT make a cell opaque for compositing/transparency-lock.
    // A space cell remains transparent even if attrs are set.
    return (cp == U' ') && (fg == 0) && (bg == 0);
}

// When a layer has "transparency lock" enabled, mutations must not change a cell's
// transparency state (transparent <-> opaque).
static inline bool TransparencyTransitionAllowed(bool lock_transparency,
                                                char32_t old_cp, AnsiCanvas::Color32 old_fg, AnsiCanvas::Color32 old_bg, AnsiCanvas::Attrs old_attrs,
                                                char32_t new_cp, AnsiCanvas::Color32 new_fg, AnsiCanvas::Color32 new_bg, AnsiCanvas::Attrs new_attrs)
{
    if (!lock_transparency)
        return true;
    const bool old_t = IsTransparentCellValue(old_cp, old_fg, old_bg, old_attrs);
    const bool new_t = IsTransparentCellValue(new_cp, new_fg, new_bg, new_attrs);
    return old_t == new_t;
}


