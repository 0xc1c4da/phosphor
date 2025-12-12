// Simple ANSI/Unicode canvas component for utf8-art-editor.
// Renders a 2D grid of cells backed by a 1D stream of UTF-8 characters.
// - Default 80 columns (configurable)
// - Effectively unbounded rows (derived from content length)
// - Keyboard + mouse navigation of a single cursor cell.

#pragma once

#include <string>
#include <vector>

// Forward declarations from Dear ImGui
struct ImVec2;
struct ImDrawList;

class AnsiCanvas
{
public:
    explicit AnsiCanvas(int columns = 80);

    // Set the number of columns used for wrapping the 1D cell stream.
    void SetColumns(int columns);
    int  GetColumns() const { return m_columns; }

    // Load content from a UTF-8 text/ANSI file.
    // For now we:
    //  - Read the file as UTF-8
    //  - Ignore line breaks and control characters
    //  - Store each Unicode codepoint as a separate cell, in order
    bool LoadFromFile(const std::string& path);

    // Render the canvas inside the current ImGui window.
    // `id` must be unique within the window (used for ImGui item id).
    void Render(const char* id);

private:
    int m_columns = 80;

    // Linear list of Unicode codepoints (one per cell).
    std::u32string m_cells;

    // Index of the "cursor" cell within m_cells.
    int  m_cursor_index = 0;

    // Whether this canvas currently has keyboard focus.
    bool m_has_focus = false;

    // Internal helpers
    int  GetRowCount() const;
    void HandleKeyboardNavigation();
    void HandleMouseInteraction(const ImVec2& origin, float cell_w, float cell_h);
    void DrawCells(ImDrawList* draw_list, const ImVec2& origin, float cell_w, float cell_h);
};


