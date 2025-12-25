#include "io/session/project_state_json.h"

#include "core/color_system.h"
#include "core/glyph_legacy.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace project_state_json
{
static std::string BytesToLowerHex(std::span<const std::uint8_t> bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes)
    {
        out.push_back(kHex[(b >> 4) & 0x0Fu]);
        out.push_back(kHex[b & 0x0Fu]);
    }
    return out;
}

static bool LowerHexToBytes(std::string_view hex, std::span<std::uint8_t> out)
{
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    if (hex.size() != out.size() * 2)
        return false;
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const int hi = nyb(hex[i * 2 + 0]);
        const int lo = nyb(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (std::uint8_t)((hi << 4) | lo);
    }
    return true;
}

static json EmbeddedBitmapFontToJson(const AnsiCanvas::EmbeddedBitmapFont& f)
{
    json jf;
    jf["cell_w"] = f.cell_w;
    jf["cell_h"] = f.cell_h;
    jf["glyph_count"] = f.glyph_count;
    jf["vga_9col_dup"] = f.vga_9col_dup;
    jf["bitmap_hex"] = BytesToLowerHex(f.bitmap);
    return jf;
}

static bool EmbeddedBitmapFontFromJson(const json& jf, AnsiCanvas::EmbeddedBitmapFont& out, std::string& err)
{
    err.clear();
    out = AnsiCanvas::EmbeddedBitmapFont{};
    if (!jf.is_object())
    {
        err = "embedded_font is not an object.";
        return false;
    }
    if (jf.contains("cell_w") && jf["cell_w"].is_number_integer())
        out.cell_w = jf["cell_w"].get<int>();
    if (jf.contains("cell_h") && jf["cell_h"].is_number_integer())
        out.cell_h = jf["cell_h"].get<int>();
    if (jf.contains("glyph_count") && jf["glyph_count"].is_number_integer())
        out.glyph_count = jf["glyph_count"].get<int>();
    if (jf.contains("vga_9col_dup") && jf["vga_9col_dup"].is_boolean())
        out.vga_9col_dup = jf["vga_9col_dup"].get<bool>();

    if (!jf.contains("bitmap_hex") || !jf["bitmap_hex"].is_string())
    {
        err = "embedded_font missing 'bitmap_hex'.";
        return false;
    }
    const std::string hex = jf["bitmap_hex"].get<std::string>();

    if (out.cell_h <= 0 || out.cell_h > 64)
    {
        err = "embedded_font.cell_h is out of range.";
        return false;
    }
    if (out.glyph_count <= 0 || out.glyph_count > 4096)
    {
        err = "embedded_font.glyph_count is out of range.";
        return false;
    }
    const std::size_t expected = (std::size_t)out.glyph_count * (std::size_t)out.cell_h;
    if (hex.size() != expected * 2)
    {
        err = "embedded_font.bitmap_hex has unexpected length.";
        return false;
    }
    out.bitmap.resize(expected);
    if (!LowerHexToBytes(hex, out.bitmap))
    {
        err = "embedded_font.bitmap_hex is not valid hex.";
        return false;
    }
    return true;
}

static phos::color::Rgb8 Rgb8FromU24(std::uint32_t rgb)
{
    return phos::color::Rgb8{
        (std::uint8_t)((rgb >> 16) & 0xFFu),
        (std::uint8_t)((rgb >> 8) & 0xFFu),
        (std::uint8_t)((rgb >> 0) & 0xFFu),
    };
}

static std::uint32_t U24FromRgb8(const phos::color::Rgb8& c)
{
    return ((std::uint32_t)c.r << 16) | ((std::uint32_t)c.g << 8) | (std::uint32_t)c.b;
}

static json PaletteRefToJson(const phos::color::PaletteRef& ref)
{
    json pj;
    if (ref.is_builtin)
    {
        pj["builtin"] = (std::uint32_t)ref.builtin;
        return pj;
    }
    if (ref.uid.IsZero())
        return pj;

    pj["uid"] = BytesToLowerHex(ref.uid.bytes);

    // Dynamic palettes: embed the RGB table so undo history remains self-contained.
    auto& cs = phos::color::GetColorSystem();
    if (auto id = cs.Palettes().Resolve(ref))
    {
        if (const phos::color::Palette* p = cs.Palettes().Get(*id))
        {
            if (!p->title.empty())
                pj["title"] = p->title;
            json rgb = json::array();
            for (const phos::color::Rgb8& c : p->rgb)
                rgb.push_back(U24FromRgb8(c));
            pj["rgb_u24"] = std::move(rgb);
        }
    }
    return pj;
}

