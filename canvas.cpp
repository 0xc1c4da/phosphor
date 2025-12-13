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

void AnsiCanvas::ApplyTypedCodepoint(char32_t cp)
{
    EnsureDocument();

    // Normalize common whitespace/control inputs.
    if (cp == U'\t')
        cp = U' ';

    // Enter -> new line.
    if (cp == U'\n' || cp == U'\r')
    {
        m_cursor_row++;
        m_cursor_col = 0;
        EnsureRows(m_cursor_row + 1);
        return;
    }

    // Ignore other control chars.
    if (cp < 0x20)
        return;

    SetActiveCell(m_cursor_row, m_cursor_col, cp);

    // Advance cursor.
    m_cursor_col++;
    if (m_cursor_col >= m_columns)
    {
        m_cursor_col = 0;
        m_cursor_row++;
        EnsureRows(m_cursor_row + 1);
    }
}

int AnsiCanvas::TextInputCallback(ImGuiInputTextCallbackData* data)
{
    if (!data || data->EventFlag != ImGuiInputTextFlags_CallbackCharFilter)
        return 0;

    AnsiCanvas* self = static_cast<AnsiCanvas*>(data->UserData);
    if (!self)
        return 0;

    const char32_t cp = static_cast<char32_t>(data->EventChar);
    self->ApplyTypedCodepoint(cp);

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
    // (When the user clicks elsewhere, we drop m_has_focus, so we stop stealing focus.)
    if (m_has_focus && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        ImGui::SetKeyboardFocusHere();

    ImGui::InputText(input_id.c_str(), dummy, IM_ARRAYSIZE(dummy), flags, &AnsiCanvas::TextInputCallback, this);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
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

int AnsiCanvas::AddLayer(const std::string& name)
{
    EnsureDocument();

    Layer layer;
    layer.name = name.empty() ? ("Layer " + std::to_string((int)m_layers.size() + 1)) : name;
    layer.visible = true;
    layer.cells.assign(static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns), U' ');

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

void AnsiCanvas::SetColumns(int columns)
{
    if (columns <= 0)
        return;
    EnsureDocument();

    if (columns == m_columns)
        return;

    const int old_cols = m_columns;
    const int old_rows = m_rows;
    m_columns = columns;

    for (Layer& layer : m_layers)
    {
        std::vector<char32_t> new_cells;
        new_cells.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), U' ');

        const int copy_cols = std::min(old_cols, m_columns);
        for (int r = 0; r < old_rows; ++r)
        {
            for (int c = 0; c < copy_cols; ++c)
            {
                const size_t src = static_cast<size_t>(r) * static_cast<size_t>(old_cols) + static_cast<size_t>(c);
                const size_t dst = static_cast<size_t>(r) * static_cast<size_t>(m_columns) + static_cast<size_t>(c);
                if (src < layer.cells.size() && dst < new_cells.size())
                    new_cells[dst] = layer.cells[src];
            }
        }

        layer.cells = std::move(new_cells);
    }

    // Clamp cursor to new width.
    if (m_cursor_col >= m_columns)
        m_cursor_col = m_columns - 1;
    if (m_cursor_col < 0)
        m_cursor_col = 0;
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

    // Reset document to a single empty row.
    m_rows = 1;
    for (Layer& layer : m_layers)
        layer.cells.assign(static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns), U' ');

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

    m_cursor_row = 0;
    m_cursor_col = 0;
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
        base.cells.assign(static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns), U' ');
        m_layers.push_back(std::move(base));
        m_active_layer = 0;
    }

    // Ensure every layer has the correct cell count.
    const size_t need = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    for (Layer& layer : m_layers)
    {
        if (layer.cells.size() != need)
            layer.cells.resize(need, U' ');
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

    m_rows = rows_needed;
    const size_t need = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    for (Layer& layer : m_layers)
        layer.cells.resize(need, U' ');
}

size_t AnsiCanvas::CellIndex(int row, int col) const
{
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;
    return static_cast<size_t>(row) * static_cast<size_t>(m_columns) + static_cast<size_t>(col);
}

