// Vulkan-backed glyph atlas texture for bitmap (1bpp) canvas fonts.
//
// This keeps Vulkan types out of the header so core/ui code can depend on the provider
// interface via `AnsiCanvas::IBitmapGlyphAtlasProvider` without including Vulkan headers.
//
// Implementation lives in src/app/bitmap_glyph_atlas_texture.cpp.
// NOTE: This atlas is intended for bitmap fonts only (CP437 + embedded fonts).
// It provides a prebuilt RGBA atlas sampled with a NEAREST sampler for crisp pixels.

#pragma once

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

    // AnsiCanvas::IBitmapGlyphAtlasProvider
    bool GetBitmapGlyphAtlas(const AnsiCanvas& canvas, AnsiCanvas::BitmapGlyphAtlasView& out) override;

private:
    struct Impl;
    Impl* m = nullptr;
};