static bool PaletteRefFromJson(const json& pj, phos::color::PaletteRef& out, std::string& err)
{
    err.clear();
    out = phos::color::PaletteRef{};
    if (!pj.is_object())
        return true;

    if (pj.contains("builtin") && pj["builtin"].is_number_unsigned())
    {
        const std::uint32_t b = pj["builtin"].get<std::uint32_t>();
        out.is_builtin = true;
        out.builtin = (phos::color::BuiltinPalette)b;
        return true;
    }

    if (pj.contains("uid") && pj["uid"].is_string())
    {
        const std::string uid_hex = pj["uid"].get<std::string>();
        phos::color::PaletteUid uid;
        if (!LowerHexToBytes(uid_hex, uid.bytes))
            return true;

        out.is_builtin = false;
        out.uid = uid;

        // If the JSON includes the dynamic palette table, register it now so the palette can be resolved.
        if (pj.contains("rgb_u24") && pj["rgb_u24"].is_array())
        {
            std::vector<phos::color::Rgb8> rgb;
            rgb.reserve(pj["rgb_u24"].size());
            for (const auto& v : pj["rgb_u24"])
            {
                if (!v.is_number_unsigned() && !v.is_number_integer())
                {
                    err = "palette_ref.rgb_u24 contains a non-integer value.";
                    return false;
                }
                std::uint32_t u24 = 0;
                if (v.is_number_unsigned())
                    u24 = v.get<std::uint32_t>();
                else
                {
                    const std::int64_t si = v.get<std::int64_t>();
                    if (si < 0)
                    {
                        err = "palette_ref.rgb_u24 contains a negative value.";
                        return false;
                    }
                    u24 = (std::uint32_t)si;
                }
                rgb.push_back(Rgb8FromU24(u24));
                if (rgb.size() >= phos::color::kMaxPaletteSize)
                    break;
            }

            const phos::color::PaletteUid computed = phos::color::ComputePaletteUid(rgb);
            if (computed != uid)
            {
                err = "palette_ref.uid does not match the palette_ref.rgb_u24 table.";
                return false;
            }

            std::string title;
            if (pj.contains("title") && pj["title"].is_string())
                title = pj["title"].get<std::string>();
            (void)phos::color::GetColorSystem().Palettes().RegisterDynamic(title, rgb);
        }
        return true;
    }
    return true;
}

static bool ParseIndexPlaneFromJson(const json& arr,
                                   std::vector<AnsiCanvas::ColorIndex16>& out,
                                   const phos::color::PaletteRef& palette_ref,
                                   int project_version,
                                   std::string& err)
{
    err.clear();
    out.clear();
    if (!arr.is_array())
        return true;

    out.reserve(arr.size());
    auto& cs = phos::color::GetColorSystem();
    phos::color::PaletteInstanceId pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
    if (auto id = cs.Palettes().Resolve(palette_ref))
        pal = *id;
    const phos::color::Palette* p = cs.Palettes().Get(pal);
    const std::uint16_t max_i = (p && !p->rgb.empty())
                                  ? (std::uint16_t)std::min<std::size_t>(p->rgb.size() - 1, 0xFFu)
                                  : (std::uint16_t)0;

    const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();
    for (const auto& v : arr)
    {
        if (!v.is_number_unsigned() && !v.is_number_integer())
        {
            err = "Color plane contains a non-integer value.";
            return false;
        }

        if (v.is_number_integer())
        {
            const std::int64_t si = v.get<std::int64_t>();
            if (si == -1)
            {
                out.push_back(AnsiCanvas::kUnsetIndex16);
                continue;
            }
            if (si < -1)
            {
                err = "Color plane contains a negative value.";
                return false;
            }
        }

        const std::uint64_t u = v.get<std::uint64_t>();
        if (u <= 0xFFFFu)
        {
            const std::uint16_t idx = (std::uint16_t)u;
            if (idx == AnsiCanvas::kUnsetIndex16)
                out.push_back(idx);
            else
                out.push_back((AnsiCanvas::ColorIndex16)std::clamp<std::uint16_t>(idx, 0, max_i));
            continue;
        }

        // Legacy packed Color32 (<= v7): 0 meant "unset".
        const std::uint32_t c32 = (std::uint32_t)u;
        if (project_version <= 7 && c32 == 0)
        {
            out.push_back(AnsiCanvas::kUnsetIndex16);
            continue;
        }

        const phos::color::ColorIndex qi = phos::color::ColorOps::Color32ToIndex(cs.Palettes(), pal, c32, qp);
        if (qi.IsUnset())
        {
            out.push_back(AnsiCanvas::kUnsetIndex16);
            continue;
        }
        out.push_back((AnsiCanvas::ColorIndex16)std::clamp<std::uint16_t>(qi.v, 0, max_i));
    }

    return true;
}

