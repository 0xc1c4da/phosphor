#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "fonts/textmode_font_sanity_cache.h"

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

        // Palette UI state (assets/color-palettes.json)
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

        // User-facing document identity for this canvas (typically an absolute file path,
        // but can also be a URL-like string for remote imports). Empty means "not explicitly saved/opened".
        // The UI uses this for window titles when available.
        std::string file_path;

        // Preferred persistence for session restore: a cached .phos project stored under
        // <config_dir>/cache (see core/paths.cpp).
        // Stored as a cache-relative path like "session_canvases/canvas_12.phos".
        std::string project_phos_cache_rel;

        // Canvas project state encoded as: zstd-compressed CBOR (nlohmann::json::to_cbor) then base64.
        // Legacy fallback (schema <= 6) or when cache writes fail.
        std::string project_cbor_zstd_b64;
        std::uint64_t project_cbor_size = 0; // uncompressed CBOR size in bytes

        // Viewport state
        float zoom = 1.0f;
        float scroll_x = 0.0f;
        float scroll_y = 0.0f;

        // Canvas background (independent of ImGui theme). False = black, true = white.
        // Per-canvas-window instance setting (so multiple open canvases can differ).
        bool canvas_bg_white = false;

        // Per-canvas "active glyph" (what tools draw with by default).
        // - `active_glyph` is a GlyphId token (lossless; may exceed Unicode range).
        // - `active_glyph_utf8` is the UTF-8 string used by tools (may be multi-codepoint).
        //   If empty, the host should fall back to encoding a best-effort representative.
        //
        // Backward compatibility: older session.json versions stored `active_glyph_cp`
        // (Unicode scalar, including legacy embedded PUA). We still parse it as a fallback.
        std::uint32_t active_glyph = 0; // 0 means "unset" (host should treat as space)
        std::uint32_t active_glyph_cp = 0; // back-compat fallback (read-only; no longer written)
        std::string   active_glyph_utf8;
    };

    struct OpenImage
    {
        int id = 0;
        bool open = true;
        std::string path; // reloaded at startup
    };

    struct AnslEditorState
    {
        // If false, the UI should use its built-in default template.
        // If true, `text` should be restored even if it's empty.
        bool text_valid = false;
        std::string text;

        // Script FPS slider value (may be overridden by script settings at runtime).
        int target_fps = 30;

        // Example dropdown selection.
        // We persist both index (fast) and identity (stable across directory changes).
        int selected_example_index = -1; // -1 = none
        std::string selected_example_label;
        std::string selected_example_path;
    };

    // ---------------------------------------------------------------------
    // Tool Parameters (persisted)
    // ---------------------------------------------------------------------
    // Stores per-tool parameter values (settings.params -> ctx.params) so switching tools
    // doesn't clobber state and values persist across app restarts.
    //
    // Keyed by tool "stable id" (ToolSpec::id / settings.id).
    struct ToolParamValue
    {
        // Matches AnslParamType numeric values:
        // 0=Bool, 1=Int, 2=Float, 3=Enum, 4=Button
        int type = 0;
        bool b = false;
        int i = 0;
        float f = 0.0f;
        std::string s; // enum string
    };
    // tool_id -> (param_key -> value)
    std::unordered_map<std::string, std::unordered_map<std::string, ToolParamValue>> tool_param_values;

    // ---------------------------------------------------------------------
    // Brush Palette (persisted)
    // ---------------------------------------------------------------------
    // Stores the user's captured multi-cell brushes (stamps).
    // This is global app state (not per-canvas) and is persisted in session.json.
    struct BrushPaletteEntry
    {
        std::string name;
        int w = 0;
        int h = 0;
        // Row-major arrays, length = w*h
        std::vector<std::uint32_t> cp;    // Unicode scalar values
        std::vector<std::uint32_t> fg;    // packed RGBA Color32 (0 = unset)
        std::vector<std::uint32_t> bg;    // packed RGBA Color32 (0 = unset)
        std::vector<std::uint32_t> attrs; // Attrs bitmask (stored as u32 for JSON simplicity)
    };
    struct BrushPaletteState
    {
        int version = 1;
        std::vector<BrushPaletteEntry> entries;
        int selected = -1;
    };
    BrushPaletteState brush_palette;

    // Main window geometry (SDL window coordinates)
    int window_w = 0;
    int window_h = 0;
    int window_x = 0;
    int window_y = 0;
    bool window_pos_valid = false;
    bool window_maximized = false;
    bool window_fullscreen = false;

    // Tool window visibility toggles
    bool show_color_picker_window = true;
    bool show_character_picker_window = true;
    bool show_character_palette_window = true;
    bool show_character_sets_window = true;
    bool show_layer_manager_window = true;
    bool show_ansl_editor_window = true;
    bool show_tool_palette_window = true;
    bool show_brush_palette_window = false;
    bool show_minimap_window = true;
    bool show_settings_window = false;
    bool show_16colors_browser_window = false;

    // UI skin/theme (ImGui style). Persisted in session.json.
    // Stable ids are defined in ui/skin.h (e.g. "moonlight", "cherry").
    std::string ui_theme = "cherry";

    // UI language / locale for ICU i18n bundles.
    // - Empty: use system default locale (ICU default at startup).
    // - Non-empty: ICU locale id matching an available bundle (e.g. "de_DE", "fr_FR", "root").
    // Persisted in session.json.
    std::string ui_locale;

    // Undo history retention limit for canvases.
    // 0 = unlimited (default).
    size_t undo_limit = 0;

    // Zoom snapping mode (applies to all canvases).
    //
    // 1 = Integer scale (always snap to N×)
    // 2 = Pixel-aligned cell width (always snap cell width to integer pixels)
    //
    // Note: older session files may contain 0 (Auto); we treat that as 2 during load.
    int zoom_snap_mode = 2; // default: Pixel-aligned

    // Global LUT cache budget (bytes).
    // This is an app-level performance/memory tuning knob intended for LUT-heavy features
    // (palette remaps, allowed-snap LUTs, blend LUTs, quantization LUTs).
    //
    // Default: 64 MiB. Typical recommended range: <= 96 MiB (under 100MB).
    size_t lut_cache_budget_bytes = 64ull * 1024ull * 1024ull;

    // Bitmap glyph atlas cache budget (bytes).
    // This caps the total live GPU memory used by cached bitmap font atlases (plus a small
    // temporary overshoot due to deferred destruction for frames-in-flight safety).
    //
    // Default: 96 MiB (chosen to allow many open canvases/fonts without unbounded growth).
    // Convention: 0 = unlimited (not recommended).
    size_t glyph_atlas_cache_budget_bytes = 96ull * 1024ull * 1024ull;

    // Canvas background (independent of ImGui theme). False = black, true = white.
    bool canvas_bg_white = false;

    // Per-tool UI state
    bool character_palette_settings_open = true;
    XtermColorPickerState xterm_color_picker;
    AnslEditorState ansl_editor;

    // A couple of useful "workspace" bits
    std::string last_import_image_dir;

    // Default ANSI import settings (used by IoManager when importing .ans/.nfo/.diz).
    // No longer persisted: ANSI import is intended to be automatic (file-driven via SAUCE where available).

    // Most recently opened/saved files (absolute paths or URI-like strings).
    // Used by File -> Recent.
    std::vector<std::string> recent_files;

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

    // Cached results of expensive FIGlet/TDF font validation (broken ids).
    // Stored in session.json so subsequent runs can skip re-validating the full font library.
    textmode_font::SanityCache font_sanity_cache;
};

// Returns an absolute directory path intended for app config/state.
// On Linux prefers $XDG_CONFIG_HOME/phosphor, then $HOME/.config/phosphor.
std::string GetPhosphorConfigDir();

// Returns absolute paths for persisted state.
std::string GetSessionStatePath();

bool LoadSessionState(SessionState& out, std::string& err);
bool SaveSessionState(const SessionState& st, std::string& err);


