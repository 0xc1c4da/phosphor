#include "canvas.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <locale>
#include <string_view>
#include <string>
#include <vector>

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
    if (!m_undo_capture_active)
        return;

    if (!m_undo_capture_has_snapshot)
    {
        m_undo_capture_snapshot = MakeSnapshot();
        m_undo_capture_has_snapshot = true;
    }
    m_undo_capture_modified = true;
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
    // Guard against mouse clicks so we don't steal focus from other UI widgets on the
    // click frame (layer rename, menus, etc). The canvas focus state will be updated
    // later in Render() based on where the click landed.
    const bool any_click =
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    if (m_has_focus &&
        !any_click &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
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
    // If a popup/modal is open, don't interpret keys as canvas commands.
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        return;

    // Match previous behavior: discrete press events.
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
    m_layers[static_cast<size_t>(index)].visible = visible;
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
    PrepareUndoSnapshot();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;
    EnsureRows(row + 1);

    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    Layer& layer = m_layers[(size_t)m_active_layer];
    const size_t idx = CellIndex(row, col);
    if (idx < layer.cells.size())
        layer.cells[idx] = cp;
}

void AnsiCanvas::SetActiveCell(int row, int col, char32_t cp, Color32 fg, Color32 bg)
{
    EnsureDocument();
    PrepareUndoSnapshot();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;
    EnsureRows(row + 1);

    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    Layer& layer = m_layers[(size_t)m_active_layer];
    const size_t idx = CellIndex(row, col);
    if (idx < layer.cells.size())
        layer.cells[idx] = cp;
    if (idx < layer.fg.size())
        layer.fg[idx] = fg;
    if (idx < layer.bg.size())
        layer.bg[idx] = bg;
}

void AnsiCanvas::ClearActiveCellStyle(int row, int col)
{
    EnsureDocument();
    PrepareUndoSnapshot();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;
    EnsureRows(row + 1);

    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    Layer& layer = m_layers[(size_t)m_active_layer];
    const size_t idx = CellIndex(row, col);
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

    PrepareUndoSnapshot();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;
    EnsureRows(row + 1);

    Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = CellIndex(row, col);
    if (idx < layer.cells.size())
        layer.cells[idx] = cp;
    return true;
}

bool AnsiCanvas::SetLayerCell(int layer_index, int row, int col, char32_t cp, Color32 fg, Color32 bg)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    PrepareUndoSnapshot();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;
    EnsureRows(row + 1);

    Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = CellIndex(row, col);
    if (idx < layer.cells.size())
        layer.cells[idx] = cp;
    if (idx < layer.fg.size())
        layer.fg[idx] = fg;
    if (idx < layer.bg.size())
        layer.bg[idx] = bg;
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
    EnsureRows(row + 1);
    Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = CellIndex(row, col);
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
    PrepareUndoSnapshot();
    ClearLayerCellStyleInternal(layer_index, row, col);
    return true;
}

bool AnsiCanvas::ClearLayer(int layer_index, char32_t cp)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    PrepareUndoSnapshot();
    Layer& layer = m_layers[(size_t)layer_index];
    std::fill(layer.cells.begin(), layer.cells.end(), cp);
    std::fill(layer.fg.begin(), layer.fg.end(), 0);
    std::fill(layer.bg.begin(), layer.bg.end(), 0);
    return true;
}

