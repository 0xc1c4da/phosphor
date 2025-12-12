// SDL3 + Vulkan + Dear ImGui boilerplate for utf8-art-editor.
// Based on references/imgui/examples/example_sdl3_vulkan/main.cpp,
// customised with a File menu and "New Canvas" windows.

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

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>  // Image loading (PNG/JPG/GIF/BMP/...)

#include "colour_picker.h"
#include "canvas.h"

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

// Simple representation of an imported image window.
struct ImageWindow
{
    bool        open   = true;
    int         id     = 0;
    std::string path;          // Original file path (for future ANSI conversion with chafa)

    // Raw pixel data owned by us: RGBA8, row-major, width * height * 4 bytes.
    int                       width  = 0;
    int                       height = 0;
    std::vector<unsigned char> pixels; // 4 bytes per pixel: R, G, B, A
};

// Load an image from disk into a RGBA8 buffer using stb_image:
//   - supports common formats (PNG, JPG, BMP, GIF, etc.)
//   - keeps design independent of Vulkan textures so we can later:
//       * feed the RGBA buffer into chafa for ANSI conversion
//       * optionally add a Vulkan texture path without changing higher-level UI code.
static bool LoadImageAsRgba32(const std::string& path,
                              int& out_width,
                              int& out_height,
                              std::vector<unsigned char>& out_pixels)
{
    int w = 0;
    int h = 0;
    int channels_in_file = 0;

    // Force 4 channels so we always get RGBA8.
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels_in_file, 4);
    if (!data)
    {
        std::fprintf(stderr, "Import Image: failed to load '%s': %s\n",
                     path.c_str(), stbi_failure_reason());
        return false;
    }

    if (w <= 0 || h <= 0)
    {
        stbi_image_free(data);
        return false;
    }

    out_width  = w;
    out_height = h;

    const size_t pixel_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    out_pixels.resize(pixel_bytes);
    std::memcpy(out_pixels.data(), data, pixel_bytes);

    stbi_image_free(data);
    return true;
}

