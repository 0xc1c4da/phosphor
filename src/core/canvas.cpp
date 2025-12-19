#include "core/canvas.h"

#include "core/fonts.h"
#include "core/key_bindings.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "io/formats/sauce.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <locale>
#include <limits>
#include <ctime>
#include <string_view>
#include <string>
#include <vector>

namespace
{
struct GlobalClipboard
{
    int w = 0;
    int h = 0;
    // Stored per-cell (same dimensions): glyph + fg + bg. 0 colors mean "unset".
    std::vector<char32_t>         cp;
    std::vector<AnsiCanvas::Color32> fg;
    std::vector<AnsiCanvas::Color32> bg;
};

static GlobalClipboard g_clipboard;
} // namespace

static inline std::uint16_t ClampU16FromInt(int v)
{
    if (v < 0) return 0;
    if (v > 65535) return 65535;
    return (std::uint16_t)v;
}

static void EnsureSauceDefaultsAndSyncGeometry(AnsiCanvas::ProjectState::SauceMeta& s,
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
static int EncodeUtf8(char32_t cp, char out[5])
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
static void DecodeUtf8(const std::string& bytes, std::vector<char32_t>& out_codepoints)
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

AnsiCanvas::AnsiCanvas(int columns)
    : m_columns(columns > 0 ? columns : 80)
{
    // New canvases should start with consistent SAUCE defaults (even before the user opens the editor).
    // Rows are always >= 1.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
}

fonts::FontId AnsiCanvas::GetFontId() const
{
    return fonts::FromSauceName(m_sauce.tinfos);
}

bool AnsiCanvas::SetFontId(fonts::FontId id)
{
    const std::string_view sname = fonts::ToSauceName(id);
    if (sname.empty())
        return false;

    if (m_sauce.tinfos == sname)
        return true;

    // Persist via SAUCE.
    m_sauce.present = true;
    m_sauce.tinfos.assign(sname.begin(), sname.end());
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);

    // Font changes affect rendering but are not part of undo/redo.
    TouchContent();
    return true;
}

void AnsiCanvas::SetZoom(float zoom)
{
    // Clamp to a sensible range so we don't generate zero-sized cells or huge buffers.
    const float min_zoom = 0.10f;
    const float max_zoom = 12.0f;
    if (zoom < min_zoom) zoom = min_zoom;
    if (zoom > max_zoom) zoom = max_zoom;
    m_zoom = zoom;
}

void AnsiCanvas::RequestScrollPixels(float scroll_x, float scroll_y)
{
    m_scroll_request_valid = true;
    m_scroll_request_x = scroll_x;
    m_scroll_request_y = scroll_y;
}

bool AnsiCanvas::GetCompositeCellPublic(int row, int col, char32_t& out_cp, Color32& out_fg, Color32& out_bg) const
{
    out_cp = U' ';
    out_fg = 0;
    out_bg = 0;
    if (row < 0 || col < 0 || col >= m_columns || row >= m_rows)
        return false;
    CompositeCell c = GetCompositeCell(row, col);
    out_cp = c.cp;
    out_fg = c.fg;
    out_bg = c.bg;
    return true;
}

// ---------------------------------------------------------------------------
// Undo / Redo
// ---------------------------------------------------------------------------

AnsiCanvas::Snapshot AnsiCanvas::MakeSnapshot() const
{
    Snapshot s;
    s.columns = m_columns;
    s.rows = m_rows;
    s.active_layer = m_active_layer;
    s.caret_row = m_caret_row;
    s.caret_col = m_caret_col;
    s.layers = m_layers;
    return s;
}

void AnsiCanvas::ApplySnapshot(const Snapshot& s)
{
    m_undo_applying_snapshot = true;

    m_columns = (s.columns > 0) ? s.columns : 80;
    if (m_columns > 4096) m_columns = 4096;
    m_rows    = (s.rows > 0) ? s.rows : 1;
    m_layers  = s.layers;
    m_active_layer = s.active_layer;
    m_caret_row = s.caret_row;
    m_caret_col = s.caret_col;

    // Transient interaction state; recomputed next frame.
    m_cursor_valid = false;
    m_mouse_capture = false;

    // Re-establish invariants.
    EnsureDocument();
    if (m_rows <= 0) m_rows = 1;
    EnsureRows(m_rows);
    if (m_caret_row < 0) m_caret_row = 0;
    if (m_caret_col < 0) m_caret_col = 0;
    if (m_caret_col >= m_columns) m_caret_col = m_columns - 1;

    m_undo_applying_snapshot = false;

    // Keep SAUCE geometry in sync with the restored document.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);

    // Snapshot application always changes visible content (Undo/Redo/load).
    TouchContent();
}

void AnsiCanvas::BeginUndoCapture()
{
    if (m_undo_applying_snapshot)
        return;
    m_undo_capture_active = true;
    m_undo_capture_modified = false;
    m_undo_capture_has_snapshot = false;
}

void AnsiCanvas::EndUndoCapture()
{
    if (!m_undo_capture_active)
        return;

    if (m_undo_capture_modified && m_undo_capture_has_snapshot)
    {
        m_undo_stack.push_back(std::move(m_undo_capture_snapshot));
        if (m_undo_stack.size() > m_undo_limit)
            m_undo_stack.erase(m_undo_stack.begin(),
                               m_undo_stack.begin() + (m_undo_stack.size() - m_undo_limit));
        m_redo_stack.clear();
    }

    m_undo_capture_active = false;
    m_undo_capture_modified = false;
    m_undo_capture_has_snapshot = false;
}

void AnsiCanvas::PrepareUndoSnapshot()
{
    if (m_undo_applying_snapshot)
        return;
    // Many callers mutate canvas content from outside AnsiCanvas::Render() (e.g. ANSL scripts).
    // Those mutations still need to bump the content revision so dependent UI caches (minimap
    // texture, previews) update immediately, even if we're not currently capturing an undo step.
    if (!m_undo_capture_active)
    {
        TouchContent();
        return;
    }

    if (!m_undo_capture_has_snapshot)
    {
        m_undo_capture_snapshot = MakeSnapshot();
        m_undo_capture_has_snapshot = true;
    }
    m_undo_capture_modified = true;

    // Content is changing within this capture scope.
    TouchContent();
}

bool AnsiCanvas::CanUndo() const
{
    return !m_undo_stack.empty();
}

bool AnsiCanvas::CanRedo() const
{
    return !m_redo_stack.empty();
}

bool AnsiCanvas::Undo()
{
    if (m_undo_stack.empty())
        return false;
    if (m_undo_applying_snapshot)
        return false;

    Snapshot current = MakeSnapshot();
    Snapshot prev = std::move(m_undo_stack.back());
    m_undo_stack.pop_back();
    m_redo_stack.push_back(std::move(current));
    ApplySnapshot(prev);
    return true;
}

bool AnsiCanvas::Redo()
{
    if (m_redo_stack.empty())
        return false;
    if (m_undo_applying_snapshot)
        return false;

    Snapshot current = MakeSnapshot();
    Snapshot next = std::move(m_redo_stack.back());
    m_redo_stack.pop_back();

    m_undo_stack.push_back(std::move(current));
    if (m_undo_stack.size() > m_undo_limit)
        m_undo_stack.erase(m_undo_stack.begin(),
                           m_undo_stack.begin() + (m_undo_stack.size() - m_undo_limit));

    ApplySnapshot(next);
    return true;
}

void AnsiCanvas::PushUndoSnapshot()
{
    if (m_undo_applying_snapshot)
        return;

    m_undo_stack.push_back(MakeSnapshot());
    if (m_undo_stack.size() > m_undo_limit)
        m_undo_stack.erase(m_undo_stack.begin(),
                           m_undo_stack.begin() + (m_undo_stack.size() - m_undo_limit));
    m_redo_stack.clear();
}

void AnsiCanvas::TakeTypedCodepoints(std::vector<char32_t>& out)
{
    out.clear();
    out.swap(m_typed_queue);
}

AnsiCanvas::KeyEvents AnsiCanvas::TakeKeyEvents()
{
    KeyEvents out = m_key_events;
    m_key_events = KeyEvents{};
    return out;
}

// ---------------------------------------------------------------------------
// Selection + clipboard
// ---------------------------------------------------------------------------

AnsiCanvas::Rect AnsiCanvas::GetSelectionRect() const
{
    Rect r;
    if (!HasSelection())
        return r;
    r.x = m_selection.x;
    r.y = m_selection.y;
    r.w = m_selection.w;
    r.h = m_selection.h;
    return r;
}

void AnsiCanvas::SetSelectionCorners(int x0, int y0, int x1, int y1)
{
    EnsureDocument();
    if (m_columns <= 0)
    {
        m_selection = SelectionState{};
        return;
    }

    x0 = std::clamp(x0, 0, m_columns - 1);
    x1 = std::clamp(x1, 0, m_columns - 1);
    if (y0 < 0) y0 = 0;
    if (y1 < 0) y1 = 0;

    const int minx = std::min(x0, x1);
    const int maxx = std::max(x0, x1);
    const int miny = std::min(y0, y1);
    const int maxy = std::max(y0, y1);

    const int w = (maxx - minx) + 1;
    const int h = (maxy - miny) + 1;
    if (w <= 0 || h <= 0)
    {
        m_selection = SelectionState{};
        return;
    }

    m_selection.active = true;
    m_selection.x = minx;
    m_selection.y = miny;
    m_selection.w = w;
    m_selection.h = h;
}

void AnsiCanvas::ClearSelection()
{
    m_selection = SelectionState{};
    if (m_move.active)
        m_move = MoveState{};
}

bool AnsiCanvas::SelectionContains(int x, int y) const
{
    if (!HasSelection())
        return false;
    if (x < m_selection.x || y < m_selection.y)
        return false;
    if (x >= m_selection.x + m_selection.w)
        return false;
    if (y >= m_selection.y + m_selection.h)
        return false;
    return true;
}

bool AnsiCanvas::ClipboardHas()
{
    if (g_clipboard.w <= 0 || g_clipboard.h <= 0)
        return false;
    const size_t n = (size_t)g_clipboard.w * (size_t)g_clipboard.h;
    return g_clipboard.cp.size() == n && g_clipboard.fg.size() == n && g_clipboard.bg.size() == n;
}

AnsiCanvas::Rect AnsiCanvas::ClipboardRect()
{
    Rect r;
    if (!ClipboardHas())
        return r;
    r.w = g_clipboard.w;
    r.h = g_clipboard.h;
    return r;
}

static int NormalizeLayerIndex(const AnsiCanvas& c, int layer_index)
{
    if (layer_index < 0)
        return c.GetActiveLayerIndex();
    return layer_index;
}

static inline bool IsTransparentCellValue(char32_t cp, AnsiCanvas::Color32 fg, AnsiCanvas::Color32 bg)
{
    // In this editor, a cell is considered "transparent" (no contribution) iff:
    // - glyph is space
    // - fg is unset (0)
    // - bg is unset (0)
    // Note: space with a non-zero bg is visually opaque (background fill).
    return (cp == U' ') && (fg == 0) && (bg == 0);
}

