#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Forward declare to keep header light.
struct ImFont;
struct SessionState;

// Dear ImGui Unicode Character Picker (Unicode 13 via ICU67).
//
// Design goals (aligned with references/character_picker_analysis.md):
// - Block dropdown (plus "All Unicode")
// - Sub-page dropdown (pages within a block, or planes for "All")
// - Grid/table rendering with mouse + keyboard navigation
// - Full-text name search (ICU u_enumCharNames)
// - Confusables side list (ICU spoof / skeleton matching)
//
// Usage:
//   static CharacterPicker picker;
//   picker.Render("Character Picker");
class CharacterPicker
{
public:
    struct SearchResult
    {
        uint32_t cp = 0;
        std::string name;   // ICU character name (may be empty)
        std::string block;  // ICU block name
    };

    CharacterPicker();
    ~CharacterPicker();

    CharacterPicker(const CharacterPicker&) = delete;
    CharacterPicker& operator=(const CharacterPicker&) = delete;

    // Render the picker UI. Returns true if it is still open (for convenience in windowing).
    bool Render(const char* window_title, bool* p_open = nullptr,
                SessionState* session = nullptr, bool apply_placement_this_frame = false);

    // Get the currently selected code point (Unicode scalar where possible).
    uint32_t SelectedCodePoint() const { return selected_cp_; }

    // Programmatically navigate the picker to a codepoint (updates plane selection, scrolls into view).
    void JumpToCodePoint(uint32_t cp);

    // Returns true if selection changed since last call, and outputs the current selection.
    bool TakeSelectionChanged(uint32_t& out_cp);

    // Returns true if the user double-clicked a glyph in the grid this frame.
    bool TakeDoubleClicked(uint32_t& out_cp);

    // Small ICU-backed helpers (useful for tooltips/callbacks).
    static std::string BlockNameFor(uint32_t cp);

private:
    struct BlockInfo
    {
        uint32_t start = 0;
        uint32_t end   = 0;
        int32_t  value = 0;     // ICU UCHAR_BLOCK value
        std::string name;       // ICU long property value name
    };

    struct OmitRange
    {
        uint32_t start = 0;
        uint32_t end   = 0; // inclusive
    };

private:
    // ---------- ICU helpers ----------
    static bool IsScalarValue(uint32_t cp);
    static std::string CodePointHex(uint32_t cp);
    static std::string GlyphUtf8(uint32_t cp);
    static std::string CharName(uint32_t cp);

    static std::vector<std::string> TokenizeUpperASCII(const std::string& q);

    // ---------- omit/visibility helpers ----------
    void InitDefaultOmitRanges();
    void AddOmitRange(uint32_t start_inclusive, uint32_t end_inclusive);
    void NormalizeOmitRanges();
    bool IsOmitted(uint32_t cp) const;
    bool IsRangeFullyOmitted(uint32_t start, uint32_t end) const;

    static bool HasGlyph(const ImFont* font, uint32_t cp);
    std::optional<uint32_t> FirstVisibleInRange(uint32_t start, uint32_t end, const ImFont* font) const;

    void RebuildVisibleCache(uint32_t view_start, uint32_t view_end, const ImFont* font);
    void RebuildAvailablePlanes(const ImFont* font);

    // ---------- model/state ----------
    void EnsureBlocksLoaded();
    void SyncRangeFromSelection();
    void ClampSelectionToCurrentView();

    // Search (expensive, performed only when requested)
    void PerformSearch();
    void ClearSearch();
    std::vector<uint32_t> FilteredSearchCpsForCurrentBlock() const;

    // Confusables (expensive, recomputed on selection change)
    void UpdateConfusablesIfNeeded();
    void ComputeConfusables(uint32_t base_cp, int limit);

    // ---------- UI ----------
    void RenderTopBar();
    void RenderGridAndSidePanel();
    void RenderGrid(uint32_t view_start, uint32_t view_end,
                    const std::vector<uint32_t>* explicit_cps /* if non-null, render these cps */);

private:
    // Blocks
    bool blocks_loaded_ = false;
    std::vector<BlockInfo> blocks_; // excludes synthetic No_Block
    int block_index_ = 0;           // 0 = All Unicode, 1..N = blocks_[index-1]

    // Range/page selection
    uint32_t range_start_ = 0;
    uint32_t range_end_   = 0x10FFFF;

    // Subpage selector:
    // - When block_index_==0 ("All"), subpage means "plane" (0..16).
    // - When block selected (block_index_>0) and search inactive, subpage means page chunk within the block.
    int subpage_index_ = 0;

    // Selected code point
    uint32_t selected_cp_ = 0x0041; // 'A' default
    bool scroll_to_selected_ = false;
    bool selection_changed_ = false;
    bool request_focus_selected_ = false; // keep ImGui keyboard-nav highlight synced to selection
    bool double_clicked_ = false;
    uint32_t double_clicked_cp_ = 0;

    // Omitted ranges (e.g. known missing glyph spans for the current font)
    std::vector<OmitRange> omit_ranges_;
    int omit_revision_ = 0;

    // Cached visible codepoints for the current view (range or plane/block page)
    uint32_t visible_cache_start_ = 0;
    uint32_t visible_cache_end_ = 0;
    const ImFont* visible_cache_font_ = nullptr;
    int visible_cache_omit_revision_ = -1;
    std::vector<uint32_t> visible_cps_cache_;

    // Available planes for "All Unicode (by Plane)" dropdown (hide empty planes)
    const ImFont* plane_cache_font_ = nullptr;
    int plane_cache_omit_revision_ = -1;
    std::vector<int> available_planes_; // values 0..16

    // Search state
    std::string search_query_;
    bool search_active_ = false;
    bool search_dirty_ = false;
    int search_limit_ = 512;
    std::vector<SearchResult> search_results_;

    // Confusables state
    uint32_t confusables_for_cp_ = 0xFFFFFFFFu;
    int confusables_limit_ = 64;
    std::vector<uint32_t> confusable_cps_;

private:
    void MarkSelectionChanged();
};


