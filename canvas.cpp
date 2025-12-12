#include "canvas.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <locale>
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

AnsiCanvas::AnsiCanvas(int columns)
    : m_columns(columns > 0 ? columns : 80)
{
}

void AnsiCanvas::SetColumns(int columns)
{
    if (columns <= 0)
        return;
    m_columns = columns;
    // Cursor index remains valid; row/column mapping will change implicitly.
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

    // Decode UTF-8 into UTF-32 manually so we can work per codepoint.
    // While decoding, collect logical lines and track the maximum column width.
    // After decoding, flatten all lines into a single cell stream, padding
    // shorter lines with spaces so every row has the same column count.
    const unsigned char* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t len = bytes.size();
    size_t i = 0;

    std::vector<std::u32string> lines;
    std::u32string current_line;
    int max_line_columns = 0;
    bool last_was_cr = false;

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
            // Invalid leading byte, skip.
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

        // Normalize CRLF: treat "\r\n" as a single newline.
        if (cp == U'\n' || cp == U'\r')
        {
            if (cp == U'\n' && last_was_cr)
            {
                last_was_cr = false;
                continue;
            }

            if ((int)current_line.size() > max_line_columns)
                max_line_columns = static_cast<int>(current_line.size());
            lines.push_back(current_line);
            current_line.clear();
            last_was_cr = (cp == U'\r');
            continue;
        }

        last_was_cr = false;
        current_line.push_back(cp);
    }

    // Account for last line if file doesn't end with a newline.
    if (!current_line.empty() || lines.empty())
    {
        if ((int)current_line.size() > max_line_columns)
            max_line_columns = static_cast<int>(current_line.size());
        lines.push_back(current_line);
    }

    if (max_line_columns > 0)
        SetColumns(max_line_columns);

    // Flatten lines into a single padded cell stream.
    m_cells.clear();
    if (!lines.empty() && max_line_columns > 0)
    {
        for (const std::u32string& line : lines)
        {
            m_cells.insert(m_cells.end(), line.begin(), line.end());
            const int pad = max_line_columns - static_cast<int>(line.size());
            if (pad > 0)
                m_cells.insert(m_cells.end(), static_cast<size_t>(pad), U' ');
        }
    }

    m_cursor_index = 0;
    if (!m_cells.empty())
        m_cursor_index = 0;

    return true;
}

int AnsiCanvas::GetRowCount() const
{
    if (m_cells.empty())
        return 1;
    return (static_cast<int>(m_cells.size()) + m_columns - 1) / m_columns;
}

