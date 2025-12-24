#include "app/canvas_preview_texture.h"

#include "core/canvas.h"
#include "core/color_system.h"
#include "core/fonts.h"
#include "core/glyph_resolve.h"

// ImGui Vulkan backend texture registration.
#include "imgui_impl_vulkan.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
static uint32_t FindMemoryType(VkPhysicalDevice phys, uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
    {
        if ((type_filter & (1u << i)) &&
            ((mem_props.memoryTypes[i].propertyFlags & properties) == properties))
            return i;
    }
    return UINT32_MAX;
}

static VkResult CreateBuffer(VkDevice device,
                             const VkAllocationCallbacks* allocator,
                             VkPhysicalDevice phys,
                             VkDeviceSize size,
                             VkBufferUsageFlags usage,
                             VkMemoryPropertyFlags mem_props,
                             VkBuffer& out_buf,
                             VkDeviceMemory& out_mem)
{
    out_buf = VK_NULL_HANDLE;
    out_mem = VK_NULL_HANDLE;

    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult err = vkCreateBuffer(device, &bi, allocator, &out_buf);
    if (err != VK_SUCCESS)
        return err;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, out_buf, &req);
    const uint32_t mem_type = FindMemoryType(phys, req.memoryTypeBits, mem_props);
    if (mem_type == UINT32_MAX)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type;
    err = vkAllocateMemory(device, &ai, allocator, &out_mem);
    if (err != VK_SUCCESS)
        return err;

    err = vkBindBufferMemory(device, out_buf, out_mem, 0);
    return err;
}

static VkResult CreateImageRGBA8(VkDevice device,
                                const VkAllocationCallbacks* allocator,
                                VkPhysicalDevice phys,
                                int width,
                                int height,
                                VkImage& out_img,
                                VkDeviceMemory& out_mem,
                                VkImageView& out_view)
{
    out_img = VK_NULL_HANDLE;
    out_mem = VK_NULL_HANDLE;
    out_view = VK_NULL_HANDLE;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = (uint32_t)std::max(1, width);
    ii.extent.height = (uint32_t)std::max(1, height);
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult err = vkCreateImage(device, &ii, allocator, &out_img);
    if (err != VK_SUCCESS)
        return err;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, out_img, &req);
    const uint32_t mem_type = FindMemoryType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type;
    err = vkAllocateMemory(device, &ai, allocator, &out_mem);
    if (err != VK_SUCCESS)
        return err;

    err = vkBindImageMemory(device, out_img, out_mem, 0);
    if (err != VK_SUCCESS)
        return err;

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out_img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = 1;
    err = vkCreateImageView(device, &vi, allocator, &out_view);
    return err;
}

} // namespace

