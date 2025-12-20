// ANSI/Unicode canvas component for utf8-art-editor.
//
// The canvas is a fixed-width grid with rows that grow on demand.
// Internally we keep a document with one-or-more layers; compositing
// treats U' ' as transparent

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

#include "core/fonts.h"

// Forward declarations from Dear ImGui
struct ImVec2;
struct ImDrawList;
struct ImGuiInputTextCallbackData;

namespace kb { class KeyBindingsEngine; }

class AnsiCanvas
{
public:
    // 32-bit packed RGBA color (compatible with Dear ImGui's ImU32 / IM_COL32()).
    // Convention in this codebase:
    //  - 0 means "unset" (use theme default for fg, and transparent/no-fill for bg).
    using Color32 = std::uint32_t;

    // Embedded bitmap font support (used by XBin and other binary formats).
    //
    // Some formats (notably XBin) can embed a raw 1bpp bitmap font table where the on-disk
    // character byte is a glyph *index* (0..255 or 0..511), not a Unicode codepoint.
    //
    // To represent this in our Unicode canvas, we map glyph indices into the Private Use Area:
    //   U+E000 + glyph_index
    // and store the font bitmap alongside the canvas so rendering can be faithful.
    static constexpr char32_t kEmbeddedGlyphBase = (char32_t)0xE000;

    struct EmbeddedBitmapFont
    {
        int cell_w = 8;   // XBin fonts are 8 pixels wide
        int cell_h = 16;  // 1..32
        int glyph_count = 256; // 256 or 512
        bool vga_9col_dup = false;

        // Glyph-major, one byte per row, MSB is leftmost pixel.
        // Size must be glyph_count * cell_h.
        std::vector<std::uint8_t> bitmap;
    };

    bool HasEmbeddedFont() const { return m_embedded_font.has_value(); }
    const EmbeddedBitmapFont* GetEmbeddedFont() const { return m_embedded_font ? &(*m_embedded_font) : nullptr; }
    void SetEmbeddedFont(EmbeddedBitmapFont font)
    {
        m_embedded_font = std::move(font);
        TouchContent();
    }
    void ClearEmbeddedFont()
    {
        if (!m_embedded_font)
            return;
        m_embedded_font.reset();
        TouchContent();
    }

    explicit AnsiCanvas(int columns = 80);

    // ---------------------------------------------------------------------
    // Dirty state (savepoint tracking)
    // ---------------------------------------------------------------------
    // Returns true if the canvas content has changed since the last time the app
    // considered it "saved" (or "clean"), e.g. after a successful Save or after
    // loading/importing a file.
    //
    // This is undo/redo-aware: if the user undoes back to the saved state, this
    // becomes false again.
    bool IsModifiedSinceLastSave() const { return m_state_token != m_saved_state_token; }
    // Marks the current canvas state as the "saved/clean" baseline.
    // Call this after successful Save, and after opening/importing a file.
    void MarkSaved() { m_saved_state_token = m_state_token; }

    // ---------------------------------------------------------------------
    // Document identity (UI/session metadata; not part of the editable project state)
    // ---------------------------------------------------------------------
    // This is the user-facing file path associated with this canvas, if any.
    // - Set by IoManager on successful Load/Save (can be a URL for remote imports)
    // - Persisted by session.json so window titles restore meaningfully
    // - NOT included in ProjectState (.phos) and NOT tracked by undo/redo
    void SetFilePath(const std::string& path) { m_file_path = path; }
    void ClearFilePath() { m_file_path.clear(); }
    bool HasFilePath() const { return !m_file_path.empty(); }
    const std::string& GetFilePath() const { return m_file_path; }

    // Set the fixed number of columns in the grid.
    // Rows are dynamic and grow as needed ("infinite rows").
    void SetColumns(int columns);
    // Sets the allocated number of rows in the grid (>= 1).
    // Unlike EnsureRows(), this can also SHRINK (crop) the document.
    void SetRows(int rows);
    int  GetColumns() const { return m_columns; }
    int  GetRows() const { return m_rows; } // allocated rows (>= 1)

