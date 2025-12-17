#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // abort
#include <string.h>         // strcmp
#include <vector>
#include <string>
#include <filesystem>
#include <cstring>          // std::memcpy
#include <csignal>          // std::signal, std::sig_atomic_t
#include <fstream>          // std::ifstream
#include <algorithm>        // std::min
#include <iterator>
#include <unordered_set>
#include <nlohmann/json.hpp>

#include "ui/colour_picker.h"
#include "ui/colour_palette.h"
#include "core/canvas.h"
#include "ui/character_picker.h"
#include "ui/character_palette.h"
#include "ui/character_set.h"
#include "ui/layer_manager.h"
#include "ui/ansl_editor.h"
#include "ansl/ansl_script_engine.h"
#include "ansl/ansl_native.h"
#include "ui/tool_palette.h"
#include "ui/ansl_params_ui.h"
#include "core/xterm256_palette.h"
#include "io/io_manager.h"
#include "io/image_loader.h"
#include "ui/image_to_chafa_dialog.h"
#include "ui/preview_window.h"
#include "ui/settings.h"
#include "ui/image_window.h"
#include "ui/imgui_window_chrome.h"
#include "io/session/session_state.h"
#include "io/session/imgui_persistence.h"
#include "io/session/open_canvas_codec.h"
#include "io/session/open_canvas_codec.h"

#include "io/file_dialog_tags.h"
#include "io/sdl_file_dialog_queue.h"

// Vulkan debug
//#define APP_USE_UNLIMITED_FRAME_RATE
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
#endif

// Vulkan data
static VkAllocationCallbacks*   g_Allocator = nullptr;
static VkInstance               g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily = (uint32_t)-1;
static VkQueue                  g_Queue = VK_NULL_HANDLE;
static VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static uint32_t                 g_MinImageCount = 2;
static bool                     g_SwapChainRebuild = false;

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
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
        abort();
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(
    VkDebugReportFlagsEXT flags,
    VkDebugReportObjectTypeEXT objectType,
    uint64_t object,
    size_t location,
    int32_t messageCode,
    const char* pLayerPrefix,
    const char* pMessage,
    void* pUserData)
{
    (void)flags; (void)object; (void)location; (void)messageCode;
    (void)pUserData; (void)pLayerPrefix; // Unused arguments
    fprintf(stderr, "[vulkan] Debug report from ObjectType: %i\nMessage: %s\n\n", objectType, pMessage);
    return VK_FALSE;
}
#endif // APP_USE_VULKAN_DEBUG_REPORT

static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension)
{
    for (const VkExtensionProperties& p : properties)
        if (strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

static void SetupVulkan(ImVector<const char*> instance_extensions)
{
    VkResult err;

    // Create Vulkan Instance
    {
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        // Enumerate available extensions
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        check_vk_result(err);

        // Enable required extensions
        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        // Enabling validation layers
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        // Create Vulkan Instance
        create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);

        // Setup the debug report callback
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkCreateDebugReportCallbackEXT =
            (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT |
                                VK_DEBUG_REPORT_WARNING_BIT_EXT |
                                VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = debug_report;
        debug_report_ci.pUserData = nullptr;
        err = f_vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci, g_Allocator, &g_DebugReport);
        check_vk_result(err);
#endif
    }

    // Select Physical Device (GPU)
    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

    // Select graphics queue family
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);

    // Create Logical Device (with 1 queue)
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        // Enumerate physical device extension
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, properties.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;

        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = 1;
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;

        err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
    }

    // Create Descriptor Pool
    {
        VkDescriptorPoolSize pool_sizes[] =
        {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE },
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes)
            pool_info.maxSets += pool_size.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;

        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height)
{
    wd->Surface = surface;

    // Check for WSI support
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE)
    {
        fprintf(stderr, "Error no WSI support on physical device 0\n");
        exit(-1);
    }

    // Select Surface Format
    const VkFormat requestSurfaceImageFormat[] =
    {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,
        VK_FORMAT_R8G8B8_UNORM
    };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_PhysicalDevice, wd->Surface,
        requestSurfaceImageFormat,
        (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat),
        requestSurfaceColorSpace);

    // Select Present Mode
#ifdef APP_USE_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] =
    {
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
        VK_PRESENT_MODE_FIFO_KHR
    };
#else
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));

    // Create SwapChain, RenderPass, Framebuffer, etc.
    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator,
        width, height, g_MinImageCount, 0);
}