static json SauceMetaToJson(const AnsiCanvas::ProjectState::SauceMeta& s)
{
    json js;
    js["present"] = s.present;
    js["title"] = s.title;
    js["author"] = s.author;
    js["group"] = s.group;
    js["date"] = s.date;
    js["file_size"] = s.file_size;
    js["data_type"] = s.data_type;
    js["file_type"] = s.file_type;
    js["tinfo1"] = s.tinfo1;
    js["tinfo2"] = s.tinfo2;
    js["tinfo3"] = s.tinfo3;
    js["tinfo4"] = s.tinfo4;
    js["tflags"] = s.tflags;
    js["tinfos"] = s.tinfos;
    js["comments"] = s.comments;
    return js;
}

static void SauceMetaFromJson(const json& js, AnsiCanvas::ProjectState::SauceMeta& out)
{
    out = AnsiCanvas::ProjectState::SauceMeta{};
    if (!js.is_object())
        return;
    if (js.contains("present") && js["present"].is_boolean())
        out.present = js["present"].get<bool>();
    if (js.contains("title") && js["title"].is_string())
        out.title = js["title"].get<std::string>();
    if (js.contains("author") && js["author"].is_string())
        out.author = js["author"].get<std::string>();
    if (js.contains("group") && js["group"].is_string())
        out.group = js["group"].get<std::string>();
    if (js.contains("date") && js["date"].is_string())
        out.date = js["date"].get<std::string>();
    if (js.contains("file_size") && (js["file_size"].is_number_unsigned() || js["file_size"].is_number_integer()))
        out.file_size = js["file_size"].get<std::uint32_t>();
    if (js.contains("data_type") && (js["data_type"].is_number_unsigned() || js["data_type"].is_number_integer()))
        out.data_type = js["data_type"].get<std::uint8_t>();
    if (js.contains("file_type") && (js["file_type"].is_number_unsigned() || js["file_type"].is_number_integer()))
        out.file_type = js["file_type"].get<std::uint8_t>();
    if (js.contains("tinfo1") && (js["tinfo1"].is_number_unsigned() || js["tinfo1"].is_number_integer()))
        out.tinfo1 = js["tinfo1"].get<std::uint16_t>();
    if (js.contains("tinfo2") && (js["tinfo2"].is_number_unsigned() || js["tinfo2"].is_number_integer()))
        out.tinfo2 = js["tinfo2"].get<std::uint16_t>();
    if (js.contains("tinfo3") && (js["tinfo3"].is_number_unsigned() || js["tinfo3"].is_number_integer()))
        out.tinfo3 = js["tinfo3"].get<std::uint16_t>();
    if (js.contains("tinfo4") && (js["tinfo4"].is_number_unsigned() || js["tinfo4"].is_number_integer()))
        out.tinfo4 = js["tinfo4"].get<std::uint16_t>();
    if (js.contains("tflags") && (js["tflags"].is_number_unsigned() || js["tflags"].is_number_integer()))
        out.tflags = js["tflags"].get<std::uint8_t>();
    if (js.contains("tinfos") && js["tinfos"].is_string())
        out.tinfos = js["tinfos"].get<std::string>();
    if (js.contains("comments") && js["comments"].is_array())
        out.comments = js["comments"].get<std::vector<std::string>>();
}

static json ProjectLayerToJson(const AnsiCanvas::ProjectLayer& l)
{
    json jl;
    jl["name"] = l.name;
    jl["visible"] = l.visible;
    jl["lock_transparency"] = l.lock_transparency;
    jl["blend_mode"] = phos::LayerBlendModeToString(l.blend_mode);
    jl["blend_alpha"] = (std::uint32_t)l.blend_alpha; // 0..255
    jl["offset_x"] = l.offset_x;
    jl["offset_y"] = l.offset_y;

    // Store glyphs as uint32 GlyphId tokens to keep CBOR compact and unambiguous.
    json cells = json::array();
    for (AnsiCanvas::GlyphId g : l.cells)
        cells.push_back(static_cast<std::uint32_t>(g));
    jl["cells"] = std::move(cells);

    jl["fg"] = l.fg;
    jl["bg"] = l.bg;
    jl["attrs"] = l.attrs;
    return jl;
}

