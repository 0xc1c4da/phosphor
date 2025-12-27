#include "core/canvas/canvas_internal.h"

#include "core/colour_blend.h"
#include "core/colour_system.h"
#include "core/glyph_resolve.h"
#include "core/key_bindings.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
static inline phos::colour::Rgb8 PaperRgb(bool white)
{
    const std::uint8_t v = white ? 255u : 0u;
    return phos::colour::Rgb8{v, v, v};
}

static inline phos::colour::Rgb8 DefaultFgRgb(bool paper_is_white)
{
    // Core fallback for "theme default fg":
    // - on white paper, default fg is black
    // - on black paper, default fg is white
    // (matches the minimap preview defaults)
    const std::uint8_t v = paper_is_white ? 0u : 255u;
    return phos::colour::Rgb8{v, v, v};
}

static inline std::uint8_t ClampPaletteIndexU8(const phos::colour::Palette* p, AnsiCanvas::ColourIndex16 idx)
{
    if (!p || p->rgb.empty())
        return 0;
    const std::uint16_t max_i = (std::uint16_t)std::min<std::size_t>(p->rgb.size() - 1, 0xFFu);
    return (std::uint8_t)std::clamp<std::uint16_t>((std::uint16_t)idx, 0, max_i);
}

static inline phos::colour::Rgb8 PaletteRgbClamped(const phos::colour::Palette* p, AnsiCanvas::ColourIndex16 idx)
{
    const std::uint8_t i = ClampPaletteIndexU8(p, idx);
    if (!p || p->rgb.empty())
        return phos::colour::Rgb8{};
    return p->rgb[(size_t)i];
}

static inline AnsiCanvas::GlyphId BlankGlyph()
{
    return phos::glyph::MakeUnicodeScalar(U' ');
}

static inline char32_t GlyphIdToUnicodeRepresentative(AnsiCanvas::GlyphId g)
{
    // Representative "best-effort Unicode" view of the glyph:
    // - UnicodeScalar: return scalar as-is
    // - BitmapIndex/EmbeddedIndex: deterministic Unicode representative (CP437 policy)
    //
    // Important: we intentionally do NOT emit legacy embedded PUA (U+E000 + idx) here anymore.
    // PUA remains accepted as an input compatibility representation (see core/glyph_resolve.h),
    // but internal "representative cp" views should be non-PUA.
    return phos::glyph::ToUnicodeRepresentative((phos::GlyphId)g);
}
} // namespace

phos::colour::PaletteInstanceId AnsiCanvas::ResolveActivePaletteId() const
{
    auto& cs = phos::colour::GetColourSystem();
    if (auto id = cs.Palettes().Resolve(m_palette_ref))
        return *id;
    return cs.Palettes().Builtin(phos::colour::BuiltinPalette::Xterm256);
}

AnsiCanvas::ColourIndex16 AnsiCanvas::QuantizeColour32ToIndex(Colour32 c32) const
{
    if (c32 == 0)
        return kUnsetIndex16;
    auto& cs = phos::colour::GetColourSystem();
    const phos::colour::PaletteInstanceId pal = ResolveActivePaletteId();
    const phos::colour::Palette* p = cs.Palettes().Get(pal);
    if (!p || p->rgb.empty())
        return kUnsetIndex16;

    const phos::colour::QuantizePolicy qp = phos::colour::DefaultQuantizePolicy();
    const phos::colour::ColourIndex idx =
        phos::colour::ColourOps::Colour32ToIndex(cs.Palettes(), pal, (std::uint32_t)c32, qp);
    if (idx.IsUnset())
        return kUnsetIndex16;
    const std::uint16_t max_i = (std::uint16_t)std::min<std::size_t>(p->rgb.size() - 1, 0xFFu);
    return (ColourIndex16)std::clamp<std::uint16_t>(idx.v, 0, max_i);
}

AnsiCanvas::Colour32 AnsiCanvas::IndexToColour32(ColourIndex16 idx) const
{
    if (idx == kUnsetIndex16)
        return 0;
    auto& cs = phos::colour::GetColourSystem();
    const phos::colour::PaletteInstanceId pal = ResolveActivePaletteId();
    return (Colour32)phos::colour::ColourOps::IndexToColour32(cs.Palettes(), pal, phos::colour::ColourIndex{idx});
}

// ---- inlined from canvas_layers.inc ----
int AnsiCanvas::GetLayerCount() const
{
    return static_cast<int>(m_layers.size());
}

int AnsiCanvas::GetActiveLayerIndex() const
{
    return m_active_layer;
}

std::string AnsiCanvas::GetLayerName(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return {};
    return m_layers[static_cast<size_t>(index)].name;
}

bool AnsiCanvas::IsLayerVisible(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    return m_layers[static_cast<size_t>(index)].visible;
}

bool AnsiCanvas::IsLayerTransparencyLocked(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    return m_layers[static_cast<size_t>(index)].lock_transparency;
}

bool AnsiCanvas::SetLayerName(int index, const std::string& name)
{
    EnsureDocument();
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    if (m_layers[static_cast<size_t>(index)].name == name)
        return true; // no-op
    // Structural change: if we're not inside the canvas render's undo capture for this frame
    // (e.g. UI panels mutate the canvas before AnsiCanvas::Render runs), still make it undoable.
    if (!m_undo_capture_active)
        PushUndoSnapshot();
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();
    m_layers[static_cast<size_t>(index)].name = name;
    return true;
}

int AnsiCanvas::AddLayer(const std::string& name)
{
    EnsureDocument();
    // Structural change: ensure this is undoable even if invoked outside an active capture.
    if (!m_undo_capture_active)
        PushUndoSnapshot();
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();

    Layer layer;
    layer.name = name.empty() ? ("Layer " + std::to_string((int)m_layers.size() + 1)) : name;
    layer.visible = true;
    layer.blend_mode = phos::LayerBlendMode::Normal;
    layer.blend_alpha = 255;
    const size_t count = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    layer.cells.assign(count, BlankGlyph());
    layer.fg.assign(count, kUnsetIndex16);
    layer.bg.assign(count, kUnsetIndex16);
    layer.attrs.assign(count, 0);

    m_layers.push_back(std::move(layer));
    m_active_layer = static_cast<int>(m_layers.size()) - 1;
    return m_active_layer;
}

bool AnsiCanvas::RemoveLayer(int index)
{
    EnsureDocument();
    if (m_layers.size() <= 1)
        return false; // must keep at least one layer
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;

    // Structural change: ensure this is undoable even if invoked outside an active capture.
    if (!m_undo_capture_active)
        PushUndoSnapshot();
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();
    m_layers.erase(m_layers.begin() + index);
    if (m_active_layer >= static_cast<int>(m_layers.size()))
        m_active_layer = static_cast<int>(m_layers.size()) - 1;
    if (m_active_layer < 0)
        m_active_layer = 0;
    return true;
}

bool AnsiCanvas::SetActiveLayerIndex(int index)
{
    EnsureDocument();
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    m_active_layer = index;
    return true;
}

bool AnsiCanvas::SetLayerVisible(int index, bool visible)
{
    EnsureDocument();
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    if (m_layers[static_cast<size_t>(index)].visible == visible)
        return true;
    m_layers[static_cast<size_t>(index)].visible = visible;
    TouchContent();
    return true;
}

bool AnsiCanvas::SetLayerTransparencyLocked(int index, bool locked)
{
    EnsureDocument();
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    m_layers[static_cast<size_t>(index)].lock_transparency = locked;
    return true;
}

phos::LayerBlendMode AnsiCanvas::GetLayerBlendMode(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return phos::LayerBlendMode::Normal;
    return m_layers[static_cast<size_t>(index)].blend_mode;
}

bool AnsiCanvas::SetLayerBlendMode(int index, phos::LayerBlendMode mode)
{
    EnsureDocument();
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    if (m_layers[static_cast<size_t>(index)].blend_mode == mode)
        return true;
    m_layers[static_cast<size_t>(index)].blend_mode = mode;
    TouchContent(); // affects compositing output
    return true;
}

std::uint8_t AnsiCanvas::GetLayerBlendAlpha(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return 255;
    return m_layers[static_cast<size_t>(index)].blend_alpha;
}

bool AnsiCanvas::SetLayerBlendAlpha(int index, std::uint8_t alpha)
{
    EnsureDocument();
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    if (m_layers[static_cast<size_t>(index)].blend_alpha == alpha)
        return true;
    m_layers[static_cast<size_t>(index)].blend_alpha = alpha;
    TouchContent();
    return true;
}

