#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SessionState;
class AnsiCanvas;

// Character Sets (F-key brush presets) UI.
//
// Loads/saves sets from a JSON file (assets/character-sets.json):
// {
//   "schema_version": 1,
//   "default_set": 5,
//   "sets": ["<12 chars>", ...]
// }
//
// Each set is conceptually 12 slots (F1..F12), stored as Unicode codepoints.
class CharacterSetWindow
{
public:
    CharacterSetWindow();
    ~CharacterSetWindow();

    CharacterSetWindow(const CharacterSetWindow&) = delete;
    CharacterSetWindow& operator=(const CharacterSetWindow&) = delete;

    // Render the window. Returns true if it remains open.
    bool Render(const char* window_title, bool* p_open = nullptr,
                SessionState* session = nullptr, bool apply_placement_this_frame = false,
                AnsiCanvas* active_canvas = nullptr);

    // Called by host when the external character picker/palette selection changes.
    // If "edit mode" is enabled and a slot is selected, this assigns the slot.
    void OnExternalSelectedCodePoint(uint32_t cp);

    // Active set + mapping queries (0-based slot index: 0..11 == F1..F12).
    int  GetActiveSetIndex() const { return active_set_index_; }
    bool SetActiveSetIndex(int idx);
    int  GetSetCount() const { return (int)sets_.size(); }
    uint32_t GetSlotCodePoint(int slot_index_0_based) const;
    void SelectSlot(int slot_index_0_based);

    // If the user requested insertion (double-click in this window), returns true and outputs cp.
    bool TakeInsertRequested(uint32_t& out_cp);

    // If the user clicked a slot in this window, returns true and outputs that slot's cp.
    bool TakeUserSelectionChanged(uint32_t& out_cp);

private:
    struct Set
    {
        std::vector<uint32_t> cps; // always size 12
    };

private:
    void EnsureLoaded();
    bool LoadFromFile(const char* path, std::string& error);
    bool SaveToFile(const char* path, std::string& error) const;
    void EnsureNonEmpty();

    void RenderTopBar(AnsiCanvas* active_canvas);
    void RenderSettingsContents(AnsiCanvas* active_canvas);
    void RenderSlots();

    static bool     IsScalarValue(uint32_t cp);
    static uint32_t DecodeFirstCodePointUtf8(const std::string& s);
    static void     DecodeAllCodePointsUtf8(const std::string& s, std::vector<uint32_t>& out);
    static std::string EncodeCodePointUtf8(uint32_t cp);
    static std::string CodePointHex(uint32_t cp);

private:
    // File state
    bool        loaded_ = false;
    std::string file_path_;
    std::string last_error_;

    // Sets
    std::vector<Set> sets_;
    int active_set_index_ = 0;
    int default_set_index_ = 0;

    // UI / edit state
    bool edit_mode_ = false;
    int  selected_slot_ = 0; // 0..11
    bool request_save_ = false;
    bool request_reload_ = false;

    // "insert requested" output (double-click on a slot)
    bool     insert_requested_ = false;
    uint32_t insert_requested_cp_ = 0;

    // "user clicked something" output (single-click on a slot)
    bool     user_selection_changed_ = false;
    uint32_t user_selected_cp_ = 0;

    // Render context
    AnsiCanvas* active_canvas_ = nullptr;
};