static void CleanupVulkan()
{
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    // Remove the debug report callback
    auto f_vkDestroyDebugReportCallbackEXT =
        (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
    f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif // APP_USE_VULKAN_DEBUG_REPORT

    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow()
{
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data)
{
    VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(
        g_Device, wd->Swapchain, UINT64_MAX,
        image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        check_vk_result(err);

        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    // Record dear imgui primitives into command buffer
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    // Submit command buffer
    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd)
{
    if (g_SwapChainRebuild)
        return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

// Simple representation of a "canvas" window.
struct CanvasWindow
{
    bool open = true;
    int  id   = 0;
    AnsiCanvas canvas;
};

// Main code
int main(int, char**)
{
    // Arrange for Ctrl+C in the terminal to request a graceful shutdown instead
    // of abruptly killing the process (which can upset Vulkan/SDL).
    std::signal(SIGINT, HandleInterruptSignal);

    // Load persisted session state (window geometry + tool window toggles).
    SessionState session_state;
    {
        std::string err;
        if (!LoadSessionState(session_state, err) && !err.empty())
            std::fprintf(stderr, "[session] %s\n", err.c_str());
    }

    // Setup SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }

    // Create window with Vulkan graphics context
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_WindowFlags window_flags =
        (SDL_WindowFlags)(SDL_WINDOW_VULKAN |
                          SDL_WINDOW_RESIZABLE |
                          SDL_WINDOW_HIDDEN |
                          SDL_WINDOW_HIGH_PIXEL_DENSITY);

    auto clamp_i = [](int v, int lo, int hi) -> int { return (v < lo) ? lo : (v > hi) ? hi : v; };
    int initial_w = (int)(1280 * main_scale);
    int initial_h = (int)(800  * main_scale);
    if (session_state.window_w > 0 && session_state.window_h > 0)
    {
        // Keep some sanity bounds so bad state can't create a 0px or enormous window.
        initial_w = clamp_i(session_state.window_w, 320, 16384);
        initial_h = clamp_i(session_state.window_h, 240, 16384);
    }

    SDL_Window* window = SDL_CreateWindow(
        "Phosphor",
        initial_w,
        initial_h,
        window_flags);
    if (window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return 1;
    }

    ImVector<const char*> extensions;
    {
        uint32_t sdl_extensions_count = 0;
        const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extensions_count);
        for (uint32_t n = 0; n < sdl_extensions_count; n++)
            extensions.push_back(sdl_extensions[n]);
    }
    SetupVulkan(extensions);

    // Create Window Surface
    VkSurfaceKHR surface;
    VkResult err;
    if (SDL_Vulkan_CreateSurface(window, g_Instance, g_Allocator, &surface) == 0)
    {
        printf("Failed to create Vulkan surface.\n");
        return 1;
    }

    // Create Framebuffers
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, w, h);

    if (session_state.window_pos_valid)
        SDL_SetWindowPosition(window, session_state.window_x, session_state.window_y);
    else
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    if (session_state.window_maximized)
        SDL_MaximizeWindow(window);

    SDL_ShowWindow(window);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    // Disable ImGui's ini persistence entirely.
    // We persist/restore window placements ourselves via SessionState (session.json).
    io.IniFilename = nullptr;

    // Load Unscii as the default font (mono, great for UTF‑8 art).
    // Relative to the working directory (project root when running ./utf8-art-editor).
    io.Fonts->AddFontFromFileTTF("assets/unscii-16-full.ttf", 16.0f);

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup scaling
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForVulkan(window);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.Allocator = g_Allocator;
    init_info.PipelineInfoMain.RenderPass = wd->RenderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    // Our state
    bool show_demo_window = false;
    ImVec4 clear_color = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    bool   show_color_picker_window = session_state.show_color_picker_window;
    bool   show_character_picker_window = session_state.show_character_picker_window;
    bool   show_character_palette_window = session_state.show_character_palette_window;
    bool   show_character_sets_window = session_state.show_character_sets_window;
    bool   show_layer_manager_window = session_state.show_layer_manager_window;
    bool   show_ansl_editor_window = session_state.show_ansl_editor_window;
    bool   show_tool_palette_window = session_state.show_tool_palette_window;
    bool   show_preview_window = session_state.show_preview_window;
    bool   show_settings_window = session_state.show_settings_window;
    SettingsWindow settings_window;
    settings_window.SetOpen(show_settings_window);

    // Shared color state for the xterm-256 color pickers.
    ImVec4 fg_color = ImVec4(session_state.xterm_color_picker.fg[0],
                             session_state.xterm_color_picker.fg[1],
                             session_state.xterm_color_picker.fg[2],
                             session_state.xterm_color_picker.fg[3]);
    ImVec4 bg_color = ImVec4(session_state.xterm_color_picker.bg[0],
                             session_state.xterm_color_picker.bg[1],
                             session_state.xterm_color_picker.bg[2],
                             session_state.xterm_color_picker.bg[3]);
    int    active_fb = session_state.xterm_color_picker.active_fb;           // 0 = foreground, 1 = background
    int    xterm_picker_mode = session_state.xterm_color_picker.picker_mode; // 0 = Hue Bar, 1 = Hue Wheel
    int    xterm_selected_palette = session_state.xterm_color_picker.selected_palette;
    int    xterm_picker_preview_fb = session_state.xterm_color_picker.picker_preview_fb; // 0 = fg, 1 = bg
    float  xterm_picker_last_hue = session_state.xterm_color_picker.last_hue;

    // Canvas state
    std::vector<CanvasWindow> canvases;
    int next_canvas_id = 1;
    int last_active_canvas_id = -1; // canvas window focus fallback for File actions

    // Character picker state
    CharacterPicker character_picker;

    // Character palette state
    CharacterPalette character_palette;

    // Character sets (F-key presets) state
    CharacterSetWindow character_sets;

    // Current brush glyph for tools (from picker/palette selection).
    uint32_t    tool_brush_cp = character_picker.SelectedCodePoint();
    std::string tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);

    // Layer manager state
    LayerManager layer_manager;

    // ANSL editor state
    AnslEditor ansl_editor;
    AnslScriptEngine ansl_engine;
    AnslScriptEngine tool_engine;
    {
        std::string err;
        // Initialize the embedded LuaJIT scripting engine and register the native `ansl` module.
        if (!ansl_engine.Init("assets", err))
            fprintf(stderr, "[ansl] init failed: %s\n", err.c_str());
    }
    {
        std::string err;
        if (!tool_engine.Init("assets", err))
            fprintf(stderr, "[tools] init failed: %s\n", err.c_str());
    }

    // Tool palette state
    ToolPalette tool_palette;
    std::string tools_error;
    std::string tool_compile_error;
    {
        std::string err;
        if (!tool_palette.LoadFromDirectory("assets/tools", err))
            tools_error = err;
    }
    // Restore last selected tool (if any).
    if (!session_state.active_tool_path.empty())
    {
        tool_palette.SetActiveToolByPath(session_state.active_tool_path);
    }
    {
        std::string tool_path;
        if (tool_palette.TakeActiveToolChanged(tool_path))
        {
            std::ifstream in(tool_path, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string err;
            if (!tool_engine.CompileUserScript(src, err))
                tool_compile_error = err;
            else
                tool_compile_error.clear();
        }
    }

    // Image state
    std::vector<ImageWindow> images;
    int next_image_id = 1;

    // Image -> ANSI (Chafa) conversion dialog
    ImageToChafaDialog image_to_chafa_dialog;

    // Canvas preview (minimap)
    PreviewWindow preview_window;

    namespace fs = std::filesystem;

    // SDL native file dialogs (async -> polled queue).
    SdlFileDialogQueue file_dialogs;

    // File IO (projects, import/export)
    IoManager io_manager;
    if (!session_state.last_import_image_dir.empty())
        io_manager.SetLastDir(session_state.last_import_image_dir);
    else
    {
        try { io_manager.SetLastDir(fs::current_path().string()); }
        catch (...) { io_manager.SetLastDir("."); }
    }

    // Restore workspace content (open canvases + images).
    if (session_state.next_canvas_id > 0)
        next_canvas_id = session_state.next_canvas_id;
    if (session_state.next_image_id > 0)
        next_image_id = session_state.next_image_id;
    last_active_canvas_id = session_state.last_active_canvas_id;

    // NOTE: OpenCanvas project-state (CBOR->zstd->base64) encoding/decoding is owned by io/session.

    // Restore canvases.
    for (const auto& oc : session_state.open_canvases)
    {
        CanvasWindow cw;
        cw.open = oc.open;
        cw.id = (oc.id > 0) ? oc.id : next_canvas_id++;

        AnsiCanvas::ProjectState ps;
        std::string derr;
        if (open_canvas_codec::DecodeProjectState(oc, ps, derr))
        {
            std::string apply_err;
            if (!cw.canvas.SetProjectState(ps, apply_err))
                std::fprintf(stderr, "[session] restore canvas %d: %s\n", cw.id, apply_err.c_str());
        }
        else if (!oc.project_cbor_zstd_b64.empty())
        {
            std::fprintf(stderr, "[session] restore canvas %d: %s\n", cw.id, derr.c_str());
        }

        cw.canvas.SetZoom(oc.zoom);
        cw.canvas.RequestScrollPixels(oc.scroll_x, oc.scroll_y);

        const int restored_id = cw.id;
        canvases.push_back(std::move(cw));
        next_canvas_id = std::max(next_canvas_id, restored_id + 1);
    }

    // Restore images (paths only; pixels reloaded).
    for (const auto& oi : session_state.open_images)
    {
        ImageWindow img;
        img.open = oi.open;
        img.id = (oi.id > 0) ? oi.id : next_image_id++;
        img.path = oi.path;
        if (!img.path.empty())
        {
            int iw = 0, ih = 0;
            std::vector<unsigned char> rgba;
            std::string ierr;
            if (image_loader::LoadImageAsRgba32(img.path, iw, ih, rgba, ierr))
            {
                img.width = iw;
                img.height = ih;
                img.pixels = std::move(rgba);
            }
        }
        images.push_back(std::move(img));
        next_image_id = std::max(next_image_id, images.back().id + 1);
    }

    // Main loop
    bool done = false;
    int frame_counter = 0;
    std::unordered_set<std::string> applied_imgui_placements;
    auto should_apply_placement = [&](const char* window_name) -> bool
    {
        if (!window_name || !*window_name)
            return false;
        return applied_imgui_placements.emplace(window_name).second;
    };
    while (!done)
    {
        // If Ctrl+C was pressed in the terminal, break out of the render loop
        // and run the normal shutdown path below.
        if (g_InterruptRequested)
            break;

        // Some platforms (e.g. Linux portals) may require pumping events for dialogs.
        SDL_PumpEvents();

        // Poll and handle events
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                done = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window))
                done = true;
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        // Resize swap chain?
        int fb_width, fb_height;
        SDL_GetWindowSize(window, &fb_width, &fb_height);
        if (fb_width > 0 && fb_height > 0 &&
            (g_SwapChainRebuild ||
             g_MainWindowData.Width != fb_width ||
             g_MainWindowData.Height != fb_height))
        {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(
                g_Instance, g_PhysicalDevice, g_Device, wd,
                g_QueueFamily, g_Allocator,
                fb_width, fb_height, g_MinImageCount, 0);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }

        // Start the Dear ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        frame_counter++;

        // Determine which canvas should receive keyboard-only actions (Undo/Redo shortcuts).
        // "Focused" is tracked by each AnsiCanvas instance (grid focus).
        AnsiCanvas* focused_canvas = nullptr;
        for (auto& c : canvases)
        {
            if (c.open && c.canvas.HasFocus())
            {
                focused_canvas = &c.canvas;
                last_active_canvas_id = c.id;
                break;
            }
        }
        // Active canvas for global actions (File menu, Edit menu items, future actions):
        // - prefer the focused grid canvas
        // - otherwise use the last active canvas window
        // - otherwise fall back to the first open canvas
        AnsiCanvas* active_canvas = focused_canvas;
        if (!focused_canvas && last_active_canvas_id != -1)
        {
            for (auto& c : canvases)
            {
                if (c.open && c.id == last_active_canvas_id)
                {
                    active_canvas = &c.canvas;
                    break;
                }
            }
        }
        if (!active_canvas)
        {
            for (auto& c : canvases)
            {
                if (c.open)
                {
                    active_canvas = &c.canvas;
                    break;
                }
            }
        }

        // Main menu bar: File > New Canvas, Quit
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New Canvas"))
                {
                    CanvasWindow canvas_window;
                    canvas_window.open = true;
                    canvas_window.id = next_canvas_id++;

                    // Load test.ans into the new canvas. If it fails, the canvas starts empty.
                    canvas_window.canvas.SetColumns(80);
                    canvas_window.canvas.LoadFromFile("test.ans");

                    canvases.push_back(canvas_window);
                    last_active_canvas_id = canvas_window.id;
                }

                // Project IO + import/export (handled by IoManager).
                {
                    IoManager::Callbacks cbs;
                    cbs.create_canvas = [&](AnsiCanvas&& c)
                    {
                        CanvasWindow canvas_window;
                        canvas_window.open = true;
                        canvas_window.id = next_canvas_id++;
                        canvas_window.canvas = std::move(c);
                        canvases.push_back(std::move(canvas_window));
                        last_active_canvas_id = canvas_window.id;
                    };
                    cbs.create_image = [&](IoManager::Callbacks::LoadedImage&& li)
                    {
                        ImageWindow img;
                        img.id = next_image_id++;
                        img.path = std::move(li.path);
                        img.width = li.width;
                        img.height = li.height;
                        img.pixels = std::move(li.pixels);
                        img.open = true;
                        images.push_back(std::move(img));
                    };
                    io_manager.RenderFileMenu(window, file_dialogs, active_canvas, cbs);
                }

                if (ImGui::MenuItem("Quit"))
                {
                    done = true;
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Settings..."))
                {
                    show_settings_window = true;
                    settings_window.SetOpen(true);
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit"))
            {
                // Use the active canvas so clicking the menu bar doesn't make Undo/Redo unavailable.
                const bool can_undo = active_canvas && active_canvas->CanUndo();
                const bool can_redo = active_canvas && active_canvas->CanRedo();

                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, can_undo))
                    active_canvas->Undo();
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, can_redo))
                    active_canvas->Redo();

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Window"))
            {
                ImGui::MenuItem("Xterm-256 Color Picker", nullptr, &show_color_picker_window);
                ImGui::MenuItem("Unicode Character Picker", nullptr, &show_character_picker_window);
                ImGui::MenuItem("Character Palette", nullptr, &show_character_palette_window);
                ImGui::MenuItem("Character Sets", nullptr, &show_character_sets_window);
                ImGui::MenuItem("Layer Manager", nullptr, &show_layer_manager_window);
                ImGui::MenuItem("ANSL Editor", nullptr, &show_ansl_editor_window);
                ImGui::MenuItem("Tool Palette", nullptr, &show_tool_palette_window);
                ImGui::MenuItem("Preview", nullptr, &show_preview_window);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // Dispatch completed native file dialogs (projects, import/export, image import).
        {
            IoManager::Callbacks cbs;
            cbs.create_canvas = [&](AnsiCanvas&& c)
            {
                CanvasWindow canvas_window;
                canvas_window.open = true;
                canvas_window.id = next_canvas_id++;
                canvas_window.canvas = std::move(c);
                canvases.push_back(std::move(canvas_window));
                last_active_canvas_id = canvas_window.id;
            };
            cbs.create_image = [&](IoManager::Callbacks::LoadedImage&& li)
            {
                ImageWindow img;
                img.id = next_image_id++;
                img.path = std::move(li.path);
                img.width = li.width;
                img.height = li.height;
                img.pixels = std::move(li.pixels);
                img.open = true;
                images.push_back(std::move(img));
            };

            SdlFileDialogResult r;
            while (file_dialogs.Poll(r))
            {
                io_manager.HandleDialogResult(r, active_canvas, cbs);
            }
        }

        // File IO feedback (success/error).
        io_manager.RenderStatusWindows(&session_state, should_apply_placement("File Error"));

        // Keyboard shortcuts for Undo/Redo (only when a canvas is focused).
        if (focused_canvas)
        {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl)
            {
                // Ctrl+Z = Undo, Ctrl+Shift+Z = Redo (common convention), Ctrl+Y = Redo.
                if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
                {
                    if (io.KeyShift)
                        focused_canvas->Redo();
                    else
                        focused_canvas->Undo();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
                    focused_canvas->Redo();
            }
        }

        // Optional: keep the ImGui demo available for reference
        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);

        // Unicode Character Picker window (ICU67 / Unicode 13)
        if (show_character_picker_window)
        {
            const char* name = "Unicode Character Picker";
            character_picker.Render(name, &show_character_picker_window,
                                    &session_state, should_apply_placement(name));
        }

        // If the picker selection changed, update the palette's selected cell (replace or select).
        {
            uint32_t cp = 0;
            if (character_picker.TakeSelectionChanged(cp))
            {
                character_palette.OnPickerSelectedCodePoint(cp);
                character_sets.OnExternalSelectedCodePoint(cp);
                tool_brush_cp = cp;
                tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
            }
        }

        // Character Palette window (loads assets/palettes.json via nlohmann_json)
        if (show_character_palette_window)
        {
            const char* name = "Character Palette";
            character_palette.Render(name, &show_character_palette_window,
                                     &session_state, should_apply_placement(name));
        }

        // If the user clicked a glyph in the palette, navigate the picker to it.
        {
            uint32_t cp = 0;
            if (character_palette.TakeUserSelectionChanged(cp))
            {
                character_picker.JumpToCodePoint(cp);
                character_sets.OnExternalSelectedCodePoint(cp);
                tool_brush_cp = cp;
                tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
            }
        }

        // Character Sets window (F-key presets)
        if (show_character_sets_window)
        {
            const char* name = "Character Sets";
            character_sets.Render(name, &show_character_sets_window,
                                  &session_state, should_apply_placement(name));
        }

        // Centralized "insert a codepoint at the caret" helper (shared by picker/palette + character sets + hotkeys).
        auto insert_cp_into_canvas = [&](AnsiCanvas* dst, uint32_t cp)
        {
            if (!dst)
                return;
            if (cp == 0)
                return;

            int caret_x = 0;
            int caret_y = 0;
            dst->GetCaretCell(caret_x, caret_y);

            // Create an undo boundary before mutating so Undo restores the previous state.
            dst->PushUndoSnapshot();

            const int layer_index = dst->GetActiveLayerIndex();
            dst->SetLayerCell(layer_index, caret_y, caret_x, (char32_t)cp);

            // Advance caret like a simple editor (wrap to next row).
            const int cols = dst->GetColumns();
            int nx = caret_x + 1;
            int ny = caret_y;
            if (cols > 0 && nx >= cols)
            {
                nx = 0;
                ny = caret_y + 1;
            }
            dst->SetCaretCell(nx, ny);
        };

        // Hotkeys for character sets:
        // - F1..F12 inserts from active set
        // - Ctrl+1..9,0 inserts slots 1..10 (F1..F10)
        if (focused_canvas)
        {
            ImGuiIO& io = ImGui::GetIO();
            if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            {
                // F-keys: only when no modifiers (to keep room for future "switch set" modifiers).
                if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift && !io.KeySuper)
                {
                    for (int i = 0; i < 12; ++i)
                    {
                        if (ImGui::IsKeyPressed((ImGuiKey)((int)ImGuiKey_F1 + i), false))
                        {
                            const uint32_t cp = character_sets.GetSlotCodePoint(i);
                            insert_cp_into_canvas(focused_canvas, cp);
                        }
                    }
                }

                // Ctrl+digits: map Ctrl+1..9 to slots 0..8, Ctrl+0 to slot 9.
                if (io.KeyCtrl && !io.KeyAlt && !io.KeyShift && !io.KeySuper)
                {
                    for (int d = 0; d <= 9; ++d)
                    {
                        const ImGuiKey k = (ImGuiKey)((int)ImGuiKey_0 + d);
                        if (!ImGui::IsKeyPressed(k, false))
                            continue;
                        const int slot = (d == 0) ? 9 : (d - 1);
                        const uint32_t cp = character_sets.GetSlotCodePoint(slot);
                        insert_cp_into_canvas(focused_canvas, cp);
                    }
                }
            }
        }

        // Double-click in picker/palette inserts the glyph into the active canvas at the caret.
        {
            uint32_t cp = 0;
            const bool dbl =
                character_picker.TakeDoubleClicked(cp) ||
                character_palette.TakeUserDoubleClicked(cp);
            if (dbl)
                insert_cp_into_canvas(active_canvas, cp);
        }

        // Double-click in the Character Sets window inserts the mapped glyph into the active canvas.
        {
            uint32_t cp = 0;
            if (character_sets.TakeInsertRequested(cp))
                insert_cp_into_canvas(active_canvas, cp);
        }

        // Xterm-256 color picker showcase window with layout inspired by the ImGui demo.
        if (show_color_picker_window)
        {
            const char* name = "Xterm-256 Color Picker";
            ApplyImGuiWindowPlacement(session_state, name, should_apply_placement(name));
            const ImGuiWindowFlags flags =
                ImGuiWindowFlags_None | GetImGuiWindowChromeExtraFlags(session_state, name);
            const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, name);
            ImGui::Begin("Xterm-256 Color Picker", &show_color_picker_window, flags);
            CaptureImGuiWindowPlacement(session_state, name);
            ApplyImGuiWindowChromeZOrder(&session_state, name);
            RenderImGuiWindowChromeMenu(&session_state, name);

            // Load palettes from assets/colours.json (with a default HSV fallback).
            static bool                         palettes_loaded    = false;
            static std::vector<ColourPaletteDef> palettes;
            static std::string                  palettes_error;
            static int                          last_palette_index = -1;
            static std::vector<ImVec4>          saved_palette;

            if (!palettes_loaded)
            {
                LoadColourPalettesFromJson("assets/colours.json", palettes, palettes_error);
                palettes_loaded = true;

                // Fallback if loading failed or file empty: single default HSV palette.
                if (!palettes_error.empty() || palettes.empty())
                {
                    ColourPaletteDef def;
                    def.title = "Default HSV";
                    for (int n = 0; n < 32; ++n)
                    {
                        ImVec4 c;
                        float h = n / 31.0f;
                        ImGui::ColorConvertHSVtoRGB(h, 0.8f, 0.8f, c.x, c.y, c.z);
                        c.w = 1.0f;
                        def.colors.push_back(c);
                    }
                    palettes.clear();
                    palettes.push_back(std::move(def));
                    palettes_error.clear();
                    xterm_selected_palette = 0;
                }
            }

            if (!palettes_error.empty())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "Palette load error: %s", palettes_error.c_str());
            }

            // Clamp palette selection after palettes are known.
            if (!palettes.empty())
            {
                if (xterm_selected_palette < 0 || xterm_selected_palette >= (int)palettes.size())
                    xterm_selected_palette = 0;
            }

            // Foreground / Background selector at the top (centered).
            {
                float sz     = ImGui::GetFrameHeight() * 2.0f;
                float offset = sz * 0.35f;
                float pad    = 2.0f;
                float widget_width = sz + offset + pad;

                float avail = ImGui::GetContentRegionAvail().x;
                float indent = (avail > widget_width) ? (avail - widget_width) * 0.5f : 0.0f;

                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                const bool fb_widget_changed =
                    ImGui::XtermForegroundBackgroundWidget("🙿", fg_color, bg_color, active_fb);
                if (fb_widget_changed)
                    xterm_picker_preview_fb = active_fb;
            }

            ImGui::Separator();

            // Picker mode combo (Hue Bar / Hue Wheel) and picker UI
            const char* picker_items[] = { "Hue Bar", "Hue Wheel" };
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::Combo("##Mode", &xterm_picker_mode, picker_items, IM_ARRAYSIZE(picker_items));

            ImGui::Separator();

            ImGui::BeginGroup();
            ImGui::SetNextItemWidth(-FLT_MIN);
            // Keep the picker reticle showing the last-edited color (left edits active, right edits the other),
            // so right-click doesn't "snap back" to the last left-click position next frame.
            ImVec4& preview_col = (xterm_picker_preview_fb == 0) ? fg_color : bg_color;
            float   picker_col[4] = { preview_col.x, preview_col.y, preview_col.z, preview_col.w };
            bool    value_changed = false;
            bool    used_right = false;
            if (xterm_picker_mode == 0)
                value_changed = ImGui::ColorPicker4_Xterm256_HueBar("##picker", picker_col, false, &used_right, &xterm_picker_last_hue);
            else
                value_changed = ImGui::ColorPicker4_Xterm256_HueWheel("##picker", picker_col, false, &used_right, &xterm_picker_last_hue);

            if (value_changed)
            {
                int dst_fb = used_right ? (1 - active_fb) : active_fb;
                xterm_picker_preview_fb = dst_fb;
                ImVec4& dst = (dst_fb == 0) ? fg_color : bg_color;
                dst.x = picker_col[0];
                dst.y = picker_col[1];
                dst.z = picker_col[2];
                dst.w = picker_col[3];
            }
            ImGui::EndGroup();

            ImGui::Separator();

            // Palette selection combo
            {
                std::vector<const char*> names;
                names.reserve(palettes.size());
                for (const auto& p : palettes)
                    names.push_back(p.title.c_str());

                if (!names.empty())
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::Combo("##Palette", &xterm_selected_palette, names.data(), (int)names.size());
                }
            }

            // Rebuild working palette when selection changes.
            if (xterm_selected_palette != last_palette_index && !palettes.empty())
            {
                saved_palette = palettes[xterm_selected_palette].colors;
                last_palette_index = xterm_selected_palette;
            }

            ImGui::BeginGroup();

            const ImGuiStyle& style = ImGui::GetStyle();
            ImVec2 avail = ImGui::GetContentRegionAvail();
            const int count = (int)saved_palette.size();

            // Adaptive grid: pick columns and button size so the palette fits in the
            // available region, maximizing button size while respecting width/height.
            int   best_cols = 1;
            float best_size = 0.0f;

            if (count > 0 && avail.x > 0.0f)
            {
                for (int cols = 1; cols <= count; ++cols)
                {
                    float total_spacing_x = style.ItemSpacing.x * (cols - 1);
                    float width_limit = (avail.x - total_spacing_x) / (float)cols;
                    if (width_limit <= 0.0f)
                        break;

                    int rows = (count + cols - 1) / cols;

                    float button_size = width_limit;
                    if (avail.y > 0.0f)
                    {
                        float total_spacing_y = style.ItemSpacing.y * (rows - 1);
                        float height_limit = (avail.y - total_spacing_y) / (float)rows;
                        if (height_limit <= 0.0f)
                            continue;
                        // Use the smaller of width/height limits so we fit in both directions.
                        button_size = std::min(width_limit, height_limit);
                    }

                    if (button_size > best_size)
                    {
                        best_size = button_size;
                        best_cols = cols;
                    }
                }

                if (best_size <= 0.0f)
                {
                    best_cols = 1;
                    best_size = style.FramePadding.y * 2.0f + 8.0f; // minimal fallback
                }
            }

            const int cols = (count > 0) ? best_cols : 1;
            ImVec2 button_size(best_size, best_size);

            ImVec4& pal_primary   = (active_fb == 0) ? fg_color : bg_color;
            ImVec4& pal_secondary = (active_fb == 0) ? bg_color : fg_color;

            for (int n = 0; n < count; n++)
            {
                ImGui::PushID(n);
                if (n % cols != 0)
                    ImGui::SameLine(0.0f, style.ItemSpacing.y);

                ImGuiColorEditFlags palette_button_flags =
                    ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip;
                bool left_clicked = ImGui::ColorButton("##palette", saved_palette[n],
                                                       palette_button_flags, button_size);
                if (left_clicked)
                {
                    pal_primary.x = saved_palette[n].x;
                    pal_primary.y = saved_palette[n].y;
                    pal_primary.z = saved_palette[n].z;
                }

                if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                {
                    pal_secondary.x = saved_palette[n].x;
                    pal_secondary.y = saved_palette[n].y;
                    pal_secondary.z = saved_palette[n].z;
                }

                // // Allow user to drop colors into each palette entry.
                // if (ImGui::BeginDragDropTarget())
                // {
                //     if (const ImGuiPayload* payload =
                //             ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_COLOR_3F))
                //         memcpy((float*)&saved_palette[n], payload->Data, sizeof(float) * 3);
                //     if (const ImGuiPayload* payload =
                //             ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_COLOR_4F))
                //         memcpy((float*)&saved_palette[n], payload->Data, sizeof(float) * 4);
                //     ImGui::EndDragDropTarget();
                // }

                ImGui::PopID();
            }

            ImGui::EndGroup();

            ImGui::End();
            PopImGuiWindowChromeAlpha(alpha_pushed);
        }

        // Tool Palette window
        if (show_tool_palette_window)
        {
            const char* name = "Tool Palette";
            const bool tool_palette_changed =
                tool_palette.Render(name, &show_tool_palette_window,
                                    &session_state, should_apply_placement(name));
            (void)tool_palette_changed;

            if (tool_palette.TakeReloadRequested())
            {
                std::string err;
                if (!tool_palette.LoadFromDirectory(tool_palette.GetToolsDir().empty() ? "assets/tools" : tool_palette.GetToolsDir(), err))
                    tools_error = err;
                else
                    tools_error.clear();
            }

            std::string tool_path;
            if (tool_palette.TakeActiveToolChanged(tool_path))
            {
                std::ifstream in(tool_path, std::ios::binary);
                const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::string err;
                if (!tool_engine.CompileUserScript(src, err))
                    tool_compile_error = err;
                else
                    tool_compile_error.clear();
            }

            if (!tool_compile_error.empty())
            {
                const char* wname = "Tool Error";
                ApplyImGuiWindowPlacement(session_state, wname, should_apply_placement(wname));
                const ImGuiWindowFlags flags =
                    ImGuiWindowFlags_AlwaysAutoResize | GetImGuiWindowChromeExtraFlags(session_state, wname);
                const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, wname);
                ImGui::Begin("Tool Error", nullptr, flags);
                CaptureImGuiWindowPlacement(session_state, wname);
                ApplyImGuiWindowChromeZOrder(&session_state, wname);
                RenderImGuiWindowChromeMenu(&session_state, wname);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", tool_compile_error.c_str());
                ImGui::End();
                PopImGuiWindowChromeAlpha(alpha_pushed);
            }

            if (!tools_error.empty())
            {
                const char* wname = "Tools Error";
                ApplyImGuiWindowPlacement(session_state, wname, should_apply_placement(wname));
                const ImGuiWindowFlags flags =
                    ImGuiWindowFlags_AlwaysAutoResize | GetImGuiWindowChromeExtraFlags(session_state, wname);
                const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, wname);
                ImGui::Begin("Tools Error", nullptr, flags);
                CaptureImGuiWindowPlacement(session_state, wname);
                ApplyImGuiWindowChromeZOrder(&session_state, wname);
                RenderImGuiWindowChromeMenu(&session_state, wname);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", tools_error.c_str());
                ImGui::End();
                PopImGuiWindowChromeAlpha(alpha_pushed);
            }

            // Tool parameters UI (settings.params -> ctx.params)
            if (tool_engine.HasParams())
            {
                const char* wname = "Tool Parameters";
                ApplyImGuiWindowPlacement(session_state, wname, should_apply_placement(wname));
                const ImGuiWindowFlags flags =
                    ImGuiWindowFlags_AlwaysAutoResize | GetImGuiWindowChromeExtraFlags(session_state, wname);
                const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, wname);
                ImGui::Begin("Tool Parameters", nullptr, flags);
                CaptureImGuiWindowPlacement(session_state, wname);
                ApplyImGuiWindowChromeZOrder(&session_state, wname);
                RenderImGuiWindowChromeMenu(&session_state, wname);
                const ToolSpec* t = tool_palette.GetActiveTool();
                if (t)
                    ImGui::Text("%s", t->label.c_str());
                ImGui::Separator();
                (void)RenderAnslParamsUI("tool_params", tool_engine);
                ImGui::End();
                PopImGuiWindowChromeAlpha(alpha_pushed);
            }
        }

        // Render each canvas window:
        //  - Draggable/movable
        //  - Resizable
        //  - Collapsible (minimisable) via the title bar arrow
        for (size_t i = 0; i < canvases.size(); ++i)
        {
            CanvasWindow& canvas = canvases[i];
            if (!canvas.open)
                continue;

            std::string title = "Canvas " + std::to_string(canvas.id) +
                                "##canvas" + std::to_string(canvas.id);

            ApplyImGuiWindowPlacement(session_state, title.c_str(), should_apply_placement(title.c_str()));
            const ImGuiWindowFlags flags =
                ImGuiWindowFlags_None | GetImGuiWindowChromeExtraFlags(session_state, title.c_str());
            const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, title.c_str());
            ImGui::Begin(title.c_str(), &canvas.open, flags);
            CaptureImGuiWindowPlacement(session_state, title.c_str());
            ApplyImGuiWindowChromeZOrder(&session_state, title.c_str());
            RenderImGuiWindowChromeMenu(&session_state, title.c_str());

            // Track last active canvas window even if the canvas grid itself isn't focused.
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                last_active_canvas_id = canvas.id;

            // Each canvas gets its own unique ImGui ID for the canvas component.
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "canvas_%d", canvas.id);

            // Run active tool script during canvas render (keyboard phase + mouse phase).
            auto to_idx = [](const ImVec4& c) -> int {
                const int r = (int)std::lround(c.x * 255.0f);
                const int g = (int)std::lround(c.y * 255.0f);
                const int b = (int)std::lround(c.z * 255.0f);
                return xterm256::NearestIndex((std::uint8_t)std::clamp(r, 0, 255),
                                             (std::uint8_t)std::clamp(g, 0, 255),
                                             (std::uint8_t)std::clamp(b, 0, 255));
            };
            const int fg_idx = to_idx(fg_color);
            const int bg_idx = to_idx(bg_color);

            auto tool_runner = [&](AnsiCanvas& c, int phase) {
                if (!tool_engine.HasRenderFunction())
                    return;

                AnslFrameContext ctx;
                ctx.cols = c.GetColumns();
                ctx.rows = c.GetRows();
                ctx.frame = frame_counter;
                ctx.time = ImGui::GetTime() * 1000.0; // ms
                ctx.metrics_aspect = c.GetLastCellAspect();
                ctx.phase = phase;
                ctx.focused = c.HasFocus();
                ctx.fg = fg_idx;
                ctx.bg = bg_idx;
                ctx.brush_utf8 = tool_brush_utf8;
                ctx.brush_cp = (int)tool_brush_cp;
                ctx.allow_caret_writeback = true;

                c.GetCaretCell(ctx.caret_x, ctx.caret_y);

                int cx = 0, cy = 0, px = 0, py = 0;
                bool l = false, r = false, pl = false, pr = false;
                ctx.cursor_valid = c.GetCursorCell(cx, cy, l, r, px, py, pl, pr);
                ctx.cursor_x = cx;
                ctx.cursor_y = cy;
                ctx.cursor_left_down = l;
                ctx.cursor_right_down = r;
                ctx.cursor_px = px;
                ctx.cursor_py = py;
                ctx.cursor_prev_left_down = pl;
                ctx.cursor_prev_right_down = pr;

                std::vector<char32_t> typed;
                if (phase == 0)
                {
                    // Keyboard phase consumes typed + key presses.
                    c.TakeTypedCodepoints(typed);
                    ctx.typed = &typed;

                    const auto keys = c.TakeKeyEvents();
                    ctx.key_left = keys.left;
                    ctx.key_right = keys.right;
                    ctx.key_up = keys.up;
                    ctx.key_down = keys.down;
                    ctx.key_home = keys.home;
                    ctx.key_end = keys.end;
                    ctx.key_backspace = keys.backspace;
                    ctx.key_delete = keys.del;
                    ctx.key_enter = keys.enter;

                    // Extra tool shortcut keys.
                    ctx.key_c = keys.c;
                    ctx.key_v = keys.v;
                    ctx.key_x = keys.x;
                    ctx.key_a = keys.a;
                    ctx.key_escape = keys.escape;

                    // Modifier state.
                    ImGuiIO& io = ImGui::GetIO();
                    ctx.mod_ctrl = io.KeyCtrl;
                    ctx.mod_shift = io.KeyShift;
                    ctx.mod_alt = io.KeyAlt;
                    ctx.mod_super = io.KeySuper;
                }

                std::string err;
                if (!tool_engine.RunFrame(c, c.GetActiveLayerIndex(), ctx, false, err))
                {
                    // Don't spam stderr every frame; stash message for UI.
                    tool_compile_error = err;
                }
            };

            canvas.canvas.Render(id_buf, tool_runner);

            ImGui::End();
            PopImGuiWindowChromeAlpha(alpha_pushed);
        }

        // Layer Manager window (targets one of the open canvases)
        if (show_layer_manager_window)
        {
            const char* name = "Layer Manager";
            std::vector<LayerManagerCanvasRef> refs;
            refs.reserve(canvases.size());
            for (auto& c : canvases)
            {
                if (!c.open)
                    continue;
                refs.push_back(LayerManagerCanvasRef{c.id, &c.canvas});
            }
            layer_manager.Render(name, &show_layer_manager_window, refs,
                                 &session_state, should_apply_placement(name));
        }

        // ANSL Editor window
        if (show_ansl_editor_window)
        {
            const char* name = "ANSL Editor";
            ApplyImGuiWindowPlacement(session_state, name, should_apply_placement(name));
            const ImGuiWindowFlags flags =
                ImGuiWindowFlags_None | GetImGuiWindowChromeExtraFlags(session_state, name);
            const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, name);
            ImGui::Begin("ANSL Editor", &show_ansl_editor_window, flags);
            CaptureImGuiWindowPlacement(session_state, name);
            ApplyImGuiWindowChromeZOrder(&session_state, name);
            RenderImGuiWindowChromeMenu(&session_state, name);
            std::vector<LayerManagerCanvasRef> refs;
            refs.reserve(canvases.size());
            for (auto& c : canvases)
            {
                if (!c.open)
                    continue;
                refs.push_back(LayerManagerCanvasRef{c.id, &c.canvas});
            }
            auto to_idx = [](const ImVec4& c) -> int {
                const int r = (int)std::lround(c.x * 255.0f);
                const int g = (int)std::lround(c.y * 255.0f);
                const int b = (int)std::lround(c.z * 255.0f);
                return xterm256::NearestIndex((std::uint8_t)std::clamp(r, 0, 255),
                                             (std::uint8_t)std::clamp(g, 0, 255),
                                             (std::uint8_t)std::clamp(b, 0, 255));
            };
            const int fg_idx = to_idx(fg_color);
            const int bg_idx = to_idx(bg_color);
            ansl_editor.Render("ansl_editor", refs, ansl_engine, fg_idx, bg_idx, ImGuiInputTextFlags_AllowTabInput);
            ImGui::End();
            PopImGuiWindowChromeAlpha(alpha_pushed);
        }

        // Render each imported image window:
        for (size_t i = 0; i < images.size(); ++i)
        {
            ImageWindow& img = images[i];
            if (!img.open)
                continue;

            std::string title = "Image " + std::to_string(img.id) +
                                "##image" + std::to_string(img.id);

            RenderImageWindow(title.c_str(), img, image_to_chafa_dialog,
                              &session_state, should_apply_placement(title.c_str()));
        }

        // Preview window for the active canvas (minimap + viewport rectangle).
        if (show_preview_window)
        {
            const char* name = "Preview";
            preview_window.Render(name, &show_preview_window, active_canvas,
                                  &session_state, should_apply_placement(name));
        }

        // Settings window (extendable tabs; includes Key Bindings editor).
        if (show_settings_window)
        {
            const char* name = "Settings";
            settings_window.SetOpen(show_settings_window);
            settings_window.Render(name, &session_state, should_apply_placement(name));
            show_settings_window = settings_window.IsOpen();
        }

        // Chafa conversion dialog (may create a new canvas on accept).
        image_to_chafa_dialog.Render();
        {
            AnsiCanvas converted;
            if (image_to_chafa_dialog.TakeAccepted(converted))
            {
                CanvasWindow canvas_window;
                canvas_window.open = true;
                canvas_window.id = next_canvas_id++;
                canvas_window.canvas = std::move(converted);
                canvases.push_back(std::move(canvas_window));
                last_active_canvas_id = canvases.back().id;
            }
        }

        // Enforce pinned z-order globally as the final UI step, so pinned-front windows
        // win over focus-induced "bring to front" behavior from normal windows.
        ApplyImGuiWindowChromeGlobalZOrder(session_state);

        // Rendering
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized =
            (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!is_minimized)
        {
            wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
            wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
            wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
            wd->ClearValue.color.float32[3] = clear_color.w;
            FrameRender(wd, draw_data);
            FramePresent(wd);
        }

    }

    // Cleanup
        // Persist session state (main window geometry + tool window visibility toggles).
        {
            SessionState st = session_state; // start from loaded defaults

            int sw = 0, sh = 0;
            SDL_GetWindowSize(window, &sw, &sh);
            st.window_w = sw;
            st.window_h = sh;

            int sx = 0, sy = 0;
            SDL_GetWindowPosition(window, &sx, &sy);
            st.window_x = sx;
            st.window_y = sy;
            st.window_pos_valid = true;

            const SDL_WindowFlags wf = SDL_GetWindowFlags(window);
            st.window_maximized = (wf & SDL_WINDOW_MAXIMIZED) != 0;

            st.show_color_picker_window = show_color_picker_window;
            st.show_character_picker_window = show_character_picker_window;
            st.show_character_palette_window = show_character_palette_window;
            st.show_character_sets_window = show_character_sets_window;
            st.show_layer_manager_window = show_layer_manager_window;
            st.show_ansl_editor_window = show_ansl_editor_window;
            st.show_tool_palette_window = show_tool_palette_window;
            st.show_preview_window = show_preview_window;
            st.show_settings_window = show_settings_window;

            // Xterm-256 picker UI state
            st.xterm_color_picker.fg[0] = fg_color.x;
            st.xterm_color_picker.fg[1] = fg_color.y;
            st.xterm_color_picker.fg[2] = fg_color.z;
            st.xterm_color_picker.fg[3] = fg_color.w;
            st.xterm_color_picker.bg[0] = bg_color.x;
            st.xterm_color_picker.bg[1] = bg_color.y;
            st.xterm_color_picker.bg[2] = bg_color.z;
            st.xterm_color_picker.bg[3] = bg_color.w;
            st.xterm_color_picker.active_fb = active_fb;
            st.xterm_color_picker.picker_mode = xterm_picker_mode;
            st.xterm_color_picker.selected_palette = xterm_selected_palette;
            st.xterm_color_picker.picker_preview_fb = xterm_picker_preview_fb;
            st.xterm_color_picker.last_hue = xterm_picker_last_hue;

            st.last_import_image_dir = io_manager.GetLastDir();

            // Active tool
            if (const ToolSpec* t = tool_palette.GetActiveTool())
                st.active_tool_path = t->path;

            // Canvas/image workspace
            st.last_active_canvas_id = last_active_canvas_id;
            st.next_canvas_id = next_canvas_id;
            st.next_image_id = next_image_id;

            st.open_canvases.clear();
            st.open_canvases.reserve(canvases.size());
            for (const auto& cw : canvases)
            {
                SessionState::OpenCanvas oc;
                oc.id = cw.id;
                oc.open = cw.open;
                oc.zoom = cw.canvas.GetZoom();
                const auto& vs = cw.canvas.GetLastViewState();
                if (vs.valid)
                {
                    oc.scroll_x = vs.scroll_x;
                    oc.scroll_y = vs.scroll_y;
                }

                std::string enc_err;
                if (!open_canvas_codec::EncodeProjectState(cw.canvas.GetProjectState(), oc, enc_err))
                {
                    std::fprintf(stderr, "[session] encode canvas %d failed: %s\n", cw.id, enc_err.c_str());
                }

                st.open_canvases.push_back(std::move(oc));
            }

            st.open_images.clear();
            st.open_images.reserve(images.size());
            for (const auto& im : images)
            {
                SessionState::OpenImage oi;
                oi.id = im.id;
                oi.open = im.open;
                oi.path = im.path;
                if (!oi.path.empty())
                    st.open_images.push_back(std::move(oi));
            }

            std::string err;
            if (!SaveSessionState(st, err) && !err.empty())
                std::fprintf(stderr, "[session] save failed: %s\n", err.c_str());
        }

    // During a Ctrl+C shutdown the Vulkan device might already be in a bad
    // state; don't abort the whole process just because vkDeviceWaitIdle()
    // reports a non-success here.
    err = vkDeviceWaitIdle(g_Device);
    if (err != VK_SUCCESS)
        fprintf(stderr, "[vulkan] vkDeviceWaitIdle during shutdown: VkResult = %d (ignored)\n", err);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    CleanupVulkanWindow();
    CleanupVulkan();

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}