static inline bool TransparencyTransitionAllowed(bool lock_transparency,
                                                 char32_t old_cp, AnsiCanvas::Color32 old_fg, AnsiCanvas::Color32 old_bg,
                                                 char32_t new_cp, AnsiCanvas::Color32 new_fg, AnsiCanvas::Color32 new_bg)
{
    if (!lock_transparency)
        return true;
    const bool old_t = IsTransparentCellValue(old_cp, old_fg, old_bg);
    const bool new_t = IsTransparentCellValue(new_cp, new_fg, new_bg);
    return old_t == new_t;
}

bool AnsiCanvas::CopySelectionToClipboard(int layer_index)
{
    EnsureDocument();
    if (!HasSelection())
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    const int x0 = m_selection.x;
    const int y0 = m_selection.y;
    const int w = m_selection.w;
    const int h = m_selection.h;
    if (w <= 0 || h <= 0)
        return false;

    const size_t n = (size_t)w * (size_t)h;
    g_clipboard.w = w;
    g_clipboard.h = h;
    g_clipboard.cp.assign(n, U' ');
    g_clipboard.fg.assign(n, 0);
    g_clipboard.bg.assign(n, 0);

    const Layer& layer = m_layers[(size_t)layer_index];
    for (int j = 0; j < h; ++j)
    {
        for (int i = 0; i < w; ++i)
        {
            const int x = x0 + i;
            const int y = y0 + j;
            const size_t out = (size_t)j * (size_t)w + (size_t)i;

            if (x < 0 || x >= m_columns || y < 0 || y >= m_rows)
                continue;

            const size_t idx = CellIndex(y, x);
            if (idx < layer.cells.size())
                g_clipboard.cp[out] = layer.cells[idx];
            if (idx < layer.fg.size())
                g_clipboard.fg[out] = layer.fg[idx];
            if (idx < layer.bg.size())
                g_clipboard.bg[out] = layer.bg[idx];
        }
    }
    return true;
}

bool AnsiCanvas::CopySelectionToClipboardComposite()
{
    EnsureDocument();
    if (!HasSelection())
        return false;

    const int x0 = m_selection.x;
    const int y0 = m_selection.y;
    const int w = m_selection.w;
    const int h = m_selection.h;
    if (w <= 0 || h <= 0)
        return false;

    const size_t n = (size_t)w * (size_t)h;
    g_clipboard.w = w;
    g_clipboard.h = h;
    g_clipboard.cp.assign(n, U' ');
    g_clipboard.fg.assign(n, 0);
    g_clipboard.bg.assign(n, 0);

    for (int j = 0; j < h; ++j)
    {
        for (int i = 0; i < w; ++i)
        {
            const int x = x0 + i;
            const int y = y0 + j;
            const size_t out = (size_t)j * (size_t)w + (size_t)i;

            if (x < 0 || x >= m_columns || y < 0 || y >= m_rows)
                continue;

            const CompositeCell c = GetCompositeCell(y, x);
            g_clipboard.cp[out] = c.cp;
            g_clipboard.fg[out] = c.fg;
            g_clipboard.bg[out] = c.bg;
        }
    }
    return true;
}

bool AnsiCanvas::DeleteSelection(int layer_index)
{
    EnsureDocument();
    if (!HasSelection())
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    Layer& layer = m_layers[(size_t)layer_index];

    const int x0 = m_selection.x;
    const int y0 = m_selection.y;
    const int w = m_selection.w;
    const int h = m_selection.h;
    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoSnapshot();
            prepared = true;
        }
    };
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            const int x = x0 + i;
            const int y = y0 + j;
            if (x < 0 || x >= m_columns || y < 0)
                continue;
            const size_t idx = CellIndex(y, x);

            // If row is beyond current document, the old cell is implicitly transparent.
            const bool in_bounds = (y < m_rows);
            const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
            const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
            const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

            const char32_t new_cp = U' ';
            const Color32  new_fg = 0;
            const Color32  new_bg = 0;

            if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
                continue;

            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
                continue; // no-op

            prepare();
            if (y >= m_rows)
                EnsureRows(y + 1);

            if (idx < layer.cells.size())
                layer.cells[idx] = new_cp;
            if (idx < layer.fg.size())
                layer.fg[idx] = new_fg;
            if (idx < layer.bg.size())
                layer.bg[idx] = new_bg;
            did_anything = true;
        }
    return did_anything;
}

bool AnsiCanvas::CutSelectionToClipboard(int layer_index)
{
    if (!CopySelectionToClipboard(layer_index))
        return false;
    return DeleteSelection(layer_index);
}

bool AnsiCanvas::PasteClipboard(int x, int y, int layer_index, PasteMode mode, bool transparent_spaces)
{
    EnsureDocument();
    if (!ClipboardHas())
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    const int w = g_clipboard.w;
    const int h = g_clipboard.h;
    if (w <= 0 || h <= 0)
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoSnapshot();
            prepared = true;
        }
    };

    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            const int px = x + i;
            const int py = y + j;
            if (px < 0 || px >= m_columns || py < 0)
                continue;
            const size_t s = (size_t)j * (size_t)w + (size_t)i;
            if (s >= g_clipboard.cp.size())
                continue;

            const char32_t cp = g_clipboard.cp[s];
            if (transparent_spaces && cp == U' ')
                continue;

            const size_t dst = CellIndex(py, px);

            const bool in_bounds = (py < m_rows);
            const char32_t old_cp = (in_bounds && dst < layer.cells.size()) ? layer.cells[dst] : U' ';
            const Color32  old_fg = (in_bounds && dst < layer.fg.size())    ? layer.fg[dst]    : 0;
            const Color32  old_bg = (in_bounds && dst < layer.bg.size())    ? layer.bg[dst]    : 0;

            char32_t new_cp = old_cp;
            Color32  new_fg = old_fg;
            Color32  new_bg = old_bg;
            if (mode == PasteMode::Both || mode == PasteMode::CharOnly)
                new_cp = cp;
            if (mode == PasteMode::Both || mode == PasteMode::ColorOnly)
            {
                new_fg = g_clipboard.fg[s];
                new_bg = g_clipboard.bg[s];
            }

            if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
                continue;

            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
                continue;

            prepare();
            if (py >= m_rows)
                EnsureRows(py + 1);

            if (dst < layer.cells.size())
                layer.cells[dst] = new_cp;
            if (dst < layer.fg.size())
                layer.fg[dst] = new_fg;
            if (dst < layer.bg.size())
                layer.bg[dst] = new_bg;
            did_anything = true;
        }

    SetSelectionCorners(x, y, x + w - 1, y + h - 1);
    return did_anything;
}

bool AnsiCanvas::BeginMoveSelection(int grab_x, int grab_y, bool copy, int layer_index)
{
    EnsureDocument();
    if (!HasSelection())
        return false;
    if (!SelectionContains(grab_x, grab_y))
        return false;
    if (m_move.active)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    // Alpha-lock: disallow cutting (clearing source), but allow copying.
    // This matches typical "Lock Transparency" semantics (alpha cannot change).
    if (m_layers[(size_t)layer_index].lock_transparency)
        copy = true;

    const int x0 = m_selection.x;
    const int y0 = m_selection.y;
    const int w = m_selection.w;
    const int h = m_selection.h;
    if (w <= 0 || h <= 0)
        return false;

    MoveState mv;
    mv.active = true;
    mv.cut = !copy;
    mv.src_x = x0;
    mv.src_y = y0;
    mv.w = w;
    mv.h = h;
    mv.dst_x = x0;
    mv.dst_y = y0;
    mv.grab_dx = std::clamp(grab_x - x0, 0, std::max(0, w - 1));
    mv.grab_dy = std::clamp(grab_y - y0, 0, std::max(0, h - 1));
    mv.cells.assign((size_t)w * (size_t)h, ClipCell{});

    const Layer& layer = m_layers[(size_t)layer_index];
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            const int sx = x0 + i;
            const int sy = y0 + j;
            const size_t out = (size_t)j * (size_t)w + (size_t)i;
            if (sx < 0 || sx >= m_columns || sy < 0 || sy >= m_rows)
                continue;
            const size_t idx = CellIndex(sy, sx);
            if (idx < layer.cells.size())
                mv.cells[out].cp = layer.cells[idx];
            if (idx < layer.fg.size())
                mv.cells[out].fg = layer.fg[idx];
            if (idx < layer.bg.size())
                mv.cells[out].bg = layer.bg[idx];
        }

    if (mv.cut)
    {
        Layer& mut = m_layers[(size_t)layer_index];
        bool prepared = false;
        auto prepare = [&]()
        {
            if (!prepared)
            {
                PrepareUndoSnapshot();
                prepared = true;
            }
        };
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
            {
                const int sx = x0 + i;
                const int sy = y0 + j;
                if (sx < 0 || sx >= m_columns || sy < 0)
                    continue;
                const size_t idx = CellIndex(sy, sx);

                const bool in_bounds = (sy < m_rows);
                const char32_t old_cp = (in_bounds && idx < mut.cells.size()) ? mut.cells[idx] : U' ';
                const Color32  old_fg = (in_bounds && idx < mut.fg.size())    ? mut.fg[idx]    : 0;
                const Color32  old_bg = (in_bounds && idx < mut.bg.size())    ? mut.bg[idx]    : 0;
                const char32_t new_cp = U' ';
                const Color32  new_fg = 0;
                const Color32  new_bg = 0;

                if (!TransparencyTransitionAllowed(mut.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
                    continue;

                if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
                    continue;

                prepare();
                if (sy >= m_rows)
                    EnsureRows(sy + 1);

                if (idx < mut.cells.size())
                    mut.cells[idx] = new_cp;
                if (idx < mut.fg.size())
                    mut.fg[idx] = new_fg;
                if (idx < mut.bg.size())
                    mut.bg[idx] = new_bg;
            }
    }

    m_move = std::move(mv);
    return true;
}

void AnsiCanvas::UpdateMoveSelection(int cursor_x, int cursor_y)
{
    if (!m_move.active)
        return;
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;
    const int nx = cursor_x - m_move.grab_dx;
    const int ny = cursor_y - m_move.grab_dy;
    m_move.dst_x = std::clamp(nx, 0, std::max(0, m_columns - 1));
    m_move.dst_y = std::max(0, ny);
    SetSelectionCorners(m_move.dst_x, m_move.dst_y,
                        m_move.dst_x + m_move.w - 1,
                        m_move.dst_y + m_move.h - 1);
}

bool AnsiCanvas::CommitMoveSelection(int layer_index)
{
    EnsureDocument();
    if (!m_move.active)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    const int w = m_move.w;
    const int h = m_move.h;
    if (w <= 0 || h <= 0 || (int)m_move.cells.size() != w * h)
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoSnapshot();
            prepared = true;
        }
    };

    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            const int px = m_move.dst_x + i;
            const int py = m_move.dst_y + j;
            if (px < 0 || px >= m_columns || py < 0)
                continue;
            const size_t idx = CellIndex(py, px);
            const ClipCell& src = m_move.cells[(size_t)j * (size_t)w + (size_t)i];

            const bool in_bounds = (py < m_rows);
            const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
            const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
            const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

            const char32_t new_cp = src.cp;
            const Color32  new_fg = src.fg;
            const Color32  new_bg = src.bg;

            if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
                continue;

            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
                continue;

            prepare();
            if (py >= m_rows)
                EnsureRows(py + 1);

            if (idx < layer.cells.size())
                layer.cells[idx] = new_cp;
            if (idx < layer.fg.size())
                layer.fg[idx] = new_fg;
            if (idx < layer.bg.size())
                layer.bg[idx] = new_bg;
            did_anything = true;
        }

    SetSelectionCorners(m_move.dst_x, m_move.dst_y,
                        m_move.dst_x + w - 1,
                        m_move.dst_y + h - 1);
    m_move = MoveState{};
    return did_anything;
}

