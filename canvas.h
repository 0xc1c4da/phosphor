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

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Forward declarations from Dear ImGui
struct ImVec2;
struct ImDrawList;
struct ImGuiInputTextCallbackData;

class AnsiCanvas
{
public:
    // 32-bit packed RGBA color (compatible with Dear ImGui's ImU32 / IM_COL32()).
    // Convention in this codebase:
    //  - 0 means "unset" (use theme default for fg, and transparent/no-fill for bg).
    using Color32 = std::uint32_t;

    explicit AnsiCanvas(int columns = 80);

    // Set the fixed number of columns in the grid.
    // Rows are dynamic and grow as needed ("infinite rows").
    void SetColumns(int columns);
    int  GetColumns() const { return m_columns; }
    int  GetRows() const { return m_rows; } // allocated rows (>= 1)

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

    // ---------------------------------------------------------------------
    // Public layer editing API (used by tools/scripts)
    // ---------------------------------------------------------------------
    // Ensures at least `rows_needed` rows are allocated.
    void EnsureRowsPublic(int rows_needed) { EnsureRows(rows_needed); }

    // Get/set a cell in a specific layer. `row` may extend the document (rows grow on demand).
    // Returns false if `layer_index` is invalid.
    bool     SetLayerCell(int layer_index, int row, int col, char32_t cp);
    // Sets glyph + optional foreground/background colors for the cell.
    // Pass 0 for fg/bg to leave them "unset" (default fg / transparent bg).
    bool     SetLayerCell(int layer_index, int row, int col, char32_t cp, Color32 fg, Color32 bg);
    char32_t GetLayerCell(int layer_index, int row, int col) const;
    // Returns false if `layer_index` is invalid or out of bounds.
    bool     GetLayerCellColors(int layer_index, int row, int col, Color32& out_fg, Color32& out_bg) const;

    // Fill an entire layer with `cp` (default: space).
    // Returns false if `layer_index` is invalid.
    bool ClearLayer(int layer_index, char32_t cp = U' ');

    // General-purpose layer fill helper.
    // Any field set to std::nullopt is left unchanged.
    // Note: `Color32` value 0 still means "unset" (theme default / transparent bg).
    bool FillLayer(int layer_index,
                   std::optional<char32_t> cp,
                   std::optional<Color32> fg,
                   std::optional<Color32> bg);

    // ---------------------------------------------------------------------
    // Pointer state (for tools/scripts)
    // ---------------------------------------------------------------------
    // Caret = the editing caret used by keyboard operations (x=col, y=row).
    void GetCaretCell(int& out_x, int& out_y) const { out_x = m_caret_col; out_y = m_caret_row; }

    // Cursor = the mouse cursor expressed in cell space (x=col, y=row) plus button state.
    // If the canvas isn't hovered/active, returns false.
    bool GetCursorCell(int& out_x,
                       int& out_y,
                       bool& out_left_down,
                       bool& out_right_down,
                       int& out_px,
                       int& out_py,
                       bool& out_prev_left_down,
                       bool& out_prev_right_down) const;

    // Latest rendered cell aspect ratio (cell_w / cell_h). Defaults to 1.
    float GetLastCellAspect() const { return m_last_cell_aspect; }

private:
    struct Layer
    {
        std::string           name;
        bool                  visible = true;
        std::vector<char32_t> cells; // size == rows * columns
        std::vector<Color32>  fg;    // per-cell foreground; 0 = unset
        std::vector<Color32>  bg;    // per-cell background; 0 = unset (transparent)
    };

    int m_columns = 80;
    int m_rows    = 1; // allocated rows (always >= 1)

    std::vector<Layer> m_layers;
    int                m_active_layer = 0;

    // Caret position (row/col) in grid space (keyboard/editing caret).
    int  m_caret_row = 0;
    int  m_caret_col = 0;

    // Whether this canvas currently has keyboard focus.
    bool m_has_focus = false;

    // Last known mouse cursor state in cell space (updated during Render()).
    bool m_cursor_valid = false;
    int  m_cursor_col = 0;
    int  m_cursor_row = 0;
    bool m_cursor_left_down = false;
    bool m_cursor_right_down = false;
    int  m_cursor_pcol = 0;
    int  m_cursor_prow = 0;
    bool m_cursor_prev_left_down = false;
    bool m_cursor_prev_right_down = false;

    float m_last_cell_aspect = 1.0f;

    // Internal helpers
    void EnsureDocument();
    void EnsureRows(int rows_needed);
    size_t CellIndex(int row, int col) const;

    struct CompositeCell
    {
        char32_t cp = U' ';
        Color32  fg = 0;
        Color32  bg = 0;
    };

    CompositeCell GetCompositeCell(int row, int col) const;
    void          SetActiveCell(int row, int col, char32_t cp);
    void          SetActiveCell(int row, int col, char32_t cp, Color32 fg, Color32 bg);
    void          ClearActiveCellStyle(int row, int col);

    void HandleKeyboardNavigation();
    void HandleTextInput();
    void HandleCharInputWidget(const char* id);
    void ApplyTypedCodepoint(char32_t cp);
    static int TextInputCallback(ImGuiInputTextCallbackData* data);
    void HandleMouseInteraction(const ImVec2& origin, float cell_w, float cell_h);
    void DrawVisibleCells(ImDrawList* draw_list,
                          const ImVec2& origin,
                          float cell_w,
                          float cell_h,
                          float font_size);
};