    // ---------------------------------------------------------------------
    // Viewport (zoom + scroll) state
    // ---------------------------------------------------------------------
    // Zoom is a multiplicative scale applied to the base font cell metrics.
    // This is independent of the window size (no auto-fit).
    float GetZoom() const { return m_zoom; }
    void  SetZoom(float zoom);

    // Optional: attach a key bindings engine so navigation/edit keys captured for
    // tools/scripts can be resolved via configurable action IDs.
    //
    // If not attached, AnsiCanvas falls back to fixed physical keys (arrows/home/end/etc).
    void SetKeyBindingsEngine(kb::KeyBindingsEngine* engine) { m_keybinds = engine; }

    // Optional: status line visibility (Cols/Rows/Caret + font picker + SAUCE button).
    bool IsStatusLineVisible() const { return m_status_line_visible; }
    void SetStatusLineVisible(bool v) { m_status_line_visible = v; }
    void ToggleStatusLineVisible() { m_status_line_visible = !m_status_line_visible; }

    // Request a scroll position in *canvas pixel space* (child window scroll).
    // Applied on next Render() call.
    void RequestScrollPixels(float scroll_x, float scroll_y);

    struct ViewState
    {
        bool  valid = false;
        int   columns = 0;
        int   rows = 0;
        float zoom = 1.0f;

        // Base metrics from the active ImGui font at Render() time.
        float base_cell_w = 0.0f;
        float base_cell_h = 0.0f;
        float cell_w = 0.0f;
        float cell_h = 0.0f;

        // Full canvas size in pixels (cell_w * columns, cell_h * rows).
        float canvas_w = 0.0f;
        float canvas_h = 0.0f;

        // Visible region in pixels (child InnerClipRect size) and scroll offset.
        float view_w = 0.0f;
        float view_h = 0.0f;
        float scroll_x = 0.0f;
        float scroll_y = 0.0f;
    };

    // Returns the last captured viewport state from Render().
    const ViewState& GetLastViewState() const { return m_last_view; }

    // If enabled, the canvas auto-scrolls to keep the caret visible when navigating/typing.
    // Tools/scripts can still request explicit scroll positions via RequestScrollPixels().
    bool IsFollowCaretEnabled() const { return m_follow_caret; }
    void SetFollowCaretEnabled(bool enabled) { m_follow_caret = enabled; }
    void ToggleFollowCaretEnabled() { m_follow_caret = !m_follow_caret; }

    // Composite cell sampling (used by preview/minimap).
    // Returns false if out of bounds.
    bool GetCompositeCellPublic(int row, int col, char32_t& out_cp, Color32& out_fg, Color32& out_bg) const;

    // ---------------------------------------------------------------------
    // Content revision (for minimaps/caches)
    // ---------------------------------------------------------------------
    // Monotonically increasing counter bumped when visible canvas content changes.
    // Intended for UI caches such as the Preview minimap texture.
    std::uint64_t GetContentRevision() const { return m_content_revision; }

    // ---------------------------------------------------------------------
    // Layers (foundation)
    // ---------------------------------------------------------------------
    int         GetLayerCount() const;
    int         GetActiveLayerIndex() const;
    std::string GetLayerName(int index) const;
    bool        IsLayerVisible(int index) const;
    // If enabled, mutations to this layer cannot change a cell's transparency state
    // (i.e., "alpha lock"). Used for mask-like workflows.
    bool        IsLayerTransparencyLocked(int index) const;
    // Renames a layer. Name may be empty (treated as "(unnamed)" in UI).
    // Captured by undo/redo and project serialization.
    bool        SetLayerName(int index, const std::string& name);

    // Returns the new layer's index, or -1 on failure.
    int  AddLayer(const std::string& name);
    // Fails if attempting to remove the last remaining layer.
    bool RemoveLayer(int index);
    bool SetActiveLayerIndex(int index);
    bool SetLayerVisible(int index, bool visible);
    bool SetLayerTransparencyLocked(int index, bool locked);

    // Reorder layers (changes compositing order / depth).
    // Lower index = further back; higher index = further front (drawn on top).
    bool MoveLayer(int from_index, int to_index);
    bool MoveLayerUp(int index);   // toward front (index + 1)
    bool MoveLayerDown(int index); // toward back  (index - 1)

