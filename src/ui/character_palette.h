#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ui/glyph_token.h"

class CharacterPicker;
class AnsiCanvas;
struct SessionState;

// Character Palette component for utf8-art-editor.
//
// Loads/saves palettes from a JSON file:
// [
//   { "title": "Name", "chars": [" ", "☺", ...] },
//   ...
// ]
//
// Each entry in "chars" is stored as UTF-8 (so multi-codepoint graphemes are supported),
// and we also keep the first codepoint (for integration with the Unicode Character Picker).
class CharacterPalette
{
public:
    struct Glyph
    {
        std::string utf8;         // what we render/copy/save
        uint32_t    first_cp = 0; // decoded first codepoint (0 if invalid/empty)
    };

    struct Palette
    {
        std::string        title;
        std::vector<Glyph> glyphs;
    };

public:
    CharacterPalette();
    ~CharacterPalette();

    CharacterPalette(const CharacterPalette&) = delete;
    CharacterPalette& operator=(const CharacterPalette&) = delete;

    // Render the palette editor window. Returns true if it remains open.
    bool Render(const char* window_title, bool* p_open = nullptr,
                SessionState* session = nullptr, bool apply_placement_this_frame = false,
                AnsiCanvas* active_canvas = nullptr);

    // Load/save palettes file.
    bool LoadFromFile(const char* path, std::string& error);
    bool SaveToFile(const char* path, std::string& error) const;

    // Current selection (first codepoint of the selected glyph).
    uint32_t SelectedCodePoint() const;

    // Called by app when the Unicode picker selection changes.
    // If "picker edits palette" mode is enabled, this will replace the currently selected cell.
    // Otherwise, it will only select an existing matching glyph (no palette mutation).
    void OnPickerSelectedCodePoint(uint32_t cp);

    // Synchronize the palette selection from an external "active glyph" (e.g. per-canvas tool brush).
    // This never mutates palette contents.
    void SyncSelectionFromActiveGlyph(phos::GlyphId glyph, const std::string& utf8,
                                      AnsiCanvas* active_canvas = nullptr);

    // Returns true if the user clicked a glyph in the palette grid this frame.
    bool TakeUserSelectionChanged(GlyphToken& out_glyph, std::string& out_utf8);

    // Returns true if the user double-clicked a glyph in the palette grid this frame.
    bool TakeUserDoubleClicked(GlyphToken& out_glyph);

    // Collect candidate glyph codepoints from the currently active palette source.
    //
    // This is intended for tools that want to restrict glyph search space (e.g. deform quantization).
    // Output codepoints are:
    // - JSON source: first codepoint of each stored glyph (may include 0 for invalid/empty)
    // - Embedded source: best-effort Unicode representatives derived from each embedded glyph index
    //   (deterministic policy; currently CP437-based representative mapping)
    //
    // `active_canvas` is only used for EmbeddedFont source; pass the current active canvas.
    void CollectCandidateCodepoints(std::vector<uint32_t>& out, const AnsiCanvas* active_canvas = nullptr) const;

    // Collect candidate glyph ids (GlyphId tokens) from the currently active palette source.
    //
    // This is intended for token-aware tools that want to restrict glyph search space without
    // losing bitmap/embedded identity.
    //
    // Output glyph ids are:
    // - JSON source: UnicodeScalar GlyphIds from each stored glyph's first codepoint (lossless for
    //   single-codepoint glyphs; multi-codepoint graphemes remain represented by their first codepoint)
    // - Embedded source: EmbeddedIndex GlyphIds, one per embedded glyph index.
    //
    // `active_canvas` is only used for EmbeddedFont source; pass the current active canvas.
    void CollectCandidateGlyphIds(std::vector<phos::GlyphId>& out, const AnsiCanvas* active_canvas = nullptr) const;

private:
    void EnsureLoaded();
    void EnsureNonEmpty();

    static uint32_t DecodeFirstCodePointUtf8(const std::string& s);
    static std::string EncodeCodePointUtf8(uint32_t cp);
    static std::string CodePointHex(uint32_t cp);

    void ReplaceSelectedCellWith(uint32_t cp);
    std::optional<int> FindGlyphIndexByFirstCp(uint32_t cp) const;

    void RenderTopBar(AnsiCanvas* active_canvas);
    void RenderGrid();

private:
    // File state
    bool        loaded_ = false;
    std::string file_path_;
    std::string last_error_;

    // UI
    bool settings_open_ = true;
    bool settings_open_init_from_session_ = false;

    // Palettes
    std::vector<Palette> palettes_;
    int selected_palette_ = 0;

    // Cell selection
    int selected_cell_ = 0;
    bool request_focus_selected_ = false; // keep ImGui keyboard-nav highlight synced to selection

    // Picker integration behavior
    bool picker_replaces_selected_cell_ = false;

    // Palette source selection:
    // - JSON file palettes (Unicode)
    // - Embedded font of the active canvas (glyph indices -> EmbeddedIndex GlyphIds; codepoint
    //   views use a deterministic Unicode representative mapping)
    // - Bitmap font indices (0..255) (glyph indices -> BitmapIndex GlyphIds; codepoint views use a
    //   deterministic Unicode representative mapping)
    enum class Source : int
    {
        JsonFile = 0,
        EmbeddedFont = 1,
        BitmapFontIndices = 2,
    };
    Source source_ = Source::JsonFile;

    // Transient UI state
    bool request_save_ = false;
    bool request_reload_ = false;
    bool open_rename_popup_ = false;
    bool open_new_popup_ = false;
    bool open_delete_popup_ = false;
    char rename_buf_[256] = {};
    char new_title_buf_[256] = {};

    // "user clicked something" output
    bool user_selection_changed_ = false;
    GlyphToken user_selected_glyph_;
    std::string user_selected_utf8_;

    // "user double-clicked something" output
    bool user_double_clicked_ = false;
    GlyphToken user_double_clicked_glyph_;

    // Render context
    AnsiCanvas* active_canvas_ = nullptr;
};


