#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Small persistent "session" state for the app:
// - SDL main window geometry (size/position/maximized)
// - which ImGui tool windows are toggled open
// - ImGui window placements (pos/size/collapsed) for deterministic restore
struct ImGuiWindowPlacement
{
    bool  valid = false;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    bool  collapsed = false;
};

struct SessionState
{
    struct ImGuiWindowChromeState
    {
        // 0..1 multiplier for ImGuiStyleVar_Alpha for this window.
        float opacity = 1.0f;

        // 0 = normal, 1 = pinned to front (always on top), 2 = pinned to back (always behind).
        int z_order = 0;
    };

    struct XtermColorPickerState
    {
        // Normalized RGBA, ImGui-style.
        float fg[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float bg[4] = {0.0f, 0.0f, 0.0f, 1.0f};

        // 0 = foreground, 1 = background
        int active_fb = 0;

        // 0 = Hue Bar, 1 = Hue Wheel
        int picker_mode = 0;

        // Palette UI state (assets/colours.json)
        int selected_palette = 0;

        // Which color the picker reticle is currently previewing (0 = fg, 1 = bg).
        int picker_preview_fb = 0;

        // Hue memory for grayscale colors so the picker doesn't reset to red after restart.
        float last_hue = 0.0f;
    };

    struct OpenCanvas
    {
        int id = 0;
        bool open = true;

        // Canvas project state encoded as: zstd-compressed CBOR (nlohmann::json::to_cbor) then base64.
        std::string project_cbor_zstd_b64;
        std::uint64_t project_cbor_size = 0; // uncompressed CBOR size in bytes

        // Viewport state
        float zoom = 1.0f;
        float scroll_x = 0.0f;
        float scroll_y = 0.0f;
    };

    struct OpenImage
    {
        int id = 0;
        bool open = true;
        std::string path; // reloaded at startup
    };

    // Main window geometry (SDL window coordinates)
    int window_w = 0;
    int window_h = 0;
    int window_x = 0;
    int window_y = 0;
    bool window_pos_valid = false;
    bool window_maximized = false;

    // Tool window visibility toggles
    bool show_color_picker_window = true;
    bool show_character_picker_window = true;
    bool show_character_palette_window = true;
    bool show_character_sets_window = true;
    bool show_layer_manager_window = true;
    bool show_ansl_editor_window = true;
    bool show_tool_palette_window = true;
    bool show_preview_window = true;
    bool show_settings_window = false;

    // UI skin/theme (ImGui style). Persisted in session.json.
    // Stable ids are defined in ui/skin.h (e.g. "moonlight", "cherry").
    std::string ui_theme = "cherry";

    // Canvas background (independent of ImGui theme). False = black, true = white.
    bool canvas_bg_white = false;

    // Per-tool UI state
    bool character_palette_settings_open = true;
    XtermColorPickerState xterm_color_picker;

    // A couple of useful "workspace" bits
    std::string last_import_image_dir;

    // ImGui window placements (keyed by the window name passed to ImGui::Begin()).
    // This replaces reliance on imgui.ini so we can guarantee restore behavior.
    std::unordered_map<std::string, ImGuiWindowPlacement> imgui_windows;

    // Per-ImGui-window "chrome" settings (opacity, z-order pinning).
    std::unordered_map<std::string, ImGuiWindowChromeState> imgui_window_chrome;

    // Workspace content
    std::string active_tool_path;
    int last_active_canvas_id = -1;
    int next_canvas_id = 1;
    int next_image_id = 1;
    std::vector<OpenCanvas> open_canvases;
    std::vector<OpenImage> open_images;
};

// Returns an absolute directory path intended for app config/state.
// On Linux prefers $XDG_CONFIG_HOME/phosphor, then $HOME/.config/phosphor.
std::string GetPhosphorConfigDir();

// Returns absolute paths for persisted state.
std::string GetSessionStatePath();

bool LoadSessionState(SessionState& out, std::string& err);
bool SaveSessionState(const SessionState& st, std::string& err);