    // Load content from a UTF-8 text/ANSI file.
    // Current behavior:
    //  - Decode as UTF-8 into Unicode codepoints.
    //  - Treat '\n' as a hard line break; '\r\n' is normalized.
    //  - Control characters (< 0x20) are ignored (except '\n' and '\t').
    //  - Content is written into a fixed-width grid; long lines wrap to next row.
    bool LoadFromFile(const std::string& path);

    // ---------------------------------------------------------------------
    // Undo / Redo (per-canvas)
    // ---------------------------------------------------------------------
    bool CanUndo() const;
    bool CanRedo() const;
    bool Undo();
    bool Redo();
    // Pushes the current document state as an undo step (clears redo).
    // Intended for "undo boundary" actions such as starting script playback.
    void PushUndoSnapshot();

    // ---------------------------------------------------------------------
    // Undo history retention
    // ---------------------------------------------------------------------
    // 0 = unlimited. Changing the limit may trim existing undo/redo stacks.
    size_t GetUndoLimit() const { return m_undo_limit; }
    bool   IsUndoUnlimited() const { return m_undo_limit == 0; }
    void   SetUndoLimit(size_t limit);

    // ---------------------------------------------------------------------
    // Project Save/Load (serialization support)
    // ---------------------------------------------------------------------
    // These structs expose the full editable state of the canvas, including
    // layers and undo/redo history, so IO code can serialize them.
    //
    // Intentionally NOT included (transient UI/input state):
    //  - focus state, mouse cursor state, typed/key queues, render metrics.
    struct ProjectLayer
    {
        std::string           name;
        bool                  visible = true;
        bool                  lock_transparency = false;
        std::vector<char32_t> cells; // size == rows * columns
        std::vector<Color32>  fg;    // per-cell foreground; 0 = unset
        std::vector<Color32>  bg;    // per-cell background; 0 = unset (transparent)
    };

    struct ProjectSnapshot
    {
        int                     columns = 80;
        int                     rows = 1;
        int                     active_layer = 0;
        int                     caret_row = 0;
        int                     caret_col = 0;
        std::vector<ProjectLayer> layers;
    };

    struct ProjectState
    {
        // Project serialization version (bumped when the on-disk schema changes).
        int                     version = 4;

        // Optional: UI colour palette identity (from assets/color-palettes.json).
        // This is a per-canvas preference used by the Colour Picker UI to offer a useful palette
        // when editing/importing artwork. It does NOT affect the stored per-cell colors.
        //
        // Stored as a palette title (string) rather than an index so it remains stable if the
        // palette list is reordered.
        std::string             colour_palette_title;

        // Optional SAUCE metadata associated with this canvas/project.
        // This is persisted in .phos and session state, and may be populated when importing
        // SAUCEd files (e.g. .ans). It is not currently used by the renderer.
        struct SauceMeta
        {
            bool present = false;
            std::string title;
            std::string author;
            std::string group;
            std::string date; // "CCYYMMDD" (raw string, may be empty)

            // Raw SAUCE fields for round-tripping.
            std::uint32_t file_size = 0;
            std::uint8_t  data_type = 1;
            std::uint8_t  file_type = 1;
            std::uint16_t tinfo1 = 0;
            std::uint16_t tinfo2 = 0;
            std::uint16_t tinfo3 = 0;
            std::uint16_t tinfo4 = 0;
            std::uint8_t  tflags = 0;
            std::string   tinfos; // font name (SAUCE TInfoS)
            std::vector<std::string> comments;
        };

        SauceMeta               sauce;

        ProjectSnapshot         current;

        // -----------------------------------------------------------------
        // Undo/Redo history (persisted)
        // -----------------------------------------------------------------
        // We store undo history as either:
        //  - full snapshots (for structural operations / script playback boundaries), or
        //  - delta patches (page-based) for efficient paint-style edits.
        struct ProjectUndoEntry
        {
            enum class Kind
            {
                Snapshot = 0,
                Patch = 1,
            };
            Kind kind = Kind::Snapshot;