static bool ProjectLayerFromJson(const json& jl,
                                 AnsiCanvas::ProjectLayer& out,
                                 const phos::color::PaletteRef& palette_ref,
                                 int embedded_glyph_count_for_migration,
                                 int project_version,
                                 std::string& err)
{
    err.clear();
    if (!jl.is_object())
    {
        err = "Layer is not an object.";
        return false;
    }

    out = AnsiCanvas::ProjectLayer{};
    if (jl.contains("name") && jl["name"].is_string())
        out.name = jl["name"].get<std::string>();
    if (jl.contains("visible") && jl["visible"].is_boolean())
        out.visible = jl["visible"].get<bool>();
    if (jl.contains("lock_transparency") && jl["lock_transparency"].is_boolean())
        out.lock_transparency = jl["lock_transparency"].get<bool>();
    if (jl.contains("blend_mode"))
    {
        if (jl["blend_mode"].is_string())
        {
            phos::LayerBlendMode m = phos::LayerBlendMode::Normal;
            const std::string s = jl["blend_mode"].get<std::string>();
            if (phos::LayerBlendModeFromString(s, m))
                out.blend_mode = m;
        }
        else if (jl["blend_mode"].is_number_unsigned() || jl["blend_mode"].is_number_integer())
        {
            const std::uint32_t v = jl["blend_mode"].get<std::uint32_t>();
            out.blend_mode = phos::LayerBlendModeFromInt(v);
        }
    }
    if (jl.contains("blend_alpha"))
    {
        if (jl["blend_alpha"].is_number_unsigned() || jl["blend_alpha"].is_number_integer())
        {
            const std::uint32_t v = jl["blend_alpha"].get<std::uint32_t>();
            out.blend_alpha = (std::uint8_t)std::clamp<std::uint32_t>(v, 0u, 255u);
        }
        else if (jl["blend_alpha"].is_number_float())
        {
            // Tolerate 0..1 float encodings.
            const double f = jl["blend_alpha"].get<double>();
            const double cl = std::clamp(f, 0.0, 1.0);
            out.blend_alpha = (std::uint8_t)std::clamp<int>((int)std::lround(cl * 255.0), 0, 255);
        }
    }
    if (jl.contains("offset_x") && jl["offset_x"].is_number_integer())
        out.offset_x = jl["offset_x"].get<int>();
    if (jl.contains("offset_y") && jl["offset_y"].is_number_integer())
        out.offset_y = jl["offset_y"].get<int>();

    if (!jl.contains("cells") || !jl["cells"].is_array())
    {
        err = "Layer missing 'cells' array.";
        return false;
    }

    const json& cells = jl["cells"];
    out.cells.clear();
    out.cells.reserve(cells.size());
    const bool can_migrate_legacy_embedded = (embedded_glyph_count_for_migration > 0);
    for (const auto& v : cells)
    {
        if (!v.is_number_unsigned() && !v.is_number_integer())
        {
            err = "Layer 'cells' contains a non-integer value.";
            return false;
        }
        std::uint32_t u = 0;
        if (v.is_number_unsigned())
            u = v.get<std::uint32_t>();
        else
        {
            const std::int64_t si = v.get<std::int64_t>();
            if (si < 0)
            {
                err = "Layer 'cells' contains a negative codepoint.";
                return false;
            }
            u = static_cast<std::uint32_t>(si);
        }
        // Migration:
        // - v<=9 stored Unicode/PUA codepoints (char32_t) in this field.
        // - v>=10 stores GlyphId tokens (u32).
        // Additionally: if an older file contains token-bit values (>=0x80000000),
        // treat them as GlyphId for forward compatibility with hybrid branches.
        if (project_version >= 10 || (u & phos::glyph::kTokenBit))
            out.cells.push_back((AnsiCanvas::GlyphId)u);
        else
        {
            const char32_t cp = (char32_t)u;
            // Deterministic migration when an embedded font payload exists:
            // Legacy embedded glyph indices were stored as PUA codepoints (U+E000 + index).
            if (can_migrate_legacy_embedded)
            {
                if (auto idx = phos::glyph::TryDecodeLegacyEmbeddedPuaCodePoint(
                        cp, (std::uint32_t)embedded_glyph_count_for_migration))
                {
                    out.cells.push_back(phos::glyph::MakeEmbeddedIndex(*idx));
                    continue;
                }
            }
            out.cells.push_back(phos::glyph::MakeUnicodeScalar(cp));
        }
    }

    out.fg.clear();
    out.bg.clear();
    if (jl.contains("fg") && jl["fg"].is_array())
    {
        if (!ParseIndexPlaneFromJson(jl["fg"], out.fg, palette_ref, project_version, err))
            return false;
    }
    if (jl.contains("bg") && jl["bg"].is_array())
    {
        if (!ParseIndexPlaneFromJson(jl["bg"], out.bg, palette_ref, project_version, err))
            return false;
    }
    if (jl.contains("attrs") && jl["attrs"].is_array())
        out.attrs = jl["attrs"].get<std::vector<AnsiCanvas::Attrs>>();

    // If missing, AnsiCanvas::SetProjectState will default these to all-unset.
    return true;
}