bool AnsiCanvas::FillLayer(int layer_index,
                           std::optional<char32_t> cp,
                           std::optional<Color32> fg,
                           std::optional<Color32> bg)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    PrepareUndoSnapshot();
    Layer& layer = m_layers[(size_t)layer_index];
    if (cp.has_value())
        std::fill(layer.cells.begin(), layer.cells.end(), *cp);
    if (fg.has_value())
        std::fill(layer.fg.begin(), layer.fg.end(), *fg);
    if (bg.has_value())
        std::fill(layer.bg.begin(), layer.bg.end(), *bg);
    return true;
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
                draw_list->AddRectFilled(cell_min, cell_max, (ImU32)cell.bg);
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

            char buf[5] = {0, 0, 0, 0, 0};
            EncodeUtf8(cp, buf);
            ImVec2 text_pos(cell_min.x, cell_min.y);
            const ImU32 fg_col = (cell.fg != 0) ? (ImU32)cell.fg : ImGui::GetColorU32(ImGuiCol_Text);
            draw_list->AddText(font, font_size, text_pos,
                               fg_col,
                               buf, nullptr);
        }
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

    // Base cell size from the current font (Unscii is monospaced).
    // We intentionally *do not auto-fit to window width*; the user controls zoom explicitly.
    const float font_size = ImGui::GetFontSize();
    const float cell_w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "M", "M" + 1).x;
    const float cell_h = font_size;

    // Quick status line (foundation for future toolbars).
    ImGui::Text("Cols: %d  Rows: %d  Cursor: (%d, %d)%s",
                m_columns, m_rows, m_caret_row, m_caret_col,
                m_has_focus ? "  [editing]" : "");

    // Hidden input widget to reliably receive UTF-8 text events from SDL3.
    //
    // IMPORTANT: this must NOT live inside the scrollable canvas child. If it does,
    // forcing keyboard focus to it (SetKeyboardFocusHere) will cause ImGui to scroll
    // the child to reveal the focused item, which feels like the canvas "jumps" to
    // the top when you click/paint while scrolled.
    ImGui::SameLine();
    HandleCharInputWidget(id);

    // Layer GUI lives in the LayerManager component (see layer_manager.*).

    // Scrollable region: fixed-width canvas, "infinite" rows (grown on demand).
    std::string child_id = std::string(id) + "##_scroll";
    ImGuiWindowFlags child_flags =
        ImGuiWindowFlags_HorizontalScrollbar |
        ImGuiWindowFlags_NoNavInputs |
        ImGuiWindowFlags_NoNavFocus;
    if (!ImGui::BeginChild(child_id.c_str(), ImVec2(0, 0), true, child_flags))
    {
        ImGui::EndChild();
        return;
    }

    const float base_font_size = font_size;
    const float base_cell_w    = cell_w;
    const float base_cell_h    = cell_h;

    // Ctrl+MouseWheel zoom on the canvas (like a typical editor).
    // We also adjust scroll so the point under the mouse stays stable.
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
                return snapped_cell_w / base_cell_w;
            };

            const float old_zoom = m_zoom;
            const float old_scale = snapped_scale_for_zoom(old_zoom);

            const float factor = (io.MouseWheel > 0.0f) ? 1.10f : (1.0f / 1.10f);
            SetZoom(old_zoom * factor);

            const float new_zoom = m_zoom;
            const float new_scale = snapped_scale_for_zoom(new_zoom);
            const float ratio = (old_scale > 0.0f) ? (new_scale / old_scale) : 1.0f;

            // The canvas origin in screen space is the current cursor position in the child.
            // (We don't add any other widgets before the InvisibleButton.)
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float local_x = io.MousePos.x - origin.x;
            const float local_y = io.MousePos.y - origin.y;

            const float world_x = ImGui::GetScrollX() + local_x;
            const float world_y = ImGui::GetScrollY() + local_y;

            const float new_scroll_x = world_x * ratio - local_x;
            const float new_scroll_y = world_y * ratio - local_y;
            RequestScrollPixels(new_scroll_x, new_scroll_y);
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

    // Focus rules:
    // - click inside the grid to focus
    // - click elsewhere *within the same canvas window* to defocus
    //
    // IMPORTANT: don't defocus on global UI clicks (e.g. main menu bar) so menu actions
    // like File/Save and Edit/Undo can still target the active canvas.
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
    if (m_has_focus && !suppress_caret_autoscroll && should_follow_caret)
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
}

AnsiCanvas::ProjectState AnsiCanvas::GetProjectState() const
{
    auto to_project_layer = [&](const Layer& l) -> ProjectLayer
    {
        ProjectLayer out;
        out.name = l.name;
        out.visible = l.visible;
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
    out.version = 1;
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

    ApplySnapshot(current_internal);

    // Clamp active layer and ensure we have at least one layer even for malformed saves.
    EnsureDocument();
    return true;
}