char32_t AnsiCanvas::GetCompositeCell(int row, int col) const
{
    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return U' ';
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return U' ';

    const size_t idx = CellIndex(row, col);
    // Topmost visible non-space wins; treat space as transparent.
    for (int i = (int)m_layers.size() - 1; i >= 0; --i)
    {
        const Layer& layer = m_layers[(size_t)i];
        if (!layer.visible)
            continue;
        if (idx >= layer.cells.size())
            continue;
        char32_t cp = layer.cells[idx];
        if (cp != U' ')
            return cp;
    }
    return U' ';
}

void AnsiCanvas::SetActiveCell(int row, int col, char32_t cp)
{
    EnsureDocument();
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

void AnsiCanvas::HandleKeyboardNavigation()
{
    if (!m_has_focus)
        return;

    EnsureDocument();

    // Arrow navigation behaves like a classic fixed-width editor:
    //  - left at col 0 goes to previous row's last col (if possible)
    //  - right at last col goes to next row's col 0 (growing rows on demand)
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
    {
        if (m_cursor_col > 0)
            m_cursor_col--;
        else if (m_cursor_row > 0)
        {
            m_cursor_row--;
            m_cursor_col = m_columns - 1;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
    {
        if (m_cursor_col < m_columns - 1)
            m_cursor_col++;
        else
        {
            m_cursor_row++;
            m_cursor_col = 0;
            EnsureRows(m_cursor_row + 1);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
    {
        if (m_cursor_row > 0)
            m_cursor_row--;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
    {
        m_cursor_row++;
        EnsureRows(m_cursor_row + 1);
    }

    // Home/End: move within the current row.
    if (ImGui::IsKeyPressed(ImGuiKey_Home))
        m_cursor_col = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_End))
        m_cursor_col = m_columns - 1;

    // Clamp.
    if (m_cursor_row < 0) m_cursor_row = 0;
    if (m_cursor_col < 0) m_cursor_col = 0;
    if (m_cursor_col >= m_columns) m_cursor_col = m_columns - 1;
}

void AnsiCanvas::HandleTextInput()
{
    if (!m_has_focus)
        return;

    EnsureDocument();

    // Editing keys (independent of text input queue).
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace))
    {
        // Move left then clear.
        if (m_cursor_col > 0)
            m_cursor_col--;
        else if (m_cursor_row > 0)
        {
            m_cursor_row--;
            m_cursor_col = m_columns - 1;
        }
        SetActiveCell(m_cursor_row, m_cursor_col, U' ');
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete))
    {
        SetActiveCell(m_cursor_row, m_cursor_col, U' ');
    }

    // Enter -> new line (handled as a key press so it works even when the backend
    // doesn't emit a text event for it).
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
    {
        m_cursor_row++;
        m_cursor_col = 0;
        EnsureRows(m_cursor_row + 1);
    }
}

void AnsiCanvas::HandleMouseInteraction(const ImVec2& origin, float cell_w, float cell_h)
{
    EnsureDocument();

    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsItemHovered())
        return;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        ImVec2 local(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
        if (local.x < 0.0f || local.y < 0.0f)
            return;

        int col = static_cast<int>(local.x / cell_w);
        int row = static_cast<int>(local.y / cell_h);

        if (col < 0) col = 0;
        if (col >= m_columns) col = m_columns - 1;
        if (row < 0) row = 0;

        EnsureRows(row + 1);
        m_cursor_row = row;
        m_cursor_col = col;
        m_has_focus = true;
    }
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

            // Cursor highlight.
            if (row == m_cursor_row && col == m_cursor_col)
            {
                ImU32 cursor_col = ImGui::GetColorU32(ImVec4(0.30f, 0.30f, 0.60f, 0.75f));
                draw_list->AddRectFilled(cell_min, cell_max, cursor_col);
            }

            char32_t cp = GetCompositeCell(row, col);
            if (cp == U' ')
                continue; // drawing spaces is unnecessary for now

            char buf[5] = {0, 0, 0, 0, 0};
            EncodeUtf8(cp, buf);
            ImVec2 text_pos(cell_min.x, cell_min.y);
            draw_list->AddText(font, font_size, text_pos,
                               ImGui::GetColorU32(ImGuiCol_Text),
                               buf, nullptr);
        }
    }
}

