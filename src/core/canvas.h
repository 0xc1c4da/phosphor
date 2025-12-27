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

#include "core/colour_index.h"
#include "core/fonts.h"
#include "core/glyph_id.h"
#include "core/glyph_legacy.h"
#include "core/layer_blend_mode.h"
#include "core/palette/palette.h"

// Forward declarations from Dear ImGui
struct ImVec2;
struct ImDrawList;
struct ImGuiInputTextCallbackData;

namespace kb { class KeyBindingsEngine; }

class AnsiCanvas
{
public:
    // 32-bit packed RGBA colour (compatible with Dear ImGui's ImU32 / IM_COL32()).
    // Convention in this codebase:
    //  - 0 means "unset" (use theme default for fg, and transparent/no-fill for bg).
    using Colour32 = std::uint32_t;

    // Glyph stored in canvas cell planes (opaque token; may represent Unicode or indexed glyphs).
    using GlyphId = phos::GlyphId;

    // Indexed-colour storage (Phase B): colours are stored as palette indices in the canvas's active palette.
    // Unset is represented by phos::colour::kUnsetIndex.
    using ColourIndex16 = std::uint16_t;
    static constexpr ColourIndex16 kUnsetIndex16 = phos::colour::kUnsetIndex;

    // Per-cell attribute bitmask (stored alongside glyph + fg/bg).
    // These correspond to ANSI SGR effects (m-codes).
    using Attrs = std::uint16_t;
    enum Attr : Attrs
    {
        Attr_None          = 0,
        Attr_Bold          = 1u << 0, // SGR 1  (reset: 22)
        Attr_Dim           = 1u << 1, // SGR 2  (reset: 22)
        Attr_Italic        = 1u << 2, // SGR 3  (reset: 23)
        Attr_Underline     = 1u << 3, // SGR 4  (reset: 24)
        Attr_Blink         = 1u << 4, // SGR 5  (reset: 25)
        Attr_Reverse       = 1u << 5, // SGR 7  (reset: 27)
        Attr_Strikethrough = 1u << 6, // SGR 9  (reset: 29)
    };

    // Project-level policy for interpreting the Attr_Bold bit.
    //
    // - AnsiBright: classic ANSI art / libansilove convention: SGR 1 selects bright foreground (ANSI16 intensity).
    //               No typographic bold should be applied to bitmap fonts.
    // - Typographic: modern terminal convention: SGR 1 is a typographic effect (heavier glyph), independent of colours.
    enum class BoldSemantics : std::uint8_t
    {
        AnsiBright = 0,
        Typographic = 1,
    };

    enum class ZoomSnapMode : std::uint8_t
    {
        IntegerScale = 1, // always snap to integer N×
        PixelAligned = 2, // always snap based on pixel-aligned cell width
    };

    // Embedded bitmap font support (used by XBin and other binary formats).
    //
    // Some formats (notably XBin) can embed a raw 1bpp bitmap font table where the on-disk
    // character byte is a glyph *index* (0..255 or 0..511), not a Unicode codepoint.
    //
    // Legacy compatibility note:
    // Historically, embedded glyph indices were encoded into the Unicode Private Use Area:
    //   U+E000 + glyph_index
    // This is still accepted as an input/interop representation in a few places, but internal
    // storage is now `GlyphId` tokens (see core/glyph_id.h).
    static constexpr char32_t kEmbeddedGlyphBase = phos::glyph::kLegacyEmbeddedPuaBase;

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
    // Bitmap font glyph atlas (render optimization / pixel accuracy)
    // ---------------------------------------------------------------------
    // Bitmap 1bpp fonts (CP437 + embedded XBin fonts) can be rendered either by:
    //  - drawing per-row rectangles (CPU-heavy and can produce subpixel edges when scaled), or
    //  - sampling a prebuilt glyph atlas texture (preferred: crisp + fast).
    //
    // The atlas texture is renderer-backend specific (Vulkan/DX/etc), so the core canvas
    // exposes an optional provider interface that the app layer can implement and attach.
    struct BitmapGlyphAtlasView
    {
        // Backend texture handle (e.g. ImGui Vulkan descriptor set). Treated as opaque.
        void* texture_id = nullptr;

        // Atlas pixel dimensions.
        int atlas_w = 0;
        int atlas_h = 0;

        // Glyph cell metrics (in atlas pixel space, excluding padding).
        int cell_w = 0;
        int cell_h = 0;

