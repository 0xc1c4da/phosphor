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

#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>  // Image loading (PNG/JPG/GIF/BMP/...)

#include "colour_picker.h"
#include "canvas.h"
#include "character_picker.h"
#include "character_palette.h"
#include "layer_manager.h"
#include "ansl_editor.h"
#include "ansl_script_engine.h"
#include "ansl_native.h"
#include "tool_palette.h"
#include "ansl_params_ui.h"
#include "xterm256_palette.h"
#include "io_manager.h"
#include "image_to_chafa_dialog.h"
#include "preview_window.h"

#include "file_dialog_tags.h"
#include "sdl_file_dialog_queue.h"

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

// ---------------------------------------------------------------------------
// Palette loading from assets/colours.json (nlohmann_json)
// ---------------------------------------------------------------------------

struct ColourPaletteDef
{
    std::string      title;
    std::vector<ImVec4> colors;
};

static bool HexToImVec4(const std::string& hex, ImVec4& out)
{
    std::string s = hex;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8)
        return false;

    auto to_u8 = [](const std::string& sub) -> int
    {
        return static_cast<int>(std::strtoul(sub.c_str(), nullptr, 16));
    };

    int r = to_u8(s.substr(0, 2));
    int g = to_u8(s.substr(2, 2));
    int b = to_u8(s.substr(4, 2));
    int a = 255;
    if (s.size() == 8)
        a = to_u8(s.substr(6, 2));

    out.x = r / 255.0f;
    out.y = g / 255.0f;
    out.z = b / 255.0f;
    out.w = a / 255.0f;
    return true;
}