void AnsiCanvas::Render(const char* id)
{
    if (!id)
        return;

    ImFont* font = ImGui::GetFont();
    if (!font)
        return;

    EnsureDocument();

    // Base cell size from the current font (Unscii is monospaced).
    // We intentionally *do not auto-scale to window width* so the grid remains stable.
    const float font_size = ImGui::GetFontSize();
    const float cell_w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "M", "M" + 1).x;
    const float cell_h = font_size;

    // Quick status line (foundation for future toolbars).
    ImGui::Text("Cols: %d  Rows: %d  Cursor: (%d, %d)%s",
                m_columns, m_rows, m_cursor_row, m_cursor_col,
                m_has_focus ? "  [editing]" : "");

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

    // Hidden input widget to reliably receive UTF-8 text events from SDL3.
    // We place it early in the child so typed input can grow rows before we compute canvas_size.
    HandleCharInputWidget(id);

    // Restore fit-to-width behavior: keep logical column count fixed, scale cells
    // so the grid fits the available width of the child.
    const float base_font_size = font_size;
    const float base_cell_w    = cell_w;

    float scaled_font_size = base_font_size;
    float scaled_cell_w    = base_cell_w;
    float scaled_cell_h    = cell_h;

    const float needed_width    = base_cell_w * static_cast<float>(m_columns);
    const float available_width = ImGui::GetContentRegionAvail().x;
    if (needed_width > 0.0f && available_width > 0.0f)
    {
        float scale = available_width / needed_width;
        const float min_scale = 0.25f;
        const float max_scale = 4.0f;
        if (scale < min_scale) scale = min_scale;
        if (scale > max_scale) scale = max_scale;

        float snapped_cell_w = std::floor(base_cell_w * scale + 0.5f);
        if (snapped_cell_w < 1.0f)
            snapped_cell_w = 1.0f;
        float snapped_scale = snapped_cell_w / base_cell_w;

        scaled_font_size = base_font_size * snapped_scale;
        scaled_cell_w    = snapped_cell_w;
        scaled_cell_h    = std::floor(base_font_size * snapped_scale + 0.5f);
        if (scaled_cell_h < 1.0f)
            scaled_cell_h = 1.0f;
    }

    // IMPORTANT: handle keyboard input before computing canvas_size, because input can
    // grow the document (rows). If we grow after creating the item, ImGui's scroll range
    // won't include the new rows until the next frame, and the cursor can disappear.
    if (m_has_focus)
    {
        HandleKeyboardNavigation();
        HandleTextInput();
    }

    // Always keep the document large enough to contain the cursor.
    EnsureRows(m_cursor_row + 1);

    ImVec2 canvas_size(scaled_cell_w * static_cast<float>(m_columns),
                       scaled_cell_h * static_cast<float>(m_rows));

    ImGui::InvisibleButton(id, canvas_size, ImGuiButtonFlags_MouseButtonLeft);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetItemRectMin();
    origin.x = std::floor(origin.x);
    origin.y = std::floor(origin.y);

    // Focus rules: click inside to focus, click outside (in same window) to defocus.
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_has_focus = true;
    else if (!ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_has_focus = false;

    HandleMouseInteraction(origin, scaled_cell_w, scaled_cell_h);

    // Keep cursor visible when navigating.
    if (m_has_focus)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImRect clip_rect = window ? window->InnerClipRect : ImRect(0, 0, 0, 0);
        const float view_w = clip_rect.GetWidth();
        const float view_h = clip_rect.GetHeight();

        const float scroll_x = ImGui::GetScrollX();
        const float scroll_y = ImGui::GetScrollY();

        const float cursor_x0 = static_cast<float>(m_cursor_col) * scaled_cell_w;
        const float cursor_x1 = cursor_x0 + scaled_cell_w;
        const float cursor_y0 = static_cast<float>(m_cursor_row) * scaled_cell_h;
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
    ImGui::EndChild();
}