bool AnsiCanvas::ConvertToPalette(const phos::colour::PaletteRef& new_ref)
{
    EnsureDocument();

    // Structural-ish in the sense of "whole-canvas transform": prefer snapshot for undo correctness.
    if (!m_undo_capture_active)
        PushUndoSnapshot();
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();

    auto& cs = phos::colour::GetColourSystem();
    const phos::colour::QuantizePolicy qp = phos::colour::DefaultQuantizePolicy();

    // Resolve source and destination palette instances.
    phos::colour::PaletteInstanceId src_id = cs.Palettes().Builtin(phos::colour::BuiltinPalette::Xterm256);
    if (auto id = cs.Palettes().Resolve(m_palette_ref))
        src_id = *id;

    const auto dst_id_opt = cs.Palettes().Resolve(new_ref);
    if (!dst_id_opt.has_value())
        return false;
    const phos::colour::PaletteInstanceId dst_id = *dst_id_opt;

    const phos::colour::Palette* src_p = cs.Palettes().Get(src_id);
    const phos::colour::Palette* dst_p = cs.Palettes().Get(dst_id);
    if (!src_p || !dst_p || src_p->rgb.empty() || dst_p->rgb.empty())
        return false;

    // Remap table: src index -> dst index (prefer cached LUT; fall back to deterministic scan if budget pressure prevents it).
    std::vector<std::uint8_t> remap_fallback;
    const auto remap_lut = cs.Luts().GetOrBuildRemap(cs.Palettes(), src_id, dst_id, qp);
    if (!remap_lut)
    {
        remap_fallback.resize(std::min<std::size_t>(src_p->rgb.size(), 256u), 0u);
        for (std::size_t i = 0; i < remap_fallback.size(); ++i)
        {
            const phos::colour::Rgb8 c = src_p->rgb[i];
            remap_fallback[i] = phos::colour::ColourOps::NearestIndexRgb(cs.Palettes(), dst_id, c.r, c.g, c.b, qp);
        }
    }
    const std::vector<std::uint8_t>& remap = remap_lut ? remap_lut->remap : remap_fallback;

    const std::uint16_t src_max =
        (std::uint16_t)std::min<std::size_t>(remap.empty() ? 0u : (remap.size() - 1), 0xFFu);

    auto remap_idx = [&](ColourIndex16 idx) -> ColourIndex16
    {
        if (idx == kUnsetIndex16)
            return kUnsetIndex16;
        const std::uint16_t si = std::clamp<std::uint16_t>((std::uint16_t)idx, 0, src_max);
        return (ColourIndex16)remap[(size_t)si];
    };

    for (Layer& layer : m_layers)
    {
        for (ColourIndex16& fg : layer.fg)
            fg = remap_idx(fg);
        for (ColourIndex16& bg : layer.bg)
            bg = remap_idx(bg);
    }

    // Swap palette identity after remap so indices remain meaningful.
    m_palette_ref = new_ref;
    TouchContent();
    return true;
}

bool AnsiCanvas::MoveLayer(int from_index, int to_index)
{
    EnsureDocument();
    const int n = static_cast<int>(m_layers.size());
    if (from_index < 0 || from_index >= n)
        return false;
    if (to_index < 0 || to_index >= n)
        return false;
    if (from_index == to_index)
        return true;

    // Structural change: ensure this is undoable even if invoked outside an active capture.
    if (!m_undo_capture_active)
        PushUndoSnapshot();
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();

    Layer moving = std::move(m_layers[(size_t)from_index]);
    m_layers.erase(m_layers.begin() + from_index);
    m_layers.insert(m_layers.begin() + to_index, std::move(moving));

    // Keep active layer pointing at the same logical layer.
    if (m_active_layer == from_index)
        m_active_layer = to_index;
    else if (from_index < to_index)
    {
        // Elements in (from_index, to_index] shift left by 1.
        if (m_active_layer > from_index && m_active_layer <= to_index)
            m_active_layer -= 1;
    }
    else // from_index > to_index
    {
        // Elements in [to_index, from_index) shift right by 1.
        if (m_active_layer >= to_index && m_active_layer < from_index)
            m_active_layer += 1;
    }

    if (m_active_layer < 0) m_active_layer = 0;
    if (m_active_layer >= (int)m_layers.size()) m_active_layer = (int)m_layers.size() - 1;
    return true;
}

bool AnsiCanvas::MoveLayerUp(int index)
{
    return MoveLayer(index, index + 1);
}

bool AnsiCanvas::MoveLayerDown(int index)
{
    return MoveLayer(index, index - 1);
}

bool AnsiCanvas::GetLayerOffset(int layer_index, int& out_x, int& out_y) const
{
    out_x = 0;
    out_y = 0;
    if (m_layers.empty())
        return false;
    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    const Layer& layer = m_layers[(size_t)layer_index];
    out_x = layer.offset_x;
    out_y = layer.offset_y;
    return true;
}

bool AnsiCanvas::SetLayerOffset(int x, int y, int layer_index)
{
    EnsureDocument();
    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    Layer& layer = m_layers[(size_t)layer_index];
    if (layer.offset_x == x && layer.offset_y == y)
        return true;
    PrepareUndoForMutation();
    EnsureUndoCaptureIsPatch();
    layer.offset_x = x;
    layer.offset_y = y;
    return true;
}

bool AnsiCanvas::NudgeLayerOffset(int dx, int dy, int layer_index)
{
    int x = 0, y = 0;
    if (!GetLayerOffset(layer_index, x, y))
        return false;
    return SetLayerOffset(x + dx, y + dy, layer_index);
}

void AnsiCanvas::SetColumns(int columns)
{
    if (columns <= 0)
        return;
    if (columns > 4096)
        columns = 4096;
    EnsureDocument();

    if (columns == m_columns)
        return;

    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();
    const int old_cols = m_columns;
    const int old_rows = m_rows;
    m_columns = columns;

    for (Layer& layer : m_layers)
    {
        std::vector<GlyphId> new_cells;
        std::vector<ColourIndex16> new_fg;
        std::vector<ColourIndex16> new_bg;
        std::vector<Attrs>    new_attrs;

        new_cells.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), BlankGlyph());
        new_fg.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), kUnsetIndex16);
        new_bg.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), kUnsetIndex16);
        new_attrs.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), 0);

        const int copy_cols = std::min(old_cols, m_columns);
        for (int r = 0; r < old_rows; ++r)
        {
            for (int c = 0; c < copy_cols; ++c)
            {
                const size_t src = static_cast<size_t>(r) * static_cast<size_t>(old_cols) + static_cast<size_t>(c);
                const size_t dst = static_cast<size_t>(r) * static_cast<size_t>(m_columns) + static_cast<size_t>(c);
                if (src < layer.cells.size() && dst < new_cells.size())
                    new_cells[dst] = layer.cells[src];
                if (src < layer.fg.size() && dst < new_fg.size())
                    new_fg[dst] = layer.fg[src];
                if (src < layer.bg.size() && dst < new_bg.size())
                    new_bg[dst] = layer.bg[src];
                if (src < layer.attrs.size() && dst < new_attrs.size())
                    new_attrs[dst] = layer.attrs[src];
            }
        }

        layer.cells = std::move(new_cells);
        layer.fg    = std::move(new_fg);
        layer.bg    = std::move(new_bg);
        layer.attrs = std::move(new_attrs);
    }

    // Clamp cursor to new width.
    if (m_caret_col >= m_columns)
        m_caret_col = m_columns - 1;
    if (m_caret_col < 0)
        m_caret_col = 0;

    // If a floating move is active, cancel it (cropping/resize is simpler than re-mapping).
    if (m_move.active)
    {
        m_move = MoveState{};
        m_selection = SelectionState{};
    }
    else if (HasSelection())
    {
        // Clamp selection to new bounds.
        const int max_x = m_columns - 1;
        const int max_y = m_rows - 1;
        if (max_x < 0 || max_y < 0)
        {
            m_selection = SelectionState{};
        }
        else
        {
            int x0 = m_selection.x;
            int y0 = m_selection.y;
            int x1 = m_selection.x + m_selection.w - 1;
            int y1 = m_selection.y + m_selection.h - 1;
            x0 = std::clamp(x0, 0, max_x);
            x1 = std::clamp(x1, 0, max_x);
            y0 = std::clamp(y0, 0, max_y);
            y1 = std::clamp(y1, 0, max_y);
            if (x1 < x0 || y1 < y0)
                m_selection = SelectionState{};
            else
                SetSelectionCorners(x0, y0, x1, y1);
        }
    }

    // Keep SAUCE metadata consistent with the document geometry.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
}

void AnsiCanvas::SetRows(int rows)
{
    if (rows <= 0)
        return;
    EnsureDocument();

    if (rows == m_rows)
        return;

    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();
    m_rows = rows;

    const size_t need = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    for (Layer& layer : m_layers)
    {
        layer.cells.resize(need, BlankGlyph());
        layer.fg.resize(need, kUnsetIndex16);
        layer.bg.resize(need, kUnsetIndex16);
        layer.attrs.resize(need, 0);
    }

    // Clamp caret to new height.
    if (m_caret_row >= m_rows)
        m_caret_row = m_rows - 1;
    if (m_caret_row < 0)
        m_caret_row = 0;

    // Cancel an active floating move (simpler/safer than trying to crop the in-flight payload).
    if (m_move.active)
    {
        m_move = MoveState{};
        m_selection = SelectionState{};
    }
    else if (HasSelection())
    {
        // Clamp selection to new bounds.
        const int max_x = m_columns - 1;
        const int max_y = m_rows - 1;
        if (max_x < 0 || max_y < 0)
        {
            m_selection = SelectionState{};
        }
        else
        {
            int x0 = m_selection.x;
            int y0 = m_selection.y;
            int x1 = m_selection.x + m_selection.w - 1;
            int y1 = m_selection.y + m_selection.h - 1;
            x0 = std::clamp(x0, 0, max_x);
            x1 = std::clamp(x1, 0, max_x);
            y0 = std::clamp(y0, 0, max_y);
            y1 = std::clamp(y1, 0, max_y);
            if (x1 < x0 || y1 < y0)
                m_selection = SelectionState{};
            else
                SetSelectionCorners(x0, y0, x1, y1);
        }
    }

    // Keep SAUCE metadata consistent with the document geometry.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
}

bool AnsiCanvas::LoadFromFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "AnsiCanvas: failed to open '%s'\n", path.c_str());
        return false;
    }

    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());

    EnsureDocument();
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();

    // Reset document to a single empty row.
    m_rows = 1;
    for (Layer& layer : m_layers)
    {
        const size_t count = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
        layer.cells.assign(count, BlankGlyph());
        layer.fg.assign(count, kUnsetIndex16);
        layer.bg.assign(count, kUnsetIndex16);
        layer.attrs.assign(count, 0);
    }

    std::vector<char32_t> cps;
    DecodeUtf8(bytes, cps);

    int row = 0;
    int col = 0;
    bool last_was_cr = false;

    for (char32_t cp : cps)
    {
        // Normalize CRLF.
        if (cp == U'\r')
        {
            last_was_cr = true;
            row++;
            col = 0;
            EnsureRows(row + 1);
            continue;
        }
        if (cp == U'\n')
        {
            if (last_was_cr)
            {
                last_was_cr = false;
                continue;
            }
            row++;
            col = 0;
            EnsureRows(row + 1);
            continue;
        }
        last_was_cr = false;

        // Filter control chars for now (ANSI parsing will come later).
        if (cp == U'\t')
            cp = U' ';
        if (cp < 0x20)
            continue;

        SetActiveCell(row, col, cp);
        col++;
        if (col >= m_columns)
        {
            row++;
            col = 0;
            EnsureRows(row + 1);
        }
    }

    m_caret_row = 0;
    m_caret_col = 0;

    // Loaded content establishes a concrete geometry; reflect it in SAUCE.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
    return true;
}