bool AnsiCanvas::CancelMoveSelection(int layer_index)
{
    EnsureDocument();
    if (!m_move.active)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (m_move.cut)
    {
        const int w = m_move.w;
        const int h = m_move.h;
        if (w > 0 && h > 0 && (int)m_move.cells.size() == w * h)
        {
            Layer& layer = m_layers[(size_t)layer_index];
            bool did_anything = false;
            bool prepared = false;
            auto prepare = [&]()
            {
                if (!prepared)
                {
                    PrepareUndoSnapshot();
                    prepared = true;
                }
            };
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i)
                {
                    const int px = m_move.src_x + i;
                    const int py = m_move.src_y + j;
                    if (px < 0 || px >= m_columns || py < 0)
                        continue;
                    const size_t idx = CellIndex(py, px);
                    const ClipCell& src = m_move.cells[(size_t)j * (size_t)w + (size_t)i];

                    const bool in_bounds = (py < m_rows);
                    const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
                    const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
                    const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

                    const char32_t new_cp = src.cp;
                    const Color32  new_fg = src.fg;
                    const Color32  new_bg = src.bg;

                    if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
                        continue;

                    if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
                        continue;

                    prepare();
                    if (py >= m_rows)
                        EnsureRows(py + 1);

                    if (idx < layer.cells.size())
                        layer.cells[idx] = new_cp;
                    if (idx < layer.fg.size())
                        layer.fg[idx] = new_fg;
                    if (idx < layer.bg.size())
                        layer.bg[idx] = new_bg;
                    did_anything = true;
                }
            (void)did_anything;
        }
    }

    SetSelectionCorners(m_move.src_x, m_move.src_y,
                        m_move.src_x + m_move.w - 1,
                        m_move.src_y + m_move.h - 1);
    m_move = MoveState{};
    return true;
}

void AnsiCanvas::SetCaretCell(int x, int y)
{
    EnsureDocument();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= m_columns) x = m_columns - 1;
    m_caret_col = x;
    m_caret_row = y;
    EnsureRows(m_caret_row + 1);
}

int AnsiCanvas::TextInputCallback(ImGuiInputTextCallbackData* data)
{
    if (!data || data->EventFlag != ImGuiInputTextFlags_CallbackCharFilter)
        return 0;

    AnsiCanvas* self = static_cast<AnsiCanvas*>(data->UserData);
    if (!self)
        return 0;

    const char32_t cp = static_cast<char32_t>(data->EventChar);
    // Queue typed codepoints so the active tool (ANSL) can implement editing behavior.
    self->m_typed_queue.push_back(cp);

    // We applied the character to the canvas; don't let InputText mutate its own buffer.
    return 1;
}

void AnsiCanvas::HandleCharInputWidget(const char* id)
{
    if (!id)
        return;

    // If the user is editing the status bar (Cols/Rows/Caret), don't run the hidden text input
    // widget at all. This prevents it from competing for ActiveId / keyboard focus.
    if (m_status_bar_editing)
        return;

    // SDL3 backend only emits text input events when ImGui indicates it wants text input.
    // The most robust way to do that is to keep a focused InputText widget.
    // We render it "invisible" and use a char-filter callback to apply typed characters
    // directly into the canvas cells.
    std::string input_id = std::string(id) + "##_text_input";

    // Tiny dummy buffer. All characters are filtered out by the callback, so it stays empty.
    static char dummy[2] = { 0, 0 };

    // Make the widget visually invisible but still interactive.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextItemWidth(1.0f);

    ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_CallbackCharFilter |
        ImGuiInputTextFlags_NoUndoRedo |
        ImGuiInputTextFlags_AlwaysOverwrite |
        ImGuiInputTextFlags_AllowTabInput |
        ImGuiInputTextFlags_EnterReturnsTrue;

    // Keep keyboard focus on this widget while the canvas is focused.
    //
    // SDL3 backend only emits text input events when ImGui indicates it wants text input,
    // so we need a focused InputText to receive characters.
    //
    // IMPORTANT: avoid stealing ActiveId from other windows.
    //
    // The tool palette (and other tool windows) render before canvases each frame.
    // If we call SetKeyboardFocusHere() later in the frame while the user is clicking
    // another window, this hidden InputText can steal ActiveId and make that click
    // appear to "not work" (often requiring a second click).
    //
    // Therefore we only refocus while:
    // - the canvas is logically focused
    // - the canvas window is focused *and hovered* (mouse is actually over it)
    // - no mouse interaction happened this frame
    // - no popup is open
    ImGuiIO& io = ImGui::GetIO();
    const bool any_mouse_down =
        io.MouseDown[ImGuiMouseButton_Left] ||
        io.MouseDown[ImGuiMouseButton_Right] ||
        io.MouseDown[ImGuiMouseButton_Middle];
    const bool any_mouse_click =
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    const bool any_mouse_release =
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
        ImGui::IsMouseReleased(ImGuiMouseButton_Right) ||
        ImGui::IsMouseReleased(ImGuiMouseButton_Middle);
    const bool any_mouse_interaction = any_mouse_down || any_mouse_click || any_mouse_release;

    // Avoid stealing focus from other UI elements (including our own status-line fields).
    // If another widget is active, don't force focus back to the hidden InputText.
    const ImGuiID hidden_id = ImGui::GetID(input_id.c_str());
    const ImGuiID active_id = ImGui::GetActiveID();
    const bool other_widget_active = (active_id != 0 && active_id != hidden_id);

    if (m_has_focus &&
        !other_widget_active &&
        !any_mouse_interaction &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        ImGui::SetKeyboardFocusHere();
    }

    ImGui::InputText(input_id.c_str(), dummy, IM_ARRAYSIZE(dummy), flags, &AnsiCanvas::TextInputCallback, this);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void AnsiCanvas::CaptureKeyEvents()
{
    m_key_events = KeyEvents{};
    if (!m_has_focus)
        return;
    // Bind keyboard navigation to *ImGui window focus* (not just our internal canvas focus).
    // Otherwise arrow keys pressed while interacting with other windows (e.g. character picker)
    // can still be consumed by the canvas because IsKeyPressed() is global.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        return;
    // If a popup/modal is open, don't interpret keys as canvas commands.
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        return;

    // Match previous behavior: discrete press events.
    //
    // If a key bindings engine is attached, resolve navigation/edit keys through action IDs
    // so tools/scripts can be remapped without editing Lua.
    if (m_keybinds)
    {
        kb::EvalContext kctx;
        kctx.global = true;
        kctx.editor = true;
        kctx.canvas = true;
        kctx.selection = HasSelection();
        kctx.platform = kb::RuntimePlatform();

        m_key_events.left  = m_keybinds->ActionPressed("nav.caret_left", kctx);
        m_key_events.right = m_keybinds->ActionPressed("nav.caret_right", kctx);
        m_key_events.up    = m_keybinds->ActionPressed("nav.caret_up", kctx);
        m_key_events.down  = m_keybinds->ActionPressed("nav.caret_down", kctx);
        m_key_events.home  = m_keybinds->ActionPressed("nav.home", kctx);
        m_key_events.end   = m_keybinds->ActionPressed("nav.end", kctx);

        m_key_events.backspace = m_keybinds->ActionPressed("editor.backspace", kctx);

        // "Delete" is ambiguous: when selection exists, treat it as "delete selection";
        // otherwise allow the (optional) forward-delete editor action.
        if (kctx.selection)
            m_key_events.del = m_keybinds->ActionPressed("selection.delete", kctx);
        else
            m_key_events.del = m_keybinds->ActionPressed("editor.delete_forward", kctx);

        m_key_events.enter = m_keybinds->ActionPressed("editor.new_line", kctx);
    }
    else
    {
        m_key_events.left      = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
        m_key_events.right     = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
        m_key_events.up        = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
        m_key_events.down      = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        m_key_events.home      = ImGui::IsKeyPressed(ImGuiKey_Home);
        m_key_events.end       = ImGui::IsKeyPressed(ImGuiKey_End);
        m_key_events.backspace = ImGui::IsKeyPressed(ImGuiKey_Backspace);
        m_key_events.del       = ImGui::IsKeyPressed(ImGuiKey_Delete);
        m_key_events.enter     = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    }

    // Selection/clipboard keys (used by tools; modifiers are checked separately via ImGuiIO in the host).
    m_key_events.c      = ImGui::IsKeyPressed(ImGuiKey_C, false);
    m_key_events.v      = ImGui::IsKeyPressed(ImGuiKey_V, false);
    m_key_events.x      = ImGui::IsKeyPressed(ImGuiKey_X, false);
    m_key_events.a      = ImGui::IsKeyPressed(ImGuiKey_A, false);
    m_key_events.escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
}

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
    PrepareUndoSnapshot();
    m_layers[static_cast<size_t>(index)].name = name;
    return true;
}

