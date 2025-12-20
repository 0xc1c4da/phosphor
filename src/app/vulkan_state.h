#pragma once

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

// Thin wrapper around the Dear ImGui SDL3+Vulkan example backend code.
//
// Goal: keep `main.cpp` focused on app wiring + UI, while isolating the verbose Vulkan
// setup/swapchain/render/present/cleanup plumbing in a dedicated translation unit.
struct VulkanState
{
    // Vulkan debug
    //#define APP_USE_UNLIMITED_FRAME_RATE
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
    VkDebugReportCallbackEXT debug_report = VK_NULL_HANDLE;
#endif

    // Vulkan core
    VkAllocationCallbacks* allocator = nullptr;
    VkInstance             instance = VK_NULL_HANDLE;
    VkPhysicalDevice       physical_device = VK_NULL_HANDLE;
    VkDevice               device = VK_NULL_HANDLE;
    uint32_t               queue_family = (uint32_t)-1;
    VkQueue                queue = VK_NULL_HANDLE;
    VkPipelineCache        pipeline_cache = VK_NULL_HANDLE;
    VkDescriptorPool       descriptor_pool = VK_NULL_HANDLE;

    // ImGui helper window data
    ImGui_ImplVulkanH_Window main_window{};
    uint32_t                 min_image_count = 2;
    bool                     swapchain_rebuild = false;

    void SetupVulkan(ImVector<const char*> instance_extensions);
    void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height);
    void ResizeMainWindow(ImGui_ImplVulkanH_Window* wd, int width, int height);

    void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data);
    void FramePresent(ImGui_ImplVulkanH_Window* wd);

    void CleanupVulkanWindow();
    void CleanupVulkan();
};