void AnsiCanvas::EnsureDocument()
{
    bool changed = false;
    if (m_columns <= 0)
    {
        m_columns = 80;
        changed = true;
    }
    if (m_rows <= 0)
    {
        m_rows = 1;
        changed = true;
    }

    if (m_layers.empty())
    {
        Layer base;
        base.name = "Base";
        base.visible = true;
        const size_t count = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
        base.cells.assign(count, BlankGlyph());
        base.fg.assign(count, kUnsetIndex16);
        base.bg.assign(count, kUnsetIndex16);
        base.attrs.assign(count, 0);
        m_layers.push_back(std::move(base));
        m_active_layer = 0;
        changed = true;
    }

    // Ensure every layer has the correct cell count.
    const size_t need = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    for (Layer& layer : m_layers)
    {
        if (layer.cells.size() != need)
        {
            layer.cells.resize(need, BlankGlyph());
            changed = true;
        }
        if (layer.fg.size() != need)
        {
            layer.fg.resize(need, kUnsetIndex16);
            changed = true;
        }
        if (layer.bg.size() != need)
        {
            layer.bg.resize(need, kUnsetIndex16);
            changed = true;
        }
        if (layer.attrs.size() != need)
        {
            layer.attrs.resize(need, 0);
            changed = true;
        }
    }

    if (m_active_layer < 0)
    {
        m_active_layer = 0;
        changed = true;
    }
    if (m_active_layer >= (int)m_layers.size())
    {
        m_active_layer = (int)m_layers.size() - 1;
        changed = true;
    }

    // Performance: EnsureDocument() is called from hot paths (per-cell tool/script writes).
    // SAUCE defaults/geometry only need syncing when we actually had to repair/init state here.
    if (changed)
        EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
}

void AnsiCanvas::EnsureRows(int rows_needed)
{
    if (rows_needed <= 0)
        rows_needed = 1;

    EnsureDocument();
    if (rows_needed <= m_rows)
        return;

    PrepareUndoForMutation();
    EnsureUndoCaptureIsPatch();
    m_rows = rows_needed;
    const size_t need = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    for (Layer& layer : m_layers)
    {
        // Growing one row at a time (common during mouse painting downward) can cause many
        // expensive reallocations/copies on large canvases. Reserve a modest amount of slack
        // capacity so repeated EnsureRows() calls are amortized, without changing `m_rows`
        // (visible canvas size) or any behavior.
        auto reserve_with_slack = [&](auto& v)
        {
            if (need <= v.size())
                return;
            if (need <= v.capacity())
                return;
            // Slack heuristic: ~12.5% extra, or ~64 rows worth of cells (whichever larger).
            const size_t row_chunk = static_cast<size_t>(std::max(1, m_columns)) * static_cast<size_t>(64);
            const size_t frac = need / 8;
            size_t slack = std::max(row_chunk, frac);
            // Avoid overflow.
            size_t want = need;
            if (slack > 0 && want <= (std::numeric_limits<size_t>::max() - slack))
                want += slack;
            v.reserve(want);
        };

        reserve_with_slack(layer.cells);
        reserve_with_slack(layer.fg);
        reserve_with_slack(layer.bg);
        reserve_with_slack(layer.attrs);
        layer.cells.resize(need, BlankGlyph());
        layer.fg.resize(need, kUnsetIndex16);
        layer.bg.resize(need, kUnsetIndex16);
        layer.attrs.resize(need, 0);
    }

    // Row growth should always be reflected in SAUCE (screen height hint).
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
}

size_t AnsiCanvas::CellIndex(int row, int col) const
{
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;
    return static_cast<size_t>(row) * static_cast<size_t>(m_columns) + static_cast<size_t>(col);
}

bool AnsiCanvas::CanvasToLayerLocalForWrite(int layer_index,
                                           int canvas_row,
                                           int canvas_col,
                                           int& out_local_row,
                                           int& out_local_col) const
{
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    if (m_columns <= 0)
        return false;

    const Layer& layer = m_layers[(size_t)layer_index];
    // Fast path: most layers sit at (0,0). Avoid 64-bit math + extra branches.
    // Note: Write-conversion intentionally does NOT clamp/check row upper bound because
    // the document can grow on demand (EnsureRows happens at the mutation site).
    if (layer.offset_x == 0 && layer.offset_y == 0)
    {
        if (canvas_row < 0 || canvas_col < 0)
            return false;
        if (canvas_col >= m_columns)
            return false;
        out_local_row = canvas_row;
        out_local_col = canvas_col;
        return true;
    }
    const long long lr = (long long)canvas_row - (long long)layer.offset_y;
    const long long lc = (long long)canvas_col - (long long)layer.offset_x;
    if (lr < 0 || lc < 0)
        return false;
    if (lc >= (long long)m_columns)
        return false;
    if (lr > (long long)std::numeric_limits<int>::max() || lc > (long long)std::numeric_limits<int>::max())
        return false;
    out_local_row = (int)lr;
    out_local_col = (int)lc;
    return true;
}

bool AnsiCanvas::CanvasToLayerLocalForRead(int layer_index,
                                          int canvas_row,
                                          int canvas_col,
                                          int& out_local_row,
                                          int& out_local_col) const
{
    if (!CanvasToLayerLocalForWrite(layer_index, canvas_row, canvas_col, out_local_row, out_local_col))
        return false;
    if (out_local_row < 0 || out_local_row >= m_rows)
        return false;
    return true;
}