int AnsiCanvas::AddLayer(const std::string& name)
{
    EnsureDocument();
    PrepareUndoSnapshot();

    Layer layer;
    layer.name = name.empty() ? ("Layer " + std::to_string((int)m_layers.size() + 1)) : name;
    layer.visible = true;
    const size_t count = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    layer.cells.assign(count, U' ');
    layer.fg.assign(count, 0);
    layer.bg.assign(count, 0);

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

    PrepareUndoSnapshot();
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

    PrepareUndoSnapshot();

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

void AnsiCanvas::SetColumns(int columns)
{
    if (columns <= 0)
        return;
    if (columns > 4096)
        columns = 4096;
    EnsureDocument();

    if (columns == m_columns)
        return;

    PrepareUndoSnapshot();
    const int old_cols = m_columns;
    const int old_rows = m_rows;
    m_columns = columns;

    for (Layer& layer : m_layers)
    {
        std::vector<char32_t> new_cells;
        std::vector<Color32>  new_fg;
        std::vector<Color32>  new_bg;

        new_cells.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), U' ');
        new_fg.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), 0);
        new_bg.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), 0);

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
            }
        }

        layer.cells = std::move(new_cells);
        layer.fg    = std::move(new_fg);
        layer.bg    = std::move(new_bg);
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

    PrepareUndoSnapshot();
    m_rows = rows;

    const size_t need = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    for (Layer& layer : m_layers)
    {
        layer.cells.resize(need, U' ');
        layer.fg.resize(need, 0);
        layer.bg.resize(need, 0);
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
    PrepareUndoSnapshot();

    // Reset document to a single empty row.
    m_rows = 1;
    for (Layer& layer : m_layers)
    {
        const size_t count = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
        layer.cells.assign(count, U' ');
        layer.fg.assign(count, 0);
        layer.bg.assign(count, 0);
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
    if (m_columns <= 0)
        m_columns = 80;
    if (m_rows <= 0)
        m_rows = 1;

    if (m_layers.empty())
    {
        Layer base;
        base.name = "Base";
        base.visible = true;
        const size_t count = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
        base.cells.assign(count, U' ');
        base.fg.assign(count, 0);
        base.bg.assign(count, 0);
        m_layers.push_back(std::move(base));
        m_active_layer = 0;
    }

    // Ensure every layer has the correct cell count.
    const size_t need = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    for (Layer& layer : m_layers)
    {
        if (layer.cells.size() != need)
            layer.cells.resize(need, U' ');
        if (layer.fg.size() != need)
            layer.fg.resize(need, 0);
        if (layer.bg.size() != need)
            layer.bg.resize(need, 0);
    }

    if (m_active_layer < 0)
        m_active_layer = 0;
    if (m_active_layer >= (int)m_layers.size())
        m_active_layer = (int)m_layers.size() - 1;

    // Ensure SAUCE defaults exist even for canvases created via bare constructor.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
}

void AnsiCanvas::EnsureRows(int rows_needed)
{
    if (rows_needed <= 0)
        rows_needed = 1;

    EnsureDocument();
    if (rows_needed <= m_rows)
        return;

    PrepareUndoSnapshot();
    m_rows = rows_needed;
    const size_t need = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    for (Layer& layer : m_layers)
    {
        layer.cells.resize(need, U' ');
        layer.fg.resize(need, 0);
        layer.bg.resize(need, 0);
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

AnsiCanvas::CompositeCell AnsiCanvas::GetCompositeCell(int row, int col) const
{
    CompositeCell out;
    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return out;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return out;

    const size_t idx = CellIndex(row, col);

    // Background: topmost visible non-zero background wins (space remains "transparent"
    // for glyph compositing, but background can be colored independently).
    for (int i = (int)m_layers.size() - 1; i >= 0; --i)
    {
        const Layer& layer = m_layers[(size_t)i];
        if (!layer.visible)
            continue;
        if (idx >= layer.bg.size())
            continue;
        const Color32 bg = layer.bg[idx];
        if (bg != 0)
        {
            out.bg = bg;
            break;
        }
    }

    // Glyph + foreground: topmost visible non-space glyph wins. Foreground color is
    // taken from the same layer if present; otherwise it falls back to theme default.
    for (int i = (int)m_layers.size() - 1; i >= 0; --i)
    {
        const Layer& layer = m_layers[(size_t)i];
        if (!layer.visible)
            continue;
        if (idx >= layer.cells.size())
            continue;
        const char32_t cp = layer.cells[idx];
        if (cp == U' ')
            continue;
        out.cp = cp;
        if (idx < layer.fg.size())
            out.fg = layer.fg[idx];
        break;
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

    Layer& layer = m_layers[(size_t)m_active_layer];
    // Determine old cell value without growing the document unless we will mutate.
    const bool in_bounds = (row < m_rows);
    const size_t idx = CellIndex(row, col);
    const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
    const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
    const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

    const char32_t new_cp = cp;
    const Color32  new_fg = old_fg;
    const Color32  new_bg = old_bg;

    if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
        return;
    if (in_bounds && old_cp == new_cp)
        return; // no-op

    PrepareUndoSnapshot();
    if (row >= m_rows)
        EnsureRows(row + 1);
    if (idx < layer.cells.size())
        layer.cells[idx] = cp;
}

void AnsiCanvas::SetActiveCell(int row, int col, char32_t cp, Color32 fg, Color32 bg)
{
    EnsureDocument();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    Layer& layer = m_layers[(size_t)m_active_layer];
    const bool in_bounds = (row < m_rows);
    const size_t idx = CellIndex(row, col);
    const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
    const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
    const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

    const char32_t new_cp = cp;
    const Color32  new_fg = fg;
    const Color32  new_bg = bg;

    if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
        return;
    if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
        return; // no-op

    PrepareUndoSnapshot();
    if (row >= m_rows)
        EnsureRows(row + 1);

    if (idx < layer.cells.size())
        layer.cells[idx] = new_cp;
    if (idx < layer.fg.size())
        layer.fg[idx] = new_fg;
    if (idx < layer.bg.size())
        layer.bg[idx] = new_bg;
}

void AnsiCanvas::ClearActiveCellStyle(int row, int col)
{
    EnsureDocument();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    Layer& layer = m_layers[(size_t)m_active_layer];
    const size_t idx = CellIndex(row, col);
    const bool in_bounds = (row < m_rows);
    const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
    const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
    const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

    const char32_t new_cp = old_cp;
    const Color32  new_fg = 0;
    const Color32  new_bg = 0;

    if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
        return;
    if (in_bounds && old_fg == 0 && old_bg == 0)
        return; // no-op

    PrepareUndoSnapshot();
    if (row >= m_rows)
        EnsureRows(row + 1);
    if (idx < layer.fg.size())
        layer.fg[idx] = 0;
    if (idx < layer.bg.size())
        layer.bg[idx] = 0;
}

bool AnsiCanvas::SetLayerCell(int layer_index, int row, int col, char32_t cp)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = CellIndex(row, col);

    const bool in_bounds = (row < m_rows);
    const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
    const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
    const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

    const char32_t new_cp = cp;
    const Color32  new_fg = old_fg;
    const Color32  new_bg = old_bg;

    if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
        return false;
    if (in_bounds && old_cp == new_cp)
        return true;

    PrepareUndoSnapshot();
    if (row >= m_rows)
        EnsureRows(row + 1);
    if (idx < layer.cells.size())
        layer.cells[idx] = new_cp;
    return true;
}

bool AnsiCanvas::SetLayerCell(int layer_index, int row, int col, char32_t cp, Color32 fg, Color32 bg)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = CellIndex(row, col);

    const bool in_bounds = (row < m_rows);
    const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
    const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
    const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

    const char32_t new_cp = cp;
    const Color32  new_fg = fg;
    const Color32  new_bg = bg;

    if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
        return false;
    if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
        return true;

    PrepareUndoSnapshot();
    if (row >= m_rows)
        EnsureRows(row + 1);
    if (idx < layer.cells.size())
        layer.cells[idx] = new_cp;
    if (idx < layer.fg.size())
        layer.fg[idx] = new_fg;
    if (idx < layer.bg.size())
        layer.bg[idx] = new_bg;
    return true;
}

char32_t AnsiCanvas::GetLayerCell(int layer_index, int row, int col) const
{
    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return U' ';
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return U' ';
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return U' ';

    const Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = CellIndex(row, col);
    if (idx >= layer.cells.size())
        return U' ';
    return layer.cells[idx];
}

bool AnsiCanvas::GetLayerCellColors(int layer_index, int row, int col, Color32& out_fg, Color32& out_bg) const
{
    out_fg = 0;
    out_bg = 0;

    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return false;
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return false;

    const Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = CellIndex(row, col);
    if (idx >= layer.fg.size() || idx >= layer.bg.size())
        return false;
    out_fg = layer.fg[idx];
    out_bg = layer.bg[idx];
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
    Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = CellIndex(row, col);

    const bool in_bounds = (row < m_rows);
    const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
    const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
    const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

    const char32_t new_cp = old_cp;
    const Color32  new_fg = 0;
    const Color32  new_bg = 0;

    if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
        return;
    if (in_bounds && old_fg == 0 && old_bg == 0)
        return;

    EnsureRows(row + 1);
    if (idx < layer.fg.size())
        layer.fg[idx] = 0;
    if (idx < layer.bg.size())
        layer.bg[idx] = 0;
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

    const bool in_bounds = (row < m_rows);
    const size_t idx = CellIndex(row, col);
    const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
    const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
    const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

    const char32_t new_cp = old_cp;
    const Color32  new_fg = 0;
    const Color32  new_bg = 0;

    if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
        return false;
    if (in_bounds && old_fg == 0 && old_bg == 0)
        return true;

    PrepareUndoSnapshot();
    ClearLayerCellStyleInternal(layer_index, row, col);
    return true;
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
            PrepareUndoSnapshot();
            prepared = true;
        }
    };

    const size_t n = layer.cells.size();
    for (size_t idx = 0; idx < n; ++idx)
    {
        const char32_t old_cp = layer.cells[idx];
        const Color32  old_fg = (idx < layer.fg.size()) ? layer.fg[idx] : 0;
        const Color32  old_bg = (idx < layer.bg.size()) ? layer.bg[idx] : 0;

        const char32_t new_cp = cp;
        const Color32  new_fg = 0;
        const Color32  new_bg = 0;

        if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
            continue;
        if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
            continue;

        prepare();
        if (idx < layer.cells.size()) layer.cells[idx] = new_cp;
        if (idx < layer.fg.size())    layer.fg[idx]    = new_fg;
        if (idx < layer.bg.size())    layer.bg[idx]    = new_bg;
        did_anything = true;
    }
    return did_anything;
}

bool AnsiCanvas::FillLayer(int layer_index,
                           std::optional<char32_t> cp,
                           std::optional<Color32> fg,
                           std::optional<Color32> bg)
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
            PrepareUndoSnapshot();
            prepared = true;
        }
    };

    const size_t n = layer.cells.size();
    for (size_t idx = 0; idx < n; ++idx)
    {
        const char32_t old_cp = layer.cells[idx];
        const Color32  old_fg = (idx < layer.fg.size()) ? layer.fg[idx] : 0;
        const Color32  old_bg = (idx < layer.bg.size()) ? layer.bg[idx] : 0;

        const char32_t new_cp = cp.has_value() ? *cp : old_cp;
        const Color32  new_fg = fg.has_value() ? *fg : old_fg;
        const Color32  new_bg = bg.has_value() ? *bg : old_bg;

        if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
            continue;
        if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
            continue;

        prepare();
        if (idx < layer.cells.size()) layer.cells[idx] = new_cp;
        if (idx < layer.fg.size())    layer.fg[idx]    = new_fg;
        if (idx < layer.bg.size())    layer.bg[idx]    = new_bg;
        did_anything = true;
    }
    return did_anything;
}

void AnsiCanvas::HandleMouseInteraction(const ImVec2& origin, float cell_w, float cell_h)
{
    EnsureDocument();

    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive(); // stays true during click+drag if the item captured the mouse button

    const bool left_down  = io.MouseDown[ImGuiMouseButton_Left];
    const bool right_down = io.MouseDown[ImGuiMouseButton_Right];
    const bool any_down   = left_down || right_down;
    const bool any_clicked =
        (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)));

    // Capture mouse for tool interactions (pencil/brush) so click+drag continues to update
    // even if ImGui ActiveId is owned by another widget (e.g. our hidden InputText).
    if (any_clicked)
        m_mouse_capture = true;
    if (!any_down)
        m_mouse_capture = false;

    const bool tracking = hovered || active || m_mouse_capture;
    if (!tracking)
    {
        m_cursor_valid = false;
        return;
    }

    // Update pointer state (hover cell + pressed state) every frame.
    {
        ImVec2 local(io.MousePos.x - origin.x, io.MousePos.y - origin.y);

        // Convert to cell coords; allow dragging outside the item rect by clamping.
        int col = static_cast<int>(std::floor(local.x / cell_w));
        int row = static_cast<int>(std::floor(local.y / cell_h));

        if (col < 0) col = 0;
        if (col >= m_columns) col = m_columns - 1;
        if (row < 0) row = 0;

        // Don't let hover accidentally grow the document; only allow row growth when interacting.
        // (This keeps keyboard editing stable even if the mouse is moving around.)
        if (!any_down && !any_clicked)
        {
            if (row >= m_rows) row = m_rows - 1;
            if (row < 0) row = 0;
        }
        else
        {
            EnsureRows(row + 1);
        }

        // Previous pointer state (for drag detection).
        m_cursor_pcol = m_cursor_col;
        m_cursor_prow = m_cursor_row;
        m_cursor_prev_left_down  = m_cursor_left_down;
        m_cursor_prev_right_down = m_cursor_right_down;

        // Current pointer state.
        m_cursor_col = col;
        m_cursor_row = row;
        m_cursor_left_down  = left_down;
        m_cursor_right_down = right_down;
        m_cursor_valid = true;

        // IMPORTANT: tools/scripts decide how mouse input affects the caret.
    }
}