static json ProjectSnapshotToJson(const AnsiCanvas::ProjectSnapshot& s)
{
    json js;
    js["columns"] = s.columns;
    js["rows"] = s.rows;
    js["active_layer"] = s.active_layer;
    js["caret_row"] = s.caret_row;
    js["caret_col"] = s.caret_col;
    js["palette_ref"] = PaletteRefToJson(s.palette_ref);
    if (!s.colour_palette_title.empty())
        js["colour_palette_title"] = s.colour_palette_title;
    json layers = json::array();
    for (const auto& l : s.layers)
        layers.push_back(ProjectLayerToJson(l));
    js["layers"] = std::move(layers);
    return js;
}

static bool ProjectSnapshotFromJson(const json& js,
                                    AnsiCanvas::ProjectSnapshot& out,
                                    const phos::color::PaletteRef& palette_ref,
                                    int embedded_glyph_count_for_migration,
                                    int project_version,
                                    std::string& err)
{
    err.clear();
    if (!js.is_object())
    {
        err = "Snapshot is not an object.";
        return false;
    }

    out = AnsiCanvas::ProjectSnapshot{};
    // Default to the project's palette identity, but allow snapshots to override it.
    out.palette_ref = palette_ref;
    out.colour_palette_title.clear();
    if (js.contains("palette_ref") && js["palette_ref"].is_object())
    {
        phos::color::PaletteRef pref;
        std::string perr;
        if (!PaletteRefFromJson(js["palette_ref"], pref, perr))
        {
            err = perr;
            return false;
        }
        if (pref.is_builtin || !pref.uid.IsZero())
            out.palette_ref = pref;
    }
    if (js.contains("colour_palette_title") && js["colour_palette_title"].is_string())
        out.colour_palette_title = js["colour_palette_title"].get<std::string>();
    if (js.contains("columns") && js["columns"].is_number_integer())
        out.columns = js["columns"].get<int>();
    if (js.contains("rows") && js["rows"].is_number_integer())
        out.rows = js["rows"].get<int>();
    if (js.contains("active_layer") && js["active_layer"].is_number_integer())
        out.active_layer = js["active_layer"].get<int>();
    if (js.contains("caret_row") && js["caret_row"].is_number_integer())
        out.caret_row = js["caret_row"].get<int>();
    if (js.contains("caret_col") && js["caret_col"].is_number_integer())
        out.caret_col = js["caret_col"].get<int>();

    if (!js.contains("layers") || !js["layers"].is_array())
    {
        err = "Snapshot missing 'layers' array.";
        return false;
    }

    out.layers.clear();
    for (const auto& jl : js["layers"])
    {
        AnsiCanvas::ProjectLayer pl;
        if (!ProjectLayerFromJson(jl, pl, out.palette_ref, embedded_glyph_count_for_migration, project_version, err))
            return false;
        out.layers.push_back(std::move(pl));
    }
    return true;
}

static json UndoEntryToJson(const AnsiCanvas::ProjectState::ProjectUndoEntry& e)
{
    json je;
    if (e.kind == AnsiCanvas::ProjectState::ProjectUndoEntry::Kind::Patch)
    {
        je["kind"] = "patch";
        je["columns"] = e.patch.columns;
        je["rows"] = e.patch.rows;
        je["active_layer"] = e.patch.active_layer;
        je["caret_row"] = e.patch.caret_row;
        je["caret_col"] = e.patch.caret_col;
        je["palette_ref"] = PaletteRefToJson(e.patch.palette_ref);
        if (!e.patch.colour_palette_title.empty())
            je["colour_palette_title"] = e.patch.colour_palette_title;
        je["state_token"] = e.patch.state_token;
        je["page_rows"] = e.patch.page_rows;

        json layers = json::array();
        for (const auto& lm : e.patch.layers)
        {
            json jl;
            jl["name"] = lm.name;
            jl["visible"] = lm.visible;
            jl["lock_transparency"] = lm.lock_transparency;
            jl["blend_mode"] = phos::LayerBlendModeToString(lm.blend_mode);
            jl["blend_alpha"] = (std::uint32_t)lm.blend_alpha;
            jl["offset_x"] = lm.offset_x;
            jl["offset_y"] = lm.offset_y;
            layers.push_back(std::move(jl));
        }
        je["layers"] = std::move(layers);

        json pages = json::array();
        for (const auto& pg : e.patch.pages)
        {
            json jp;
            jp["layer"] = pg.layer;
            jp["page"] = pg.page;
            jp["page_rows"] = pg.page_rows;
            jp["row_count"] = pg.row_count;

            // Store glyphs as uint32 GlyphId tokens to keep CBOR compact and unambiguous.
            json cells = json::array();
            for (AnsiCanvas::GlyphId g : pg.cells)
                cells.push_back(static_cast<std::uint32_t>(g));
            jp["cells"] = std::move(cells);
            jp["fg"] = pg.fg;
            jp["bg"] = pg.bg;
            jp["attrs"] = pg.attrs;
            pages.push_back(std::move(jp));
        }
        je["pages"] = std::move(pages);
    }
    else
    {
        je["kind"] = "snapshot";
        je["snapshot"] = ProjectSnapshotToJson(e.snapshot);
    }
    return je;
}

