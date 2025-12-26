#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "ansl/ansl_native.h"
#include "ansl/ansl_script_engine.h"

#include "app/app_state.h"
#include "app/canvas_preview_texture.h"
#include "app/bitmap_glyph_atlas_texture.h"
#include "app/run_frame.h"
#include "app/vulkan_state.h"
#include "app/workspace.h"
#include "app/workspace_persist.h"

#include "core/embedded_assets.h"
#include "core/color_system.h"
#include "core/glyph_id.h"
#include "core/glyph_resolve.h"
#include "core/i18n.h"
#include "core/key_bindings.h"
#include "core/paths.h"

#include "io/io_manager.h"
#include "io/sdl_file_dialog_queue.h"
#include "io/session/session_state.h"

#include "ui/ansl_editor.h"
#include "ui/brush_palette_window.h"
#include "ui/character_palette.h"
#include "ui/character_picker.h"
#include "ui/character_set.h"
#include "ui/export_dialog.h"
#include "ui/image_to_chafa_dialog.h"
#include "ui/markdown_to_ansi_dialog.h"
#include "ui/image_window.h"
#include "ui/layer_manager.h"
#include "ui/minimap_window.h"
#include "ui/settings.h"
#include "ui/sixteen_colors_browser.h"
#include "ui/tool_palette.h"
#include "ui/skin.h"

// Set when we receive SIGINT (Ctrl+C) so the main loop can exit cleanly.
static volatile std::sig_atomic_t g_InterruptRequested = 0;

static void HandleInterruptSignal(int signal)
{
    if (signal == SIGINT)
        g_InterruptRequested = 1;
}

static void check_vk_result(VkResult err)
{
    if (err == VK_SUCCESS)
        return;
    std::fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
        std::abort();
}

