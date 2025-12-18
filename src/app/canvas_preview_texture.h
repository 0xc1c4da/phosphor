// Vulkan-backed minimap texture for PreviewWindow.
//
// This intentionally keeps Vulkan types out of the header so UI code can
// depend on the "view" (ImTextureID + size) without including Vulkan headers.
//
// Implementation lives in src/app/canvas_preview_texture.cpp.

#pragma once

#include <cstdint>

#include "imgui.h" // ImTextureID

class AnsiCanvas;

struct CanvasPreviewTextureView
{
    ImTextureID texture_id = (ImTextureID)0; // VkDescriptorSet in the Vulkan backend
    int         width = 0;
    int         height = 0;
    ImVec2      uv0 = ImVec2(0.0f, 0.0f);
    ImVec2      uv1 = ImVec2(1.0f, 1.0f);

    bool Valid() const { return texture_id != (ImTextureID)0 && width > 0 && height > 0; }
};

class CanvasPreviewTexture
{
public:
    struct InitInfo
    {
        // Dispatchable Vulkan handles from the app renderer.
        // (These are pointers in Vulkan headers, so void* is sufficient here.)
        void*    device = nullptr;          // VkDevice
        void*    physical_device = nullptr; // VkPhysicalDevice
        void*    queue = nullptr;           // VkQueue
        uint32_t queue_family = 0;
        void*    allocator = nullptr;       // VkAllocationCallbacks* (may be null)
    };

    CanvasPreviewTexture() = default;
    ~CanvasPreviewTexture();

    bool Init(const InitInfo& info, const char* debug_name = "CanvasPreviewTexture");
    void Shutdown();

    // Returns the current view (stable until the next Update() that resizes/recreates).
    CanvasPreviewTextureView View() const { return m_view; }

    // Update the minimap texture for `canvas` (if non-null).
    // - `max_dim` caps the larger side of the generated texture.
    // - `now_s` is wall time in seconds (used for throttling).
    void Update(AnsiCanvas* canvas, int max_dim, double now_s);

private:
    struct Impl;
    Impl* m = nullptr;
    CanvasPreviewTextureView m_view;
};