bool AnsiCanvas::GetCursorCell(int& out_x,
                               int& out_y,
                               bool& out_left_down,
                               bool& out_right_down,
                               int& out_px,
                               int& out_py,
                               bool& out_prev_left_down,
                               bool& out_prev_right_down) const
{
    if (!m_cursor_valid)
        return false;
    out_x = m_cursor_col;
    out_y = m_cursor_row;
    out_left_down = m_cursor_left_down;
    out_right_down = m_cursor_right_down;
    out_px = m_cursor_pcol;
    out_py = m_cursor_prow;
    out_prev_left_down = m_cursor_prev_left_down;
    out_prev_right_down = m_cursor_prev_right_down;
    return true;
}

void AnsiCanvas::DrawVisibleCells(ImDrawList* draw_list,
                                  const ImVec2& origin,
                                  float cell_w,
                                  float cell_h,
                                  float font_size)
{
    if (!draw_list)
        return;

    ImFont* font = ImGui::GetFont();
    if (!font)
        return;

    EnsureDocument();

    const int rows = m_rows;
    if (rows <= 0 || m_columns <= 0)
        return;

    const fonts::FontInfo& finfo = fonts::Get(GetFontId());
    const EmbeddedBitmapFont* ef = GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    const bool bitmap_font = embedded_font || (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0);

    // Compute visible cell range based on ImGui's actual clipping rectangle.
    // Using GetWindowContentRegionMin/Max is tempting but becomes subtly wrong under
    // child scrolling + scrollbars; InnerClipRect is what the renderer really clips to.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window)
        return;
    const ImRect clip_rect = window->InnerClipRect;
    const ImVec2 clip_min(clip_rect.Min.x, clip_rect.Min.y);
    const ImVec2 clip_max(clip_rect.Max.x, clip_rect.Max.y);

    int start_row = static_cast<int>(std::floor((clip_min.y - origin.y) / cell_h));
    int end_row   = static_cast<int>(std::ceil ((clip_max.y - origin.y) / cell_h));
    int start_col = static_cast<int>(std::floor((clip_min.x - origin.x) / cell_w));
    int end_col   = static_cast<int>(std::ceil ((clip_max.x - origin.x) / cell_w));

    if (start_row < 0) start_row = 0;
    if (start_col < 0) start_col = 0;
    if (end_row > rows) end_row = rows;
    if (end_col > m_columns) end_col = m_columns;

    for (int row = start_row; row < end_row; ++row)
    {
        for (int col = start_col; col < end_col; ++col)
        {
            ImVec2 cell_min(origin.x + col * cell_w,
                            origin.y + row * cell_h);
            ImVec2 cell_max(cell_min.x + cell_w,
                            cell_min.y + cell_h);

            CompositeCell cell = GetCompositeCell(row, col);

            // Background fill (if set).
            if (cell.bg != 0)
            {
                draw_list->AddRectFilled(cell_min, cell_max, ApplyCurrentStyleAlpha((ImU32)cell.bg));
            }

            // Caret highlight.
            if (row == m_caret_row && col == m_caret_col)
            {
                ImU32 cursor_col = ImGui::GetColorU32(ImVec4(0.30f, 0.30f, 0.60f, 0.75f));
                draw_list->AddRectFilled(cell_min, cell_max, cursor_col);
            }

            const char32_t cp = cell.cp;
            if (cp == U' ')
                continue; // spaces are only meaningful if they have a bg (drawn above)

            // Canvas background is a fixed black/white fill (not theme-driven), so the
            // "default" foreground must remain readable regardless of UI skin.
            const ImU32 default_fg = m_canvas_bg_white ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
            const ImU32 fg_col = (cell.fg != 0) ? (ImU32)cell.fg : default_fg;

            if (!bitmap_font)
            {
                char buf[5] = {0, 0, 0, 0, 0};
                EncodeUtf8(cp, buf);
                ImVec2 text_pos(cell_min.x, cell_min.y);
                draw_list->AddText(font, font_size, text_pos,
                                   ApplyCurrentStyleAlpha(fg_col),
                                   buf, nullptr);
            }
            else
            {
                // Bitmap path:
                // - If an embedded font is present, interpret U+E000.. as glyph indices.
                // - Otherwise map Unicode -> CP437 glyph index (0..255) in the selected bitmap font.
                int glyph_cell_w = finfo.cell_w;
                int glyph_cell_h = finfo.cell_h;
                bool vga_dup = finfo.vga_9col_dup;

                std::uint16_t glyph_index = 0;
                if (embedded_font)
                {
                    glyph_cell_w = ef->cell_w;
                    glyph_cell_h = ef->cell_h;
                    vga_dup = ef->vga_9col_dup;

                    if (cp >= kEmbeddedGlyphBase && cp < (kEmbeddedGlyphBase + (char32_t)ef->glyph_count))
                    {
                        glyph_index = (std::uint16_t)(cp - kEmbeddedGlyphBase);
                    }
                    else
                    {
                        // Best-effort: if the embedded font is CP437-ordered, map Unicode to CP437.
                        std::uint8_t b = 0;
                        if (fonts::UnicodeToCp437Byte(cp, b))
                            glyph_index = (std::uint16_t)b;
                        else
                            glyph_index = (std::uint16_t)'?';
                    }
                }
                else
                {
                    std::uint8_t glyph = 0;
                    if (!fonts::UnicodeToCp437Byte(cp, glyph))
                    {
                        // Fallbacks: prefer '?' if representable, otherwise space.
                        std::uint8_t q = 0;
                        glyph = (fonts::UnicodeToCp437Byte(U'?', q)) ? q : (std::uint8_t)' ';
                    }
                    glyph_index = (std::uint16_t)glyph;
                }

                auto glyph_row_bits = [&](std::uint16_t gi, int yy) -> std::uint8_t
                {
                    if (embedded_font)
                    {
                        if (gi >= (std::uint16_t)ef->glyph_count) return 0;
                        if (yy < 0 || yy >= ef->cell_h) return 0;
                        return ef->bitmap[(size_t)gi * (size_t)ef->cell_h + (size_t)yy];
                    }
                    return fonts::BitmapGlyphRowBits(finfo.id, gi, yy);
                };

                const float px_w = cell_w / (float)std::max(1, glyph_cell_w);
                const float px_h = cell_h / (float)std::max(1, glyph_cell_h);
                const ImU32 col = ApplyCurrentStyleAlpha(fg_col);
                const std::uint8_t glyph8 = (std::uint8_t)(glyph_index & 0xFFu);

                for (int yy = 0; yy < glyph_cell_h; ++yy)
                {
                    const std::uint8_t bits = glyph_row_bits(glyph_index, yy);
                    int run_start = -1;
                    auto bit_set = [&](int x) -> bool
                    {
                        if (x < 0)
                            return false;
                        if (x < 8)
                            return (bits & (std::uint8_t)(0x80u >> x)) != 0;
                        if (x == 8 && vga_dup && glyph_cell_w == 9 && glyph8 >= 192 && glyph8 <= 223)
                            return (bits & 0x01u) != 0; // x==7 is LSB when shifting 0x80>>7
                        return false;
                    };

                    for (int xx = 0; xx < glyph_cell_w; ++xx)
                    {
                        const bool on = bit_set(xx);
                        if (on && run_start < 0)
                            run_start = xx;
                        if ((!on || xx == glyph_cell_w - 1) && run_start >= 0)
                        {
                            const int run_end = on ? (xx + 1) : xx; // exclusive
                            const float x0 = cell_min.x + (float)run_start * px_w;
                            const float x1 = cell_min.x + (float)run_end * px_w;
                            const float y0 = cell_min.y + (float)yy * px_h;
                            const float y1 = cell_min.y + (float)(yy + 1) * px_h;
                            (void)y0;
                            (void)y1;
                            draw_list->AddRectFilled(ImVec2(x0, cell_min.y + (float)yy * px_h),
                                                     ImVec2(x1, cell_min.y + (float)(yy + 1) * px_h),
                                                     col);
                            run_start = -1;
                        }
                    }
                }
            }
        }
    }
}