struct CanvasPreviewTexture::Impl
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    const VkAllocationCallbacks* allocator = nullptr;

    VkCommandPool upload_pool = VK_NULL_HANDLE;
    VkFence upload_fence = VK_NULL_HANDLE;

    VkSampler sampler = VK_NULL_HANDLE;

    static constexpr int kSlots = 3; // triple-buffer to avoid overwrite while GPU is sampling previous frames
    struct Slot
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_mem = VK_NULL_HANDLE;
        VkImageView image_view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkImageLayout image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    Slot slots[kSlots];
    int slot_next = 0;

    int backing_dim = 0; // backing texture is square: backing_dim x backing_dim

    const AnsiCanvas* last_canvas = nullptr;
    uint64_t last_canvas_rev = 0;
    int last_w = 0;
    int last_h = 0;
    int slot_current = 0;
    double last_upload_s = -1.0;
    const char* debug_name = "CanvasPreviewTexture";

    // Cache last known *base* cell aspect (unscaled font metrics).
    // This keeps preview dimensions stable even if ViewState isn't valid yet.
    float last_base_aspect = 0.0f;

    bool InitUploadObjects()
    {
        // Command pool for one-off transfers.
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = queue_family;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VkResult err = vkCreateCommandPool(device, &pci, allocator, &upload_pool);
        if (err != VK_SUCCESS)
            return false;

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        err = vkCreateFence(device, &fci, allocator, &upload_fence);
        if (err != VK_SUCCESS)
            return false;

        // Sampler used for the minimap.
        //
        // Use NEAREST so the minimap stays crisp when scaled (no blur).
        // We encode per-cell details into the texture itself (see rasterizer below).
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.minLod = 0.0f;
        sci.maxLod = 0.0f;
        sci.maxAnisotropy = 1.0f;
        err = vkCreateSampler(device, &sci, allocator, &sampler);
        if (err != VK_SUCCESS)
            return false;

        return true;
    }

    void DestroyTextureObjects()
    {
        for (int i = 0; i < kSlots; ++i)
        {
            Slot& s = slots[i];
            if (s.descriptor_set != VK_NULL_HANDLE)
            {
                ImGui_ImplVulkan_RemoveTexture(s.descriptor_set);
                s.descriptor_set = VK_NULL_HANDLE;
            }
            if (s.image_view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(device, s.image_view, allocator);
                s.image_view = VK_NULL_HANDLE;
            }
            if (s.image != VK_NULL_HANDLE)
            {
                vkDestroyImage(device, s.image, allocator);
                s.image = VK_NULL_HANDLE;
            }
            if (s.image_mem != VK_NULL_HANDLE)
            {
                vkFreeMemory(device, s.image_mem, allocator);
                s.image_mem = VK_NULL_HANDLE;
            }
            s.image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        backing_dim = 0;
        slot_next = 0;
    }

    void Shutdown()
    {
        DestroyTextureObjects();

        if (sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, sampler, allocator);
            sampler = VK_NULL_HANDLE;
        }
        if (upload_fence != VK_NULL_HANDLE)
        {
            vkDestroyFence(device, upload_fence, allocator);
            upload_fence = VK_NULL_HANDLE;
        }
        if (upload_pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(device, upload_pool, allocator);
            upload_pool = VK_NULL_HANDLE;
        }

        device = VK_NULL_HANDLE;
        physical = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
        queue_family = 0;
        allocator = nullptr;
    }

    bool EnsureBacking(int dim)
    {
        dim = std::max(64, dim);
        if (dim == backing_dim && slots[0].descriptor_set != VK_NULL_HANDLE)
            return true;

        DestroyTextureObjects();
        backing_dim = dim;

        for (int i = 0; i < kSlots; ++i)
        {
            Slot& s = slots[i];
            VkResult err = CreateImageRGBA8(device, allocator, physical, backing_dim, backing_dim,
                                            s.image, s.image_mem, s.image_view);
            if (err != VK_SUCCESS)
            {
                DestroyTextureObjects();
                return false;
            }
            s.descriptor_set = ImGui_ImplVulkan_AddTexture(sampler, s.image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            s.image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (s.descriptor_set == VK_NULL_HANDLE)
            {
                DestroyTextureObjects();
                return false;
            }
        }
        return true;
    }

    bool ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record)
    {
        if (!record)
            return false;

        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = upload_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkResult err = vkAllocateCommandBuffers(device, &ai, &cmd);
        if (err != VK_SUCCESS)
            return false;

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(cmd, &bi);
        if (err != VK_SUCCESS)
            return false;

        record(cmd);

        err = vkEndCommandBuffer(cmd);
        if (err != VK_SUCCESS)
            return false;

        err = vkResetFences(device, 1, &upload_fence);
        if (err != VK_SUCCESS)
            return false;

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        err = vkQueueSubmit(queue, 1, &si, upload_fence);
        if (err != VK_SUCCESS)
            return false;

        err = vkWaitForFences(device, 1, &upload_fence, VK_TRUE, UINT64_MAX);
        if (err != VK_SUCCESS)
            return false;

        vkFreeCommandBuffers(device, upload_pool, 1, &cmd);
        return true;
    }

    bool UploadRGBA(Slot& slot, const void* rgba, size_t size_bytes, int w, int h)
    {
        if (!rgba || size_bytes == 0 || w <= 0 || h <= 0)
            return false;

        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_mem = VK_NULL_HANDLE;

        VkResult err = CreateBuffer(device, allocator, physical,
                                    (VkDeviceSize)size_bytes,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    staging, staging_mem);
        if (err != VK_SUCCESS)
            return false;

        void* mapped = nullptr;
        err = vkMapMemory(device, staging_mem, 0, (VkDeviceSize)size_bytes, 0, &mapped);
        if (err == VK_SUCCESS && mapped)
        {
            std::memcpy(mapped, rgba, size_bytes);
            vkUnmapMemory(device, staging_mem);
        }
        else
        {
            vkDestroyBuffer(device, staging, allocator);
            vkFreeMemory(device, staging_mem, allocator);
            return false;
        }

        const bool ok = ImmediateSubmit([&](VkCommandBuffer cmd) {
            // Transition: <current> -> TRANSFER_DST
            VkImageMemoryBarrier to_transfer{};
            to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            const VkPipelineStageFlags src_stage =
                (slot.image_layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            to_transfer.srcAccessMask =
                (slot.image_layout == VK_IMAGE_LAYOUT_UNDEFINED) ? 0 : VK_ACCESS_SHADER_READ_BIT;
            to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_transfer.oldLayout = slot.image_layout;
            to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_transfer.image = slot.image;
            to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            to_transfer.subresourceRange.baseMipLevel = 0;
            to_transfer.subresourceRange.levelCount = 1;
            to_transfer.subresourceRange.baseArrayLayer = 0;
            to_transfer.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmd,
                                 src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_transfer);
            slot.image_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

            VkBufferImageCopy copy{};
            copy.bufferOffset = 0;
            copy.bufferRowLength = 0;   // tightly packed
            copy.bufferImageHeight = 0; // tightly packed
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = 0;
            copy.imageSubresource.baseArrayLayer = 0;
            copy.imageSubresource.layerCount = 1;
            copy.imageOffset = { 0, 0, 0 };
            copy.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
            vkCmdCopyBufferToImage(cmd, staging, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            // Transition: TRANSFER_DST -> SHADER_READ
            VkImageMemoryBarrier to_read{};
            to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_read.image = slot.image;
            to_read.subresourceRange = to_transfer.subresourceRange;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_read);
            slot.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        });

        vkDestroyBuffer(device, staging, allocator);
        vkFreeMemory(device, staging_mem, allocator);
        return ok;
    }
};