            // Snapshot (Kind::Snapshot)
            ProjectSnapshot snapshot;

            // Patch (Kind::Patch)
            struct PatchLayerMeta
            {
                std::string name;
                bool        visible = true;
                bool        lock_transparency = false;
            };
            struct PatchPage
            {
                int layer = 0;
                int page = 0;       // page index in row-page space
                int page_rows = 64; // must match encoder/decoder; stored for forward safety
                int row_count = 0;  // number of rows captured in this page (<= page_rows)
                // Stored per-cell, row-major, length = row_count * columns.
                std::vector<char32_t> cells;
                std::vector<Color32>  fg;
                std::vector<Color32>  bg;
            };
            struct Patch
            {
                int columns = 80;
                int rows = 1;
                int active_layer = 0;
                int caret_row = 0;
                int caret_col = 0;
                std::uint64_t state_token = 1;

                int page_rows = 64;
                std::vector<PatchLayerMeta> layers;
                std::vector<PatchPage> pages;
            } patch;
        };

        std::vector<ProjectUndoEntry> undo;
        std::vector<ProjectUndoEntry> redo;
        // Max number of undo snapshots retained in memory.
        // 0 = unlimited.
        size_t                  undo_limit = 0;
    };

    ProjectState GetProjectState() const;
    // Replaces the entire canvas document + undo/redo history from `state`.
    // Returns false on validation failure and leaves the canvas unchanged.
    bool SetProjectState(const ProjectState& state, std::string& out_error);

    // SAUCE metadata accessors (stored alongside the canvas, persisted via ProjectState).
    const ProjectState::SauceMeta& GetSauceMeta() const { return m_sauce; }
    void SetSauceMeta(const ProjectState::SauceMeta& meta) { m_sauce = meta; }

    // Optional: UI colour palette identity (persisted via ProjectState).
    const std::string& GetColourPaletteTitle() const { return m_colour_palette_title; }
    void SetColourPaletteTitle(const std::string& title) { m_colour_palette_title = title; }
    void ClearColourPaletteTitle() { m_colour_palette_title.clear(); }

    // ---------------------------------------------------------------------
    // Canvas font selection (persisted via SAUCE TInfoS)
    // ---------------------------------------------------------------------
    // Note: this controls how the canvas grid is rendered and rasterized (export image/minimap).
    // The UI font remains the ImGui font (currently Unscii).
    fonts::FontId GetFontId() const;
    bool          SetFontId(fonts::FontId id);

    // UI hook: raised when the canvas status bar "Edit SAUCE…" button is clicked.
    // This allows UI code (in src/ui) to show a dialog without introducing a core->ui dependency.
    bool TakeOpenSauceEditorRequest()
    {
        const bool v = m_request_open_sauce_editor;
        m_request_open_sauce_editor = false;
        return v;
    }

    // Render the canvas inside the current ImGui window.
    // `id` must be unique within the window (used for ImGui item id).
    void Render(const char* id);
    // `tool_runner` is called by the canvas during Render() to run the active tool script.
    // The canvas will call it twice per frame:
    //  - phase=0 (keyboard): after collecting typed+key events, before computing canvas size
    //    (so row growth affects scroll range immediately).
    //  - phase=1 (mouse): after the canvas InvisibleButton updates cursor state for this frame.
    void Render(const char* id, const std::function<void(AnsiCanvas& canvas, int phase)>& tool_runner = {});

    // ---------------------------------------------------------------------
    // Canvas background (view preference; independent of ImGui theme)
    // ---------------------------------------------------------------------
    bool IsCanvasBackgroundWhite() const { return m_canvas_bg_white; }
    void SetCanvasBackgroundWhite(bool white)
    {
        if (m_canvas_bg_white == white)
            return;
        m_canvas_bg_white = white;
        TouchContent();
    }
    void ToggleCanvasBackgroundWhite()
    {
        m_canvas_bg_white = !m_canvas_bg_white;
        TouchContent();
    }

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
    // Clears fg/bg style for a cell (sets to 0/unset). Returns false if layer_index invalid.
    bool     ClearLayerCellStyle(int layer_index, int row, int col);

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
    void SetCaretCell(int x, int y);
    bool HasFocus() const { return m_has_focus; }
    // Forcefully clears focus (used by the app to ensure focus is exclusive across canvases).
    void ClearFocus()
    {
        m_has_focus = false;
        m_mouse_capture = false;
        m_cursor_valid = false;
        m_focus_gained = false;
    }
    // Returns true exactly once when this canvas gains focus via a click inside the grid.
    bool TakeFocusGained()
    {
        const bool v = m_focus_gained;
        m_focus_gained = false;
        return v;
    }

