#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace kb { class KeyBindingsEngine; }

struct SessionState;

// Forward declarations to avoid pulling imgui headers into all compilation units.
struct ImGuiTextFilter;

// Settings window with an extendable tab system.
// For now it hosts the Key Bindings editor (load/edit/save JSON in assets/key-bindings.json).
class SettingsWindow
{
public:
    struct Tab
    {
        std::string id;    // stable internal id
        std::string title; // visible label
        std::function<void()> render;
    };

    SettingsWindow();

    void SetOpen(bool open) { open_ = open; }
    bool IsOpen() const { return open_; }

    // Attaches the keybinding engine that backs the Key Bindings tab.
    // The window does not own the engine.
    void SetKeyBindingsEngine(kb::KeyBindingsEngine* engine) { keybinds_ = engine; }

    // Provides the current UI scale factor so style/theme application can re-scale correctly.
    void SetMainScale(float scale) { main_scale_ = scale; }

    // Optional: apply an undo-limit preference across canvases.
    // Convention: 0 = unlimited.
    void SetUndoLimitApplier(const std::function<void(size_t)>& fn) { undo_limit_applier_ = fn; }

    // Optional: apply a global LUT cache budget (in bytes).
    // Convention: 0 = unlimited (not recommended).
    void SetLutCacheBudgetApplier(const std::function<void(size_t)>& fn) { lut_cache_budget_applier_ = fn; }

    // Optional: apply a bitmap glyph atlas cache budget (in bytes).
    // Convention: 0 = unlimited (not recommended).
    void SetGlyphAtlasCacheBudgetApplier(const std::function<void(size_t)>& fn) { glyph_atlas_cache_budget_applier_ = fn; }

    // Optional: query live glyph atlas cache usage (bytes).
    void SetGlyphAtlasCacheUsedBytesGetter(const std::function<size_t()>& fn) { glyph_atlas_cache_used_bytes_getter_ = fn; }

    // Extendable: allows future subsystems to register additional tabs/panels.
    // If a tab with the same id exists, it is replaced.
    void RegisterTab(const Tab& tab);

    // Main render call. Safe to call every frame; does nothing if closed.
    // If session is provided, window placement (pos/size/collapsed) is captured/restored via SessionState.
    void Render(const char* title = "Settings", SessionState* session = nullptr, bool apply_placement_this_frame = false);

private:
    void EnsureDefaultTabsRegistered();
    void RenderTab_General();
    void RenderTab_KeyBindings();
    void RenderTab_Skin();

private:
    bool open_ = false;

    // Tabs
    bool                  tabs_registered_ = false;
    std::vector<Tab>      tabs_;
    std::string           active_tab_id_;

    // Key bindings model lives in core; UI just edits it.
    kb::KeyBindingsEngine* keybinds_ = nullptr;

    // Session state (non-owning) provided by Render().
    SessionState*          session_ = nullptr;

    // UI scale factor (HiDPI). Set by the app.
    float                 main_scale_ = 1.0f;

    // Optional: handler used by the General tab to apply undo limit across canvases.
    std::function<void(size_t)> undo_limit_applier_;

    // Optional: handler used by the General tab to apply LUT cache budget globally.
    std::function<void(size_t)> lut_cache_budget_applier_;

    // Optional: handler used by the General tab to apply glyph atlas cache budget globally.
    std::function<void(size_t)> glyph_atlas_cache_budget_applier_;
    // Optional: getter for live glyph atlas cache bytes for pressure UI.
    std::function<size_t()>     glyph_atlas_cache_used_bytes_getter_;

    // UI state
    std::string           filter_text_;
    bool                  show_ids_ = false;

    // "Record binding" capture state (UI-only for now).
    bool                  capture_active_ = false;
    size_t                capture_action_idx_ = 0;
    size_t                capture_binding_idx_ = 0;
};