        // Per-glyph tile metrics in the atlas (includes padding).
        // Tile size is typically (cell_w + pad*2) x (cell_h + pad*2).
        int pad = 0;
        int tile_w = 0;
        int tile_h = 0;

        // Grid layout for glyphs in the atlas.
        int cols = 0;        // glyphs per row
        int rows = 0;        // glyph rows per variant
        int glyph_count = 0; // 256 or 512

        // Variant packing: 1 = normal only, 4 = normal/bold/italic/bolditalic stacked vertically.
        int variant_count = 1;
    };

    class IBitmapGlyphAtlasProvider
    {
    public:
        virtual ~IBitmapGlyphAtlasProvider() = default;
        // Returns true and fills `out` if an atlas is available for the canvas's current font.
        // Implementations may cache and lazily build/upload textures.
        virtual bool GetBitmapGlyphAtlas(const AnsiCanvas& canvas, BitmapGlyphAtlasView& out) = 0;
    };

    void SetBitmapGlyphAtlasProvider(IBitmapGlyphAtlasProvider* provider) { m_bitmap_atlas_provider = provider; }
    IBitmapGlyphAtlasProvider* GetBitmapGlyphAtlasProvider() const { return m_bitmap_atlas_provider; }

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

    // ---------------------------------------------------------------------
    // Active glyph selection (UI/session metadata; not part of the editable project state)
    // ---------------------------------------------------------------------
    // This is the "current brush glyph" associated with this canvas window.
    // - Used by tools (ctx.glyph / ctx.glyphCp)
    // - Persisted by session.json (per open canvas)
    // - NOT included in ProjectState (.phos) and NOT tracked by undo/redo
    //
    // Note: `active_glyph_utf8` may be empty (hosts can derive UTF-8 from glyph/cp representative).
    GlyphId GetActiveGlyph() const { return m_active_glyph; }
    // Best-effort Unicode representative for the active glyph (for legacy/UI convenience).
    std::uint32_t GetActiveGlyphCodePoint() const
    {
        if (phos::glyph::IsUnicodeScalar(m_active_glyph))
            return (std::uint32_t)phos::glyph::ToUnicodeScalar(m_active_glyph);
        const phos::glyph::Kind k = phos::glyph::GetKind(m_active_glyph);
        if (k == phos::glyph::Kind::BitmapIndex)
            return (std::uint32_t)fonts::Cp437ByteToUnicode((std::uint8_t)phos::glyph::BitmapIndexValue(m_active_glyph));
        if (k == phos::glyph::Kind::EmbeddedIndex)
            return (std::uint32_t)fonts::Cp437ByteToUnicode((std::uint8_t)(phos::glyph::EmbeddedIndexValue(m_active_glyph) & 0xFFu));
        return (std::uint32_t)U'?';
    }
    const std::string& GetActiveGlyphUtf8() const { return m_active_glyph_utf8; }
    void SetActiveGlyph(GlyphId glyph, std::string utf8)
    {
        m_active_glyph = glyph;
        m_active_glyph_utf8 = std::move(utf8);
    }
    // Legacy convenience: set a Unicode-scalar active glyph (note: NOT token-aware).
    void SetActiveGlyphCodePoint(std::uint32_t cp, std::string utf8)
    {
        SetActiveGlyph(phos::glyph::MakeUnicodeScalar((char32_t)cp), std::move(utf8));
    }

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

    ZoomSnapMode GetZoomSnapMode() const { return m_zoom_snap_mode; }
    void SetZoomSnapMode(ZoomSnapMode m) { m_zoom_snap_mode = m; }

    // Returns the snapped render scale for a candidate zoom value, based on the configured snap mode.
    // `base_cell_w_px` should be the pre-snap base cell width used by the renderer (in pixels).
    // If <= 0, a safe fallback will be used.
    float SnappedScaleForZoom(float zoom, float base_cell_w_px) const;

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
    bool GetCompositeCellPublic(int row, int col, char32_t& out_cp, Colour32& out_fg, Colour32& out_bg) const;
    // Extended composite sampling including attributes.
    bool GetCompositeCellPublic(int row, int col, char32_t& out_cp, Colour32& out_fg, Colour32& out_bg, Attrs& out_attrs) const;
    // Index-native composite sampling (Phase B): returns palette indices in the canvas's active palette.
    // Unset is returned as kUnsetIndex16 (preserves fg/bg unset semantics).
    bool GetCompositeCellPublicIndices(int row, int col, char32_t& out_cp, ColourIndex16& out_fg, ColourIndex16& out_bg) const;
    bool GetCompositeCellPublicIndices(int row, int col, char32_t& out_cp, ColourIndex16& out_fg, ColourIndex16& out_bg, Attrs& out_attrs) const;