    // ---------------------------------------------------------------------
    // Input events captured during Render() for tools/scripts to consume.
    // ---------------------------------------------------------------------
    struct KeyEvents
    {
        bool left = false;
        bool right = false;
        bool up = false;
        bool down = false;
        bool home = false;
        bool end = false;
        bool backspace = false;
        bool del = false;
        bool enter = false;

        // Common editing/selection shortcuts (captured as discrete presses).
        bool c = false;
        bool v = false;
        bool x = false;
        bool a = false;
        bool escape = false;
    };

    // Moves queued typed codepoints into `out` (clearing the internal queue).
    void TakeTypedCodepoints(std::vector<char32_t>& out);
    // Returns and clears the last captured key events.
    KeyEvents TakeKeyEvents();

    // Cursor = the mouse cursor expressed in cell space (x=col, y=row) plus button state.
    // If the canvas isn't hovered/active, returns false.
    bool GetCursorCell(int& out_x,
                       int& out_y,
                       int& out_half_y,
                       bool& out_left_down,
                       bool& out_right_down,
                       int& out_px,
                       int& out_py,
                       int& out_phalf_y,
                       bool& out_prev_left_down,
                       bool& out_prev_right_down) const;

    // Latest rendered cell aspect ratio (cell_w / cell_h). Defaults to 1.
    float GetLastCellAspect() const { return m_last_cell_aspect; }

    // ---------------------------------------------------------------------
    // Selection + clipboard (for tools; clipboard is global across canvases)
    // ---------------------------------------------------------------------
    struct Rect
    {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
    };

    // Selection rectangle is stored in cell space. Corners are inclusive.
    bool HasSelection() const { return m_selection.active && m_selection.w > 0 && m_selection.h > 0; }
    Rect GetSelectionRect() const;
    void SetSelectionCorners(int x0, int y0, int x1, int y1);
    void ClearSelection();
    bool SelectionContains(int x, int y) const;

    // Clipboard is shared across all canvases (copy/paste between canvases).
    static bool ClipboardHas();
    static Rect ClipboardRect(); // returns {0,0,w,h} (w/h may be 0)

    // Copy/cut/delete operate on the current selection rect in the given layer.
    // If layer_index < 0, uses the active layer.
    bool CopySelectionToClipboard(int layer_index = -1);
    bool CutSelectionToClipboard(int layer_index = -1);
    bool DeleteSelection(int layer_index = -1);

    // Paste the global clipboard into the given layer at (x,y).
    // If layer_index < 0, uses the active layer.
    enum class PasteMode
    {
        Both = 0,     // overwrite glyph + fg + bg
        CharOnly = 1, // overwrite glyph only
        ColorOnly = 2 // overwrite fg + bg only (glyph preserved)
    };

    // If transparent_spaces is true, space glyphs in the clipboard do not apply (no-op for that cell).
    // Returns false if clipboard empty or paste target invalid.
    bool PasteClipboard(int x,
                        int y,
                        int layer_index = -1,
                        PasteMode mode = PasteMode::Both,
                        bool transparent_spaces = false);

    // Copy mode: either the active layer's cells, or the composited "what you see" result.
    bool CopySelectionToClipboardComposite();

    // Interactive move/duplicate of the current selection (floating selection preview).
    bool IsMovingSelection() const { return m_move.active; }
    bool BeginMoveSelection(int grab_x, int grab_y, bool copy, int layer_index = -1);
    void UpdateMoveSelection(int cursor_x, int cursor_y);
    bool CommitMoveSelection(int layer_index = -1);
    bool CancelMoveSelection(int layer_index = -1);

private:
    struct Layer
    {
        std::string           name;
        bool                  visible = true;
        bool                  lock_transparency = false;
        std::vector<char32_t> cells; // size == rows * columns
        std::vector<Color32>  fg;    // per-cell foreground; 0 = unset
        std::vector<Color32>  bg;    // per-cell background; 0 = unset (transparent)
    };