int main(int, char**)
{
    // Arrange for Ctrl+C in the terminal to request a graceful shutdown instead
    // of abruptly killing the process (which can upset Vulkan/SDL).
    std::signal(SIGINT, HandleInterruptSignal);

    // Extract embedded default assets to the user's config directory on first run.
    {
        std::string err;
        if (!EnsureBundledAssetsExtracted(err))
            std::fprintf(stderr, "[assets] %s\n", err.c_str());
    }

    // Initialize ICU resource bundle i18n from extracted assets.
    {
        std::string err;
        if (!phos::i18n::Init(PhosphorAssetPath("i18n"), /*locale=*/"", err) && !err.empty())
            std::fprintf(stderr, "[i18n] %s\n", err.c_str());
    }

    // Load persisted session state (window geometry + tool window toggles).
    // If no user session exists yet, this may fall back to assets/session.json.
    SessionState session_state;
    {
        std::string err;
        if (!LoadSessionState(session_state, err) && !err.empty())
            std::fprintf(stderr, "[session] %s\n", err.c_str());
    }

    // Apply session preferences to the core LUT cache (global for now).
    phos::color::GetColorSystem().Luts().SetBudgetBytes(session_state.lut_cache_budget_bytes);

    // Setup SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        std::printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }

    // Create window with Vulkan graphics context
    const float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_WindowFlags window_flags =
        (SDL_WindowFlags)(SDL_WINDOW_VULKAN |
                          SDL_WINDOW_RESIZABLE |
                          SDL_WINDOW_HIDDEN |
                          SDL_WINDOW_HIGH_PIXEL_DENSITY);

    auto clamp_i = [](int v, int lo, int hi) -> int { return (v < lo) ? lo : (v > hi) ? hi : v; };
    int initial_w = (int)(1280 * main_scale);
    int initial_h = (int)(800 * main_scale);
    if (session_state.window_w > 0 && session_state.window_h > 0)
    {
        // Keep some sanity bounds so bad state can't create a 0px or enormous window.
        initial_w = clamp_i(session_state.window_w, 320, 16384);
        initial_h = clamp_i(session_state.window_h, 240, 16384);
    }

    SDL_Window* window = SDL_CreateWindow("Phosphor", initial_w, initial_h, window_flags);
    if (window == nullptr)
    {
        std::printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return 1;
    }

    ImVector<const char*> extensions;
    {
        uint32_t sdl_extensions_count = 0;
        const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extensions_count);
        for (uint32_t n = 0; n < sdl_extensions_count; n++)
            extensions.push_back(sdl_extensions[n]);
    }

    VulkanState vk;
    vk.SetupVulkan(extensions);

    // Create Window Surface
    VkSurfaceKHR surface;
    VkResult err;
    if (SDL_Vulkan_CreateSurface(window, vk.instance, vk.allocator, &surface) == 0)
    {
        std::printf("Failed to create Vulkan surface.\n");
        return 1;
    }

    // Create Framebuffers
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &vk.main_window;
    vk.SetupVulkanWindow(wd, surface, w, h);

    if (session_state.window_pos_valid)
        SDL_SetWindowPosition(window, session_state.window_x, session_state.window_y);
    else
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    // Restore fullscreen/maximized state (best-effort; may be denied by the WM).
    if (session_state.window_fullscreen)
        (void)SDL_SetWindowFullscreen(window, true);
    if (session_state.window_maximized)
        SDL_MaximizeWindow(window);

    SDL_ShowWindow(window);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    // Disable ImGui's ini persistence entirely.
    // We persist/restore window placements ourselves via SessionState (session.json).
    io.IniFilename = nullptr;

    // Load Unscii as the default UI font (mono, great for UTF‑8 art).
    // Keep an explicit handle so Unicode-only widgets can force a known-good Unicode font.
    if (ImFont* unscii = io.Fonts->AddFontFromFileTTF(PhosphorAssetPath("unscii-16-full.ttf").c_str(), 16.0f))
        io.FontDefault = unscii;

    // Setup Dear ImGui style (theme + HiDPI scaling).
    if (session_state.ui_theme.empty())
        session_state.ui_theme = ui::DefaultThemeId();
    ui::ApplyTheme(session_state.ui_theme.c_str(), main_scale);

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForVulkan(window);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = vk.instance;
    init_info.PhysicalDevice = vk.physical_device;
    init_info.Device = vk.device;
    init_info.QueueFamily = vk.queue_family;
    init_info.Queue = vk.queue;
    init_info.PipelineCache = vk.pipeline_cache;
    init_info.DescriptorPool = vk.descriptor_pool;
    init_info.MinImageCount = vk.min_image_count;
    init_info.ImageCount = wd->ImageCount;
    init_info.Allocator = vk.allocator;
    init_info.PipelineInfoMain.RenderPass = wd->RenderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    // ---------------------------------------------------------------------
    // App state / subsystems owned by main()
    // ---------------------------------------------------------------------

    bool show_demo_window = false;
    ImVec4 clear_color = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    bool show_color_picker_window = session_state.show_color_picker_window;
    bool show_character_picker_window = session_state.show_character_picker_window;
    bool show_character_palette_window = session_state.show_character_palette_window;
    bool show_character_sets_window = session_state.show_character_sets_window;
    bool show_layer_manager_window = session_state.show_layer_manager_window;
    bool show_ansl_editor_window = session_state.show_ansl_editor_window;
    bool show_tool_palette_window = session_state.show_tool_palette_window;
    bool show_brush_palette_window = session_state.show_brush_palette_window;
    bool show_minimap_window = session_state.show_minimap_window;
    bool show_settings_window = session_state.show_settings_window;
    bool show_16colors_browser_window = session_state.show_16colors_browser_window;
    bool window_fullscreen = session_state.window_fullscreen;

    SettingsWindow settings_window;
    settings_window.SetOpen(show_settings_window);
    settings_window.SetMainScale(main_scale);
    settings_window.SetLutCacheBudgetApplier([](size_t bytes) {
        phos::color::GetColorSystem().Luts().SetBudgetBytes(bytes);
    });

    ExportDialog export_dialog;

    kb::KeyBindingsEngine keybinds;
    keybinds.SetPath(PhosphorAssetPath("key-bindings.json"));
    {
        std::string kerr;
        (void)keybinds.LoadFromFile(keybinds.Path(), kerr);
    }
    settings_window.SetKeyBindingsEngine(&keybinds);

    // Shared color state for the xterm-256 color pickers.
    ImVec4 fg_color = ImVec4(session_state.xterm_color_picker.fg[0],
                             session_state.xterm_color_picker.fg[1],
                             session_state.xterm_color_picker.fg[2],
                             session_state.xterm_color_picker.fg[3]);
    ImVec4 bg_color = ImVec4(session_state.xterm_color_picker.bg[0],
                             session_state.xterm_color_picker.bg[1],
                             session_state.xterm_color_picker.bg[2],
                             session_state.xterm_color_picker.bg[3]);
    int active_fb = session_state.xterm_color_picker.active_fb;                     // 0 = foreground, 1 = background
    int xterm_picker_mode = session_state.xterm_color_picker.picker_mode;           // 0 = Hue Bar, 1 = Hue Wheel
    int xterm_selected_palette = session_state.xterm_color_picker.selected_palette;
    int xterm_picker_preview_fb = session_state.xterm_color_picker.picker_preview_fb; // 0 = fg, 1 = bg
    float xterm_picker_last_hue = session_state.xterm_color_picker.last_hue;

    // Workspace (documents)
    std::vector<std::unique_ptr<CanvasWindow>> canvases;
    canvases.reserve(128); // stable addresses (unique_ptr-owned)
    int next_canvas_id = 1;
    int last_active_canvas_id = -1;

    std::vector<ImageWindow> images;
    images.reserve(64);
    int next_image_id = 1;

    // Character picker state
    CharacterPicker character_picker;
    CharacterPalette character_palette;
    CharacterSetWindow character_sets;
    BrushPaletteWindow brush_palette;

    // Current brush glyph for tools (from picker/palette selection).
    std::uint32_t tool_brush_cp = character_picker.SelectedCodePoint();
    std::string tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
    std::uint32_t tool_brush_glyph = (std::uint32_t)phos::glyph::MakeUnicodeScalar((char32_t)tool_brush_cp);
    // Current attribute selection for tools (bitmask of AnsiCanvas::Attr_*). 0 = none.
    std::uint32_t tool_attrs_mask = 0;

    LayerManager layer_manager;

    AnslEditor ansl_editor;
    AnslScriptEngine ansl_engine;
    AnslScriptEngine tool_engine;
    {
        std::string aerr;
        if (!ansl_engine.Init(GetPhosphorAssetsDir(), aerr, &session_state.font_sanity_cache, true))
            std::fprintf(stderr, "[ansl] init failed: %s\n", aerr.c_str());
    }
    {
        std::string terr;
        // Reuse the same cache, but don't re-run the expensive validation pass.
        if (!tool_engine.Init(GetPhosphorAssetsDir(), terr, &session_state.font_sanity_cache, false))
            std::fprintf(stderr, "[tools] init failed: %s\n", terr.c_str());
    }

    // Restore ANSL editor persisted state (script text + dropdown selection + fps).
    ansl_editor.SetTargetFps(session_state.ansl_editor.target_fps);
    ansl_editor.SetSelectedExamplePreference(session_state.ansl_editor.selected_example_index,
                                             session_state.ansl_editor.selected_example_label,
                                             session_state.ansl_editor.selected_example_path);
    if (session_state.ansl_editor.text_valid)
        ansl_editor.SetText(session_state.ansl_editor.text);

    // Tool palette state
    ToolPalette tool_palette;
    std::string tools_error;
    std::string tool_compile_error;
    {
        std::string perr;
        if (!tool_palette.LoadFromDirectory(PhosphorAssetPath("tools"), perr))
            tools_error = perr;
    }
    if (!session_state.active_tool_path.empty())
        tool_palette.SetActiveToolByPath(session_state.active_tool_path);

    auto set_all_tool_actions = [&]() {
        std::vector<kb::Action> all;
        for (const ToolSpec& t : tool_palette.GetTools())
            for (const kb::Action& a : t.actions)
                all.push_back(a);
        keybinds.SetToolActions(std::move(all));
    };

    // Compile initial tool and seed keybinding tool actions.
    {
        std::string tool_path;
        if (tool_palette.TakeActiveToolChanged(tool_path))
        {
            std::ifstream in(tool_path, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string cerr;
            // No canvas is available at startup compile time; compile under the default palette.
            if (!tool_engine.CompileUserScript(src, /*canvas=*/nullptr, cerr))
                tool_compile_error = cerr;
            else
                tool_compile_error.clear();

            // Register tool actions for all tools (not just active) so keybindings UI is complete
            // and the host action router can route fallback actions deterministically.
            set_all_tool_actions();
        }
    }

    auto compile_tool_script = [&](const std::string& tool_path) {
        if (tool_path.empty())
            return;
        std::ifstream in(tool_path, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string cerr;
        // No canvas context here; runtime RunFrame() will rebind the active palette per-canvas.
        if (!tool_engine.CompileUserScript(src, /*canvas=*/nullptr, cerr))
            tool_compile_error = cerr;
        else
            tool_compile_error.clear();
        set_all_tool_actions();
    };

    // Tool history: stable ids, not filenames.
    std::vector<std::string> tool_stack;
    auto active_tool_id = [&]() -> std::string {
        if (const ToolSpec* t = tool_palette.GetActiveTool())
            return t->id;
        return {};
    };
    auto sync_tool_stack = [&]() {
        const std::string id = active_tool_id();
        if (id.empty())
            return;
        if (tool_stack.empty() || tool_stack.back() != id)
            tool_stack.push_back(id);
        const size_t kMax = 64;
        if (tool_stack.size() > kMax)
            tool_stack.erase(tool_stack.begin(), tool_stack.begin() + (tool_stack.size() - kMax));
    };
    sync_tool_stack();

    auto activate_tool_by_id = [&](const std::string& id) {
        if (id.empty())
            return;
        if (tool_palette.SetActiveToolById(id))
        {
            std::string changed_path;
            if (tool_palette.TakeActiveToolChanged(changed_path))
                compile_tool_script(changed_path);
            sync_tool_stack();
        }
    };
    auto activate_prev_tool = [&]() {
        if (tool_stack.size() >= 2)
        {
            tool_stack.pop_back();
            activate_tool_by_id(tool_stack.back());
            return;
        }
        activate_tool_by_id("edit");
    };

    ImageToChafaDialog image_to_chafa_dialog;
    MarkdownToAnsiDialog markdown_to_ansi_dialog;
    MinimapWindow minimap_window;
    CanvasPreviewTexture preview_texture;
    BitmapGlyphAtlasTextureCache bitmap_glyph_atlas;
    SixteenColorsBrowserWindow sixteen_browser;

    // Initialize the Vulkan-backed preview texture after the ImGui Vulkan backend is initialized.
    {
        CanvasPreviewTexture::InitInfo pi;
        pi.device = (void*)vk.device;
        pi.physical_device = (void*)vk.physical_device;
        pi.queue = (void*)vk.queue;
        pi.queue_family = vk.queue_family;
        pi.allocator = (void*)vk.allocator;
        (void)preview_texture.Init(pi);
    }

    // Initialize the Vulkan-backed bitmap glyph atlas cache (used by bitmap canvas fonts).
    {
        BitmapGlyphAtlasTextureCache::InitInfo gi;
        gi.device = (void*)vk.device;
        gi.physical_device = (void*)vk.physical_device;
        gi.queue = (void*)vk.queue;
        gi.queue_family = vk.queue_family;
        gi.allocator = (void*)vk.allocator;
        (void)bitmap_glyph_atlas.Init(gi);
        // Cache policy tuning from user session (mirrors LUT cache UX).
        bitmap_glyph_atlas.SetBudgetBytes(session_state.glyph_atlas_cache_budget_bytes);
        // Conservative deferred-destruction window (swapchain images in flight).
        bitmap_glyph_atlas.SetFramesInFlight((std::uint32_t)wd->ImageCount);
    }

    // Hook Settings UI controls to the live glyph atlas cache.
    settings_window.SetGlyphAtlasCacheBudgetApplier([&](size_t bytes) {
        bitmap_glyph_atlas.SetBudgetBytes(bytes);
    });
    settings_window.SetGlyphAtlasCacheUsedBytesGetter([&]() -> size_t {
        return bitmap_glyph_atlas.UsedBytes();
    });

    // SDL native file dialogs (async -> polled queue).
    SdlFileDialogQueue file_dialogs;

    // File IO (projects, import/export)
    IoManager io_manager;
    namespace fs = std::filesystem;
    if (!session_state.last_import_image_dir.empty())
        io_manager.SetLastDir(session_state.last_import_image_dir);
    else
    {
        try { io_manager.SetLastDir(fs::current_path().string()); }
        catch (...) { io_manager.SetLastDir("."); }
    }

    // Restore workspace content (open canvases + images).
    workspace_persist::RestoreWorkspaceFromSession(session_state, keybinds,
                                                  canvases, next_canvas_id, last_active_canvas_id,
                                                  images, next_image_id);

    // Seed the global tool brush glyph from the active canvas (per-canvas state),
    // and synchronize the picker/palette selections for a consistent startup state.
    {
        AnsiCanvas* ui_active_canvas = ResolveUiActiveCanvas(canvases, last_active_canvas_id);
        if (ui_active_canvas)
        {
            tool_brush_glyph = (std::uint32_t)ui_active_canvas->GetActiveGlyph();
            tool_brush_cp = (std::uint32_t)phos::glyph::ToUnicodeRepresentative((phos::GlyphId)tool_brush_glyph);
            if (tool_brush_cp == 0)
                tool_brush_cp = (std::uint32_t)U' ';
            tool_brush_utf8 = ui_active_canvas->GetActiveGlyphUtf8();
            if (tool_brush_utf8.empty())
                tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);

            character_picker.RestoreSelectedCodePoint(tool_brush_cp);
            character_palette.SyncSelectionFromActiveGlyph((phos::GlyphId)tool_brush_glyph, tool_brush_utf8, ui_active_canvas);
        }
    }

    // ---------------------------------------------------------------------
    // Main loop (per-frame logic lives in app::RunFrame)
    // ---------------------------------------------------------------------
    AppState st;
    st.platform.window = window;
    st.vulkan.vk = &vk;
    st.vulkan.wd = wd;
    st.persist.session_state = &session_state;
    st.services.keybinds = &keybinds;
    st.services.io_manager = &io_manager;
    st.services.file_dialogs = &file_dialogs;
    st.services.export_dialog = &export_dialog;
    st.services.settings_window = &settings_window;

    st.tools.tool_palette = &tool_palette;
    st.tools.tools_error = &tools_error;
    st.tools.tool_compile_error = &tool_compile_error;
    st.tools.ansl_editor = &ansl_editor;
    st.tools.ansl_engine = &ansl_engine;
    st.tools.tool_engine = &tool_engine;

    st.workspace.canvases = &canvases;
    st.workspace.next_canvas_id = &next_canvas_id;
    st.workspace.last_active_canvas_id = &last_active_canvas_id;
    st.workspace.images = &images;
    st.workspace.next_image_id = &next_image_id;

    st.ui.character_picker = &character_picker;
    st.ui.character_palette = &character_palette;
    st.ui.character_sets = &character_sets;
    st.ui.layer_manager = &layer_manager;
    st.ui.image_to_chafa_dialog = &image_to_chafa_dialog;
    st.ui.markdown_to_ansi_dialog = &markdown_to_ansi_dialog;
    st.ui.minimap_window = &minimap_window;
    st.ui.preview_texture = &preview_texture;
    st.ui.bitmap_glyph_atlas = &bitmap_glyph_atlas;
    st.ui.sixteen_browser = &sixteen_browser;
    st.ui.brush_palette_window = &brush_palette;

    st.tools.tool_brush_glyph = &tool_brush_glyph;
    st.tools.tool_brush_cp = &tool_brush_cp;
    st.tools.tool_brush_utf8 = &tool_brush_utf8;
    st.tools.tool_attrs_mask = &tool_attrs_mask;

    st.colors.clear_color = &clear_color;
    st.colors.fg_color = &fg_color;
    st.colors.bg_color = &bg_color;
    st.colors.active_fb = &active_fb;
    st.colors.xterm_picker_mode = &xterm_picker_mode;
    st.colors.xterm_selected_palette = &xterm_selected_palette;
    st.colors.xterm_picker_preview_fb = &xterm_picker_preview_fb;
    st.colors.xterm_picker_last_hue = &xterm_picker_last_hue;

    st.toggles.show_demo_window = &show_demo_window;
    st.toggles.show_color_picker_window = &show_color_picker_window;
    st.toggles.show_character_picker_window = &show_character_picker_window;
    st.toggles.show_character_palette_window = &show_character_palette_window;
    st.toggles.show_character_sets_window = &show_character_sets_window;
    st.toggles.show_layer_manager_window = &show_layer_manager_window;
    st.toggles.show_ansl_editor_window = &show_ansl_editor_window;
    st.toggles.show_tool_palette_window = &show_tool_palette_window;
    st.toggles.show_brush_palette_window = &show_brush_palette_window;
    st.toggles.show_minimap_window = &show_minimap_window;
    st.toggles.show_settings_window = &show_settings_window;
    st.toggles.show_16colors_browser_window = &show_16colors_browser_window;
    st.toggles.window_fullscreen = &window_fullscreen;

    st.tools.compile_tool_script = compile_tool_script;
    st.tools.sync_tool_stack = sync_tool_stack;
    st.tools.active_tool_id = active_tool_id;
    st.tools.activate_tool_by_id = activate_tool_by_id;
    st.tools.activate_prev_tool = activate_prev_tool;
    st.interrupt_requested = []() -> bool { return g_InterruptRequested != 0; };

    while (!st.done)
        app::RunFrame(st);

    // ---------------------------------------------------------------------
    // Shutdown / persistence
    // ---------------------------------------------------------------------
    workspace_persist::SaveSessionStateOnExit(session_state,
                                              window,
                                              io_manager,
                                              tool_palette,
                                              ansl_editor,
                                              show_color_picker_window,
                                              show_character_picker_window,
                                              show_character_palette_window,
                                              show_character_sets_window,
                                              show_layer_manager_window,
                                              show_ansl_editor_window,
                                              show_tool_palette_window,
                                              show_brush_palette_window,
                                              show_minimap_window,
                                              show_settings_window,
                                              show_16colors_browser_window,
                                              fg_color,
                                              bg_color,
                                              active_fb,
                                              xterm_picker_mode,
                                              xterm_selected_palette,
                                              xterm_picker_preview_fb,
                                              xterm_picker_last_hue,
                                              last_active_canvas_id,
                                              next_canvas_id,
                                              next_image_id,
                                              canvases,
                                              images);

    // Destroy preview texture before tearing down the ImGui Vulkan backend / Vulkan device.
    preview_texture.Shutdown();
    bitmap_glyph_atlas.Shutdown();

    // During a Ctrl+C shutdown the Vulkan device might already be in a bad
    // state; don't abort the whole process just because vkDeviceWaitIdle()
    // reports a non-success here.
    err = vkDeviceWaitIdle(vk.device);
    if (err != VK_SUCCESS)
        std::fprintf(stderr, "[vulkan] vkDeviceWaitIdle during shutdown: VkResult = %d (ignored)\n", err);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    vk.CleanupVulkanWindow();
    vk.CleanupVulkan();

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}