AnsiCanvas::CompositeCell AnsiCanvas::GetCompositeCell(int row, int col) const
{
    CompositeCell out;
    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return out;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return out;

    // Fast path: if every visible layer is Normal @ 100% opacity, compositing reduces to the legacy
    // "topmost wins" rules and we should NOT pay the blend+quantize cost.
    //
    // This is a common case (default layer settings) and it's critical for script performance.
    bool all_visible_normal_opaque = true;
    bool any_visible = false;
    for (const Layer& layer : m_layers)
    {
        if (!layer.visible)
            continue;
        any_visible = true;
        if (layer.blend_mode != phos::LayerBlendMode::Normal || layer.blend_alpha != 255)
        {
            all_visible_normal_opaque = false;
            break;
        }
    }
    if (!any_visible)
        return out;
    if (all_visible_normal_opaque)
    {
        // Background: topmost visible non-unset bg wins.
        for (int i = (int)m_layers.size() - 1; i >= 0; --i)
        {
            const Layer& layer = m_layers[(size_t)i];
            if (!layer.visible)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForRead(i, row, col, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx < layer.bg.size())
            {
                const ColourIndex16 bg = layer.bg[idx];
                if (bg != kUnsetIndex16)
                {
                    out.bg = bg;
                    break;
                }
            }
        }

        // Glyph/fg/attrs: topmost visible non-space glyph wins; attrs only apply with glyph.
        for (int i = (int)m_layers.size() - 1; i >= 0; --i)
        {
            const Layer& layer = m_layers[(size_t)i];
            if (!layer.visible)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForRead(i, row, col, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx >= layer.cells.size())
                continue;
            const GlyphId g = layer.cells[idx];
            if (phos::glyph::IsBlank((phos::GlyphId)g))
                continue;
            out.glyph = g;
            out.cp = GlyphIdToUnicodeRepresentative(g);
            out.fg = (idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
            out.attrs = (idx < layer.attrs.size()) ? layer.attrs[idx] : 0;
            break;
        }

        return out;
    }

    // Compositing rules (preserved + extended, Phase D v1):
    // - Glyph: topmost visible non-space glyph wins (attrs only apply with glyph).
    // - Background: blending accumulated back -> front:
    //     - bg == Unset means "transparent/no-fill" and contributes nothing.
    //     - first concrete bg becomes the base; each subsequent concrete bg blends over it
    //       using the CURRENT (upper) layer's blend mode.
    // - Foreground: if a glyph is present, blend the chosen glyph layer's fg against:
    //     - the visible fg underneath (next glyph below), if present
    //     - otherwise the composited background (or paper if no bg)
    //   using the SAME layer blend mode.
    //
    // Note: bg needs back->front order; glyph selection remains top->bottom.

    // ---- Background plane (background-only blend, v1) ----
    std::optional<std::uint8_t> out_bg_idx;
    {
        auto& cs = phos::colour::GetColourSystem();
        const phos::colour::PaletteInstanceId pal = ResolveActivePaletteId();
        const phos::colour::Palette* p = cs.Palettes().Get(pal);
        const phos::colour::QuantizePolicy qp = phos::colour::DefaultQuantizePolicy();
        const phos::colour::Rgb8 paper = PaperRgb(IsCanvasBackgroundWhite());

        // Fallback path: if palette isn't available, preserve legacy "topmost bg wins".
        if (!p || p->rgb.empty())
        {
            for (int i = (int)m_layers.size() - 1; i >= 0; --i)
            {
                const Layer& layer = m_layers[(size_t)i];
                if (!layer.visible)
                    continue;
                int lr = 0, lc = 0;
                if (!CanvasToLayerLocalForRead(i, row, col, lr, lc))
                    continue;
                const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
                if (idx >= layer.bg.size())
                    continue;
                const ColourIndex16 bg = layer.bg[idx];
                if (bg == kUnsetIndex16)
                    continue;
                out.bg = bg;
                break;
            }
            // Even without a palette, keep out_bg_rgb unset so fg blending later can safely fall back to paper.
            goto bg_done;
        }

        const std::uint8_t paper_i =
            phos::colour::ColourOps::NearestIndexRgb(cs.Palettes(), pal, paper.r, paper.g, paper.b, qp);

        for (int i = 0; i < (int)m_layers.size(); ++i)
        {
            const Layer& layer = m_layers[(size_t)i];
            if (!layer.visible)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForRead(i, row, col, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx >= layer.bg.size())
                continue;
            const ColourIndex16 src_bg = layer.bg[idx];
            if (src_bg == kUnsetIndex16)
                continue;
            if (layer.blend_alpha == 0)
                continue; // fully transparent contribution

            const std::uint8_t src_i = ClampPaletteIndexU8(p, src_bg);
            const std::uint8_t base_i = out_bg_idx.has_value() ? *out_bg_idx : paper_i;

            const auto blut = cs.Luts().GetOrBuildBlend(cs.Palettes(), pal, layer.blend_mode, layer.blend_alpha, qp);
            if (blut && blut->pal_size == (std::uint16_t)p->rgb.size())
            {
                const std::size_t n = p->rgb.size();
                out_bg_idx = blut->table[(std::size_t)base_i * n + (std::size_t)src_i];
            }
            else
            {
                // Exact fallback (must match LUT builder semantics).
                const phos::colour::Rgb8 base_rgb = p->rgb[(size_t)base_i];
                const phos::colour::Rgb8 src_rgb = p->rgb[(size_t)src_i];
                const phos::colour::Rgb8 res_rgb = phos::colour::BlendOverRgb(base_rgb, src_rgb, layer.blend_mode, layer.blend_alpha);
                const std::uint8_t oi =
                    phos::colour::ColourOps::NearestIndexRgb(cs.Palettes(), pal, res_rgb.r, res_rgb.g, res_rgb.b, qp);
                out_bg_idx = oi;
            }
        }
        out.bg = out_bg_idx.has_value() ? (ColourIndex16)(*out_bg_idx) : kUnsetIndex16;
    }
bg_done:

    // ---- Glyph / attrs (topmost non-space glyph wins, preserved) ----
    phos::LayerBlendMode glyph_blend_mode = phos::LayerBlendMode::Normal;
    std::uint8_t glyph_blend_alpha = 255;
    ColourIndex16 under_fg = kUnsetIndex16;
    bool have_under_glyph = false;
    bool top_fg_was_unset = false;
    for (int i = (int)m_layers.size() - 1; i >= 0; --i)
    {
        const Layer& layer = m_layers[(size_t)i];
        if (!layer.visible)
            continue;
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForRead(i, row, col, lr, lc))
            continue;
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (idx >= layer.cells.size())
            continue;
        const GlyphId g = layer.cells[idx];
        if (phos::glyph::IsBlank((phos::GlyphId)g))
            continue;

        if (phos::glyph::IsBlank((phos::GlyphId)out.glyph))
        {
            // First (topmost) glyph: choose glyph/attrs from here.
            out.glyph = g;
            out.cp = GlyphIdToUnicodeRepresentative(g);
            out.fg = (idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
            out.attrs = (idx < layer.attrs.size()) ? layer.attrs[idx] : 0;
            glyph_blend_mode = layer.blend_mode;
            glyph_blend_alpha = layer.blend_alpha;
            top_fg_was_unset = (out.fg == kUnsetIndex16);
        }
        else
        {
            // Next glyph below: its fg participates as the base for fg blending.
            under_fg = (idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
            have_under_glyph = true;
            break;
        }
    }

    // ---- Foreground blend: blend chosen glyph fg against underlying fg (if present) else bg ----
    if (!phos::glyph::IsBlank((phos::GlyphId)out.glyph))
    {
        auto& cs = phos::colour::GetColourSystem();
        const phos::colour::PaletteInstanceId pal = ResolveActivePaletteId();
        const phos::colour::Palette* p = cs.Palettes().Get(pal);
        const phos::colour::QuantizePolicy qp = phos::colour::DefaultQuantizePolicy();
        if (p && !p->rgb.empty())
        {
            const bool paper_white = IsCanvasBackgroundWhite();
            const phos::colour::Rgb8 paper = PaperRgb(paper_white);
            const std::uint8_t paper_i =
                phos::colour::ColourOps::NearestIndexRgb(cs.Palettes(), pal, paper.r, paper.g, paper.b, qp);
            const phos::colour::Rgb8 def_fg_rgb = DefaultFgRgb(paper_white);
            const std::uint8_t def_fg_i =
                phos::colour::ColourOps::NearestIndexRgb(cs.Palettes(), pal, def_fg_rgb.r, def_fg_rgb.g, def_fg_rgb.b, qp);

            // Source fg (top glyph layer) for blend math:
            // - If unset: use theme default fg *as a colour* for blending, but preserve "unset" output when blending is identity.
            const std::uint8_t src_i =
                (out.fg == kUnsetIndex16) ? def_fg_i : ClampPaletteIndexU8(p, out.fg);

            // Base for fg blending:
            // - If there is a glyph below, use its fg (or default fg if unset).
            // - Otherwise, use composited background (or paper).
            std::uint8_t base_i = paper_i;
            if (have_under_glyph)
            {
                base_i = (under_fg == kUnsetIndex16) ? def_fg_i : ClampPaletteIndexU8(p, under_fg);
            }
            else
            {
                base_i = (out.bg != kUnsetIndex16) ? ClampPaletteIndexU8(p, out.bg) : paper_i;
            }

            // Foreground blending opacity is the layer opacity only.
            // (Glyph coverage is handled by the renderer when drawing the glyph.)
            const std::uint8_t alpha = glyph_blend_alpha;

            if (alpha == 0)
            {
                // Fully transparent effect: make ink match background (glyph becomes visually invisible).
                out.fg = (ColourIndex16)base_i;
            }
            else
            {
                // Preserve "theme default fg" semantics when the blend would be an identity.
                // (Normal @ 100% does not depend on base and should remain unset if it started unset.)
                const bool identity = (glyph_blend_mode == phos::LayerBlendMode::Normal && alpha == 255 && !have_under_glyph);
                if (top_fg_was_unset && identity)
                {
                    out.fg = kUnsetIndex16;
                }
                else
                {
                    const auto blut = cs.Luts().GetOrBuildBlend(cs.Palettes(), pal, glyph_blend_mode, alpha, qp);
                    if (blut && blut->pal_size == (std::uint16_t)p->rgb.size())
                    {
                        const std::size_t n = p->rgb.size();
                        out.fg = (ColourIndex16)blut->table[(std::size_t)base_i * n + (std::size_t)src_i];
                    }
                    else
                    {
                        const phos::colour::Rgb8 base_rgb = p->rgb[(size_t)base_i];
                        const phos::colour::Rgb8 src_rgb = p->rgb[(size_t)src_i];
                        const phos::colour::Rgb8 res_rgb = phos::colour::BlendOverRgb(base_rgb, src_rgb, glyph_blend_mode, alpha);
                        const std::uint8_t oi =
                            phos::colour::ColourOps::NearestIndexRgb(cs.Palettes(), pal, res_rgb.r, res_rgb.g, res_rgb.b, qp);
                        out.fg = (ColourIndex16)oi;
                    }
                }
            }
        }
    }

    return out;
}

void AnsiCanvas::SetActiveCell(int row, int col, char32_t cp)
{
    EnsureDocument();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    auto write_one = [&](int write_col)
    {
        if (!ToolWriteAllowed(row, write_col))
            return;
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(m_active_layer, row, write_col, lr, lc))
            return;

        Layer& layer = m_layers[(size_t)m_active_layer];
        // Determine old cell value without growing the document unless we will mutate.
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const GlyphId old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
        const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const GlyphId new_cp = phos::glyph::MakeUnicodeScalar(cp);
        const ColourIndex16 new_fg = old_fg;
        const ColourIndex16 new_bg = old_bg;
        const Attrs    new_attrs = old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return;
        if (in_bounds && old_cp == new_cp)
            return; // no-op

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(m_active_layer, lr);
        if (lr >= m_rows)
            EnsureRows(lr + 1);
        const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (widx < layer.cells.size())
            layer.cells[widx] = new_cp;
        if (widx < layer.attrs.size())
            layer.attrs[widx] = new_attrs;
    };

    write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            write_one(mirror_col);
    }
}

void AnsiCanvas::SetActiveCell(int row, int col, char32_t cp, Colour32 fg, Colour32 bg)
{
    EnsureDocument();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    auto write_one = [&](int write_col)
    {
        if (!ToolWriteAllowed(row, write_col))
            return;
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(m_active_layer, row, write_col, lr, lc))
            return;

        Layer& layer = m_layers[(size_t)m_active_layer];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const GlyphId old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
        const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const GlyphId new_cp = phos::glyph::MakeUnicodeScalar(cp);
        const ColourIndex16 new_fg = QuantizeColour32ToIndex(fg);
        const ColourIndex16 new_bg = QuantizeColour32ToIndex(bg);
        const Attrs    new_attrs = old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return;
        if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
            return; // no-op

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(m_active_layer, lr);
        if (lr >= m_rows)
            EnsureRows(lr + 1);

        const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (widx < layer.cells.size())
            layer.cells[widx] = new_cp;
        if (widx < layer.fg.size())
            layer.fg[widx] = new_fg;
        if (widx < layer.bg.size())
            layer.bg[widx] = new_bg;
        if (widx < layer.attrs.size())
            layer.attrs[widx] = new_attrs;
    };

    write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            write_one(mirror_col);
    }
}