// Render an ImageWindow's pixels scaled to fit the current ImGui window content region.
// We deliberately keep this renderer agnostic of Vulkan textures by drawing a coarse
// grid of colored rectangles that approximates the image. This is sufficient for a
// preview and keeps the RGBA buffer directly reusable for chafa-based ANSI conversion.
static void RenderImageWindowContents(const ImageWindow& image)
{
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty())
    {
        ImGui::TextUnformatted("No image data.");
        return;
    }

    const int img_w = image.width;
    const int img_h = image.height;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x <= 0.0f || avail.y <= 0.0f)
        return;

    float scale = std::min(avail.x / static_cast<float>(img_w),
                           avail.y / static_cast<float>(img_h));
    if (scale <= 0.0f)
        return;

    float draw_w = static_cast<float>(img_w) * scale;
    float draw_h = static_cast<float>(img_h) * scale;

    // Limit the grid resolution so we don't draw millions of rectangles for large images.
    const int max_grid_dim = 160;
    int grid_w = img_w;
    int grid_h = img_h;
    if (grid_w > max_grid_dim || grid_h > max_grid_dim)
    {
        if (img_w >= img_h)
        {
            grid_w = max_grid_dim;
            grid_h = std::max(1, static_cast<int>(static_cast<float>(img_h) *
                                                  (static_cast<float>(grid_w) / img_w)));
        }
        else
        {
            grid_h = max_grid_dim;
            grid_w = std::max(1, static_cast<int>(static_cast<float>(img_w) *
                                                  (static_cast<float>(grid_h) / img_h)));
        }
    }

    // Reserve an interactive region for future context menu / drag handling.
    ImGui::InvisibleButton("image_canvas", ImVec2(draw_w, draw_h));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetItemRectMin();

    // Right-click context menu hook for future "Convert to ANSI" action.
    if (ImGui::BeginPopupContextItem("image_canvas_context"))
    {
        // For now, just stub out the menu item so the UI contract is in place.
        if (ImGui::MenuItem("Convert to ANSI (not implemented yet)", nullptr, false, false))
        {
            // Placeholder: in a subsequent step we will:
            //  - run chafa on image.path or image.pixels
            //  - create a new CanvasWindow with the resulting ANSI data.
        }
        ImGui::EndPopup();
    }

    // Draw the scaled image as a coarse grid of filled rectangles.
    const float cell_w = draw_w / static_cast<float>(grid_w);
    const float cell_h = draw_h / static_cast<float>(grid_h);

    for (int gy = 0; gy < grid_h; ++gy)
    {
        float y0 = origin.y + gy * cell_h;
        float y1 = y0 + cell_h;

        // Sample source Y in original image space.
        int src_y = static_cast<int>((static_cast<float>(gy) + 0.5f) *
                                     (static_cast<float>(img_h) / grid_h));
        if (src_y < 0) src_y = 0;
        if (src_y >= img_h) src_y = img_h - 1;

        for (int gx = 0; gx < grid_w; ++gx)
        {
            float x0 = origin.x + gx * cell_w;
            float x1 = x0 + cell_w;

            int src_x = static_cast<int>((static_cast<float>(gx) + 0.5f) *
                                         (static_cast<float>(img_w) / grid_w));
            if (src_x < 0) src_x = 0;
            if (src_x >= img_w) src_x = img_w - 1;

            const size_t base = (static_cast<size_t>(src_y) * static_cast<size_t>(img_w) +
                                 static_cast<size_t>(src_x)) * 4u;
            if (base + 3 >= image.pixels.size())
                continue;

            unsigned char r = image.pixels[base + 0];
            unsigned char g = image.pixels[base + 1];
            unsigned char b = image.pixels[base + 2];
            unsigned char a = image.pixels[base + 3];

            ImU32 col = IM_COL32(r, g, b, a);
            draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
        }
    }
}

