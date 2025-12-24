// Vulkan-backed glyph atlas texture for bitmap (1bpp) canvas fonts.
//
// This keeps Vulkan types out of the header so core/ui code can depend on the provider
// interface via `AnsiCanvas::IBitmapGlyphAtlasProvider` without including Vulkan headers.
//
// Implementation lives in src/app/bitmap_glyph_atlas_texture.cpp.
// NOTE: This atlas is intended for bitmap fonts only (CP437 + embedded fonts).
// It provides a prebuilt RGBA atlas sampled with a NEAREST sampler for crisp pixels.

#pragma once

#include <cstddef>
#include <cstdint>

#include "imgui.h" // ImTextureID

#include "core/canvas.h"

class BitmapGlyphAtlasTextureCache final : public AnsiCanvas::IBitmapGlyphAtlasProvider
{
public:
    struct InitInfo
    {
        void*    device = nullptr;          // VkDevice
        void*    physical_device = nullptr; // VkPhysicalDevice
        void*    queue = nullptr;           // VkQueue
        uint32_t queue_family = 0;
        void*    allocator = nullptr;       // VkAllocationCallbacks* (may be null)
    };

    BitmapGlyphAtlasTextureCache() = default;
    ~BitmapGlyphAtlasTextureCache();

    bool Init(const InitInfo& info, const char* debug_name = "BitmapGlyphAtlasTextureCache");
    void Shutdown();

    // Cache policy (budget is in bytes; 0 = unlimited).
    void SetBudgetBytes(std::size_t bytes);
    std::size_t BudgetBytes() const;
    std::size_t UsedBytes() const; // live GPU bytes (cached + deferred-free pending)

    // Deferred destruction safety. Should be set to swapchain images-in-flight (usually 2-3).
    void SetFramesInFlight(std::uint32_t n);
    std::uint32_t FramesInFlight() const;

    // Call once per rendered frame to advance the cache clock and collect deferred frees.
    void BeginFrame();

    // AnsiCanvas::IBitmapGlyphAtlasProvider
    bool GetBitmapGlyphAtlas(const AnsiCanvas& canvas, AnsiCanvas::BitmapGlyphAtlasView& out) override;

private:
    struct Impl;
    Impl* m = nullptr;
};