CanvasPreviewTexture::~CanvasPreviewTexture()
{
    Shutdown();
}

bool CanvasPreviewTexture::Init(const InitInfo& info, const char* debug_name)
{
    Shutdown();

    if (!info.device || !info.physical_device || !info.queue)
        return false;

    m = new Impl();
    m->device = (VkDevice)info.device;
    m->physical = (VkPhysicalDevice)info.physical_device;
    m->queue = (VkQueue)info.queue;
    m->queue_family = info.queue_family;
    m->allocator = (const VkAllocationCallbacks*)info.allocator;
    m->debug_name = debug_name ? debug_name : "CanvasPreviewTexture";

    if (!m->InitUploadObjects())
    {
        Shutdown();
        return false;
    }

    return true;
}

void CanvasPreviewTexture::Shutdown()
{
    if (m)
    {
        m->Shutdown();
        delete m;
        m = nullptr;
    }
    m_view = CanvasPreviewTextureView{};
}

static void RasterizeMinimapRGBA(const AnsiCanvas& canvas,
                                 int dst_w,
                                 int dst_h,
                                 float cell_aspect,
                                 std::vector<std::uint8_t>& out_rgba)
{
    dst_w = std::max(1, dst_w);
    dst_h = std::max(1, dst_h);
    out_rgba.assign((size_t)dst_w * (size_t)dst_h * 4u, 0u);

    const int cols = canvas.GetColumns();
    const int rows = canvas.GetRows();
    if (cols <= 0 || rows <= 0)
        return;

    if (!(cell_aspect > 0.0f))
        cell_aspect = 0.5f; // Unscii-ish fallback if we don't have view metrics yet.

    const float src_w_units = (float)cols * cell_aspect;
    const float src_h_units = (float)rows;

    // Render a *downscaled composite* without rendering glyph geometry.
    //
    // Key idea: use a cached "ink coverage" value per glyph (0..1) derived from the font atlas.
    // Then approximate a cell's perceived color as:
    //    color = lerp(bg, fg, coverage)
    //
    // This preserves shading characters (░▒▓), gradients and dithering much better than
    // treating cells as flat bg/fg blocks, and avoids per-pixel glyph rendering.
    const float sx = src_w_units / (float)dst_w;
    const float sy = src_h_units / (float)dst_h;

    const ImU32 paper = canvas.IsCanvasBackgroundWhite() ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
    const ImU32 default_fg = canvas.IsCanvasBackgroundWhite() ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);

    const fonts::FontInfo& finfo = fonts::Get(canvas.GetFontId());
    const bool bitmap_font = (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0);

    ImFont* font = ImGui::GetFont();
    ImFontBaked* baked = ImGui::GetFontBaked();
    if (!bitmap_font && !baked && font)
    {
        const float bake_size = (font->LegacySize > 0.0f) ? font->LegacySize : 16.0f;
        baked = const_cast<ImFont*>(font)->GetFontBaked(bake_size);
    }

    ImFontAtlas* atlas = font ? font->OwnerAtlas : nullptr;
    if (!atlas)
        atlas = ImGui::GetIO().Fonts;

    unsigned char* atlas_rgba = nullptr;
    int atlas_w = 0, atlas_h = 0;
    if (atlas)
    {
        // Prefer already-built RGBA32 data when available (matches this app's Vulkan backend expectations).
        if (atlas->TexData && atlas->TexData->Pixels && atlas->TexData->Format == ImTextureFormat_RGBA32)
        {
            atlas_rgba = atlas->TexData->Pixels;
            atlas_w = atlas->TexData->Width;
            atlas_h = atlas->TexData->Height;
        }
        // IMPORTANT: avoid forcing atlas (re)builds from the preview path.
        // If TexData isn't available, fall back to a conservative behavior (treat glyphs as solid fg)
        // rather than calling GetTexDataAsRGBA32() every frame, which can be expensive and may cause
        // visual instability depending on backend/font atlas lifecycle.
    }

    auto unpack = [](ImU32 c, int& r, int& g, int& b, int& a) {
        r = (int)(c & 0xFF);
        g = (int)((c >> 8) & 0xFF);
        b = (int)((c >> 16) & 0xFF);
        a = (int)((c >> 24) & 0xFF);
    };
    auto pack = [](int r, int g, int b, int a) -> ImU32 {
        r = std::clamp(r, 0, 255);
        g = std::clamp(g, 0, 255);
        b = std::clamp(b, 0, 255);
        a = std::clamp(a, 0, 255);
        return (ImU32)((a << 24) | (b << 16) | (g << 8) | (r));
    };

    struct Ink2x2
    {
        // Quadrant coverage (0..1): [y][x] where x,y in {0,1}.
        // This approximates half-blocks, box drawing, and thin strokes better than a single average.
        float q00 = 0.0f; // top-left
        float q10 = 0.0f; // top-right
        float q01 = 0.0f; // bottom-left
        float q11 = 0.0f; // bottom-right
    };

    // Glyph ink cache (per process). This avoids re-scanning atlas rectangles.
    // Key includes the font id so changing canvas font doesn't reuse stale coverage.
    static std::unordered_map<std::uint64_t, Ink2x2> s_ink_cache;

    auto glyph_ink2x2 = [&](char32_t cp) -> Ink2x2
    {
        Ink2x2 out{};
        if (cp == U' ')
            return out;

        const std::uint64_t key = ((std::uint64_t)finfo.id << 32) | (std::uint32_t)cp;
        if (auto it = s_ink_cache.find(key); it != s_ink_cache.end())
            return it->second;

        if (bitmap_font)
        {
            std::uint8_t glyph = 0;
            if (!fonts::UnicodeToCp437Byte(cp, glyph))
            {
                std::uint8_t q = 0;
                glyph = (fonts::UnicodeToCp437Byte(U'?', q)) ? q : (std::uint8_t)' ';
            }

            const int w = finfo.cell_w;
            const int h = finfo.cell_h;
            const int mid_x = w / 2;
            const int mid_y = h / 2;

            std::uint64_t sum00 = 0, cnt00 = 0;
            std::uint64_t sum10 = 0, cnt10 = 0;
            std::uint64_t sum01 = 0, cnt01 = 0;
            std::uint64_t sum11 = 0, cnt11 = 0;

            for (int yy = 0; yy < h; ++yy)
            {
                const std::uint8_t bits = fonts::BitmapGlyphRowBits(finfo.id, glyph, yy);
                for (int xx = 0; xx < w; ++xx)
                {
                    bool on = false;
                    if (xx < 8)
                        on = (bits & (std::uint8_t)(0x80u >> xx)) != 0;
                    else if (xx == 8 && finfo.vga_9col_dup && finfo.cell_w == 9 && glyph >= 192 && glyph <= 223)
                        on = (bits & 0x01u) != 0;

                    const std::uint64_t a = on ? 255u : 0u;
                    const bool right = (xx >= mid_x);
                    const bool bot = (yy >= mid_y);
                    if (!right && !bot) { sum00 += a; ++cnt00; }
                    else if (right && !bot) { sum10 += a; ++cnt10; }
                    else if (!right && bot) { sum01 += a; ++cnt01; }
                    else { sum11 += a; ++cnt11; }
                }
            }

            auto cov = [](std::uint64_t sum, std::uint64_t cnt) -> float
            {
                if (cnt == 0)
                    return 0.0f;
                const float v = (float)sum / (float)(cnt * 255.0);
                return std::clamp(v, 0.0f, 1.0f);
            };

            out.q00 = cov(sum00, cnt00);
            out.q10 = cov(sum10, cnt10);
            out.q01 = cov(sum01, cnt01);
            out.q11 = cov(sum11, cnt11);
            s_ink_cache.emplace(key, out);
            return out;
        }

        if (!baked || !atlas_rgba || atlas_w <= 0 || atlas_h <= 0)
        {
            // Best effort: treat as solid fg.
            out.q00 = out.q10 = out.q01 = out.q11 = 1.0f;
            s_ink_cache.emplace(key, out);
            return out;
        }

        const ImFontGlyph* g = baked->FindGlyphNoFallback((ImWchar)cp);
        if (!g)
        {
            s_ink_cache.emplace(key, out);
            return out;
        }

        const int x0 = std::clamp((int)std::floor(g->U0 * (float)atlas_w), 0, atlas_w);
        const int y0 = std::clamp((int)std::floor(g->V0 * (float)atlas_h), 0, atlas_h);
        const int x1 = std::clamp((int)std::ceil (g->U1 * (float)atlas_w), 0, atlas_w);
        const int y1 = std::clamp((int)std::ceil (g->V1 * (float)atlas_h), 0, atlas_h);
        if (x1 <= x0 || y1 <= y0)
        {
            s_ink_cache.emplace(key, out);
            return out;
        }

        const int w = x1 - x0;
        const int h = y1 - y0;
        const int mid_x = x0 + w / 2;
        const int mid_y = y0 + h / 2;

        std::uint64_t sum00 = 0, cnt00 = 0;
        std::uint64_t sum10 = 0, cnt10 = 0;
        std::uint64_t sum01 = 0, cnt01 = 0;
        std::uint64_t sum11 = 0, cnt11 = 0;
        for (int yy = y0; yy < y1; ++yy)
        {
            for (int xx = x0; xx < x1; ++xx)
            {
                const size_t base = (size_t)(yy * atlas_w + xx) * 4u;
                const std::uint64_t a = (std::uint64_t)atlas_rgba[base + 3];
                const bool right = (xx >= mid_x);
                const bool bot = (yy >= mid_y);
                if (!right && !bot) { sum00 += a; ++cnt00; }
                else if (right && !bot) { sum10 += a; ++cnt10; }
                else if (!right && bot) { sum01 += a; ++cnt01; }
                else { sum11 += a; ++cnt11; }
            }
        }

        auto cov = [](std::uint64_t sum, std::uint64_t cnt) -> float
        {
            if (cnt == 0)
                return 0.0f;
            const float v = (float)sum / (float)(cnt * 255.0);
            return std::clamp(v, 0.0f, 1.0f);
        };

        out.q00 = cov(sum00, cnt00);
        out.q10 = cov(sum10, cnt10);
        out.q01 = cov(sum01, cnt01);
        out.q11 = cov(sum11, cnt11);
        s_ink_cache.emplace(key, out);
        return out;
    };

    auto sample_cell_color = [&](int row, int col, float lx, float ly) -> ImU32
    {
        AnsiCanvas::GlyphId glyph = phos::glyph::MakeUnicodeScalar(U' ');
        AnsiCanvas::ColorIndex16 fg = AnsiCanvas::kUnsetIndex16;
        AnsiCanvas::ColorIndex16 bg = AnsiCanvas::kUnsetIndex16;
        (void)canvas.GetCompositeCellPublicGlyphIndices(row, col, glyph, fg, bg);
        const char32_t cp_rep = phos::glyph::ToUnicodeRepresentative((phos::GlyphId)glyph);

        auto& cs = phos::color::GetColorSystem();
        phos::color::PaletteInstanceId pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
        if (auto id = cs.Palettes().Resolve(canvas.GetPaletteRef()))
            pal = *id;
        auto idx_to_u32 = [&](AnsiCanvas::ColorIndex16 idx) -> ImU32 {
            if (idx == AnsiCanvas::kUnsetIndex16)
                return 0;
            return (ImU32)phos::color::ColorOps::IndexToColor32(cs.Palettes(), pal, phos::color::ColorIndex{idx});
        };

        const ImU32 bg_col = (bg != AnsiCanvas::kUnsetIndex16) ? idx_to_u32(bg) : paper;
        const ImU32 fg_col = (fg != AnsiCanvas::kUnsetIndex16) ? idx_to_u32(fg) : default_fg;
        const Ink2x2 ink = glyph_ink2x2(cp_rep);
        const bool right = (lx >= 0.5f);
        const bool bot   = (ly >= 0.5f);
        float t = 0.0f;
        if (!right && !bot) t = ink.q00;
        else if (right && !bot) t = ink.q10;
        else if (!right && bot) t = ink.q01;
        else t = ink.q11;

        // Sharpen coverage a bit so thin dark outlines survive downscale.
        // (A simple contrast curve around 0.5).
        const float sharp = 1.6f;
        t = std::clamp((t - 0.5f) * sharp + 0.5f, 0.0f, 1.0f);

        int br, bgc, bb, ba;
        int fr, fgcc, fb, fa;
        unpack(bg_col, br, bgc, bb, ba);
        unpack(fg_col, fr, fgcc, fb, fa);

        const int r = (int)std::lround((double)br + ((double)fr - (double)br) * (double)t);
        const int g = (int)std::lround((double)bgc + ((double)fgcc - (double)bgc) * (double)t);
        const int b = (int)std::lround((double)bb + ((double)fb - (double)bb) * (double)t);
        const int a = 255;
        return pack(r, g, b, a);
    };

    for (int y = 0; y < dst_h; ++y)
    {
        for (int x = 0; x < dst_w; ++x)
        {
            // Single-sample nearest in *cell space* for crisp minimap pixels.
            // (Supersampling smooths/blur edges, which is undesirable for a crisp minimap.)
            const float u = ((float)x + 0.5f) * sx; // aspect-adjusted cols units
            const float v = ((float)y + 0.5f) * sy; // rows units
            const float fx_cell = (cell_aspect > 0.0f) ? (u / cell_aspect) : 0.0f; // in columns
            const int col = std::clamp((int)std::floor(fx_cell), 0, cols - 1);
            const int row = std::clamp((int)std::floor(v), 0, rows - 1);
            const float lx = fx_cell - (float)col; // 0..1 within cell
            const float ly = v - (float)row;       // 0..1 within cell

            const ImU32 c = sample_cell_color(row, col, lx, ly);

            const size_t idx = ((size_t)y * (size_t)dst_w + (size_t)x) * 4u;
            out_rgba[idx + 0] = (std::uint8_t)(c & 0xFF);
            out_rgba[idx + 1] = (std::uint8_t)((c >> 8) & 0xFF);
            out_rgba[idx + 2] = (std::uint8_t)((c >> 16) & 0xFF);
            out_rgba[idx + 3] = (std::uint8_t)((c >> 24) & 0xFF);
        }
    }
}