void AnsiCanvas::ClearActiveCellStyle(int row, int col)
{
    EnsureDocument();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    auto write_one = [&](int write_col)
    {
        if (!ToolWriteAllowed(row, write_col))
            return;
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(m_active_layer, row, write_col, lr, lc))
            return;

        Layer& layer = m_layers[(size_t)m_active_layer];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const GlyphId old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
        const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const char32_t new_cp = old_cp;
        const ColourIndex16 new_fg = kUnsetIndex16;
        const ColourIndex16 new_bg = kUnsetIndex16;
        const Attrs    new_attrs = 0;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return;
        if (in_bounds && old_fg == 0 && old_bg == 0 && old_attrs == 0)
            return; // no-op

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(m_active_layer, lr);
        if (lr >= m_rows)
            EnsureRows(lr + 1);
        const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (widx < layer.fg.size())
            layer.fg[widx] = kUnsetIndex16;
        if (widx < layer.bg.size())
            layer.bg[widx] = kUnsetIndex16;
        if (widx < layer.attrs.size())
            layer.attrs[widx] = new_attrs;
    };

    write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            write_one(mirror_col);
    }
}

bool AnsiCanvas::DeleteForwardShift(int layer_index)
{
    EnsureDocument();

    if (m_columns <= 0 || m_rows <= 0)
        return false;
    if (m_caret_row < 0 || m_caret_row >= m_rows)
        return false;

    const int canvas_row = m_caret_row;
    int canvas_col = m_caret_col;
    if (canvas_col < 0) canvas_col = 0;
    if (canvas_col >= m_columns) canvas_col = m_columns - 1;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;

    struct PendingWrite
    {
        int lr = 0;
        int lc = 0;
        GlyphId       old_cp = BlankGlyph();
        ColourIndex16 old_fg = kUnsetIndex16;
        ColourIndex16 old_bg = kUnsetIndex16;
        Attrs         old_attrs = 0;
        GlyphId       new_cp = BlankGlyph();
        ColourIndex16 new_fg = kUnsetIndex16;
        ColourIndex16 new_bg = kUnsetIndex16;
        Attrs         new_attrs = 0;
    };

    std::vector<PendingWrite> writes;
    writes.reserve((size_t)std::max(0, m_columns - canvas_col));

    // Build the write set first so we can keep the operation coherent (especially with transparency lock).
    for (int dcol = canvas_col; dcol < m_columns; ++dcol)
    {
        if (!ToolWriteAllowed(canvas_row, dcol))
            continue;

        int dlr = 0, dlc = 0;
        if (!CanvasToLayerLocalForReadFast(canvas_row, dcol, off_x, off_y, m_columns, m_rows, dlr, dlc))
            continue;

        const size_t didx = (size_t)dlr * (size_t)m_columns + (size_t)dlc;
        const GlyphId       old_cp = (didx < layer.cells.size()) ? layer.cells[didx] : BlankGlyph();
        const ColourIndex16 old_fg = (didx < layer.fg.size()) ? layer.fg[didx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (didx < layer.bg.size()) ? layer.bg[didx] : kUnsetIndex16;
        const Attrs         old_attrs = (didx < layer.attrs.size()) ? layer.attrs[didx] : 0;

        GlyphId       new_cp = BlankGlyph();
        ColourIndex16 new_fg = kUnsetIndex16;
        ColourIndex16 new_bg = kUnsetIndex16;
        Attrs         new_attrs = 0;

        if (dcol < (m_columns - 1))
        {
            const int scol = dcol + 1;
            int slr = 0, slc = 0;
            if (CanvasToLayerLocalForReadFast(canvas_row, scol, off_x, off_y, m_columns, m_rows, slr, slc))
            {
                const size_t sidx = (size_t)slr * (size_t)m_columns + (size_t)slc;
                if (sidx < layer.cells.size()) new_cp = layer.cells[sidx];
                if (sidx < layer.fg.size())    new_fg = layer.fg[sidx];
                if (sidx < layer.bg.size())    new_bg = layer.bg[sidx];
                if (sidx < layer.attrs.size()) new_attrs = layer.attrs[sidx];
            }
        }

        if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
            continue;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                           old_cp, old_fg, old_bg, old_attrs,
                                           new_cp, new_fg, new_bg, new_attrs))
        {
            // Structural op: avoid partial shifts under transparency lock.
            return false;
        }

        PendingWrite w;
        w.lr = dlr;
        w.lc = dlc;
        w.old_cp = old_cp;
        w.old_fg = old_fg;
        w.old_bg = old_bg;
        w.old_attrs = old_attrs;
        w.new_cp = new_cp;
        w.new_fg = new_fg;
        w.new_bg = new_bg;
        w.new_attrs = new_attrs;
        writes.push_back(w);
    }

    if (writes.empty())
        return false;

    PrepareUndoForMutation();
    EnsureUndoCaptureIsPatch();

    // Capture affected pages (typically just one row).
    for (const auto& w : writes)
        CaptureUndoPageIfNeeded(layer_index, w.lr);

    for (const auto& w : writes)
    {
        const size_t idx = (size_t)w.lr * (size_t)m_columns + (size_t)w.lc;
        if (idx < layer.cells.size()) layer.cells[idx] = w.new_cp;
        if (idx < layer.fg.size())    layer.fg[idx]    = w.new_fg;
        if (idx < layer.bg.size())    layer.bg[idx]    = w.new_bg;
        if (idx < layer.attrs.size()) layer.attrs[idx] = w.new_attrs;
    }

    return true;
}

bool AnsiCanvas::RemoveRowShiftUp(int row, int layer_index)
{
    EnsureDocument();

    if (m_columns <= 0 || m_rows <= 0)
        return false;
    if (row < 0 || row >= m_rows)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;

    struct PendingWrite
    {
        int lr = 0;
        int lc = 0;
        GlyphId       old_cp = BlankGlyph();
        ColourIndex16 old_fg = kUnsetIndex16;
        ColourIndex16 old_bg = kUnsetIndex16;
        Attrs         old_attrs = 0;
        GlyphId       new_cp = BlankGlyph();
        ColourIndex16 new_fg = kUnsetIndex16;
        ColourIndex16 new_bg = kUnsetIndex16;
        Attrs         new_attrs = 0;
    };

    std::vector<PendingWrite> writes;
    writes.reserve((size_t)std::max(0, (m_rows - row) * m_columns));

    // Build the write set first so the operation is all-or-nothing under transparency lock.
    // Note: This operation is a selection structural op; it must NOT be clipped by ToolWriteAllowed().
    for (int cy = row; cy < m_rows; ++cy)
    {
        for (int cx = 0; cx < m_columns; ++cx)
        {
            int dlr = 0, dlc = 0;
            if (!CanvasToLayerLocalForReadFast(cy, cx, off_x, off_y, m_columns, m_rows, dlr, dlc))
                continue;

            const size_t didx = (size_t)dlr * (size_t)m_columns + (size_t)dlc;
            const GlyphId       old_cp = (didx < layer.cells.size()) ? layer.cells[didx] : BlankGlyph();
            const ColourIndex16 old_fg = (didx < layer.fg.size()) ? layer.fg[didx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (didx < layer.bg.size()) ? layer.bg[didx] : kUnsetIndex16;
            const Attrs         old_attrs = (didx < layer.attrs.size()) ? layer.attrs[didx] : 0;

            GlyphId       new_cp = BlankGlyph();
            ColourIndex16 new_fg = kUnsetIndex16;
            ColourIndex16 new_bg = kUnsetIndex16;
            Attrs         new_attrs = 0;

            if (cy < (m_rows - 1))
            {
                const int sy = cy + 1;
                int slr = 0, slc = 0;
                if (CanvasToLayerLocalForReadFast(sy, cx, off_x, off_y, m_columns, m_rows, slr, slc))
                {
                    const size_t sidx = (size_t)slr * (size_t)m_columns + (size_t)slc;
                    if (sidx < layer.cells.size()) new_cp = layer.cells[sidx];
                    if (sidx < layer.fg.size())    new_fg = layer.fg[sidx];
                    if (sidx < layer.bg.size())    new_bg = layer.bg[sidx];
                    if (sidx < layer.attrs.size()) new_attrs = layer.attrs[sidx];
                }
            }

            if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                               old_cp, old_fg, old_bg, old_attrs,
                                               new_cp, new_fg, new_bg, new_attrs))
            {
                // Structural op: avoid partial shifts under transparency lock.
                return false;
            }

            PendingWrite w;
            w.lr = dlr;
            w.lc = dlc;
            w.old_cp = old_cp;
            w.old_fg = old_fg;
            w.old_bg = old_bg;
            w.old_attrs = old_attrs;
            w.new_cp = new_cp;
            w.new_fg = new_fg;
            w.new_bg = new_bg;
            w.new_attrs = new_attrs;
            writes.push_back(w);
        }
    }

    if (writes.empty())
        return false;

    PrepareUndoForMutation();
    EnsureUndoCaptureIsPatch();

    for (const auto& w : writes)
        CaptureUndoPageIfNeeded(layer_index, w.lr);

    for (const auto& w : writes)
    {
        const size_t idx = (size_t)w.lr * (size_t)m_columns + (size_t)w.lc;
        if (idx < layer.cells.size()) layer.cells[idx] = w.new_cp;
        if (idx < layer.fg.size())    layer.fg[idx]    = w.new_fg;
        if (idx < layer.bg.size())    layer.bg[idx]    = w.new_bg;
        if (idx < layer.attrs.size()) layer.attrs[idx] = w.new_attrs;
    }

    return true;
}

