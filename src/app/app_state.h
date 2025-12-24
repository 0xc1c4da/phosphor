#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>
#include <memory>

#include "imgui.h"

// Forward declarations (we mostly store pointers/references in AppState).
struct SDL_Window;
struct ImGui_ImplVulkanH_Window;

struct VulkanState;
struct SessionState;

namespace kb { class KeyBindingsEngine; }

class IoManager;
class SdlFileDialogQueue;

class ExportDialog;
class SettingsWindow;

class ToolPalette;
class AnslEditor;
class AnslScriptEngine;

class CharacterPicker;
class CharacterPalette;
class CharacterSetWindow;

class LayerManager;
class ImageToChafaDialog;
class MarkdownToAnsiDialog;
class MinimapWindow;
class CanvasPreviewTexture;
class BitmapGlyphAtlasTextureCache;
class SixteenColorsBrowserWindow;

struct CanvasWindow;
class ImageWindow;

// AppState is an integration-level container used by `RunFrame(AppState&)`.
// It owns only lightweight loop bookkeeping (done/frame counters/placement cache)
// and stores pointers to the heavier subsystems that are initialized in `main()`.
struct AppState
{
    struct Platform
    {
        SDL_Window* window = nullptr;
    } platform;

    struct Vulkan
    {
        VulkanState* vk = nullptr;
        ImGui_ImplVulkanH_Window* wd = nullptr;
    } vulkan;

    struct Persistence
    {
        SessionState* session_state = nullptr;
    } persist;

    struct Services
    {
        kb::KeyBindingsEngine* keybinds = nullptr;
        IoManager* io_manager = nullptr;
        SdlFileDialogQueue* file_dialogs = nullptr;
        ExportDialog* export_dialog = nullptr;
        SettingsWindow* settings_window = nullptr;
    } services;

    struct Tooling
    {
        ToolPalette* tool_palette = nullptr;
        std::string* tools_error = nullptr;
        std::string* tool_compile_error = nullptr;
        AnslEditor* ansl_editor = nullptr;
        AnslScriptEngine* ansl_engine = nullptr;
        AnslScriptEngine* tool_engine = nullptr;

        std::uint32_t* tool_brush_cp = nullptr;
        std::string* tool_brush_utf8 = nullptr;
        // Current attribute bitmask used by tools (e.g. bold/underline). 0 = none.
        std::uint32_t* tool_attrs_mask = nullptr;

        // Tool helpers (defined in main, used inside the per-frame loop)
        std::function<void(const std::string& tool_path)> compile_tool_script;
        std::function<void()> sync_tool_stack;
        std::function<std::string()> active_tool_id;
        std::function<void(const std::string& id)> activate_tool_by_id;
        std::function<void()> activate_prev_tool;
    } tools;

    struct Workspace
    {
        std::vector<std::unique_ptr<CanvasWindow>>* canvases = nullptr;
        int* next_canvas_id = nullptr;
        int* last_active_canvas_id = nullptr;

        std::vector<ImageWindow>* images = nullptr;
        int* next_image_id = nullptr;
    } workspace;

    struct Ui
    {
        CharacterPicker* character_picker = nullptr;
        CharacterPalette* character_palette = nullptr;
        CharacterSetWindow* character_sets = nullptr;
        LayerManager* layer_manager = nullptr;
        ImageToChafaDialog* image_to_chafa_dialog = nullptr;
        MarkdownToAnsiDialog* markdown_to_ansi_dialog = nullptr;
        MinimapWindow* minimap_window = nullptr;
        CanvasPreviewTexture* preview_texture = nullptr;
        BitmapGlyphAtlasTextureCache* bitmap_glyph_atlas = nullptr;
        SixteenColorsBrowserWindow* sixteen_browser = nullptr;
        class BrushPaletteWindow* brush_palette_window = nullptr;
    } ui;

    struct Colors
    {
        ImVec4* clear_color = nullptr;
        ImVec4* fg_color = nullptr;
        ImVec4* bg_color = nullptr;
        int* active_fb = nullptr;
        int* xterm_picker_mode = nullptr;
        int* xterm_selected_palette = nullptr;
        int* xterm_picker_preview_fb = nullptr;
        float* xterm_picker_last_hue = nullptr;
    } colors;

    struct Toggles
    {
        bool* show_demo_window = nullptr;
        bool* show_color_picker_window = nullptr;
        bool* show_character_picker_window = nullptr;
        bool* show_character_palette_window = nullptr;
        bool* show_character_sets_window = nullptr;
        bool* show_layer_manager_window = nullptr;
        bool* show_ansl_editor_window = nullptr;
        bool* show_tool_palette_window = nullptr;
        bool* show_brush_palette_window = nullptr;
        bool* show_minimap_window = nullptr;
        bool* show_settings_window = nullptr;
        bool* show_16colors_browser_window = nullptr;
        bool* window_fullscreen = nullptr;
    } toggles;

    // Graceful shutdown hook (e.g. Ctrl+C in terminal)
    std::function<bool()> interrupt_requested;

    // Frame loop bookkeeping
    bool done = false;
    int frame_counter = 0;
    double last_input_s = 0.0;
    bool mouse_down_prev = false;
    std::unordered_set<std::string> applied_imgui_placements;

    // UX flows / timers
    bool quit_modal_open = false;
    bool quit_waiting_on_save = false;
    size_t quit_save_queue_index = 0;
    std::vector<int> quit_save_queue_ids;

    double autosave_last_s = 0.0;
};