    struct Snapshot
    {
        int                columns = 80;
        int                rows = 1;
        int                active_layer = 0;
        int                caret_row = 0;
        int                caret_col = 0;
        std::vector<Layer> layers;

        // Monotonic state identity token used for savepoint/dirty tracking.
        // Not serialized into project files; only used at runtime.
        std::uint64_t      state_token = 1;
    };

    // Undo entry: either a full snapshot (structural ops / script boundaries), or
    // a delta patch (row-page capture) for efficient paint-style edits.
    struct UndoEntry
    {
        enum class Kind
        {
            Snapshot = 0,
            Patch = 1,
        };
        Kind kind = Kind::Snapshot;

        // Snapshot (Kind::Snapshot)
        Snapshot snapshot;

        // Patch (Kind::Patch)
        struct PatchLayerMeta
        {
            std::string name;
            bool        visible = true;
            bool        lock_transparency = false;
        };
        struct PatchPage
        {
            int layer = 0;
            int page = 0;
            int page_rows = 64;
            int row_count = 0; // <= page_rows
            std::vector<char32_t> cells;
            std::vector<Color32>  fg;
            std::vector<Color32>  bg;
        };
        struct Patch
        {
            int columns = 80;
            int rows = 1;
            int active_layer = 0;
            int caret_row = 0;
            int caret_col = 0;
            std::uint64_t state_token = 1;

            int page_rows = 64;
            std::vector<PatchLayerMeta> layers;
            std::vector<PatchPage> pages;
        } patch;
    };

    int m_columns = 80;
    int m_rows    = 1; // allocated rows (always >= 1)

    // User-facing document path (see SetFilePath()).
    std::string m_file_path;

    std::vector<Layer> m_layers;
    int                m_active_layer = 0;

    // Caret position (row/col) in grid space (keyboard/editing caret).
    int  m_caret_row = 0;
    int  m_caret_col = 0;

    // Whether this canvas currently has keyboard focus.
    bool m_has_focus = false;
    // Transient: set during Render() when focus becomes true due to a click in the grid.
    bool m_focus_gained = false;
    // Last known mouse cursor state in cell space (updated during Render()).
    bool m_cursor_valid = false;
    int  m_cursor_col = 0;
    int  m_cursor_row = 0;
    int  m_cursor_half_row = 0;  // y in "half rows" (row*2 + 0/1)
    bool m_cursor_left_down = false;
    bool m_cursor_right_down = false;
    int  m_cursor_pcol = 0;
    int  m_cursor_prow = 0;
    int  m_cursor_phalf_row = 0;
    bool m_cursor_prev_left_down = false;
    bool m_cursor_prev_right_down = false;

    float m_last_cell_aspect = 1.0f;

    // Zoom and last captured viewport metrics.
    float    m_zoom = 1.0f;
    ViewState m_last_view;
    bool     m_follow_caret = true;
    // Zoom stabilization: keep certain layout decisions stable for a few frames after zoom changes
    // to avoid scrollbar/clip-rect churn (visible as flicker/jitter).
    int m_zoom_stabilize_frames = 0;

    // Canvas base background fill (not theme-driven).
    bool m_canvas_bg_white = false;

    // Monotonic content revision for caches (minimap texture).
    std::uint64_t m_content_revision = 1;

    // Monotonic document-state token for dirty tracking (undo/redo aware).
    // Incremented on any content mutation and restored on Undo/Redo.
    std::uint64_t m_state_token = 1;
    // The state token corresponding to the last saved/clean baseline.
    std::uint64_t m_saved_state_token = 1;

    // Optional SAUCE metadata associated with this canvas (persisted).
    ProjectState::SauceMeta m_sauce;

    // Optional UI colour palette title (persisted via ProjectState).
    std::string m_colour_palette_title;