bool AnsiCanvas::RemoveColumnShiftLeft(int col, int layer_index)
{
    EnsureDocument();

    if (m_columns <= 0 || m_rows <= 0)
        return false;
    if (col < 0 || col >= m_columns)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;

    struct PendingWrite
    {
        int lr = 0;
        int lc = 0;
        GlyphId       old_cp = BlankGlyph();
        ColourIndex16 old_fg = kUnsetIndex16;
        ColourIndex16 old_bg = kUnsetIndex16;
        Attrs         old_attrs = 0;
        GlyphId       new_cp = BlankGlyph();
        ColourIndex16 new_fg = kUnsetIndex16;
        ColourIndex16 new_bg = kUnsetIndex16;
        Attrs         new_attrs = 0;
    };

    std::vector<PendingWrite> writes;
    writes.reserve((size_t)std::max(0, m_rows * (m_columns - col)));

    // Build the write set first so the operation is all-or-nothing under transparency lock.
    // Note: This operation is a selection structural op; it must NOT be clipped by ToolWriteAllowed().
    for (int cy = 0; cy < m_rows; ++cy)
    {
        for (int cx = col; cx < m_columns; ++cx)
        {
            int dlr = 0, dlc = 0;
            if (!CanvasToLayerLocalForReadFast(cy, cx, off_x, off_y, m_columns, m_rows, dlr, dlc))
                continue;

            const size_t didx = (size_t)dlr * (size_t)m_columns + (size_t)dlc;
            const GlyphId       old_cp = (didx < layer.cells.size()) ? layer.cells[didx] : BlankGlyph();
            const ColourIndex16 old_fg = (didx < layer.fg.size()) ? layer.fg[didx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (didx < layer.bg.size()) ? layer.bg[didx] : kUnsetIndex16;
            const Attrs         old_attrs = (didx < layer.attrs.size()) ? layer.attrs[didx] : 0;

            GlyphId       new_cp = BlankGlyph();
            ColourIndex16 new_fg = kUnsetIndex16;
            ColourIndex16 new_bg = kUnsetIndex16;
            Attrs         new_attrs = 0;

            if (cx < (m_columns - 1))
            {
                const int sx = cx + 1;
                int slr = 0, slc = 0;
                if (CanvasToLayerLocalForReadFast(cy, sx, off_x, off_y, m_columns, m_rows, slr, slc))
                {
                    const size_t sidx = (size_t)slr * (size_t)m_columns + (size_t)slc;
                    if (sidx < layer.cells.size()) new_cp = layer.cells[sidx];
                    if (sidx < layer.fg.size())    new_fg = layer.fg[sidx];
                    if (sidx < layer.bg.size())    new_bg = layer.bg[sidx];
                    if (sidx < layer.attrs.size()) new_attrs = layer.attrs[sidx];
                }
            }

            if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                               old_cp, old_fg, old_bg, old_attrs,
                                               new_cp, new_fg, new_bg, new_attrs))
            {
                // Structural op: avoid partial shifts under transparency lock.
                return false;
            }

            PendingWrite w;
            w.lr = dlr;
            w.lc = dlc;
            w.old_cp = old_cp;
            w.old_fg = old_fg;
            w.old_bg = old_bg;
            w.old_attrs = old_attrs;
            w.new_cp = new_cp;
            w.new_fg = new_fg;
            w.new_bg = new_bg;
            w.new_attrs = new_attrs;
            writes.push_back(w);
        }
    }

    if (writes.empty())
        return false;

    PrepareUndoForMutation();
    EnsureUndoCaptureIsPatch();

    for (const auto& w : writes)
        CaptureUndoPageIfNeeded(layer_index, w.lr);

    for (const auto& w : writes)
    {
        const size_t idx = (size_t)w.lr * (size_t)m_columns + (size_t)w.lc;
        if (idx < layer.cells.size()) layer.cells[idx] = w.new_cp;
        if (idx < layer.fg.size())    layer.fg[idx]    = w.new_fg;
        if (idx < layer.bg.size())    layer.bg[idx]    = w.new_bg;
        if (idx < layer.attrs.size()) layer.attrs[idx] = w.new_attrs;
    }

    return true;
}

bool AnsiCanvas::InsertRowShiftDown(int row, int layer_index)
{
    EnsureDocument();

    if (m_columns <= 0 || m_rows <= 0)
        return false;
    if (row < 0 || row >= m_rows)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;

    struct PendingWrite
    {
        int lr = 0;
        int lc = 0;
        GlyphId       old_cp = BlankGlyph();
        ColourIndex16 old_fg = kUnsetIndex16;
        ColourIndex16 old_bg = kUnsetIndex16;
        Attrs         old_attrs = 0;
        GlyphId       new_cp = BlankGlyph();
        ColourIndex16 new_fg = kUnsetIndex16;
        ColourIndex16 new_bg = kUnsetIndex16;
        Attrs         new_attrs = 0;
    };

    std::vector<PendingWrite> writes;
    writes.reserve((size_t)std::max(0, (m_rows - row) * m_columns));

    // Build the write set first so the operation is all-or-nothing under transparency lock.
    // Note: This operation is a selection structural op; it must NOT be clipped by ToolWriteAllowed().
    for (int cy = row; cy < m_rows; ++cy)
    {
        for (int cx = 0; cx < m_columns; ++cx)
        {
            int dlr = 0, dlc = 0;
            if (!CanvasToLayerLocalForReadFast(cy, cx, off_x, off_y, m_columns, m_rows, dlr, dlc))
                continue;

            const size_t didx = (size_t)dlr * (size_t)m_columns + (size_t)dlc;
            const GlyphId       old_cp = (didx < layer.cells.size()) ? layer.cells[didx] : BlankGlyph();
            const ColourIndex16 old_fg = (didx < layer.fg.size()) ? layer.fg[didx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (didx < layer.bg.size()) ? layer.bg[didx] : kUnsetIndex16;
            const Attrs         old_attrs = (didx < layer.attrs.size()) ? layer.attrs[didx] : 0;

            GlyphId       new_cp = BlankGlyph();
            ColourIndex16 new_fg = kUnsetIndex16;
            ColourIndex16 new_bg = kUnsetIndex16;
            Attrs         new_attrs = 0;

            if (cy > row)
            {
                const int sy = cy - 1;
                int slr = 0, slc = 0;
                if (CanvasToLayerLocalForReadFast(sy, cx, off_x, off_y, m_columns, m_rows, slr, slc))
                {
                    const size_t sidx = (size_t)slr * (size_t)m_columns + (size_t)slc;
                    if (sidx < layer.cells.size()) new_cp = layer.cells[sidx];
                    if (sidx < layer.fg.size())    new_fg = layer.fg[sidx];
                    if (sidx < layer.bg.size())    new_bg = layer.bg[sidx];
                    if (sidx < layer.attrs.size()) new_attrs = layer.attrs[sidx];
                }
            }
            // else: cy == row -> inserted blank row

            if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                               old_cp, old_fg, old_bg, old_attrs,
                                               new_cp, new_fg, new_bg, new_attrs))
            {
                // Structural op: avoid partial shifts under transparency lock.
                return false;
            }

            PendingWrite w;
            w.lr = dlr;
            w.lc = dlc;
            w.old_cp = old_cp;
            w.old_fg = old_fg;
            w.old_bg = old_bg;
            w.old_attrs = old_attrs;
            w.new_cp = new_cp;
            w.new_fg = new_fg;
            w.new_bg = new_bg;
            w.new_attrs = new_attrs;
            writes.push_back(w);
        }
    }

    if (writes.empty())
        return false;

    PrepareUndoForMutation();
    EnsureUndoCaptureIsPatch();

    for (const auto& w : writes)
        CaptureUndoPageIfNeeded(layer_index, w.lr);

    for (const auto& w : writes)
    {
        const size_t idx = (size_t)w.lr * (size_t)m_columns + (size_t)w.lc;
        if (idx < layer.cells.size()) layer.cells[idx] = w.new_cp;
        if (idx < layer.fg.size())    layer.fg[idx]    = w.new_fg;
        if (idx < layer.bg.size())    layer.bg[idx]    = w.new_bg;
        if (idx < layer.attrs.size()) layer.attrs[idx] = w.new_attrs;
    }

    return true;
}