    // GlyphId-native composite sampling (Option B): returns the stored glyph token losslessly.
    // `out_cp` style representatives remain available via the existing APIs above.
    bool GetCompositeCellPublicGlyphIndices(int row, int col, GlyphId& out_glyph, ColourIndex16& out_fg, ColourIndex16& out_bg) const;
    bool GetCompositeCellPublicGlyphIndices(int row, int col, GlyphId& out_glyph, ColourIndex16& out_fg, ColourIndex16& out_bg, Attrs& out_attrs) const;

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

    // Phase D groundwork: per-layer background blend mode.
    // v1 affects background compositing only (glyph/fg/attrs unchanged).
    phos::LayerBlendMode GetLayerBlendMode(int index) const;
    bool                 SetLayerBlendMode(int index, phos::LayerBlendMode mode);

    // Phase D: per-layer blend opacity/strength (0..255). 255 = full effect, 0 = no contribution.
    std::uint8_t GetLayerBlendAlpha(int index) const;
    bool         SetLayerBlendAlpha(int index, std::uint8_t alpha);

    // Reorder layers (changes compositing order / depth).
    // Lower index = further back; higher index = further front (drawn on top).
    bool MoveLayer(int from_index, int to_index);
    bool MoveLayerUp(int index);   // toward front (index + 1)
    bool MoveLayerDown(int index); // toward back  (index - 1)