void AnsiCanvas::HandleKeyboardNavigation()
{
    if (!m_has_focus || m_cells.empty())
        return;

    ImGuiIO& io = ImGui::GetIO();
    int max_index = static_cast<int>(m_cells.size()) - 1;

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
    {
        if (m_cursor_index > 0)
            m_cursor_index--;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
    {
        if (m_cursor_index < max_index)
            m_cursor_index++;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
    {
        if (m_cursor_index >= m_columns)
            m_cursor_index -= m_columns;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
    {
        if (m_cursor_index + m_columns <= max_index)
            m_cursor_index += m_columns;
    }

    // Clamp just in case.
    if (m_cursor_index < 0)
        m_cursor_index = 0;
    if (m_cursor_index > max_index)
        m_cursor_index = max_index;

    (void)io; // io currently unused but likely to be useful later (modifiers, etc.).
}

void AnsiCanvas::HandleMouseInteraction(const ImVec2& origin, float cell_w, float cell_h)
{
    if (m_cells.empty())
        return;

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

        int index = row * m_columns + col;
        int max_index = static_cast<int>(m_cells.size()) - 1;
        if (index > max_index)
            index = max_index;

        m_cursor_index = index;
        m_has_focus = true;
    }
}

void AnsiCanvas::DrawCells(ImDrawList* draw_list, const ImVec2& origin, float cell_w, float cell_h, float font_size)
{
    if (!draw_list)
        return;

    ImFont* font = ImGui::GetFont();
    if (!font)
        return;

    const int row_count = GetRowCount();
    const int total_cells = row_count * m_columns;

    for (int index = 0; index < total_cells; ++index)
    {
        int row = index / m_columns;
        int col = index % m_columns;

        ImVec2 cell_min(origin.x + col * cell_w,
                        origin.y + row * cell_h);
        ImVec2 cell_max(cell_min.x + cell_w,
                        cell_min.y + cell_h);

        // Highlight the cursor cell.
        if (index == m_cursor_index)
        {
            ImU32 cursor_col = ImGui::GetColorU32(ImVec4(0.30f, 0.30f, 0.60f, 0.75f));
            draw_list->AddRectFilled(cell_min, cell_max, cursor_col);
        }

        if (index >= static_cast<int>(m_cells.size()))
            continue; // beyond loaded content; treat as empty cell

        char32_t cp = m_cells[static_cast<size_t>(index)];
        char buf[5] = {0, 0, 0, 0, 0};
        int bytes = EncodeUtf8(cp, buf);
        (void)bytes;

        // Slightly inset the text to avoid touching cell borders visually.
        ImVec2 text_pos(cell_min.x, cell_min.y);

        draw_list->AddText(font, font_size, text_pos,
                           ImGui::GetColorU32(ImGuiCol_Text),
                           buf, nullptr);
    }
}

void AnsiCanvas::Render(const char* id)
{
    if (m_columns <= 0)
        m_columns = 80;

    ImFont* font = ImGui::GetFont();
    if (!font)
        return;

    // Base cell size from the current font (Unscii is monospaced).
    const float base_font_size = ImGui::GetFontSize();
    const float base_cell_w = font->CalcTextSizeA(base_font_size, FLT_MAX, 0.0f, "M", "M" + 1).x;

    // Auto-scale the canvas horizontally to fit the current window width,
    // keeping the logical column count constant.
    float font_size = base_font_size;
    float cell_w    = base_cell_w;
    float cell_h    = base_font_size;
    const float needed_width   = base_cell_w * static_cast<float>(m_columns);
    const float available_width = ImGui::GetContentRegionAvail().x;
    if (needed_width > 0.0f && available_width > 0.0f)
    {
        float scale = available_width / needed_width;
        // Optionally clamp scale to avoid extreme sizes.
        const float min_scale = 0.25f;
        const float max_scale = 4.0f;
        if (scale < min_scale) scale = min_scale;
        if (scale > max_scale) scale = max_scale;

        // Snap scale so that cell sizes land on pixel boundaries to avoid gaps/overlaps.
        // We quantize horizontally, then derive a refined scale from that.
        float snapped_cell_w = std::floor(base_cell_w * scale + 0.5f);
        if (snapped_cell_w < 1.0f)
            snapped_cell_w = 1.0f;
        float snapped_scale = snapped_cell_w / base_cell_w;

        font_size = base_font_size * snapped_scale;
        cell_w    = snapped_cell_w;
        cell_h    = std::floor(base_font_size * snapped_scale + 0.5f);
        if (cell_h < 1.0f)
            cell_h = 1.0f;
    }

    const int row_count = GetRowCount();
    ImVec2 canvas_size(cell_w * m_columns,
                       cell_h * row_count);

    // Create an invisible interactive region; we'll draw into it manually.
    ImGui::InvisibleButton(id, canvas_size);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetItemRectMin();
    // Snap drawing origin to whole pixels to reduce rendering artifacts between cells.
    origin.x = std::floor(origin.x);
    origin.y = std::floor(origin.y);

    // Mouse interaction is tied to this "item".
    HandleMouseInteraction(origin, cell_w, cell_h);

    // Canvas gets keyboard focus if clicked and the window is focused.
    if (!ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_has_focus = false;

    // Navigation with arrow keys when focused.
    HandleKeyboardNavigation();

    // Draw all cells.
    DrawCells(draw_list, origin, cell_w, cell_h, font_size);
}