bool AnsiCanvas::InsertColumnShiftRight(int col, int layer_index)
{
    EnsureDocument();

    if (m_columns <= 0 || m_rows <= 0)
        return false;
    if (col < 0 || col >= m_columns)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;

    struct PendingWrite
    {
        int lr = 0;
        int lc = 0;
        GlyphId       old_cp = BlankGlyph();
        ColourIndex16 old_fg = kUnsetIndex16;
        ColourIndex16 old_bg = kUnsetIndex16;
        Attrs         old_attrs = 0;
        GlyphId       new_cp = BlankGlyph();
        ColourIndex16 new_fg = kUnsetIndex16;
        ColourIndex16 new_bg = kUnsetIndex16;
        Attrs         new_attrs = 0;
    };

    std::vector<PendingWrite> writes;
    writes.reserve((size_t)std::max(0, m_rows * (m_columns - col)));

    // Build the write set first so the operation is all-or-nothing under transparency lock.
    // Note: This operation is a selection structural op; it must NOT be clipped by ToolWriteAllowed().
    for (int cy = 0; cy < m_rows; ++cy)
    {
        for (int cx = col; cx < m_columns; ++cx)
        {
            int dlr = 0, dlc = 0;
            if (!CanvasToLayerLocalForReadFast(cy, cx, off_x, off_y, m_columns, m_rows, dlr, dlc))
                continue;

            const size_t didx = (size_t)dlr * (size_t)m_columns + (size_t)dlc;
            const GlyphId       old_cp = (didx < layer.cells.size()) ? layer.cells[didx] : BlankGlyph();
            const ColourIndex16 old_fg = (didx < layer.fg.size()) ? layer.fg[didx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (didx < layer.bg.size()) ? layer.bg[didx] : kUnsetIndex16;
            const Attrs         old_attrs = (didx < layer.attrs.size()) ? layer.attrs[didx] : 0;

            GlyphId       new_cp = BlankGlyph();
            ColourIndex16 new_fg = kUnsetIndex16;
            ColourIndex16 new_bg = kUnsetIndex16;
            Attrs         new_attrs = 0;

            if (cx > col)
            {
                const int sx = cx - 1;
                int slr = 0, slc = 0;
                if (CanvasToLayerLocalForReadFast(cy, sx, off_x, off_y, m_columns, m_rows, slr, slc))
                {
                    const size_t sidx = (size_t)slr * (size_t)m_columns + (size_t)slc;
                    if (sidx < layer.cells.size()) new_cp = layer.cells[sidx];
                    if (sidx < layer.fg.size())    new_fg = layer.fg[sidx];
                    if (sidx < layer.bg.size())    new_bg = layer.bg[sidx];
                    if (sidx < layer.attrs.size()) new_attrs = layer.attrs[sidx];
                }
            }
            // else: cx == col -> inserted blank column

            if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                               old_cp, old_fg, old_bg, old_attrs,
                                               new_cp, new_fg, new_bg, new_attrs))
            {
                // Structural op: avoid partial shifts under transparency lock.
                return false;
            }

            PendingWrite w;
            w.lr = dlr;
            w.lc = dlc;
            w.old_cp = old_cp;
            w.old_fg = old_fg;
            w.old_bg = old_bg;
            w.old_attrs = old_attrs;
            w.new_cp = new_cp;
            w.new_fg = new_fg;
            w.new_bg = new_bg;
            w.new_attrs = new_attrs;
            writes.push_back(w);
        }
    }

    if (writes.empty())
        return false;

    PrepareUndoForMutation();
    EnsureUndoCaptureIsPatch();

    for (const auto& w : writes)
        CaptureUndoPageIfNeeded(layer_index, w.lr);

    for (const auto& w : writes)
    {
        const size_t idx = (size_t)w.lr * (size_t)m_columns + (size_t)w.lc;
        if (idx < layer.cells.size()) layer.cells[idx] = w.new_cp;
        if (idx < layer.fg.size())    layer.fg[idx]    = w.new_fg;
        if (idx < layer.bg.size())    layer.bg[idx]    = w.new_bg;
        if (idx < layer.attrs.size()) layer.attrs[idx] = w.new_attrs;
    }

    return true;
}

bool AnsiCanvas::SetLayerCell(int layer_index, int row, int col, char32_t cp)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    auto write_one = [&](int write_col) -> bool
    {
        if (!ToolWriteAllowed(row, write_col))
            return true; // clipped -> treat as no-op success
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(layer_index, row, write_col, lr, lc))
            return false;

        Layer& layer = m_layers[(size_t)layer_index];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const GlyphId old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
        const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const GlyphId new_cp = phos::glyph::MakeUnicodeScalar(cp);
        const ColourIndex16 new_fg = old_fg;
        const ColourIndex16 new_bg = old_bg;
        const Attrs    new_attrs = old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return false;
        if (in_bounds && old_cp == new_cp)
            return true;

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(layer_index, lr);
        if (lr >= m_rows)
            EnsureRows(lr + 1);
        const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (widx < layer.cells.size())
            layer.cells[widx] = new_cp;
        if (widx < layer.attrs.size())
            layer.attrs[widx] = new_attrs;
        return true;
    };

    const bool ok_primary = write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            (void)write_one(mirror_col);
    }

    return ok_primary;
}

bool AnsiCanvas::SetLayerCellIndices(int layer_index, int row, int col, char32_t cp, ColourIndex16 fg, ColourIndex16 bg)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    auto write_one = [&](int write_col) -> bool
    {
        if (!ToolWriteAllowed(row, write_col))
            return true; // clipped -> treat as no-op success
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(layer_index, row, write_col, lr, lc))
            return false;

        Layer& layer = m_layers[(size_t)layer_index];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const GlyphId old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
        const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const GlyphId new_cp = phos::glyph::MakeUnicodeScalar(cp);
        const ColourIndex16 new_fg = fg;
        const ColourIndex16 new_bg = bg;
        const Attrs    new_attrs = old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return false;
        if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
            return true;

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(layer_index, lr);
        if (lr >= m_rows)
            EnsureRows(lr + 1);
        const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (widx < layer.cells.size())
            layer.cells[widx] = new_cp;
        if (widx < layer.fg.size())
            layer.fg[widx] = new_fg;
        if (widx < layer.bg.size())
            layer.bg[widx] = new_bg;
        if (widx < layer.attrs.size())
            layer.attrs[widx] = new_attrs;
        return true;
    };

    const bool ok_primary = write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            (void)write_one(mirror_col);
    }

    return ok_primary;
}

bool AnsiCanvas::SetLayerCellIndices(int layer_index, int row, int col, char32_t cp, ColourIndex16 fg, ColourIndex16 bg, Attrs attrs)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    auto write_one = [&](int write_col) -> bool
    {
        if (!ToolWriteAllowed(row, write_col))
            return true; // clipped -> treat as no-op success
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(layer_index, row, write_col, lr, lc))
            return false;

        Layer& layer = m_layers[(size_t)layer_index];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const GlyphId old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
        const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const GlyphId new_cp = phos::glyph::MakeUnicodeScalar(cp);
        const ColourIndex16 new_fg = fg;
        const ColourIndex16 new_bg = bg;
        const Attrs    new_attrs = attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return false;
        if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
            return true;

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(layer_index, lr);
        if (lr >= m_rows)
            EnsureRows(lr + 1);
        const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (widx < layer.cells.size())
            layer.cells[widx] = new_cp;
        if (widx < layer.fg.size())
            layer.fg[widx] = new_fg;
        if (widx < layer.bg.size())
            layer.bg[widx] = new_bg;
        if (widx < layer.attrs.size())
            layer.attrs[widx] = new_attrs;
        return true;
    };

    const bool ok_primary = write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            (void)write_one(mirror_col);
    }

    return ok_primary;
}

bool AnsiCanvas::SetLayerCellIndicesPartial(int layer_index,
                                           int row,
                                           int col,
                                           char32_t cp,
                                           std::optional<ColourIndex16> fg,
                                           std::optional<ColourIndex16> bg,
                                           std::optional<Attrs> attrs)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    auto write_one = [&](int write_col) -> bool
    {
        if (!ToolWriteAllowed(row, write_col))
            return true; // clipped -> treat as no-op success
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(layer_index, row, write_col, lr, lc))
            return false;

        Layer& layer = m_layers[(size_t)layer_index];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const GlyphId old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
        const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const GlyphId new_cp = phos::glyph::MakeUnicodeScalar(cp);
        const ColourIndex16 new_fg = fg.has_value() ? *fg : old_fg;
        const ColourIndex16 new_bg = bg.has_value() ? *bg : old_bg;
        const Attrs    new_attrs = attrs.has_value() ? *attrs : old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return false;
        if (in_bounds &&
            old_cp == new_cp &&
            old_fg == new_fg &&
            old_bg == new_bg &&
            old_attrs == new_attrs)
            return true;

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(layer_index, lr);
        if (lr >= m_rows)
            EnsureRows(lr + 1);
        const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (widx < layer.cells.size())
            layer.cells[widx] = new_cp;
        if (widx < layer.fg.size())
            layer.fg[widx] = new_fg;
        if (widx < layer.bg.size())
            layer.bg[widx] = new_bg;
        if (widx < layer.attrs.size())
            layer.attrs[widx] = new_attrs;
        return true;
    };

    const bool ok_primary = write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            (void)write_one(mirror_col);
    }

    return ok_primary;
}

bool AnsiCanvas::SetLayerGlyphIndicesPartial(int layer_index,
                                            int row,
                                            int col,
                                            GlyphId glyph,
                                            std::optional<ColourIndex16> fg,
                                            std::optional<ColourIndex16> bg,
                                            std::optional<Attrs> attrs)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    auto write_one = [&](int write_col) -> bool
    {
        if (!ToolWriteAllowed(row, write_col))
            return true; // clipped -> treat as no-op success
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(layer_index, row, write_col, lr, lc))
            return false;

        Layer& layer = m_layers[(size_t)layer_index];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const GlyphId old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
        const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const GlyphId new_cp = glyph;
        const ColourIndex16 new_fg = fg.has_value() ? *fg : old_fg;
        const ColourIndex16 new_bg = bg.has_value() ? *bg : old_bg;
        const Attrs    new_attrs = attrs.has_value() ? *attrs : old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return false;
        if (in_bounds &&
            old_cp == new_cp &&
            old_fg == new_fg &&
            old_bg == new_bg &&
            old_attrs == new_attrs)
            return true;

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(layer_index, lr);
        if (lr >= m_rows)
            EnsureRows(lr + 1);
        const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (widx < layer.cells.size())
            layer.cells[widx] = new_cp;
        if (widx < layer.fg.size())
            layer.fg[widx] = new_fg;
        if (widx < layer.bg.size())
            layer.bg[widx] = new_bg;
        if (widx < layer.attrs.size())
            layer.attrs[widx] = new_attrs;
        return true;
    };

    const bool ok_primary = write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            (void)write_one(mirror_col);
    }

    return ok_primary;
}

AnsiCanvas::GlyphId AnsiCanvas::GetLayerGlyph(int layer_index, int row, int col) const
{
    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return BlankGlyph();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return BlankGlyph();
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return BlankGlyph();

    int lr = 0, lc = 0;
    if (!CanvasToLayerLocalForRead(layer_index, row, col, lr, lc))
        return BlankGlyph();
    const Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
    if (idx >= layer.cells.size())
        return BlankGlyph();
    return layer.cells[idx];
}

char32_t AnsiCanvas::GetLayerCell(int layer_index, int row, int col) const
{
    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return U' ';
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return U' ';
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return U' ';

    int lr = 0, lc = 0;
    if (!CanvasToLayerLocalForRead(layer_index, row, col, lr, lc))
        return U' ';
    const Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
    if (idx >= layer.cells.size())
        return U' ';
    return GlyphIdToUnicodeRepresentative(layer.cells[idx]);
}