static bool UndoEntryFromJson(const json& je,
                              AnsiCanvas::ProjectState::ProjectUndoEntry& out,
                              const phos::color::PaletteRef& palette_ref,
                              int embedded_glyph_count_for_migration,
                              int project_version,
                              std::string& err)
{
    err.clear();
    out = AnsiCanvas::ProjectState::ProjectUndoEntry{};
    if (!je.is_object())
    {
        err = "Undo entry is not an object.";
        return false;
    }
    std::string kind = "snapshot";
    if (je.contains("kind") && je["kind"].is_string())
        kind = je["kind"].get<std::string>();
    if (kind == "patch")
    {
        out.kind = AnsiCanvas::ProjectState::ProjectUndoEntry::Kind::Patch;
        auto& p = out.patch;
        if (je.contains("columns") && je["columns"].is_number_integer()) p.columns = je["columns"].get<int>();
        if (je.contains("rows") && je["rows"].is_number_integer()) p.rows = je["rows"].get<int>();
        if (je.contains("active_layer") && je["active_layer"].is_number_integer()) p.active_layer = je["active_layer"].get<int>();
        if (je.contains("caret_row") && je["caret_row"].is_number_integer()) p.caret_row = je["caret_row"].get<int>();
        if (je.contains("caret_col") && je["caret_col"].is_number_integer()) p.caret_col = je["caret_col"].get<int>();
        // Default to the project's palette identity, but allow patches to override it.
        p.palette_ref = palette_ref;
        p.colour_palette_title.clear();
        if (je.contains("palette_ref") && je["palette_ref"].is_object())
        {
            phos::color::PaletteRef pref;
            std::string perr;
            if (!PaletteRefFromJson(je["palette_ref"], pref, perr))
            {
                err = perr;
                return false;
            }
            if (pref.is_builtin || !pref.uid.IsZero())
                p.palette_ref = pref;
        }
        if (je.contains("colour_palette_title") && je["colour_palette_title"].is_string())
            p.colour_palette_title = je["colour_palette_title"].get<std::string>();
        if (je.contains("state_token") && (je["state_token"].is_number_unsigned() || je["state_token"].is_number_integer()))
            p.state_token = je["state_token"].get<std::uint64_t>();
        if (je.contains("page_rows") && je["page_rows"].is_number_integer())
            p.page_rows = je["page_rows"].get<int>();

        p.layers.clear();
        if (je.contains("layers") && je["layers"].is_array())
        {
            for (const auto& jl : je["layers"])
            {
                if (!jl.is_object())
                    continue;
                AnsiCanvas::ProjectState::ProjectUndoEntry::PatchLayerMeta lm;
                if (jl.contains("name") && jl["name"].is_string()) lm.name = jl["name"].get<std::string>();
                if (jl.contains("visible") && jl["visible"].is_boolean()) lm.visible = jl["visible"].get<bool>();
                if (jl.contains("lock_transparency") && jl["lock_transparency"].is_boolean()) lm.lock_transparency = jl["lock_transparency"].get<bool>();
                if (jl.contains("blend_mode"))
                {
                    if (jl["blend_mode"].is_string())
                    {
                        phos::LayerBlendMode m = phos::LayerBlendMode::Normal;
                        const std::string s = jl["blend_mode"].get<std::string>();
                        if (phos::LayerBlendModeFromString(s, m))
                            lm.blend_mode = m;
                    }
                    else if (jl["blend_mode"].is_number_unsigned() || jl["blend_mode"].is_number_integer())
                    {
                        const std::uint32_t v = jl["blend_mode"].get<std::uint32_t>();
                        lm.blend_mode = phos::LayerBlendModeFromInt(v);
                    }
                }
                if (jl.contains("blend_alpha"))
                {
                    if (jl["blend_alpha"].is_number_unsigned() || jl["blend_alpha"].is_number_integer())
                    {
                        const std::uint32_t v = jl["blend_alpha"].get<std::uint32_t>();
                        lm.blend_alpha = (std::uint8_t)std::clamp<std::uint32_t>(v, 0u, 255u);
                    }
                    else if (jl["blend_alpha"].is_number_float())
                    {
                        const double f = jl["blend_alpha"].get<double>();
                        const double cl = std::clamp(f, 0.0, 1.0);
                        lm.blend_alpha = (std::uint8_t)std::clamp<int>((int)std::lround(cl * 255.0), 0, 255);
                    }
                }
                if (jl.contains("offset_x") && jl["offset_x"].is_number_integer()) lm.offset_x = jl["offset_x"].get<int>();
                if (jl.contains("offset_y") && jl["offset_y"].is_number_integer()) lm.offset_y = jl["offset_y"].get<int>();
                p.layers.push_back(std::move(lm));
            }
        }

        p.pages.clear();
        if (je.contains("pages") && je["pages"].is_array())
        {
            for (const auto& jp : je["pages"])
            {
                if (!jp.is_object())
                    continue;
                AnsiCanvas::ProjectState::ProjectUndoEntry::PatchPage pg;
                if (jp.contains("layer") && jp["layer"].is_number_integer()) pg.layer = jp["layer"].get<int>();
                if (jp.contains("page") && jp["page"].is_number_integer()) pg.page = jp["page"].get<int>();
                if (jp.contains("page_rows") && jp["page_rows"].is_number_integer()) pg.page_rows = jp["page_rows"].get<int>();
                if (jp.contains("row_count") && jp["row_count"].is_number_integer()) pg.row_count = jp["row_count"].get<int>();

                if (!jp.contains("cells") || !jp["cells"].is_array())
                {
                    err = "Undo patch page missing 'cells' array.";
                    return false;
                }
                pg.cells.clear();
                pg.cells.reserve(jp["cells"].size());
                const bool can_migrate_legacy_embedded = (embedded_glyph_count_for_migration > 0);
                for (const auto& v : jp["cells"])
                {
                    if (!v.is_number_unsigned() && !v.is_number_integer())
                    {
                        err = "Undo patch page 'cells' contains a non-integer value.";
                        return false;
                    }
                    std::uint32_t u = 0;
                    if (v.is_number_unsigned())
                        u = v.get<std::uint32_t>();
                    else
                    {
                        const std::int64_t si = v.get<std::int64_t>();
                        if (si < 0)
                        {
                            err = "Undo patch page 'cells' contains a negative codepoint.";
                            return false;
                        }
                        u = static_cast<std::uint32_t>(si);
                    }
                    if (project_version >= 10 || (u & phos::glyph::kTokenBit))
                        pg.cells.push_back((AnsiCanvas::GlyphId)u);
                    else
                    {
                        const char32_t cp = (char32_t)u;
                        if (can_migrate_legacy_embedded)
                        {
                            if (auto idx = phos::glyph::TryDecodeLegacyEmbeddedPuaCodePoint(
                                    cp, (std::uint32_t)embedded_glyph_count_for_migration))
                            {
                                pg.cells.push_back(phos::glyph::MakeEmbeddedIndex(*idx));
                                continue;
                            }
                        }
                        pg.cells.push_back(phos::glyph::MakeUnicodeScalar(cp));
                    }
                }
                if (jp.contains("fg") && jp["fg"].is_array())
                {
                    if (!ParseIndexPlaneFromJson(jp["fg"], pg.fg, out.patch.palette_ref, project_version, err))
                        return false;
                }
                if (jp.contains("bg") && jp["bg"].is_array())
                {
                    if (!ParseIndexPlaneFromJson(jp["bg"], pg.bg, out.patch.palette_ref, project_version, err))
                        return false;
                }
                if (jp.contains("attrs") && jp["attrs"].is_array())
                    pg.attrs = jp["attrs"].get<std::vector<AnsiCanvas::Attrs>>();
                p.pages.push_back(std::move(pg));
            }
        }
        return true;
    }
    // snapshot
    out.kind = AnsiCanvas::ProjectState::ProjectUndoEntry::Kind::Snapshot;
    if (!je.contains("snapshot"))
    {
        err = "Undo snapshot entry missing 'snapshot'.";
        return false;
    }
    return ProjectSnapshotFromJson(je["snapshot"], out.snapshot, palette_ref, embedded_glyph_count_for_migration, project_version, err);
}

