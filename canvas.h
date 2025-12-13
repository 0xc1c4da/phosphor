// Simple ANSI/Unicode canvas component for utf8-art-editor.
//
// IMPORTANT DESIGN NOTE
// The previous implementation stored content as a 1D stream of codepoints and
// "wrapped" it by column count. That makes it hard to:
//   - treat the canvas as a traditional fixed-width grid
//   - support infinite rows (allocated on demand)
//   - add layers (which require stable row/col addressing)
//   - implement editing operations (overwrite, backspace, line breaks, etc.)
//
// The canvas is now a fixed-width grid with rows that grow on demand.
// Internally we keep a document with one-or-more layers; compositing currently
// treats U' ' as transparent so future layers can be added without rewriting UI.

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

    // Set the fixed number of columns in the grid.
    // Rows are dynamic and grow as needed ("infinite rows").
    void SetColumns(int columns);
    int  GetColumns() const { return m_columns; }

    // ---------------------------------------------------------------------
    // Layers (foundation)
    // ---------------------------------------------------------------------
    int         GetLayerCount() const;
    int         GetActiveLayerIndex() const;
    std::string GetLayerName(int index) const;
    bool        IsLayerVisible(int index) const;

    // Returns the new layer's index, or -1 on failure.
    int  AddLayer(const std::string& name);
    // Fails if attempting to remove the last remaining layer.
    bool RemoveLayer(int index);
    bool SetActiveLayerIndex(int index);
    bool SetLayerVisible(int index, bool visible);

    // Load content from a UTF-8 text/ANSI file.
    // Current behavior:
    //  - Decode as UTF-8 into Unicode codepoints.
    //  - Treat '\n' as a hard line break; '\r\n' is normalized.
    //  - Control characters (< 0x20) are ignored (except '\n' and '\t').
    //  - Content is written into a fixed-width grid; long lines wrap to next row.
    bool LoadFromFile(const std::string& path);

    // Render the canvas inside the current ImGui window.
    // `id` must be unique within the window (used for ImGui item id).
    void Render(const char* id);

private:
    struct Layer
    {
        std::string           name;
        bool                  visible = true;
        std::vector<char32_t> cells; // size == rows * columns
    };

    int m_columns = 80;
    int m_rows    = 1; // allocated rows (always >= 1)

    std::vector<Layer> m_layers;
    int                m_active_layer = 0;

    // Cursor position (row/col) in grid space.
    int  m_cursor_row = 0;
    int  m_cursor_col = 0;

    // Whether this canvas currently has keyboard focus.
    bool m_has_focus = false;

    // Internal helpers
    void EnsureDocument();
    void EnsureRows(int rows_needed);
    size_t CellIndex(int row, int col) const;

    char32_t GetCompositeCell(int row, int col) const;
    void     SetActiveCell(int row, int col, char32_t cp);

    void HandleKeyboardNavigation();
    void HandleTextInput();
    void HandleMouseInteraction(const ImVec2& origin, float cell_w, float cell_h);
    void DrawVisibleCells(ImDrawList* draw_list,
                          const ImVec2& origin,
                          float cell_w,
                          float cell_h,
                          float font_size);
};