void AnsiCanvas::DrawSelectionOverlay(ImDrawList* draw_list,
                                      const ImVec2& origin,
                                      float cell_w,
                                      float cell_h,
                                      float font_size)
{
    if (!draw_list)
        return;
    ImFont* font = ImGui::GetFont();
    if (!font)
        return;

    const fonts::FontInfo& finfo = fonts::Get(GetFontId());
    const EmbeddedBitmapFont* ef = GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    const bool bitmap_font = embedded_font || (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0);

    // Floating selection preview (drawn above the document).
    if (m_move.active && m_move.w > 0 && m_move.h > 0 && (int)m_move.cells.size() == m_move.w * m_move.h)
    {
        const int w = m_move.w;
        const int h = m_move.h;
        for (int j = 0; j < h; ++j)
        {
            for (int i = 0; i < w; ++i)
            {
                const int x = m_move.dst_x + i;
                const int y = m_move.dst_y + j;
                if (x < 0 || x >= m_columns || y < 0 || y >= m_rows)
                    continue;

                const ClipCell& c = m_move.cells[(size_t)j * (size_t)w + (size_t)i];
                ImVec2 cell_min(origin.x + x * cell_w,
                                origin.y + y * cell_h);
                ImVec2 cell_max(cell_min.x + cell_w,
                                cell_min.y + cell_h);
                if (c.bg != 0)
                    draw_list->AddRectFilled(cell_min, cell_max, ApplyCurrentStyleAlpha((ImU32)c.bg));
                if (c.cp != U' ')
                {
                    const ImU32 default_fg = m_canvas_bg_white ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
                    const ImU32 fg_col = (c.fg != 0) ? (ImU32)c.fg : default_fg;

                    if (!bitmap_font)
                    {
                        char buf[5] = {0, 0, 0, 0, 0};
                        EncodeUtf8(c.cp, buf);
                        draw_list->AddText(font, font_size, cell_min, ApplyCurrentStyleAlpha(fg_col), buf, nullptr);
                    }
                    else
                    {
                        int glyph_cell_w = finfo.cell_w;
                        int glyph_cell_h = finfo.cell_h;
                        bool vga_dup = finfo.vga_9col_dup;

                        std::uint16_t glyph_index = 0;
                        if (embedded_font)
                        {
                            glyph_cell_w = ef->cell_w;
                            glyph_cell_h = ef->cell_h;
                            vga_dup = ef->vga_9col_dup;
                            if (c.cp >= kEmbeddedGlyphBase && c.cp < (kEmbeddedGlyphBase + (char32_t)ef->glyph_count))
                                glyph_index = (std::uint16_t)(c.cp - kEmbeddedGlyphBase);
                            else
                            {
                                std::uint8_t b = 0;
                                if (fonts::UnicodeToCp437Byte(c.cp, b))
                                    glyph_index = (std::uint16_t)b;
                                else
                                    glyph_index = (std::uint16_t)'?';
                            }
                        }
                        else
                        {
                            std::uint8_t glyph = 0;
                            if (!fonts::UnicodeToCp437Byte(c.cp, glyph))
                            {
                                std::uint8_t q = 0;
                                glyph = (fonts::UnicodeToCp437Byte(U'?', q)) ? q : (std::uint8_t)' ';
                            }
                            glyph_index = (std::uint16_t)glyph;
                        }

                        auto glyph_row_bits = [&](std::uint16_t gi, int yy) -> std::uint8_t
                        {
                            if (embedded_font)
                            {
                                if (gi >= (std::uint16_t)ef->glyph_count) return 0;
                                if (yy < 0 || yy >= ef->cell_h) return 0;
                                return ef->bitmap[(size_t)gi * (size_t)ef->cell_h + (size_t)yy];
                            }
                            return fonts::BitmapGlyphRowBits(finfo.id, gi, yy);
                        };

                        const float px_w = cell_w / (float)std::max(1, glyph_cell_w);
                        const float px_h = cell_h / (float)std::max(1, glyph_cell_h);
                        const ImU32 col = ApplyCurrentStyleAlpha(fg_col);
                        const std::uint8_t glyph8 = (std::uint8_t)(glyph_index & 0xFFu);

                        for (int yy = 0; yy < glyph_cell_h; ++yy)
                        {
                            const std::uint8_t bits = glyph_row_bits(glyph_index, yy);
                            int run_start = -1;
                            auto bit_set = [&](int x) -> bool
                            {
                                if (x < 0)
                                    return false;
                                if (x < 8)
                                    return (bits & (std::uint8_t)(0x80u >> x)) != 0;
                                if (x == 8 && vga_dup && glyph_cell_w == 9 && glyph8 >= 192 && glyph8 <= 223)
                                    return (bits & 0x01u) != 0;
                                return false;
                            };

                            for (int xx = 0; xx < glyph_cell_w; ++xx)
                            {
                                const bool on = bit_set(xx);
                                if (on && run_start < 0)
                                    run_start = xx;
                                if ((!on || xx == glyph_cell_w - 1) && run_start >= 0)
                                {
                                    const int run_end = on ? (xx + 1) : xx; // exclusive
                                    const float x0 = cell_min.x + (float)run_start * px_w;
                                    const float x1 = cell_min.x + (float)run_end * px_w;
                                    draw_list->AddRectFilled(ImVec2(x0, cell_min.y + (float)yy * px_h),
                                                             ImVec2(x1, cell_min.y + (float)(yy + 1) * px_h),
                                                             col);
                                    run_start = -1;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Selection border (uses selection rect, which tracks floating selection during move).
    if (HasSelection())
    {
        const int x0 = m_selection.x;
        const int y0 = m_selection.y;
        const int x1 = x0 + m_selection.w;
        const int y1 = y0 + m_selection.h;

        ImVec2 p0(origin.x + x0 * cell_w, origin.y + y0 * cell_h);
        ImVec2 p1(origin.x + x1 * cell_w, origin.y + y1 * cell_h);
        p0.x = std::floor(p0.x) + 0.5f;
        p0.y = std::floor(p0.y) + 0.5f;
        p1.x = std::floor(p1.x) - 0.5f;
        p1.y = std::floor(p1.y) - 0.5f;

        const ImU32 col = ImGui::GetColorU32(ImVec4(0.15f, 0.75f, 1.0f, 0.90f));
        draw_list->AddRect(p0, p1, col, 0.0f, 0, 2.0f);
    }
}

void AnsiCanvas::Render(const char* id)
{
    Render(id, {});
}

void AnsiCanvas::Render(const char* id, const std::function<void(AnsiCanvas& canvas, int phase)>& tool_runner)
{
    if (!id)
        return;

    ImFont* font = ImGui::GetFont();
    if (!font)
        return;

    EnsureDocument();

    // Zoom stabilization:
    // Track whether zoom changed recently, and keep layout decisions stable for a few frames.
    // This prevents scrollbar toggling on rounding thresholds (InnerClipRect changes => flicker/jitter).
    const bool zoom_changed_since_last_frame = (m_last_view.valid && m_last_view.zoom != m_zoom);
    if (zoom_changed_since_last_frame)
        m_zoom_stabilize_frames = 6; // ~100ms at 60fps; enough to cover discrete trackpad steps
    else if (m_zoom_stabilize_frames > 0)
        --m_zoom_stabilize_frames;
    const bool zoom_stabilizing = (m_zoom_stabilize_frames > 0);

    // Base cell size:
    // - For Unscii (ImGui atlas): use the current ImGui font metrics.
    // - For bitmap fonts: use the selected font's textmode cell metrics, scaled by the
    //   current ImGui font size so HiDPI stays consistent with the rest of the UI.
    //
    // We intentionally *do not auto-fit to window width*; the user controls zoom explicitly.
    const float base_font_size = ImGui::GetFontSize();
    const fonts::FontInfo& finfo = fonts::Get(GetFontId());
    const EmbeddedBitmapFont* ef = GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    float cell_w = 0.0f;
    float cell_h = 0.0f;
    if (embedded_font)
    {
        const float base_scale = base_font_size / 16.0f;
        cell_w = (float)ef->cell_w * base_scale;
        cell_h = (float)ef->cell_h * base_scale;
    }
    else if (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0)
    {
        const float base_scale = base_font_size / 16.0f;
        cell_w = (float)finfo.cell_w * base_scale;
        cell_h = (float)finfo.cell_h * base_scale;
    }
    else
    {
        cell_w = font->CalcTextSizeA(base_font_size, FLT_MAX, 0.0f, "M", "M" + 1).x;
        cell_h = base_font_size;
    }

    // Quick status line (foundation for future toolbars).
    if (m_status_line_visible)
    {
        ImGui::PushID(id);
        bool status_editing = false;

        // With the canvas window rendered full-bleed (zero WindowPadding), add a tiny
        // amount of breathing room for the status line only.
        const ImGuiStyle& style_status = ImGui::GetStyle();
        const float status_pad_x = std::max(0.0f, style_status.FramePadding.x);
        const float status_pad_y = std::max(0.0f, style_status.FramePadding.y * 0.5f);
        if (status_pad_y > 0.0f)
            ImGui::Dummy(ImVec2(0.0f, status_pad_y));
        if (status_pad_x > 0.0f)
            ImGui::Indent(status_pad_x);

        const ImGuiInputTextFlags num_flags =
            ImGuiInputTextFlags_CharsDecimal |
            ImGuiInputTextFlags_AutoSelectAll;

        auto sync_buf = [&](const char* label, char* buf, size_t buf_sz, int value)
        {
            const ImGuiID wid = ImGui::GetID(label);
            if (ImGui::GetActiveID() == wid)
                return;
            std::snprintf(buf, buf_sz, "%d", value);
        };

        auto parse_int = [&](const char* buf, int& out) -> bool
        {
            if (!buf || !*buf)
                return false;
            char* end = nullptr;
            long v = std::strtol(buf, &end, 10);
            if (end == buf)
                return false;
            if (v < (long)std::numeric_limits<int>::min()) v = (long)std::numeric_limits<int>::min();
            if (v > (long)std::numeric_limits<int>::max()) v = (long)std::numeric_limits<int>::max();
            out = (int)v;
            return true;
        };

        const ImGuiStyle& style = ImGui::GetStyle();
        const float w_int =
            std::max(90.0f,
                     ImGui::CalcTextSize("000000").x + style.FramePadding.x * 2.0f);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Cols:");
        ImGui::SameLine();
        ImGui::PushItemWidth(w_int);
        sync_buf("##cols", m_status_cols_buf, sizeof(m_status_cols_buf), m_columns);
        ImGui::InputText("##cols", m_status_cols_buf, sizeof(m_status_cols_buf), num_flags);
        ImGui::PopItemWidth();
        status_editing = status_editing || ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            int v = m_columns;
            if (parse_int(m_status_cols_buf, v))
            {
                if (v < 1) v = 1;
                if (v != m_columns)
                    SetColumns(v);
                std::snprintf(m_status_cols_buf, sizeof(m_status_cols_buf), "%d", m_columns);
            }
            else
            {
                std::snprintf(m_status_cols_buf, sizeof(m_status_cols_buf), "%d", m_columns);
            }
        }

        ImGui::SameLine();
        ImGui::TextUnformatted("Rows:");
        ImGui::SameLine();
        ImGui::PushItemWidth(w_int);
        sync_buf("##rows", m_status_rows_buf, sizeof(m_status_rows_buf), m_rows);
        ImGui::InputText("##rows", m_status_rows_buf, sizeof(m_status_rows_buf), num_flags);
        ImGui::PopItemWidth();
        status_editing = status_editing || ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            int v = m_rows;
            if (parse_int(m_status_rows_buf, v))
            {
                if (v < 1) v = 1;
                if (v != m_rows)
                    SetRows(v);
                std::snprintf(m_status_rows_buf, sizeof(m_status_rows_buf), "%d", m_rows);
            }
            else
            {
                std::snprintf(m_status_rows_buf, sizeof(m_status_rows_buf), "%d", m_rows);
            }
        }

        ImGui::SameLine();
        ImGui::TextUnformatted("Caret:");
        ImGui::SameLine();
        ImGui::TextUnformatted("(");
        ImGui::SameLine();

        ImGui::PushItemWidth(w_int);
        sync_buf("##caret_x", m_status_caret_x_buf, sizeof(m_status_caret_x_buf), m_caret_col);
        ImGui::InputText("##caret_x", m_status_caret_x_buf, sizeof(m_status_caret_x_buf), num_flags);
        ImGui::PopItemWidth();
        status_editing = status_editing || ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            int x = m_caret_col;
            if (parse_int(m_status_caret_x_buf, x))
            {
                if (x < 0) x = 0;
                if (m_columns > 0 && x >= m_columns) x = m_columns - 1;
                SetCaretCell(x, m_caret_row);
                std::snprintf(m_status_caret_x_buf, sizeof(m_status_caret_x_buf), "%d", m_caret_col);
            }
            else
            {
                std::snprintf(m_status_caret_x_buf, sizeof(m_status_caret_x_buf), "%d", m_caret_col);
            }
        }

        ImGui::SameLine();
        ImGui::TextUnformatted(",");
        ImGui::SameLine();

        ImGui::PushItemWidth(w_int);
        sync_buf("##caret_y", m_status_caret_y_buf, sizeof(m_status_caret_y_buf), m_caret_row);
        ImGui::InputText("##caret_y", m_status_caret_y_buf, sizeof(m_status_caret_y_buf), num_flags);
        ImGui::PopItemWidth();
        status_editing = status_editing || ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            int y = m_caret_row;
            if (parse_int(m_status_caret_y_buf, y))
            {
                if (y < 0) y = 0;
                // Keep caret within current canvas rows; resize first if you want to move beyond.
                if (m_rows > 0 && y >= m_rows) y = m_rows - 1;
                SetCaretCell(m_caret_col, y);
                std::snprintf(m_status_caret_y_buf, sizeof(m_status_caret_y_buf), "%d", m_caret_row);
            }
            else
            {
                std::snprintf(m_status_caret_y_buf, sizeof(m_status_caret_y_buf), "%d", m_caret_row);
            }
        }

        ImGui::SameLine();
        ImGui::TextUnformatted(")");

        // Right-aligned "Edit SAUCE…" button (requested).
        {
            const char* btn_label = "Edit SAUCE...";
            const float btn_w = ImGui::CalcTextSize(btn_label).x + style.FramePadding.x * 2.0f;
            const float right_x = ImGui::GetWindowContentRegionMax().x; // window-local

            ImGui::SameLine();
            // Canvas font combo lives left of the background toggle + SAUCE button.
            const float combo_w = 240.0f;
            const float sq = ImGui::GetFrameHeight();
            const float total_w = combo_w + style.ItemSpacing.x + sq + style.ItemSpacing.x + btn_w;

            float x = right_x - total_w;
            // Avoid going backwards too aggressively; this is a best-effort alignment.
            if (x > ImGui::GetCursorPosX())
                ImGui::SetCursorPosX(x);

            {
                ImGui::SetNextItemWidth(combo_w);
                const fonts::FontId cur = GetFontId();
                const fonts::FontInfo& cur_info = fonts::Get(cur);
                const char* preview = (cur_info.label && *cur_info.label) ? cur_info.label : "(unknown)";
                if (ImGui::BeginCombo("##canvas_font_combo", preview))
                {
                    for (const auto& f : fonts::AllFonts())
                    {
                        const bool selected = (f.id == cur);
                        if (ImGui::Selectable(f.label ? f.label : "(unnamed)", selected))
                        {
                            (void)SetFontId(f.id);
                            status_editing = true; // prevent hidden input focus this frame
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::SameLine();
            // Canvas background toggle square (black/white) lives just left of the SAUCE button.
            const ImVec4 bg_col = m_canvas_bg_white ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1);
            ImGuiColorEditFlags cflags =
                ImGuiColorEditFlags_NoTooltip |
                ImGuiColorEditFlags_NoAlpha |
                ImGuiColorEditFlags_NoPicker |
                ImGuiColorEditFlags_NoDragDrop;
            if (ImGui::ColorButton("##canvas_bg", bg_col, cflags, ImVec2(sq, sq)))
            {
                ToggleCanvasBackgroundWhite();
                status_editing = true; // prevent the hidden input widget from stealing focus this frame
            }
            // Outline for visibility regardless of theme.
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p0 = ImGui::GetItemRectMin();
                const ImVec2 p1 = ImGui::GetItemRectMax();
                const ImU32 outline = m_canvas_bg_white ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
                dl->AddRect(p0, p1, ApplyCurrentStyleAlpha(outline));
            }

            ImGui::SameLine();
            if (ImGui::Button(btn_label))
            {
                m_request_open_sauce_editor = true;
                status_editing = true; // prevent the hidden input widget from stealing focus this frame
            }
        }

        // Tell the hidden canvas text-input widget to stand down while the user edits these fields.
        // Also drop canvas focus so tools don't react to keystrokes during numeric entry.
        m_status_bar_editing = status_editing;
        if (status_editing)
            m_has_focus = false;

        if (status_pad_x > 0.0f)
            ImGui::Unindent(status_pad_x);
        if (status_pad_y > 0.0f)
            ImGui::Dummy(ImVec2(0.0f, status_pad_y));

        ImGui::PopID();
    }

    // Hidden input widget to reliably receive UTF-8 text events from SDL3.
    //
    // IMPORTANT: this must NOT live inside the scrollable canvas child. If it does,
    // forcing keyboard focus to it (SetKeyboardFocusHere) will cause ImGui to scroll
    // the child to reveal the focused item, which feels like the canvas "jumps" to
    // the top when you click/paint while scrolled.
    //
    // Also IMPORTANT: do not let this widget alter layout or become visible (caret '|').
    // We render it off-screen and restore cursor pos so the canvas placement is unchanged.
    if (!m_status_bar_editing)
    {
        const ImVec2 saved = ImGui::GetCursorPos();
        const float line_h = ImGui::GetFrameHeightWithSpacing();
        ImGui::SetCursorPos(ImVec2(-10000.0f, saved.y - line_h));
        HandleCharInputWidget(id);
        ImGui::SetCursorPos(saved);
    }

    // Layer GUI lives in the LayerManager component (see layer_manager.*).

    // Scrollable region: fixed-width canvas, "infinite" rows (grown on demand).
    std::string child_id = std::string(id) + "##_scroll";
    ImGuiWindowFlags child_flags =
        ImGuiWindowFlags_HorizontalScrollbar |
        ImGuiWindowFlags_NoNavInputs |
        ImGuiWindowFlags_NoNavFocus;
    // During zoom changes, force scrollbars to remain present so the viewport (InnerClipRect)
    // dimensions stay stable. This avoids a common flicker source where the vertical scrollbar
    // toggles on/off across rounding thresholds.
    if (zoom_stabilizing)
    {
        child_flags |= ImGuiWindowFlags_AlwaysVerticalScrollbar;
        child_flags |= ImGuiWindowFlags_AlwaysHorizontalScrollbar;
    }
    // Canvas "paper" background is independent of the UI theme, so also override the
    // child window background (covers areas outside the grid, e.g. when the canvas is small).
    const ImVec4 canvas_bg = m_canvas_bg_white ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, canvas_bg);
    // No child border: it reads as a margin/frame around the canvas, especially on white.
    if (!ImGui::BeginChild(child_id.c_str(), ImVec2(0, 0), false, child_flags))
    {
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    const float base_cell_w    = cell_w;
    const float base_cell_h    = cell_h;

    // Ctrl+MouseWheel zoom on the canvas (like a typical editor).
    // We also adjust scroll so the point under the mouse stays stable.
    // NOTE: We apply the zoom immediately (so sizing updates this frame), but we defer the
    // scroll correction until after the canvas InvisibleButton is created, because the
    // correct "origin" for mouse anchoring is GetItemRectMin() (the actual canvas item rect),
    // not GetCursorScreenPos() (which can drift with child scrolling/scrollbars).
    bool  wheel_zoom_this_frame = false;
    float wheel_zoom_ratio = 1.0f; // ratio between snapped scales (new/old)
    float wheel_pre_scroll_x = 0.0f;
    float wheel_pre_scroll_y = 0.0f;
    ImVec2 wheel_mouse_pos(0.0f, 0.0f);
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && io.MouseWheel != 0.0f &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
        {
            auto snapped_scale_for_zoom = [&](float zoom) -> float
            {
                // Must match the snapping logic below (snap based on cell_w).
                float snapped_cell_w = std::floor(base_cell_w * zoom + 0.5f);
                if (snapped_cell_w < 1.0f)
                    snapped_cell_w = 1.0f;
                return (base_cell_w > 0.0f) ? (snapped_cell_w / base_cell_w) : 1.0f;
            };

            const float old_zoom = m_zoom;
            const float old_scale = snapped_scale_for_zoom(old_zoom);

            wheel_pre_scroll_x = ImGui::GetScrollX();
            wheel_pre_scroll_y = ImGui::GetScrollY();
            wheel_mouse_pos = io.MousePos;

            const float factor = (io.MouseWheel > 0.0f) ? 1.10f : (1.0f / 1.10f);
            SetZoom(old_zoom * factor);

            const float new_zoom = m_zoom;
            const float new_scale = snapped_scale_for_zoom(new_zoom);
            wheel_zoom_ratio = (old_scale > 0.0f) ? (new_scale / old_scale) : 1.0f;
            wheel_zoom_this_frame = true;
        }
    }

    // Explicit zoom (no auto-fit), but SNAP to the nearest pixel-aligned glyph cell.
    //
    // IMPORTANT: do NOT round width/height independently based on m_zoom.
    // That breaks the font's cell aspect ratio and can create visible seams between glyphs.
    // Instead:
    //  - snap cell_w to integer pixels
    //  - derive a single snapped_scale from that
    //  - compute font size and cell_h from the same snapped_scale
    float snapped_cell_w = std::floor(base_cell_w * m_zoom + 0.5f);
    if (snapped_cell_w < 1.0f)
        snapped_cell_w = 1.0f;
    const float snapped_scale = snapped_cell_w / base_cell_w;

    float scaled_font_size = std::max(1.0f, std::floor(base_font_size * snapped_scale + 0.5f));
    float scaled_cell_w    = snapped_cell_w;
    float scaled_cell_h    = std::floor(base_cell_h * snapped_scale + 0.5f);
    if (scaled_cell_h < 1.0f)
        scaled_cell_h = 1.0f;

    // Expose last aspect for tools/scripts.
    if (scaled_cell_h > 0.0f)
        m_last_cell_aspect = scaled_cell_w / scaled_cell_h;
    else
        m_last_cell_aspect = 1.0f;

    // Capture keyboard events and let the active tool handle them *before* we compute canvas_size,
    // so row growth (typing/enter/wrap) updates ImGui's scroll range immediately.
    BeginUndoCapture();
    CaptureKeyEvents();
    const int caret_start_row = m_caret_row;
    const int caret_start_col = m_caret_col;
    const bool had_typed_input = !m_typed_queue.empty();
    const bool had_key_input =
        m_key_events.left || m_key_events.right || m_key_events.up || m_key_events.down ||
        m_key_events.home || m_key_events.end || m_key_events.backspace || m_key_events.del ||
        m_key_events.enter;
    if (tool_runner)
        tool_runner(*this, 0); // keyboard phase

    // Keep document large enough for caret after tool run.
    EnsureRows(m_caret_row + 1);

    ImVec2 canvas_size(scaled_cell_w * static_cast<float>(m_columns),
                       scaled_cell_h * static_cast<float>(m_rows));

    // Apply any deferred scroll request now that we have a valid child window.
    // Note: clamp to scrollable bounds using InnerClipRect (what the renderer clips to).
    bool suppress_caret_autoscroll = false;
    if (m_scroll_request_valid)
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        const ImRect clip = w ? w->InnerClipRect : ImRect(0, 0, 0, 0);
        const float view_w = clip.GetWidth();
        const float view_h = clip.GetHeight();
        const float max_x = std::max(0.0f, canvas_size.x - view_w);
        const float max_y = std::max(0.0f, canvas_size.y - view_h);

        float sx = m_scroll_request_x;
        float sy = m_scroll_request_y;
        if (sx < 0.0f) sx = 0.0f;
        if (sy < 0.0f) sy = 0.0f;
        if (sx > max_x) sx = max_x;
        if (sy > max_y) sy = max_y;

        ImGui::SetScrollX(sx);
        ImGui::SetScrollY(sy);

        suppress_caret_autoscroll = true;
        m_scroll_request_valid = false;
    }

    // Capture both left and right mouse buttons so tools/scripts can react to either click+drag.
    ImGui::InvisibleButton(id, canvas_size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetItemRectMin();
    origin.x = std::floor(origin.x);
    origin.y = std::floor(origin.y);

    // If we zoomed this frame via Ctrl+MouseWheel, correct scroll so the point under the mouse
    // stays stable in *canvas pixel space*.
    //
    // This must happen AFTER InvisibleButton() so we can use GetItemRectMin() as the true origin.
    if (wheel_zoom_this_frame && wheel_zoom_ratio > 0.0f)
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        const ImRect clip = w ? w->InnerClipRect : ImRect(0, 0, 0, 0);
        const float view_w = clip.GetWidth();
        const float view_h = clip.GetHeight();

        // We'll adjust scroll *now* (after InvisibleButton exists), but that means the
        // screen-space position of the canvas item changes immediately with scroll.
        // If we don't compensate, we'll draw one frame with an origin computed under
        // the old scroll, then the next frame under the new scroll -> visible flicker.
        const float scroll_before_x = ImGui::GetScrollX();
        const float scroll_before_y = ImGui::GetScrollY();

        // Choose anchor point:
        // - prefer the real mouse position if it's inside the visible canvas viewport
        // - otherwise fall back to viewport center (more robust when wheel comes from scrollbars)
        float local_x = (wheel_mouse_pos.x - origin.x);
        float local_y = (wheel_mouse_pos.y - origin.y);
        const bool mouse_in_view =
            (wheel_mouse_pos.x >= clip.Min.x && wheel_mouse_pos.x <= clip.Max.x &&
             wheel_mouse_pos.y >= clip.Min.y && wheel_mouse_pos.y <= clip.Max.y);
        if (!mouse_in_view)
        {
            local_x = view_w * 0.5f;
            local_y = view_h * 0.5f;
        }
        local_x = std::clamp(local_x, 0.0f, std::max(0.0f, view_w));
        local_y = std::clamp(local_y, 0.0f, std::max(0.0f, view_h));

        const float world_x = wheel_pre_scroll_x + local_x;
        const float world_y = wheel_pre_scroll_y + local_y;

        float new_scroll_x = world_x * wheel_zoom_ratio - local_x;
        float new_scroll_y = world_y * wheel_zoom_ratio - local_y;

        // Clamp to scrollable bounds for the new canvas size.
        const float max_x = std::max(0.0f, canvas_size.x - view_w);
        const float max_y = std::max(0.0f, canvas_size.y - view_h);
        if (new_scroll_x < 0.0f) new_scroll_x = 0.0f;
        if (new_scroll_y < 0.0f) new_scroll_y = 0.0f;
        if (new_scroll_x > max_x) new_scroll_x = max_x;
        if (new_scroll_y > max_y) new_scroll_y = max_y;

        ImGui::SetScrollX(new_scroll_x);
        ImGui::SetScrollY(new_scroll_y);

        // Compensate origin for the scroll we just applied so drawing uses the correct
        // screen-space origin for this same frame.
        const float dx = new_scroll_x - scroll_before_x;
        const float dy = new_scroll_y - scroll_before_y;
        origin.x -= dx;
        origin.y -= dy;
        origin.x = std::floor(origin.x);
        origin.y = std::floor(origin.y);

        suppress_caret_autoscroll = true; // avoid “fight” between zoom anchoring and caret-follow
    }

    // Base canvas background is NOT theme-driven; it's a fixed black/white fill so
    // the editing "paper" stays consistent regardless of UI skin.
    {
        const ImU32 bg = m_canvas_bg_white ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
        draw_list->AddRectFilled(origin,
                                 ImVec2(origin.x + canvas_size.x, origin.y + canvas_size.y),
                                 ApplyCurrentStyleAlpha(bg));
    }

    // Focus rules:
    // - click inside the grid to focus
    // - click elsewhere *within the same canvas window* to defocus
    //
    // IMPORTANT: don't defocus on global UI clicks (e.g. main menu bar) so menu actions
    // like File/Save and Edit/Undo can still target the active canvas.
    m_focus_gained = false; // transient per-frame
    const bool was_focused = m_has_focus;
    const bool any_click = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    if (ImGui::IsItemHovered() && any_click)
        m_has_focus = true;
    else if (!ImGui::IsItemHovered() && any_click)
    {
        // Only clear focus if the click was in this window (or its child windows).
        // If the click was outside (e.g. main menu bar, another window), keep focus.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
            m_has_focus = false;
    }
    if (!was_focused && m_has_focus)
        m_focus_gained = true;

    HandleMouseInteraction(origin, scaled_cell_w, scaled_cell_h);

    // Mouse phase: tools can react to cursor state for this frame.
    if (tool_runner)
        tool_runner(*this, 1);
    EndUndoCapture();

    // Keep cursor visible when navigating.
    //
    // Important: only auto-scroll to caret when there was keyboard/text input this frame.
    // This prevents "snap-back" after mouse-driven scrolling/panning (e.g. preview minimap drag),
    // and avoids fighting tools that adjust the caret during mouse painting.
    const bool caret_moved = (m_caret_row != caret_start_row) || (m_caret_col != caret_start_col);
    const bool mouse_painting = m_cursor_valid && (m_cursor_left_down || m_cursor_right_down);
    const bool should_follow_caret = had_key_input || had_typed_input || (caret_moved && mouse_painting);
    if (m_has_focus && m_follow_caret && !suppress_caret_autoscroll && should_follow_caret)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImRect clip_rect = window ? window->InnerClipRect : ImRect(0, 0, 0, 0);
        const float view_w = clip_rect.GetWidth();
        const float view_h = clip_rect.GetHeight();

        const float scroll_x = ImGui::GetScrollX();
        const float scroll_y = ImGui::GetScrollY();

        const float cursor_x0 = static_cast<float>(m_caret_col) * scaled_cell_w;
        const float cursor_x1 = cursor_x0 + scaled_cell_w;
        const float cursor_y0 = static_cast<float>(m_caret_row) * scaled_cell_h;
        const float cursor_y1 = cursor_y0 + scaled_cell_h;

        if (cursor_x0 < scroll_x)
            ImGui::SetScrollX(cursor_x0);
        else if (cursor_x1 > scroll_x + view_w)
            ImGui::SetScrollX(cursor_x1 - view_w);

        if (cursor_y0 < scroll_y)
            ImGui::SetScrollY(cursor_y0);
        else if (cursor_y1 > scroll_y + view_h)
            ImGui::SetScrollY(cursor_y1 - view_h);
    }

    DrawVisibleCells(draw_list, origin, scaled_cell_w, scaled_cell_h, scaled_font_size);
    DrawSelectionOverlay(draw_list, origin, scaled_cell_w, scaled_cell_h, scaled_font_size);

    // Capture last viewport metrics for minimap/preview. Do this at the very end so any
    // caret auto-scroll or scroll requests are reflected.
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImRect clip_rect = window ? window->InnerClipRect : ImRect(0, 0, 0, 0);
        m_last_view.valid = true;
        m_last_view.columns = m_columns;
        m_last_view.rows = m_rows;
        m_last_view.zoom = m_zoom;
        m_last_view.base_cell_w = base_cell_w;
        m_last_view.base_cell_h = base_cell_h;
        m_last_view.cell_w = scaled_cell_w;
        m_last_view.cell_h = scaled_cell_h;
        m_last_view.canvas_w = canvas_size.x;
        m_last_view.canvas_h = canvas_size.y;
        m_last_view.view_w = clip_rect.GetWidth();
        m_last_view.view_h = clip_rect.GetHeight();
        m_last_view.scroll_x = ImGui::GetScrollX();
        m_last_view.scroll_y = ImGui::GetScrollY();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

AnsiCanvas::ProjectState AnsiCanvas::GetProjectState() const
{
    auto to_project_layer = [&](const Layer& l) -> ProjectLayer
    {
        ProjectLayer out;
        out.name = l.name;
        out.visible = l.visible;
        out.lock_transparency = l.lock_transparency;
        out.cells = l.cells;
        out.fg = l.fg;
        out.bg = l.bg;
        return out;
    };

    auto to_project_snapshot = [&](const Snapshot& s) -> ProjectSnapshot
    {
        ProjectSnapshot out;
        out.columns = s.columns;
        out.rows = s.rows;
        out.active_layer = s.active_layer;
        out.caret_row = s.caret_row;
        out.caret_col = s.caret_col;
        out.layers.reserve(s.layers.size());
        for (const auto& l : s.layers)
            out.layers.push_back(to_project_layer(l));
        return out;
    };

    ProjectState out;
    out.version = 3;
    out.colour_palette_title = m_colour_palette_title;
    out.sauce = m_sauce;
    out.current = to_project_snapshot(MakeSnapshot());
    out.undo_limit = m_undo_limit;

    out.undo.reserve(m_undo_stack.size());
    for (const auto& s : m_undo_stack)
        out.undo.push_back(to_project_snapshot(s));
    out.redo.reserve(m_redo_stack.size());
    for (const auto& s : m_redo_stack)
        out.redo.push_back(to_project_snapshot(s));
    return out;
}

bool AnsiCanvas::SetProjectState(const ProjectState& state, std::string& out_error)
{
    out_error.clear();

    auto to_internal_layer = [&](const ProjectLayer& l, Layer& out, std::string& err) -> bool
    {
        out.name = l.name;
        out.visible = l.visible;
        out.lock_transparency = l.lock_transparency;
        out.cells = l.cells;
        out.fg = l.fg;
        out.bg = l.bg;

        if (!out.fg.empty() && out.fg.size() != out.cells.size())
        {
            err = "Layer fg size does not match cells size.";
            return false;
        }
        if (!out.bg.empty() && out.bg.size() != out.cells.size())
        {
            err = "Layer bg size does not match cells size.";
            return false;
        }

        if (out.fg.empty())
            out.fg.assign(out.cells.size(), 0);
        if (out.bg.empty())
            out.bg.assign(out.cells.size(), 0);
        return true;
    };

    auto to_internal_snapshot = [&](const ProjectSnapshot& s, Snapshot& out, std::string& err) -> bool
    {
        out.columns = (s.columns > 0) ? s.columns : 80;
        if (out.columns > 4096) out.columns = 4096;
        out.rows = (s.rows > 0) ? s.rows : 1;
        out.active_layer = s.active_layer;
        out.caret_row = s.caret_row;
        out.caret_col = s.caret_col;
        out.layers.clear();
        out.layers.reserve(s.layers.size());
        for (const auto& pl : s.layers)
        {
            Layer l;
            if (!to_internal_layer(pl, l, err))
                return false;
            out.layers.push_back(std::move(l));
        }
        return true;
    };

    // Convert everything up-front so we can fail without mutating `this`.
    Snapshot current_internal;
    if (!to_internal_snapshot(state.current, current_internal, out_error))
        return false;

    std::vector<Snapshot> undo_internal;
    undo_internal.reserve(state.undo.size());
    for (const auto& s : state.undo)
    {
        Snapshot tmp;
        if (!to_internal_snapshot(s, tmp, out_error))
            return false;
        undo_internal.push_back(std::move(tmp));
    }

    std::vector<Snapshot> redo_internal;
    redo_internal.reserve(state.redo.size());
    for (const auto& s : state.redo)
    {
        Snapshot tmp;
        if (!to_internal_snapshot(s, tmp, out_error))
            return false;
        redo_internal.push_back(std::move(tmp));
    }

    // Apply in one go.
    m_has_focus = false;
    m_typed_queue.clear();
    m_key_events = KeyEvents{};
    m_mouse_capture = false;
    m_cursor_valid = false;

    m_undo_capture_active = false;
    m_undo_capture_modified = false;
    m_undo_capture_has_snapshot = false;
    m_undo_applying_snapshot = false;

    m_undo_limit = (state.undo_limit > 0) ? state.undo_limit : 256;
    m_undo_stack = std::move(undo_internal);
    m_redo_stack = std::move(redo_internal);

    // Metadata (non-undoable, persisted).
    m_sauce = state.sauce;
    m_colour_palette_title = state.colour_palette_title;

    ApplySnapshot(current_internal);

    // Clamp active layer and ensure we have at least one layer even for malformed saves.
    EnsureDocument();

    // Post-load: ensure SAUCE defaults and geometry are consistent with the applied snapshot.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
    return true;
}