json ToJson(const AnsiCanvas::ProjectState& st)
{
    json j;
    j["magic"] = "utf8-art-editor";
    j["version"] = st.version;
    j["bold_semantics"] = st.bold_semantics;
    j["undo_limit"] = st.undo_limit;
    // Core palette identity.
    j["palette_ref"] = PaletteRefToJson(st.palette_ref);
    if (!st.colour_palette_title.empty())
        j["colour_palette_title"] = st.colour_palette_title;
    j["sauce"] = SauceMetaToJson(st.sauce);
    if (st.embedded_font.has_value())
        j["embedded_font"] = EmbeddedBitmapFontToJson(*st.embedded_font);
    j["current"] = ProjectSnapshotToJson(st.current);

    json undo = json::array();
    for (const auto& s : st.undo)
        undo.push_back(UndoEntryToJson(s));
    j["undo"] = std::move(undo);

    json redo = json::array();
    for (const auto& s : st.redo)
        redo.push_back(UndoEntryToJson(s));
    j["redo"] = std::move(redo);
    return j;
}

bool FromJson(const json& j, AnsiCanvas::ProjectState& out, std::string& err)
{
    err.clear();
    if (!j.is_object())
    {
        err = "Project file root is not an object.";
        return false;
    }

    if (j.contains("magic") && j["magic"].is_string())
    {
        const std::string magic = j["magic"].get<std::string>();
        if (magic != "utf8-art-editor")
        {
            err = "Not a utf8-art-editor project file.";
            return false;
        }
    }

    out = AnsiCanvas::ProjectState{};
    if (j.contains("version") && j["version"].is_number_integer())
        out.version = j["version"].get<int>();
    if (j.contains("bold_semantics") && j["bold_semantics"].is_number_integer())
        out.bold_semantics = j["bold_semantics"].get<int>();
    if (j.contains("undo_limit") && j["undo_limit"].is_number_unsigned())
        out.undo_limit = j["undo_limit"].get<size_t>();
    else if (j.contains("undo_limit") && j["undo_limit"].is_number_integer())
    {
        const int v = j["undo_limit"].get<int>();
        // 0 (or negative) = unlimited.
        out.undo_limit = (v > 0) ? static_cast<size_t>(v) : 0;
    }

    // Optional SAUCE metadata (safe default if absent).
    if (j.contains("sauce"))
        SauceMetaFromJson(j["sauce"], out.sauce);

    // Optional embedded bitmap font payload.
    if (j.contains("embedded_font"))
    {
        AnsiCanvas::EmbeddedBitmapFont f;
        std::string ferr;
        if (!EmbeddedBitmapFontFromJson(j["embedded_font"], f, ferr))
        {
            err = ferr;
            return false;
        }
        out.embedded_font = std::move(f);
    }

    // Optional UI colour palette identity.
    if (j.contains("colour_palette_title") && j["colour_palette_title"].is_string())
        out.colour_palette_title = j["colour_palette_title"].get<std::string>();

    // Core palette identity (optional; defaults to xterm256).
    if (j.contains("palette_ref") && j["palette_ref"].is_object())
    {
        phos::color::PaletteRef pref;
        std::string perr;
        if (!PaletteRefFromJson(j["palette_ref"], pref, perr))
        {
            err = perr;
            return false;
        }
        if (pref.is_builtin || !pref.uid.IsZero())
            out.palette_ref = pref;
    }

    if (!j.contains("current"))
    {
        err = "Project missing 'current' snapshot.";
        return false;
    }
    const int embedded_glyph_count_for_migration =
        (out.embedded_font.has_value() ? std::max(0, out.embedded_font->glyph_count) : 0);
    if (!ProjectSnapshotFromJson(j["current"], out.current, out.palette_ref, embedded_glyph_count_for_migration, out.version, err))
        return false;

    out.undo.clear();
    if (j.contains("undo") && j["undo"].is_array())
    {
        for (const auto& s : j["undo"])
        {
            // Backward compatibility: older project versions stored undo as raw snapshots.
            // IMPORTANT: patch entries also contain "columns" and "layers", so only treat an entry
            // as an old-style snapshot if it does NOT declare a "kind" field.
            if (s.is_object() && !s.contains("kind") && s.contains("columns") && s.contains("layers"))
            {
                AnsiCanvas::ProjectState::ProjectUndoEntry e;
                e.kind = AnsiCanvas::ProjectState::ProjectUndoEntry::Kind::Snapshot;
                if (!ProjectSnapshotFromJson(s, e.snapshot, out.palette_ref, embedded_glyph_count_for_migration, out.version, err))
                    return false;
                out.undo.push_back(std::move(e));
                continue;
            }

            AnsiCanvas::ProjectState::ProjectUndoEntry e;
            if (!UndoEntryFromJson(s, e, out.palette_ref, embedded_glyph_count_for_migration, out.version, err))
                return false;
            out.undo.push_back(std::move(e));
        }
    }

    out.redo.clear();
    if (j.contains("redo") && j["redo"].is_array())
    {
        for (const auto& s : j["redo"])
        {
            if (s.is_object() && !s.contains("kind") && s.contains("columns") && s.contains("layers"))
            {
                AnsiCanvas::ProjectState::ProjectUndoEntry e;
                e.kind = AnsiCanvas::ProjectState::ProjectUndoEntry::Kind::Snapshot;
                if (!ProjectSnapshotFromJson(s, e.snapshot, out.palette_ref, embedded_glyph_count_for_migration, out.version, err))
                    return false;
                out.redo.push_back(std::move(e));
                continue;
            }

            AnsiCanvas::ProjectState::ProjectUndoEntry e;
            if (!UndoEntryFromJson(s, e, out.palette_ref, embedded_glyph_count_for_migration, out.version, err))
                return false;
            out.redo.push_back(std::move(e));
        }
    }

    return true;
}
} // namespace project_state_json