bool AnsiCanvas::GetLayerCellIndices(int layer_index, int row, int col, ColourIndex16& out_fg, ColourIndex16& out_bg) const
{
    out_fg = kUnsetIndex16;
    out_bg = kUnsetIndex16;

    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return false;
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return false;

    int lr = 0, lc = 0;
    if (!CanvasToLayerLocalForRead(layer_index, row, col, lr, lc))
        return false;
    const Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
    if (idx >= layer.fg.size() || idx >= layer.bg.size())
        return false;
    out_fg = layer.fg[idx];
    out_bg = layer.bg[idx];
    return true;
}

bool AnsiCanvas::GetLayerCellAttrs(int layer_index, int row, int col, Attrs& out_attrs) const
{
    out_attrs = 0;

    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return false;
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return false;

    int lr = 0, lc = 0;
    if (!CanvasToLayerLocalForRead(layer_index, row, col, lr, lc))
        return false;
    const Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
    if (idx >= layer.attrs.size())
        return false;
    out_attrs = layer.attrs[idx];
    return true;
}

void AnsiCanvas::ClearLayerCellStyleInternal(int layer_index, int row, int col)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;
    int lr = 0, lc = 0;
    if (!CanvasToLayerLocalForWrite(layer_index, row, col, lr, lc))
        return;
    Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;

    const bool in_bounds = (lr < m_rows);
    const GlyphId old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
    const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
    const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
    const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

    const char32_t new_cp = old_cp;
    const ColourIndex16 new_fg = kUnsetIndex16;
    const ColourIndex16 new_bg = kUnsetIndex16;
    const Attrs    new_attrs = 0;

    if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                      old_cp, old_fg, old_bg, old_attrs,
                                      new_cp, new_fg, new_bg, new_attrs))
        return;
    if (in_bounds && old_fg == 0 && old_bg == 0 && old_attrs == 0)
        return;

    EnsureRows(lr + 1);
    if (idx < layer.fg.size())
        layer.fg[idx] = kUnsetIndex16;
    if (idx < layer.bg.size())
        layer.bg[idx] = kUnsetIndex16;
    if (idx < layer.attrs.size())
        layer.attrs[idx] = new_attrs;
}

bool AnsiCanvas::ClearLayerCellStyle(int layer_index, int row, int col)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    // Avoid capturing an undo snapshot if the mutation is blocked by alpha-lock or is a no-op.
    Layer& layer = m_layers[(size_t)layer_index];
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    auto write_one = [&](int write_col) -> bool
    {
        if (!ToolWriteAllowed(row, write_col))
            return true; // clipped -> treat as no-op success
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(layer_index, row, write_col, lr, lc))
            return false;
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const GlyphId old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
        const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const char32_t new_cp = old_cp;
        const ColourIndex16 new_fg = kUnsetIndex16;
        const ColourIndex16 new_bg = kUnsetIndex16;
        const Attrs    new_attrs = 0;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return false;
        if (in_bounds && old_fg == kUnsetIndex16 && old_bg == kUnsetIndex16 && old_attrs == 0)
            return true;

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(layer_index, lr);
        ClearLayerCellStyleInternal(layer_index, row, write_col);
        return true;
    };

    const bool ok_primary = write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            (void)write_one(mirror_col);
    }

    return ok_primary;
}

bool AnsiCanvas::ClearLayer(int layer_index, char32_t cp)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    Layer& layer = m_layers[(size_t)layer_index];
    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoForMutation();
            EnsureUndoCaptureIsPatch();
            prepared = true;
        }
    };

    const bool clip_to_selection = m_tool_running && HasSelection() && !m_move.active;
    if (clip_to_selection)
    {
        const Rect r = GetSelectionRect();
        if (r.w <= 0 || r.h <= 0 || m_columns <= 0 || m_rows <= 0)
            return false;
        const int x0 = std::clamp(r.x, 0, m_columns - 1);
        const int x1 = std::clamp(r.x + r.w - 1, 0, m_columns - 1);
        const int y0 = std::max(0, r.y);
        const int y1 = std::min(m_rows - 1, r.y + r.h - 1);
        if (y0 > y1)
            return false;

        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const size_t idx = (size_t)y * (size_t)m_columns + (size_t)x;
                if (idx >= layer.cells.size())
                    continue;

                const GlyphId old_cp = layer.cells[idx];
                const ColourIndex16 old_fg = (idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
                const ColourIndex16 old_bg = (idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
                const Attrs    old_attrs = (idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

                const GlyphId new_cp = phos::glyph::MakeUnicodeScalar(cp);
                const ColourIndex16 new_fg = kUnsetIndex16;
                const ColourIndex16 new_bg = kUnsetIndex16;
                const Attrs    new_attrs = 0;

                if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                                  old_cp, old_fg, old_bg, old_attrs,
                                                  new_cp, new_fg, new_bg, new_attrs))
                    continue;
                if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                    continue;

                prepare();
                CaptureUndoPageIfNeeded(layer_index, y);
                if (idx < layer.cells.size()) layer.cells[idx] = new_cp;
                if (idx < layer.fg.size())    layer.fg[idx]    = new_fg;
                if (idx < layer.bg.size())    layer.bg[idx]    = new_bg;
                if (idx < layer.attrs.size()) layer.attrs[idx] = new_attrs;
                did_anything = true;
            }
        return did_anything;
    }

    const size_t n = layer.cells.size();
    for (size_t idx = 0; idx < n; ++idx)
    {
        const GlyphId old_cp = layer.cells[idx];
        const ColourIndex16 old_fg = (idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
        const Attrs    old_attrs = (idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const GlyphId new_cp = phos::glyph::MakeUnicodeScalar(cp);
        const ColourIndex16 new_fg = kUnsetIndex16;
        const ColourIndex16 new_bg = kUnsetIndex16;
        const Attrs    new_attrs = 0;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            continue;
        if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
            continue;

        prepare();
        if (m_columns > 0)
            CaptureUndoPageIfNeeded(layer_index, (int)(idx / (size_t)m_columns));
        if (idx < layer.cells.size()) layer.cells[idx] = new_cp;
        if (idx < layer.fg.size())    layer.fg[idx]    = new_fg;
        if (idx < layer.bg.size())    layer.bg[idx]    = new_bg;
        if (idx < layer.attrs.size()) layer.attrs[idx] = new_attrs;
        did_anything = true;
    }
    return did_anything;
}

bool AnsiCanvas::FillLayer(int layer_index,
                           std::optional<char32_t> cp,
                           std::optional<Colour32> fg,
                           std::optional<Colour32> bg)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    Layer& layer = m_layers[(size_t)layer_index];
    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoForMutation();
            EnsureUndoCaptureIsPatch();
            prepared = true;
        }
    };

    const bool clip_to_selection = m_tool_running && HasSelection() && !m_move.active;
    if (clip_to_selection)
    {
        const Rect r = GetSelectionRect();
        if (r.w <= 0 || r.h <= 0 || m_columns <= 0 || m_rows <= 0)
            return false;
        const int x0 = std::clamp(r.x, 0, m_columns - 1);
        const int x1 = std::clamp(r.x + r.w - 1, 0, m_columns - 1);
        const int y0 = std::max(0, r.y);
        const int y1 = std::min(m_rows - 1, r.y + r.h - 1);
        if (y0 > y1)
            return false;

        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const size_t idx = (size_t)y * (size_t)m_columns + (size_t)x;
                if (idx >= layer.cells.size())
                    continue;

                const GlyphId old_cp = layer.cells[idx];
                const ColourIndex16 old_fg = (idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
                const ColourIndex16 old_bg = (idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
                const Attrs    old_attrs = (idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

                const GlyphId new_cp = cp.has_value() ? phos::glyph::MakeUnicodeScalar(*cp) : old_cp;
                const ColourIndex16 new_fg = fg.has_value() ? QuantizeColour32ToIndex(*fg) : old_fg;
                const ColourIndex16 new_bg = bg.has_value() ? QuantizeColour32ToIndex(*bg) : old_bg;
                const Attrs    new_attrs = old_attrs;

                if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                                  old_cp, old_fg, old_bg, old_attrs,
                                                  new_cp, new_fg, new_bg, new_attrs))
                    continue;
                if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
                    continue;

                prepare();
                CaptureUndoPageIfNeeded(layer_index, y);
                if (idx < layer.cells.size()) layer.cells[idx] = new_cp;
                if (idx < layer.fg.size())    layer.fg[idx]    = new_fg;
                if (idx < layer.bg.size())    layer.bg[idx]    = new_bg;
                did_anything = true;
            }
        return did_anything;
    }

    const size_t n = layer.cells.size();
    for (size_t idx = 0; idx < n; ++idx)
    {
        const GlyphId old_cp = layer.cells[idx];
        const ColourIndex16 old_fg = (idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
        const ColourIndex16 old_bg = (idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
        const Attrs    old_attrs = (idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const GlyphId new_cp = cp.has_value() ? phos::glyph::MakeUnicodeScalar(*cp) : old_cp;
        const ColourIndex16 new_fg = fg.has_value() ? QuantizeColour32ToIndex(*fg) : old_fg;
        const ColourIndex16 new_bg = bg.has_value() ? QuantizeColour32ToIndex(*bg) : old_bg;
        const Attrs    new_attrs = old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            continue;
        if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
            continue;

        prepare();
        if (m_columns > 0)
            CaptureUndoPageIfNeeded(layer_index, (int)(idx / (size_t)m_columns));
        if (idx < layer.cells.size()) layer.cells[idx] = new_cp;
        if (idx < layer.fg.size())    layer.fg[idx]    = new_fg;
        if (idx < layer.bg.size())    layer.bg[idx]    = new_bg;
        did_anything = true;
    }
    return did_anything;
}

// ---- end inlined from canvas_layers.inc ----