// Main code
int main(int, char**)
{
    // Arrange for Ctrl+C in the terminal to request a graceful shutdown instead
    // of abruptly killing the process (which can upset Vulkan/SDL).
    std::signal(SIGINT, HandleInterruptSignal);

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
    SDL_Window* window = SDL_CreateWindow(
        "utf8-art-editor",
        (int)(1280 * main_scale),
        (int)(800  * main_scale),
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
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

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
    bool   show_color_picker_window = true;

    // Shared color state for the xterm-256 color pickers.
    ImVec4 fg_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 bg_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    int    active_fb = 0;          // 0 = foreground, 1 = background
    int    xterm_picker_mode = 0;  // 0 = Hue Bar, 1 = Hue Wheel

    // Canvas state
    std::vector<CanvasWindow> canvases;
    int next_canvas_id = 1;

    // Image state
    std::vector<ImageWindow> images;
    int next_image_id = 1;

    // Import Image dialog state
    bool show_import_image_popup = false;
    std::string import_error;
    namespace fs = std::filesystem;
    fs::path current_import_dir = fs::current_path();
    std::string selected_import_name;

    // Main loop
    bool done = false;
    while (!done)
    {
        // If Ctrl+C was pressed in the terminal, break out of the render loop
        // and run the normal shutdown path below.
        if (g_InterruptRequested)
            break;

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
                }

                if (ImGui::MenuItem("Import Image..."))
                {
                    // Open our in-app file picker. We keep this entirely in ImGui so it works
                    // cross-platform and doesn't depend on external dialog libraries.
                    show_import_image_popup = true;
                    import_error.clear();
                    selected_import_name.clear();
                }

                if (ImGui::MenuItem("Quit"))
                {
                    done = true;
                }

                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // Import Image modal dialog
        if (show_import_image_popup)
        {
            ImGui::OpenPopup("Import Image");
            show_import_image_popup = false;
        }
        if (ImGui::BeginPopupModal("Import Image", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("Choose an image file to import.\n"
                               "Images are loaded via stb_image into an RGBA buffer, "
                               "which we can later feed into chafa for ANSI conversion.");

            ImGui::Separator();
            ImGui::Text("Directory: %s", current_import_dir.string().c_str());

            ImVec2 list_size(600.0f, 300.0f);
            if (ImGui::BeginChild("import_file_list", list_size, true, ImGuiWindowFlags_HorizontalScrollbar))
            {
                // ".." entry to go up one directory
                if (current_import_dir.has_parent_path())
                {
                    if (ImGui::Selectable("..", false))
                    {
                        current_import_dir = current_import_dir.parent_path();
                        selected_import_name.clear();
                    }
                }

                // Enumerate directory entries
                try
                {
                    for (const auto& entry : fs::directory_iterator(current_import_dir))
                    {
                        const fs::path& p = entry.path();
                        std::string name = p.filename().string();
                        if (name.empty())
                            continue;

                        if (entry.is_directory())
                        {
                            std::string label = "[dir] " + name + "/";
                            if (ImGui::Selectable(label.c_str(), false))
                            {
                                current_import_dir = p;
                                selected_import_name.clear();
                            }
                        }
                        else if (entry.is_regular_file())
                        {
                            // Basic filter for common image extensions; SDL currently only
                            // loads BMP here, but we keep the UI future-proof.
                            std::string ext = p.extension().string();
                            for (auto& c : ext)
                                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                            bool looks_like_image =
                                (ext == ".bmp" || ext == ".png" ||
                                 ext == ".jpg" || ext == ".jpeg" ||
                                 ext == ".gif");
                            if (!looks_like_image)
                                continue;

                            bool selected = (name == selected_import_name);
                            if (ImGui::Selectable(name.c_str(), selected))
                            {
                                selected_import_name = name;
                            }
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    import_error = e.what();
                }

                ImGui::EndChild();
            }

            if (!import_error.empty())
            {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", import_error.c_str());
            }

            ImGui::Separator();
            bool can_open = !selected_import_name.empty();
            if (!can_open)
                ImGui::BeginDisabled();
            if (ImGui::Button("Open"))
            {
                fs::path full_path = current_import_dir / selected_import_name;
                ImageWindow img;
                img.id = next_image_id++;
                img.path = full_path.string();

                int w = 0, h = 0;
                std::vector<unsigned char> rgba;
                if (!LoadImageAsRgba32(img.path, w, h, rgba))
                {
                    import_error = "Failed to load image.";
                }
                else
                {
                    img.width = w;
                    img.height = h;
                    img.pixels = std::move(rgba);
                    img.open = true;
                    images.push_back(std::move(img));
                    ImGui::CloseCurrentPopup();
                }
            }
            if (!can_open)
                ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        // Optional: keep the ImGui demo available for reference
        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);

        // Xterm-256 color picker showcase window with layout inspired by the ImGui demo.
        if (show_color_picker_window)
        {
            ImGui::Begin("Xterm-256 Color Picker", &show_color_picker_window, ImGuiWindowFlags_None);

            // Generate a default palette. The palette will persist and can be edited.
            static bool   saved_palette_init = true;
            static ImVec4 saved_palette[32] = {};
            if (saved_palette_init)
            {
                for (int n = 0; n < IM_ARRAYSIZE(saved_palette); n++)
                {
                    float h = n / 31.0f;
                    ImGui::ColorConvertHSVtoRGB(h, 0.8f, 0.8f,
                                                saved_palette[n].x,
                                                saved_palette[n].y,
                                                saved_palette[n].z);
                    saved_palette[n].w = 1.0f;
                }
                saved_palette_init = false;
            }

            static ImVec4 backup_color(1.0f, 0.0f, 0.0f, 1.0f);

            // Picker mode combo (Hue Bar / Hue Wheel)
            const char* picker_items[] = { "Hue Bar", "Hue Wheel" };
            ImGui::Combo("Mode", &xterm_picker_mode, picker_items, IM_ARRAYSIZE(picker_items));

            ImGui::Separator();

            // Foreground/background preview + selection
            ImGui::XtermForegroundBackgroundWidget("FG/BG", fg_color, bg_color, active_fb);

            // Layout: picker on the left, side preview + palette on the right.
            ImGui::BeginGroup();
            ImVec4& active_col = (active_fb == 0) ? fg_color : bg_color;
            ImVec4  before_edit = active_col;
            float   picker_col[4] = { active_col.x, active_col.y, active_col.z, active_col.w };
            bool    value_changed = false;
            if (xterm_picker_mode == 0)
                value_changed = ImGui::ColorPicker4_Xterm256_HueBar("##picker", picker_col, false);
            else
                value_changed = ImGui::ColorPicker4_Xterm256_HueWheel("##picker", picker_col, false);

            if (value_changed)
            {
                active_col.x = picker_col[0];
                active_col.y = picker_col[1];
                active_col.z = picker_col[2];
                active_col.w = picker_col[3];
            }
            ImGui::EndGroup();

            ImGui::SameLine();

            ImGui::BeginGroup(); // Lock X position
            ImGui::Text("Current");
            ImVec4 current_color = (active_fb == 0) ? fg_color : bg_color;
            ImGui::ColorButton("##current", current_color,
                               ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_AlphaPreviewHalf,
                               ImVec2(60, 40));
            ImGui::Text("Previous");
            if (ImGui::ColorButton("##previous", backup_color,
                                   ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_AlphaPreviewHalf,
                                   ImVec2(60, 40)))
            {
                if (active_fb == 0)
                    fg_color = backup_color;
                else
                    bg_color = backup_color;
            }

            if (value_changed)
                backup_color = before_edit;

            ImGui::Separator();
            ImGui::Text("Palette");
            for (int n = 0; n < IM_ARRAYSIZE(saved_palette); n++)
            {
                ImGui::PushID(n);
                if ((n % 8) != 0)
                    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.y);

                ImGuiColorEditFlags palette_button_flags =
                    ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip;
                if (ImGui::ColorButton("##palette", saved_palette[n],
                                       palette_button_flags, ImVec2(20, 20)))
                {
                    ImVec4& dst = (active_fb == 0) ? fg_color : bg_color;
                    dst.x = saved_palette[n].x;
                    dst.y = saved_palette[n].y;
                    dst.z = saved_palette[n].z;
                }

                // Allow user to drop colors into each palette entry.
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_COLOR_3F))
                        memcpy((float*)&saved_palette[n], payload->Data, sizeof(float) * 3);
                    if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_COLOR_4F))
                        memcpy((float*)&saved_palette[n], payload->Data, sizeof(float) * 4);
                    ImGui::EndDragDropTarget();
                }

                ImGui::PopID();
            }
            ImGui::EndGroup();

            ImGui::End();
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

            ImGui::Begin(title.c_str(), &canvas.open, ImGuiWindowFlags_None);

            // Each canvas gets its own unique ImGui ID for the canvas component.
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "canvas_%d", canvas.id);
            canvas.canvas.Render(id_buf);

            ImGui::End();
        }

        // Render each imported image window:
        for (size_t i = 0; i < images.size(); ++i)
        {
            ImageWindow& img = images[i];
            if (!img.open)
                continue;

            std::string title = "Image " + std::to_string(img.id) +
                                "##image" + std::to_string(img.id);

            ImGui::Begin(title.c_str(), &img.open, ImGuiWindowFlags_None);

            // Display basic metadata and then the scalable preview.
            ImGui::Text("Path: %s", img.path.c_str());
            ImGui::Text("Size: %dx%d", img.width, img.height);
            ImGui::Separator();

            RenderImageWindowContents(img);

            ImGui::End();
        }

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