static bool LoadColourPalettesFromJson(const char* path,
                                       std::vector<ColourPaletteDef>& out,
                                       std::string& error)
{
    using nlohmann::json;

    std::ifstream f(path);
    if (!f)
    {
        error = std::string("Failed to open ") + path;
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    if (!j.is_array())
    {
        error = "Expected top-level JSON array in colours.json";
        return false;
    }

    out.clear();
    for (const auto& item : j)
    {
        if (!item.is_object())
            continue;

        ColourPaletteDef def;
        if (auto it = item.find("title"); it != item.end() && it->is_string())
            def.title = it->get<std::string>();
        else
            continue;

        if (auto it = item.find("colors"); it != item.end() && it->is_array())
        {
            for (const auto& c : *it)
            {
                if (!c.is_string())
                    continue;
                ImVec4 col;
                if (HexToImVec4(c.get<std::string>(), col))
                    def.colors.push_back(col);
            }
        }

        if (!def.colors.empty())
            out.push_back(std::move(def));
    }

    if (out.empty())
    {
        error = "No valid palettes found in colours.json";
        return false;
    }

    error.clear();
    return true;
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
static void RenderImageWindowContents(const ImageWindow& image, ImageToChafaDialog& dialog)
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
        if (ImGui::MenuItem("Convert to ANSI..."))
        {
            ImageToChafaDialog::ImageRgba src;
            src.label = image.path;
            src.width = image.width;
            src.height = image.height;
            src.rowstride = image.width * 4;
            src.pixels.assign(image.pixels.begin(), image.pixels.end());
            dialog.Open(std::move(src));
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
        "Phosphor",
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
    bool   show_character_picker_window = true;
    bool   show_character_palette_window = true;
    bool   show_layer_manager_window = true;
    bool   show_ansl_editor_window = true;
    bool   show_tool_palette_window = true;
    bool   show_preview_window = true;

    // Shared color state for the xterm-256 color pickers.
    ImVec4 fg_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 bg_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    int    active_fb = 0;          // 0 = foreground, 1 = background
    int    xterm_picker_mode = 0;  // 0 = Hue Bar, 1 = Hue Wheel

    // Canvas state
    std::vector<CanvasWindow> canvases;
    int next_canvas_id = 1;
    int last_active_canvas_id = -1; // canvas window focus fallback for File actions

    // Character picker state
    CharacterPicker character_picker;

    // Character palette state
    CharacterPalette character_palette;

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

    // Import Image result state (native dialog)
    std::string import_image_error;
    std::string last_import_image_dir;
    try { last_import_image_dir = fs::current_path().string(); }
    catch (...) { last_import_image_dir = "."; }

    // File IO (projects, import/export)
    IoManager io_manager;

    // Main loop
    bool done = false;
    int frame_counter = 0;
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
                    io_manager.RenderFileMenu(window, file_dialogs, active_canvas, cbs);
                }

                if (ImGui::MenuItem("Import Image..."))
                {
                    import_image_error.clear();
                    std::vector<SdlFileDialogQueue::FilterPair> filters = {
                        {"Images (*.png;*.jpg;*.jpeg;*.gif;*.bmp)", "png;jpg;jpeg;gif;bmp"},
                        {"All files", "*"},
                    };
                    file_dialogs.ShowOpenFileDialog(kDialog_ImportImage, window, filters, last_import_image_dir, false);
                }

                if (ImGui::MenuItem("Quit"))
                {
                    done = true;
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

            SdlFileDialogResult r;
            while (file_dialogs.Poll(r))
            {
                if (r.tag == kDialog_ImportImage)
                {
                    if (!r.error.empty())
                    {
                        import_image_error = r.error;
                        continue;
                    }
                    if (r.canceled || r.paths.empty())
                        continue;

                    const std::string path = r.paths[0];
                    int iw = 0, ih = 0;
                    std::vector<unsigned char> rgba;
                    if (!LoadImageAsRgba32(path, iw, ih, rgba))
                    {
                        import_image_error = "Failed to load image.";
                        continue;
                    }

                    ImageWindow img;
                    img.id = next_image_id++;
                    img.path = path;
                    img.width = iw;
                    img.height = ih;
                    img.pixels = std::move(rgba);
                    img.open = true;
                    images.push_back(std::move(img));

                    if (path.find("://") == std::string::npos)
                    {
                        try
                        {
                            fs::path p(path);
                            if (p.has_parent_path())
                                last_import_image_dir = p.parent_path().string();
                        }
                        catch (...) {}
                    }
                }
                else
                {
                    io_manager.HandleDialogResult(r, active_canvas, cbs);
                }
            }
        }

        // File IO feedback (success/error).
        io_manager.RenderStatusWindows();

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

        // Import Image error reporting (native dialog).
        if (!import_image_error.empty())
        {
            ImGui::Begin("Import Image Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", import_image_error.c_str());
            ImGui::End();
        }

        // Optional: keep the ImGui demo available for reference
        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);

        // Unicode Character Picker window (ICU67 / Unicode 13)
        if (show_character_picker_window)
        {
            character_picker.Render("Unicode Character Picker", &show_character_picker_window);
        }

        // If the picker selection changed, update the palette's selected cell (replace or select).
        {
            uint32_t cp = 0;
            if (character_picker.TakeSelectionChanged(cp))
            {
                character_palette.OnPickerSelectedCodePoint(cp);
                tool_brush_cp = cp;
                tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
            }
        }

        // Character Palette window (loads assets/palettes.json via nlohmann_json)
        if (show_character_palette_window)
        {
            character_palette.Render("Character Palette", &show_character_palette_window);
        }

        // If the user clicked a glyph in the palette, navigate the picker to it.
        {
            uint32_t cp = 0;
            if (character_palette.TakeUserSelectionChanged(cp))
            {
                character_picker.JumpToCodePoint(cp);
                tool_brush_cp = cp;
                tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
            }
        }

        // Xterm-256 color picker showcase window with layout inspired by the ImGui demo.
        if (show_color_picker_window)
        {
            ImGui::Begin("Xterm-256 Color Picker", &show_color_picker_window, ImGuiWindowFlags_None);

            // Load palettes from assets/colours.json (with a default HSV fallback).
            static bool                         palettes_loaded    = false;
            static std::vector<ColourPaletteDef> palettes;
            static std::string                  palettes_error;
            static int                          selected_palette   = 0;
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
                    selected_palette = 0;
                }
            }

            if (!palettes_error.empty())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "Palette load error: %s", palettes_error.c_str());
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
                ImGui::XtermForegroundBackgroundWidget("🙿", fg_color, bg_color, active_fb);
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
            static int picker_preview_fb = 0;      // 0 = fg, 1 = bg
            static int last_active_fb_seen = 0;    // track focus changes
            if (active_fb != last_active_fb_seen)
            {
                picker_preview_fb = active_fb;
                last_active_fb_seen = active_fb;
            }

            ImVec4& preview_col = (picker_preview_fb == 0) ? fg_color : bg_color;
            float   picker_col[4] = { preview_col.x, preview_col.y, preview_col.z, preview_col.w };
            bool    value_changed = false;
            bool    used_right = false;
            if (xterm_picker_mode == 0)
                value_changed = ImGui::ColorPicker4_Xterm256_HueBar("##picker", picker_col, false, &used_right);
            else
                value_changed = ImGui::ColorPicker4_Xterm256_HueWheel("##picker", picker_col, false, &used_right);

            if (value_changed)
            {
                int dst_fb = used_right ? (1 - active_fb) : active_fb;
                picker_preview_fb = dst_fb;
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
                    ImGui::Combo("##Palette", &selected_palette, names.data(), (int)names.size());
                }
            }

            // Rebuild working palette when selection changes.
            if (selected_palette != last_palette_index && !palettes.empty())
            {
                saved_palette = palettes[selected_palette].colors;
                last_palette_index = selected_palette;
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
        }

        // Tool Palette window
        if (show_tool_palette_window)
        {
            const bool tool_palette_changed = tool_palette.Render("Tool Palette", &show_tool_palette_window);
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
                ImGui::Begin("Tool Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", tool_compile_error.c_str());
                ImGui::End();
            }

            if (!tools_error.empty())
            {
                ImGui::Begin("Tools Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", tools_error.c_str());
                ImGui::End();
            }

            // Tool parameters UI (settings.params -> ctx.params)
            if (tool_engine.HasParams())
            {
                ImGui::Begin("Tool Parameters", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
                const ToolSpec* t = tool_palette.GetActiveTool();
                if (t)
                    ImGui::Text("%s", t->label.c_str());
                ImGui::Separator();
                (void)RenderAnslParamsUI("tool_params", tool_engine);
                ImGui::End();
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

            ImGui::Begin(title.c_str(), &canvas.open, ImGuiWindowFlags_None);

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
        }

        // Layer Manager window (targets one of the open canvases)
        if (show_layer_manager_window)
        {
            std::vector<LayerManagerCanvasRef> refs;
            refs.reserve(canvases.size());
            for (auto& c : canvases)
            {
                if (!c.open)
                    continue;
                refs.push_back(LayerManagerCanvasRef{c.id, &c.canvas});
            }
            layer_manager.Render("Layer Manager", &show_layer_manager_window, refs);
        }

        // ANSL Editor window
        if (show_ansl_editor_window)
        {
            ImGui::Begin("ANSL Editor", &show_ansl_editor_window, ImGuiWindowFlags_None);
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

            RenderImageWindowContents(img, image_to_chafa_dialog);

            ImGui::End();
        }

        // Preview window for the active canvas (minimap + viewport rectangle).
        if (show_preview_window)
        {
            preview_window.Render("Preview", &show_preview_window, active_canvas);
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