    // Optional embedded bitmap font (not currently serialized into .phos; supplied by some importers like XBin).
    std::optional<EmbeddedBitmapFont> m_embedded_font;

    bool m_request_open_sauce_editor = false;

    // Deferred scroll request (applied during next Render() when child is active).
    bool  m_scroll_request_valid = false;
    float m_scroll_request_x = 0.0f;
    float m_scroll_request_y = 0.0f;

    // Mouse capture independent of ImGui ActiveId: once the user clicks on the canvas,
    // we keep updating cursor cell coords while the button is held (enables click+drag tools).
    bool m_mouse_capture = false;

    // Input captured from ImGui:
    std::vector<char32_t> m_typed_queue;
    KeyEvents             m_key_events;
    kb::KeyBindingsEngine* m_keybinds = nullptr; // not owned

    // UI visibility toggles (canvas-local).
    bool m_status_line_visible = true;

    // Selection state (per-canvas, transient; not serialized).
    struct SelectionState
    {
        bool active = false;
        int  x = 0;
        int  y = 0;
        int  w = 0;
        int  h = 0;
    };
    SelectionState m_selection;

    struct ClipCell
    {
        char32_t cp = U' ';
        Color32  fg = 0;
        Color32  bg = 0;
    };

    // Floating selection state for interactive move/copy preview (per-canvas, transient).
    struct MoveState
    {
        bool active = false;
        bool cut = false; // true if we cleared the source region (move); false if copy/duplicate

        int src_x = 0;
        int src_y = 0;
        int w = 0;
        int h = 0;

        int dst_x = 0;
        int dst_y = 0;

        int grab_dx = 0; // cursor - dst_x
        int grab_dy = 0;

        std::vector<ClipCell> cells; // size w*h
    };
    MoveState m_move;

    // Status-line edit buffers (so inline numeric InputText can be edited across frames).
    char m_status_cols_buf[16] = {};
    char m_status_rows_buf[16] = {};
    char m_status_caret_x_buf[16] = {};
    char m_status_caret_y_buf[16] = {};
    bool m_status_bar_editing = false;

    // Undo/Redo stacks. Each entry is a full document snapshot.
    std::vector<UndoEntry> m_undo_stack;
    std::vector<UndoEntry> m_redo_stack;
    size_t                m_undo_limit = 0; // 0 = unlimited

    // "Capture scope" used to group multiple mutations into a single undo step.
    bool     m_undo_capture_active = false;
    bool     m_undo_capture_modified = false;
    bool     m_undo_capture_has_entry = false;
    bool     m_undo_applying_snapshot = false;
    std::optional<UndoEntry> m_undo_capture_entry;
    std::unordered_map<std::uint64_t, size_t> m_undo_capture_page_index; // key=(layer<<32)|page -> pages[] index

    // Internal helpers
    void TouchContent()
    {
        // Avoid wrap to 0 (treat 0 as "uninitialized" in some callers).
        ++m_content_revision;
        if (m_content_revision == 0)
            ++m_content_revision;
    }
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
    void          ClearLayerCellStyleInternal(int layer_index, int row, int col);

    // Undo helpers
    Snapshot MakeSnapshot() const;
    void     ApplySnapshot(const Snapshot& s);
    void     BeginUndoCapture();
    void     EndUndoCapture();
    void     PrepareUndoForMutation();
    void     EnsureUndoCaptureIsPatch();
    void     EnsureUndoCaptureIsSnapshot();
    void     CaptureUndoPageIfNeeded(int layer_index, int row);

    void HandleCharInputWidget(const char* id);
    static int TextInputCallback(ImGuiInputTextCallbackData* data);
    void CaptureKeyEvents();
    void HandleMouseInteraction(const ImVec2& origin, float cell_w, float cell_h);
    void DrawVisibleCells(ImDrawList* draw_list,
                          const ImVec2& origin,
                          float cell_w,
                          float cell_h,
                          float font_size);
    void DrawSelectionOverlay(ImDrawList* draw_list,
                              const ImVec2& origin,
                              float cell_w,
                              float cell_h,
                              float font_size);
};