    // ---------------------------------------------------------------------
    // Layer transforms (translation offsets)
    // ---------------------------------------------------------------------
    // Offsets are in canvas cell units and are part of the editable project state:
    // - persisted in ProjectState
    // - captured by undo/redo (patch meta)
    //
    // If layer_index < 0, uses the active layer.
    bool GetLayerOffset(int layer_index, int& out_x, int& out_y) const;
    bool SetLayerOffset(int x, int y, int layer_index = -1);
    bool NudgeLayerOffset(int dx, int dy, int layer_index = -1);

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
        phos::LayerBlendMode  blend_mode = phos::LayerBlendMode::Normal;
        std::uint8_t          blend_alpha = 255;
        // Layer translation in canvas cell-space.
        // Canvas-space (C) and layer-local (L) relate by: C = L + offset.
        int                   offset_x = 0;
        int                   offset_y = 0;
        // Stored glyph plane (lossless token; may be Unicode or indexed glyph id).
        // Serialized as uint32 values in .phos.
        std::vector<GlyphId>  cells; // size == rows * columns
        std::vector<ColourIndex16> fg; // per-cell foreground index; kUnsetIndex16 = unset
        std::vector<ColourIndex16> bg; // per-cell background index; kUnsetIndex16 = unset (transparent)
        std::vector<Attrs>    attrs; // per-cell attribute bitmask; 0 = none
    };

    struct ProjectSnapshot
    {
        int                     columns = 80;
        int                     rows = 1;
        int                     active_layer = 0;
        int                     caret_row = 0;
        int                     caret_col = 0;
        // Core palette identity for interpreting fg/bg indices.
        phos::colour::PaletteRef palette_ref;
        // UI palette selection identity (used by Colour Picker/palette UI; does not affect stored indices).
        // Stable identity: builtin enum or dynamic palette UID.
        phos::colour::PaletteRef ui_palette_ref;
        std::vector<ProjectLayer> layers;
    };

    struct ProjectState
    {
        // Project serialization version (bumped when the on-disk schema changes).
        int                     version = 14;

        // Persisted bold policy:
        // 0 = AnsiBright, 1 = Typographic.
        // (Stored as int for schema stability across compilation units.)
        int                     bold_semantics = 0;

        // Optional embedded bitmap font payload (used by formats like XBin).
        // When present, EmbeddedIndex GlyphIds are meaningful and can be rendered/exported losslessly.
        std::optional<EmbeddedBitmapFont> embedded_font;

        // Core palette identity for this canvas (LUT/index-centric pipeline).
        // Default is xterm256 to preserve current behavior.
        phos::colour::PaletteRef palette_ref = []{
            phos::colour::PaletteRef r;
            r.is_builtin = true;
            r.builtin = phos::colour::BuiltinPalette::Xterm256;
            return r;
        }();

        // Optional: UI colour palette identity (from assets/colour-palettes.json).
        // This is a per-canvas preference used by the Colour Picker UI to select a discrete palette
        // for browsing and tool constraints. It does NOT affect the stored per-cell colours.
        //
        // Stored as a stable palette ref (builtin enum or dynamic palette uid).
        // Default: follow `palette_ref`.
        phos::colour::PaletteRef ui_palette_ref = []{
            phos::colour::PaletteRef r;
            r.is_builtin = true;
            r.builtin = phos::colour::BuiltinPalette::Xterm256;
            return r;
        }();

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
                phos::LayerBlendMode blend_mode = phos::LayerBlendMode::Normal;
                std::uint8_t blend_alpha = 255;
                int         offset_x = 0;
                int         offset_y = 0;
            };
            struct PatchPage
            {
                int layer = 0;
                int page = 0;       // page index in row-page space
                int page_rows = 64; // must match encoder/decoder; stored for forward safety
                int row_count = 0;  // number of rows captured in this page (<= page_rows)
                // Stored per-cell, row-major, length = row_count * columns.
                // Stored glyph plane (lossless token; may be Unicode or indexed glyph id).
                // Serialized as uint32 values in .phos.
                std::vector<GlyphId> cells;
                std::vector<ColourIndex16> fg;
                std::vector<ColourIndex16> bg;
                std::vector<Attrs>    attrs;
            };
            struct Patch
            {
                int columns = 80;
                int rows = 1;
                int active_layer = 0;
                int caret_row = 0;
                int caret_col = 0;
                // Core palette identity for interpreting fg/bg indices.
                phos::colour::PaletteRef palette_ref;
                // UI palette selection identity (see ProjectState::ui_palette_ref).
                phos::colour::PaletteRef ui_palette_ref;
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

    // UI palette selection identity (persisted via ProjectState).
    const phos::colour::PaletteRef& GetUiPaletteRef() const { return m_ui_palette_ref; }
    void SetUiPaletteRef(const phos::colour::PaletteRef& ref) { m_ui_palette_ref = ref; TouchContent(); }

    BoldSemantics GetBoldSemantics() const { return m_bold_semantics; }
    void SetBoldSemantics(BoldSemantics s)
    {
        if (m_bold_semantics == s)
            return;
        m_bold_semantics = s;
        TouchContent();
    }

    // Core palette identity (used by LUT/index pipelines; not yet enforced on stored Colour32 cells).
    const phos::colour::PaletteRef& GetPaletteRef() const { return m_palette_ref; }
    void SetPaletteRef(const phos::colour::PaletteRef& ref) { m_palette_ref = ref; TouchContent(); }

    // Convert the entire canvas to a new palette:
    // - Remaps all stored fg/bg indices from the current palette into `new_ref`
    //   via deterministic nearest-colour mapping (Unset stays Unset).
    // - Sets the canvas palette_ref to `new_ref`.
    // This is an undoable structural operation (captured as a snapshot).
    bool ConvertToPalette(const phos::colour::PaletteRef& new_ref);

    // ---------------------------------------------------------------------
    // Public colour/index helpers
    // ---------------------------------------------------------------------
    // Convenience wrappers around the internal Phase-B palette index helpers.
    // These are intentionally public so UI code can render indexed colours (e.g. brush thumbnails)
    // without duplicating palette-resolution logic.
    //
    // Semantics:
    // - `kUnsetIndex16` maps to Colour32 0 ("unset")
    // - Colour32 0 maps to `kUnsetIndex16`
    ColourIndex16 QuantizeColour32ToIndexPublic(Colour32 c32) const { return QuantizeColour32ToIndex(c32); }
    Colour32      IndexToColour32Public(ColourIndex16 idx) const { return IndexToColour32(idx); }

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

    // Small POD rect used throughout the public API (cell coordinates).
    // Defined later in the header (Selection + clipboard section); forward-declared here
    // so earlier APIs can reference it.
    struct Rect;

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
    // Mirror mode (editor drawing assist)
    // ---------------------------------------------------------------------
    // When enabled, tool-driven mutations are mirrored across the vertical axis of the canvas
    // (left-right symmetry). This is applied at the mutation layer (SetCell/ClearStyle), so
    // existing tools automatically inherit the behavior.
    bool IsMirrorModeEnabled() const { return m_mirror_mode; }
    void SetMirrorModeEnabled(bool enabled) { m_mirror_mode = enabled; }
    void ToggleMirrorModeEnabled() { m_mirror_mode = !m_mirror_mode; }

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
    // Index-native variant: set glyph + fg/bg palette indices (in the canvas's active palette index space).
    // Use kUnsetIndex16 for unset fg/bg (theme default fg / transparent bg).
    bool     SetLayerCellIndices(int layer_index, int row, int col, char32_t cp, ColourIndex16 fg, ColourIndex16 bg);
    // Index-native variant: set glyph + fg/bg indices + attrs.
    bool     SetLayerCellIndices(int layer_index, int row, int col, char32_t cp, ColourIndex16 fg, ColourIndex16 bg, Attrs attrs);
    // Index-native partial update: preserve-friendly without requiring a separate read.
    // - `std::nullopt` preserves the existing channel value
    // - `kUnsetIndex16` explicitly unsets fg/bg (theme default fg / transparent bg)
    //
    // This exists primarily to support high-frequency tool/script writers (e.g. ANSL)
    // without doing two passes (GetLayerCellIndices + SetLayerCellIndices) per cell.
    bool     SetLayerCellIndicesPartial(int layer_index,
                                        int row,
                                        int col,
                                        char32_t cp,
                                        std::optional<ColourIndex16> fg,
                                        std::optional<ColourIndex16> bg,
                                        std::optional<Attrs> attrs);

    // GlyphId-native variants (lossless token surface).
    // These are intended for tool/script writers that want to preserve bitmap/embedded glyph identity.
    bool     SetLayerGlyphIndicesPartial(int layer_index,
                                         int row,
                                         int col,
                                         GlyphId glyph,
                                         std::optional<ColourIndex16> fg,
                                         std::optional<ColourIndex16> bg,
                                         std::optional<Attrs> attrs);
    GlyphId  GetLayerGlyph(int layer_index, int row, int col) const;

    char32_t GetLayerCell(int layer_index, int row, int col) const;
    // Index-native: returns false if invalid/out of bounds. Outputs palette indices in the canvas's active palette.
    bool     GetLayerCellIndices(int layer_index, int row, int col, ColourIndex16& out_fg, ColourIndex16& out_bg) const;
    // Returns false if `layer_index` is invalid or out of bounds.
    bool     GetLayerCellAttrs(int layer_index, int row, int col, Attrs& out_attrs) const;
    // Clears fg/bg style for a cell (sets to 0/unset). Returns false if layer_index invalid.
    bool     ClearLayerCellStyle(int layer_index, int row, int col);

    // Fill an entire layer with `cp` (default: space).
    // Returns false if `layer_index` is invalid.
    bool ClearLayer(int layer_index, char32_t cp = U' ');

    // General-purpose layer fill helper.
    // Any field set to std::nullopt is left unchanged.
    // Note: `Colour32` value 0 still means "unset" (theme default / transparent bg).
    bool FillLayer(int layer_index,
                   std::optional<char32_t> cp,
                   std::optional<Colour32> fg,
                   std::optional<Colour32> bg);

    // ---------------------------------------------------------------------
    // Pointer state (for tools/scripts)
    // ---------------------------------------------------------------------
    // Caret = the editing caret used by keyboard operations (x=col, y=row).
    void GetCaretCell(int& out_x, int& out_y) const { out_x = m_caret_col; out_y = m_caret_row; }
    void SetCaretCell(int x, int y);

    // ---------------------------------------------------------------------
    // Editor-style structural cell operations
    // ---------------------------------------------------------------------
    // Forward delete at the caret that shifts the remainder of the line left by 1 cell,
    // filling the last cell with a transparent blank.
    //
    // This is intentionally distinct from selection delete:
    // - Backspace is "clear cell" semantics (tools may implement their own caret movement)
    // - Delete is "shift-delete" semantics when no selection exists
    //
    // Returns true if any cell changed.
    bool DeleteForwardShift(int layer_index = -1);

    // ---------------------------------------------------------------------
    // Selection structural ops (shift + keep size)
    // ---------------------------------------------------------------------
    // Remove a row/column by shifting content to fill the gap, keeping the canvas
    // dimensions the same (last row/col is filled with a transparent blank).
    //
    // Important:
    // - These are intended for explicit selection operations (e.g. Shift+Delete on a
    //   full-row/full-column selection).
    // - Unlike tool brush writes, these ops must NOT be clipped by selection-as-mask.
    //
    // Returns true if any cell changed.
    bool RemoveRowShiftUp(int row, int layer_index = -1);
    bool RemoveColumnShiftLeft(int col, int layer_index = -1);

    // Insert a blank row/column at the given index by shifting content to make room,
    // keeping the canvas dimensions the same (the shifted-out last row/col is discarded).
    //
    // Important:
    // - These are intended for explicit selection operations (e.g. Alt+Down/Alt+Right).
    // - Unlike tool brush writes, these ops must NOT be clipped by selection-as-mask.
    //
    // Returns true if any cell changed.
    bool InsertRowShiftDown(int row, int layer_index = -1);
    bool InsertColumnShiftRight(int col, int layer_index = -1);
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

    // ---------------------------------------------------------------------
    // Multi-cell brush ("stamp") support
    // ---------------------------------------------------------------------
    // A brush is a rectangular block of cells (glyph + fg + bg + attrs).
    // This is transient tool state (like selection), not part of the project file and not undo-tracked.
    struct Brush
    {
        int w = 0;
        int h = 0;
        // Stored per-cell, row-major, length = w*h.
        // NOTE: this is GlyphId (token) storage; do not assume Unicode semantics.
        std::vector<GlyphId>  cp;
        std::vector<ColourIndex16> fg; // kUnsetIndex16 = unset
        std::vector<ColourIndex16> bg; // kUnsetIndex16 = unset (transparent)
        std::vector<Attrs>    attrs; // 0 = none
    };

    bool HasCurrentBrush() const;
    const Brush* GetCurrentBrush() const;
    void ClearCurrentBrush();
    bool SetCurrentBrush(const Brush& brush);

    // Captures a brush from the current selection.
    // - `CaptureBrushFromSelection`: captures cells from a specific layer (default active layer)
    // - `CaptureBrushFromSelectionComposite`: captures composited "what you see" result
    bool CaptureBrushFromSelection(Brush& out, int layer_index = -1);
    bool CaptureBrushFromSelectionComposite(Brush& out);

    // Selection rectangle is stored in cell space. Corners are inclusive.
    bool HasSelection() const { return m_selection.active && m_selection.w > 0 && m_selection.h > 0; }
    Rect GetSelectionRect() const;
    void SetSelectionCorners(int x0, int y0, int x1, int y1);
    // Convenience: selects the entire allocated canvas extent (0..cols-1, 0..rows-1).
    // This is UI-agnostic and is safe to call from menu items / keybindings.
    void SelectAll();
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
        ColourOnly = 2 // overwrite fg + bg only (glyph preserved)
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

    // Marks tool/script execution so we can scope certain behaviors (mirror mode, selection clipping)
    // to user-driven mutations, without affecting file I/O, undo replay, or other core operations.
    //
    // Intended usage:
    //   {
    //     AnsiCanvas::ToolRunScope scope(canvas);
    //     ... tool or ANSL script code that mutates the canvas ...
    //   }
    struct ToolRunScope
    {
        AnsiCanvas& c;
        bool        prev = false;
        explicit ToolRunScope(AnsiCanvas& canvas) : c(canvas), prev(canvas.m_tool_running) { c.m_tool_running = true; }
        ~ToolRunScope() { c.m_tool_running = prev; }
    };

    // ---------------------------------------------------------------------
    // Tool brush preview (transient overlay; not serialized)
    // ---------------------------------------------------------------------
    // Tools/scripts can request a transient brush outline preview (e.g. size indicator).
    // This is NOT part of selection state and is NOT persisted. The canvas clears it at the
    // start of each Render() call; tools should re-send it each frame while active.
    struct ToolBrushPreview
    {
        bool active = false;
        // Inclusive cell-space rect in canvas coordinates (x=col, y=row).
        int x0 = 0;
        int y0 = 0;
        int x1 = -1;
        int y1 = -1;
    };

    void ClearToolBrushPreview()
    {
        m_tool_brush_preview.active = false;
        m_tool_brush_preview.x0 = 0;
        m_tool_brush_preview.y0 = 0;
        m_tool_brush_preview.x1 = -1;
        m_tool_brush_preview.y1 = -1;
    }

    void SetToolBrushPreviewRect(int x0, int y0, int x1, int y1)
    {
        m_tool_brush_preview.active = true;
        m_tool_brush_preview.x0 = x0;
        m_tool_brush_preview.y0 = y0;
        m_tool_brush_preview.x1 = x1;
        m_tool_brush_preview.y1 = y1;
    }

    const ToolBrushPreview& GetToolBrushPreview() const { return m_tool_brush_preview; }

    // ---------------------------------------------------------------------
    // External mutation batching (performance)
    // ---------------------------------------------------------------------
    // Many systems mutate the canvas outside AnsiCanvas::Render() (e.g. ANSL scripts run
    // from the script editor, file importers, batch ops). In that mode we are not capturing
    // undo deltas, but we still bump state + content revision on every single cell write,
    // which can be unnecessarily expensive for scripts that touch thousands of cells.
    //
    // `ExternalMutationScope` coalesces those bumps: within the scope, the *first* mutation
    // triggers a state/content bump, and subsequent mutations become cheaper (until the
    // scope ends). This is ONLY applied when undo capture is inactive.
    struct ExternalMutationScope
    {
        AnsiCanvas& c;
        explicit ExternalMutationScope(AnsiCanvas& canvas) : c(canvas) { ++c.m_external_mutation_depth; }
        ~ExternalMutationScope()
        {
            if (c.m_external_mutation_depth > 0)
                --c.m_external_mutation_depth;
            if (c.m_external_mutation_depth == 0)
                c.m_external_mutation_bumped = false;
        }
    };

private:
    // Selection-as-mask for tool/script-driven edits:
    // when a selection exists, tools/ANSL scripts may only mutate cells *inside* it.
    //
    // This is intentionally scoped to tool execution (`m_tool_running`) so non-tool core operations
    // (file I/O, undo replay, etc) are unaffected.
    //
    // Important: while moving a floating selection (`m_move.active`), we do NOT clip writes,
    // otherwise committing the move outside the original selection would be blocked.
    bool ToolWriteAllowed(int canvas_row, int canvas_col) const
    {
        if (!m_tool_running)
            return true;
        if (m_move.active)
            return true;
        if (!HasSelection())
            return true;
        return SelectionContains(canvas_col, canvas_row);
    }

    struct Layer
    {
        std::string           name;
        bool                  visible = true;
        bool                  lock_transparency = false;
        phos::LayerBlendMode  blend_mode = phos::LayerBlendMode::Normal;
        std::uint8_t          blend_alpha = 255;
        int                   offset_x = 0;
        int                   offset_y = 0;
        std::vector<GlyphId>  cells; // size == rows * columns (GlyphId tokens)
        std::vector<ColourIndex16> fg; // per-cell foreground index; kUnsetIndex16 = unset
        std::vector<ColourIndex16> bg; // per-cell background index; kUnsetIndex16 = unset (transparent)
        std::vector<Attrs>    attrs; // per-cell attribute bitmask; 0 = none
    };

    struct Snapshot
    {
        int                columns = 80;
        int                rows = 1;
        int                active_layer = 0;
        int                caret_row = 0;
        int                caret_col = 0;
        // Core palette identity for interpreting fg/bg indices.
        phos::colour::PaletteRef palette_ref;
        // UI palette selection identity (does not affect stored indices, but is persisted/undoable).
        phos::colour::PaletteRef ui_palette_ref;
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
            phos::LayerBlendMode blend_mode = phos::LayerBlendMode::Normal;
            std::uint8_t blend_alpha = 255;
            int         offset_x = 0;
            int         offset_y = 0;
        };
        struct PatchPage
        {
            int layer = 0;
            int page = 0;
            int page_rows = 64;
            int row_count = 0; // <= page_rows
            std::vector<GlyphId>  cells;
            std::vector<ColourIndex16> fg;
            std::vector<ColourIndex16> bg;
            std::vector<Attrs>    attrs;
        };
        struct Patch
        {
            int columns = 80;
            int rows = 1;
            int active_layer = 0;
            int caret_row = 0;
            int caret_col = 0;
            // Core palette identity for interpreting fg/bg indices.
            phos::colour::PaletteRef palette_ref;
            // UI palette selection identity.
            phos::colour::PaletteRef ui_palette_ref;
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

    // Per-canvas active glyph selection (see SetActiveGlyph()).
    GlyphId      m_active_glyph = phos::glyph::MakeUnicodeScalar(U' ');
    std::string  m_active_glyph_utf8 = " ";

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
    ZoomSnapMode m_zoom_snap_mode = ZoomSnapMode::PixelAligned;
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

    // Bold semantics policy (persisted via ProjectState, not undoable).
    BoldSemantics m_bold_semantics = BoldSemantics::AnsiBright;

    // UI palette selection identity (persisted via ProjectState).
    phos::colour::PaletteRef m_ui_palette_ref;
    phos::colour::PaletteRef m_palette_ref;

    // Optional embedded bitmap font payload (persisted via ProjectState).
    std::optional<EmbeddedBitmapFont> m_embedded_font;

    bool m_request_open_sauce_editor = false;

    // Deferred scroll request (applied during next Render() when child is active).
    bool  m_scroll_request_valid = false;
    float m_scroll_request_x = 0.0f;
    float m_scroll_request_y = 0.0f;

    // Mouse capture independent of ImGui ActiveId: once the user clicks on the canvas,
    // we keep updating cursor cell coords while the button is held (enables click+drag tools).
    bool m_mouse_capture = false;

    // Mirror mode: enabled via UI; applied to tool-driven mutations only.
    bool m_mirror_mode = false;
    // Transient: true while executing the active tool script callback during Render().
    bool m_tool_running = false;

    // External mutation batching state (see ExternalMutationScope).
    int  m_external_mutation_depth = 0;
    bool m_external_mutation_bumped = false;

    // Input captured from ImGui:
    std::vector<char32_t> m_typed_queue;
    KeyEvents             m_key_events;
    kb::KeyBindingsEngine* m_keybinds = nullptr; // not owned

    // Optional atlas provider for bitmap-font rendering (app-owned).
    IBitmapGlyphAtlasProvider* m_bitmap_atlas_provider = nullptr;

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
        GlyphId  cp = phos::glyph::MakeUnicodeScalar(U' ');
        ColourIndex16 fg = kUnsetIndex16;
        ColourIndex16 bg = kUnsetIndex16;
        Attrs    attrs = 0;
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

    // Current multi-cell brush (stamp) for tools (per-canvas, transient).
    std::optional<Brush> m_current_brush;

    // Transient tool brush preview overlay state (cleared each Render()).
    ToolBrushPreview m_tool_brush_preview;

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
    bool   CanvasToLayerLocalForWrite(int layer_index, int canvas_row, int canvas_col, int& out_local_row, int& out_local_col) const;
    bool   CanvasToLayerLocalForRead(int layer_index, int canvas_row, int canvas_col, int& out_local_row, int& out_local_col) const;

    // Colour/index helpers (Phase B).
    phos::colour::PaletteInstanceId ResolveActivePaletteId() const;
    ColourIndex16 QuantizeColour32ToIndex(Colour32 c32) const; // 0 => unset
    Colour32      IndexToColour32(ColourIndex16 idx) const;    // unset => 0

    struct CompositeCell
    {
        GlyphId  glyph = phos::glyph::MakeUnicodeScalar(U' ');
        char32_t cp = U' ';
        ColourIndex16 fg = kUnsetIndex16; // index in active palette, or unset
        ColourIndex16 bg = kUnsetIndex16; // index in active palette, or unset
        Attrs    attrs = 0;
    };

    CompositeCell GetCompositeCell(int row, int col) const;
    void          SetActiveCell(int row, int col, char32_t cp);
    void          SetActiveCell(int row, int col, char32_t cp, Colour32 fg, Colour32 bg);
    void          ClearActiveCellStyle(int row, int col);
    void          ClearLayerCellStyleInternal(int layer_index, int row, int col);
    void          DrawActiveLayerBoundsOverlay(ImDrawList* draw_list, const ImVec2& origin, float cell_w, float cell_h);

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
    void DrawToolBrushPreviewOverlay(ImDrawList* draw_list,
                                     const ImVec2& origin,
                                     float cell_w,
                                     float cell_h,
                                     const ImVec2& canvas_size);

    void DrawMirrorAxisOverlay(ImDrawList* draw_list,
                               const ImVec2& origin,
                               float cell_w,
                               float cell_h,
                               const ImVec2& canvas_size);
};