void CanvasPreviewTexture::Update(AnsiCanvas* canvas, int max_dim, double now_s)
{
    if (!m)
        return;

    if (!canvas)
    {
        // Keep last texture around (MinimapWindow can decide how to handle null canvas).
        return;
    }

    // Throttle uploads: preview can look great at ~15-20fps during painting.
    // IMPORTANT: do NOT throttle canvas switches or dimension changes, otherwise the
    // minimap can show a warped previous canvas.
    const double min_dt = 1.0 / 20.0;
    const uint64_t rev = canvas->GetContentRevision();
    const bool canvas_changed = (m->last_canvas != canvas);

    max_dim = std::clamp(max_dim, 64, 1024);

    // Match canvas aspect if we can. Prefer the last captured Render() view metrics,
    // because those are derived from the actual font in use and are stable across zoom.
    float aspect = 0.0f;
    const AnsiCanvas::ViewState& vs = canvas->GetLastViewState();
    if (vs.valid && vs.base_cell_h > 0.0f && vs.base_cell_w > 0.0f)
    {
        aspect = vs.base_cell_w / vs.base_cell_h;
        if (aspect > 0.0f)
            m->last_base_aspect = aspect;
    }
    // If we don't have view metrics yet, fall back to the last known base aspect (stable).
    if (!(aspect > 0.0f) && m->last_base_aspect > 0.0f)
        aspect = m->last_base_aspect;
    if (!(aspect > 0.0f))
        aspect = 0.5f;

    const int cols = canvas->GetColumns();
    const int rows = canvas->GetRows();
    if (cols <= 0 || rows <= 0)
        return;

    const float src_w_units = (float)cols * aspect;
    const float src_h_units = (float)rows;
    if (!(src_w_units > 0.0f) || !(src_h_units > 0.0f))
        return;

    const float ratio = src_w_units / src_h_units;
    int w = 1, h = 1;
    if (ratio >= 1.0f)
    {
        w = max_dim;
        h = std::max(1, (int)std::lround((double)max_dim / (double)ratio));
    }
    else
    {
        h = max_dim;
        w = std::max(1, (int)std::lround((double)max_dim * (double)ratio));
    }

    const bool dims_changed = (w != m->last_w) || (h != m->last_h);
    const bool rev_changed = (rev != m->last_canvas_rev);

    // Backing texture is square (max_dim x max_dim). We render into a sub-rect (w x h)
    // and expose UVs so MinimapWindow preserves aspect without reallocating GPU objects.
    if (!m->EnsureBacking(max_dim))
        return;

    // If nothing changed, we can skip uploading.
    if (!canvas_changed && !dims_changed && !rev_changed && m_view.Valid())
        return;
    // Throttle only for continuous edits on the *same* canvas with the same dimensions.
    if (!canvas_changed && !dims_changed &&
        m->last_upload_s >= 0.0 && (now_s - m->last_upload_s) < min_dt && m_view.Valid())
        return;

    std::vector<std::uint8_t> rgba;
    RasterizeMinimapRGBA(*canvas, w, h, aspect, rgba);

    // Upload into next slot to avoid overwriting textures still in flight.
    const int slot_index = m->slot_next;
    CanvasPreviewTexture::Impl::Slot& slot = m->slots[slot_index];
    m->slot_next = (m->slot_next + 1) % CanvasPreviewTexture::Impl::kSlots;

    if (!m->UploadRGBA(slot, rgba.data(), rgba.size(), w, h))
        return;

    m->last_canvas = canvas;
    m->last_canvas_rev = rev;
    m->last_w = w;
    m->last_h = h;
    m->slot_current = slot_index;
    m->last_upload_s = now_s;
    m_view.texture_id = (ImTextureID)slot.descriptor_set;
    m_view.width = w;
    m_view.height = h;
    m_view.uv0 = ImVec2(0.0f, 0.0f);
    m_view.uv1 = ImVec2((float)w / (float)m->backing_dim,
                        (float)h / (float)m->backing_dim);
}


