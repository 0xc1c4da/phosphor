#include "app/bitmap_glyph_atlas_texture.h"

#include "core/fonts.h"

// ImGui Vulkan backend texture registration.
#include "imgui_impl_vulkan.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
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

static inline std::uint64_t Fnv1a64(const void* data, size_t n, std::uint64_t seed = 1469598103934665603ull)
{
    const std::uint8_t* p = (const std::uint8_t*)data;
    std::uint64_t h = seed;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= (std::uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static inline std::uint64_t Mix64(std::uint64_t a, std::uint64_t b)
{
    // Simple reversible-ish mix for cache keys.
    a ^= b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2);
    return a;
}
} // namespace

struct BitmapGlyphAtlasTextureCache::Impl
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    const VkAllocationCallbacks* allocator = nullptr;

    VkCommandPool upload_pool = VK_NULL_HANDLE;
    VkFence upload_fence = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    const char* debug_name = "BitmapGlyphAtlasTextureCache";

    struct Entry
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_mem = VK_NULL_HANDLE;
        VkImageView image_view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkImageLayout image_layout = VK_IMAGE_LAYOUT_UNDEFINED;

        AnsiCanvas::BitmapGlyphAtlasView view;
        std::uint64_t key = 0;

        // Cache policy
        std::uint64_t last_used_frame = 0;
        std::size_t   bytes = 0; // estimated GPU bytes (RGBA8)
    };

    std::unordered_map<std::uint64_t, Entry> cache;

    // Deferred destruction: entries evicted from `cache` are moved here and destroyed after
    // a few frames to avoid freeing textures still referenced by in-flight command buffers.
    struct Retired
    {
        Entry entry;
        std::uint64_t retire_frame = 0;
    };
    std::deque<Retired> retired;

    // Cache tuning knobs (0 = unlimited).
    std::size_t budget_bytes = 96ull * 1024ull * 1024ull;
    std::size_t live_bytes = 0; // cached + retired (until actually destroyed)
    std::uint32_t frames_in_flight = 3;
    std::uint64_t frame_counter = 0;
    std::size_t max_entries = 1024; // safety rail even under unlimited budget

    bool InitUploadObjects()
    {
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

        // NEAREST sampler for crisp pixel scaling.
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

    void DestroyEntry(Entry& e)
    {
        if (e.descriptor_set != VK_NULL_HANDLE)
        {
            ImGui_ImplVulkan_RemoveTexture(e.descriptor_set);
            e.descriptor_set = VK_NULL_HANDLE;
        }
        if (e.image_view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, e.image_view, allocator);
            e.image_view = VK_NULL_HANDLE;
        }
        if (e.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, e.image, allocator);
            e.image = VK_NULL_HANDLE;
        }
        if (e.image_mem != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, e.image_mem, allocator);
            e.image_mem = VK_NULL_HANDLE;
        }
        e.image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        e.view = AnsiCanvas::BitmapGlyphAtlasView{};
    }

    void Shutdown()
    {
        // Destroy retired entries first.
        for (auto& r : retired)
            DestroyEntry(r.entry);
        retired.clear();

        for (auto& kv : cache)
            DestroyEntry(kv.second);
        cache.clear();
        live_bytes = 0;

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

    static std::size_t AtlasBytes(int w, int h)
    {
        if (w <= 0 || h <= 0)
            return 0;
        return (std::size_t)w * (std::size_t)h * 4u;
    }

    void CollectGarbage()
    {
        // Conservative: defer at least `frames_in_flight + 1` frames.
        // This matches the common "N frames in flight" resource lifetime rule.
        const std::uint64_t safe_before =
            (frame_counter > (std::uint64_t)frames_in_flight + 1)
                ? (frame_counter - (std::uint64_t)frames_in_flight - 1)
                : 0;

        // Retired entries are appended in eviction order; drain from front while safe.
        while (!retired.empty() && retired.front().retire_frame <= safe_before)
        {
            Retired r = std::move(retired.front());
            retired.pop_front();
            const std::size_t b = r.entry.bytes;
            DestroyEntry(r.entry);
            if (live_bytes >= b)
                live_bytes -= b;
            else
                live_bytes = 0;
        }
    }

    bool EvictOneLru()
    {
        if (cache.empty())
            return false;

        // Find least-recently-used entry.
        auto best = cache.begin();
        for (auto it = cache.begin(); it != cache.end(); ++it)
        {
            if (it->second.last_used_frame < best->second.last_used_frame)
                best = it;
        }

        Retired r;
        r.entry = std::move(best->second);
        r.retire_frame = frame_counter;
        retired.push_back(std::move(r));
        cache.erase(best);
        // live_bytes stays the same until actual destruction.
        return true;
    }

    void EnforceBudget(std::size_t incoming_bytes)
    {
        // If budget is unlimited, we still respect max_entries.
        const bool unlimited = (budget_bytes == 0);

        // Evict until under entry cap.
        while (cache.size() >= max_entries)
        {
            if (!EvictOneLru())
                break;
        }

        if (unlimited)
            return;

        // Budget is a "soft" cap because we defer frees. Eviction reduces future churn and keeps
        // the active cache bounded, but live_bytes may temporarily exceed budget while retired
        // entries are waiting to be safely destroyed.
        const std::size_t target_budget = std::max(budget_bytes, incoming_bytes);

        // Evict to reduce active set pressure when adding a new atlas would exceed the budget.
        // (Note: live_bytes won't drop until CollectGarbage() runs.)
        while (!cache.empty() && (live_bytes + incoming_bytes) > target_budget)
        {
            if (!EvictOneLru())
                break;
            // Stop if we've evicted everything; we'll allow overshoot for the incoming atlas.
        }
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

    bool UploadRGBA(Entry& e, const void* rgba, size_t size_bytes, int w, int h)
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
            VkImageMemoryBarrier to_transfer{};
            to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            const VkPipelineStageFlags src_stage =
                (e.image_layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            to_transfer.srcAccessMask =
                (e.image_layout == VK_IMAGE_LAYOUT_UNDEFINED) ? 0 : VK_ACCESS_SHADER_READ_BIT;
            to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_transfer.oldLayout = e.image_layout;
            to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_transfer.image = e.image;
            to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            to_transfer.subresourceRange.baseMipLevel = 0;
            to_transfer.subresourceRange.levelCount = 1;
            to_transfer.subresourceRange.baseArrayLayer = 0;
            to_transfer.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmd,
                                 src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_transfer);
            e.image_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

            VkBufferImageCopy copy{};
            copy.bufferOffset = 0;
            copy.bufferRowLength = 0;
            copy.bufferImageHeight = 0;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = 0;
            copy.imageSubresource.baseArrayLayer = 0;
            copy.imageSubresource.layerCount = 1;
            copy.imageOffset = { 0, 0, 0 };
            copy.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
            vkCmdCopyBufferToImage(cmd, staging, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            VkImageMemoryBarrier to_read{};
            to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_read.image = e.image;
            to_read.subresourceRange = to_transfer.subresourceRange;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_read);
            e.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        });

        vkDestroyBuffer(device, staging, allocator);
        vkFreeMemory(device, staging_mem, allocator);
        return ok;
    }

    static inline bool IsVgaDupRange(std::uint8_t glyph8)
    {
        return glyph8 >= 192 && glyph8 <= 223;
    }

    // Build atlas RGBA pixels for a (possibly embedded) bitmap font.
    static void BuildAtlasRGBA(int cell_w,
                               int cell_h,
                               int glyph_count,
                               bool vga_9col_dup,
                               const std::function<std::uint8_t(std::uint16_t glyph_index, int yy)>& row_bits,
                               std::vector<std::uint8_t>& out_rgba,
                               int& out_w,
                               int& out_h,
                               int& out_cols,
                               int& out_rows,
                               int variant_count,
                               int pad,
                               int& out_tile_w,
                               int& out_tile_h)
    {
        out_rgba.clear();
        out_w = out_h = out_cols = out_rows = 0;
        out_tile_w = out_tile_h = 0;

        cell_w = std::max(1, cell_w);
        cell_h = std::max(1, cell_h);
        glyph_count = std::clamp(glyph_count, 1, 512);
        variant_count = std::clamp(variant_count, 1, 4);
        pad = std::clamp(pad, 0, 8);

        // Layout: choose 16 cols for 256 glyphs, 32 cols for 512 glyphs.
        out_cols = (glyph_count > 256) ? 32 : 16;
        out_rows = (glyph_count + out_cols - 1) / out_cols;

        out_tile_w = cell_w + pad * 2;
        out_tile_h = cell_h + pad * 2;
        out_w = out_cols * out_tile_w;
        out_h = out_rows * out_tile_h * variant_count;

        out_rgba.assign((size_t)out_w * (size_t)out_h * 4u, 0u);

        auto set_px = [&](int x, int y)
        {
            if (x < 0 || y < 0 || x >= out_w || y >= out_h)
                return;
            const size_t idx = ((size_t)y * (size_t)out_w + (size_t)x) * 4u;
            out_rgba[idx + 0] = 255;
            out_rgba[idx + 1] = 255;
            out_rgba[idx + 2] = 255;
            out_rgba[idx + 3] = 255;
        };

        // Italic shear in *glyph pixel* space (integer shift per row).
        auto italic_shift = [&](int yy) -> int
        {
            // Match the canvas renderer's intent: top leans right more than bottom.
            const float shear = 0.20f * ((float)cell_w / (float)std::max(1, cell_h));
            const float y_mid = (float)yy + 0.5f;
            const float shift = shear * ((float)cell_h - y_mid);
            return (int)std::floor(shift + 0.5f);
        };

        for (int variant = 0; variant < variant_count; ++variant)
        {
            const bool want_bold = (variant == 1 || variant == 3);
            const bool want_italic = (variant == 2 || variant == 3);

            for (int gi = 0; gi < glyph_count; ++gi)
            {
                const int tile_x = gi % out_cols;
                const int tile_y = gi / out_cols;
                const int ox = tile_x * out_tile_w;
                const int oy = (variant * out_rows + tile_y) * out_tile_h;
                const std::uint8_t glyph8 = (std::uint8_t)(gi & 0xFFu);

                for (int yy = 0; yy < cell_h; ++yy)
                {
                    const std::uint8_t bits = row_bits((std::uint16_t)gi, yy);
                    const int shift = want_italic ? italic_shift(yy) : 0;

                    auto bit_on = [&](int xx) -> bool
                    {
                        if (xx < 0)
                            return false;
                        if (xx < 8)
                            return (bits & (std::uint8_t)(0x80u >> xx)) != 0;
                        if (xx == 8 && vga_9col_dup && cell_w == 9 && IsVgaDupRange(glyph8))
                            return (bits & 0x01u) != 0;
                        return false;
                    };

                    for (int xx = 0; xx < cell_w; ++xx)
                    {
                        if (!bit_on(xx))
                            continue;

                        int x0 = xx + shift;
                        int x1 = x0;
                        set_px(ox + pad + x0, oy + pad + yy);

                        if (want_bold)
                        {
                            // 1px dilation to the right.
                            set_px(ox + pad + x1 + 1, oy + pad + yy);
                        }
                    }
                }

                // Extrude the glyph's edge pixels into the padding to prevent seams when sampling atlas tiles.
                if (pad > 0)
                {
                    // Horizontal extrusion (left/right).
                    for (int yy = 0; yy < cell_h; ++yy)
                    {
                        const int y = oy + pad + yy;
                        // Copy first interior column to padding columns on the left.
                        for (int p = 1; p <= pad; ++p)
                        {
                            const int x_src = ox + pad;
                            const int x_dst = ox + (pad - p);
                            const size_t src = ((size_t)y * (size_t)out_w + (size_t)x_src) * 4u;
                            const size_t dst = ((size_t)y * (size_t)out_w + (size_t)x_dst) * 4u;
                            out_rgba[dst + 0] = out_rgba[src + 0];
                            out_rgba[dst + 1] = out_rgba[src + 1];
                            out_rgba[dst + 2] = out_rgba[src + 2];
                            out_rgba[dst + 3] = out_rgba[src + 3];
                        }
                        // Copy last interior column to padding columns on the right.
                        for (int p = 1; p <= pad; ++p)
                        {
                            const int x_src = ox + pad + (cell_w - 1);
                            const int x_dst = ox + pad + cell_w - 1 + p;
                            const size_t src = ((size_t)y * (size_t)out_w + (size_t)x_src) * 4u;
                            const size_t dst = ((size_t)y * (size_t)out_w + (size_t)x_dst) * 4u;
                            out_rgba[dst + 0] = out_rgba[src + 0];
                            out_rgba[dst + 1] = out_rgba[src + 1];
                            out_rgba[dst + 2] = out_rgba[src + 2];
                            out_rgba[dst + 3] = out_rgba[src + 3];
                        }
                    }

                    // Vertical extrusion (top/bottom) across the full tile width (including left/right padding).
                    for (int xx = 0; xx < out_tile_w; ++xx)
                    {
                        const int x = ox + xx;
                        // Top padding rows copy from first interior row.
                        for (int p = 1; p <= pad; ++p)
                        {
                            const int y_src = oy + pad;
                            const int y_dst = oy + (pad - p);
                            const size_t src = ((size_t)y_src * (size_t)out_w + (size_t)x) * 4u;
                            const size_t dst = ((size_t)y_dst * (size_t)out_w + (size_t)x) * 4u;
                            out_rgba[dst + 0] = out_rgba[src + 0];
                            out_rgba[dst + 1] = out_rgba[src + 1];
                            out_rgba[dst + 2] = out_rgba[src + 2];
                            out_rgba[dst + 3] = out_rgba[src + 3];
                        }
                        // Bottom padding rows copy from last interior row.
                        for (int p = 1; p <= pad; ++p)
                        {
                            const int y_src = oy + pad + (cell_h - 1);
                            const int y_dst = oy + pad + cell_h - 1 + p;
                            const size_t src = ((size_t)y_src * (size_t)out_w + (size_t)x) * 4u;
                            const size_t dst = ((size_t)y_dst * (size_t)out_w + (size_t)x) * 4u;
                            out_rgba[dst + 0] = out_rgba[src + 0];
                            out_rgba[dst + 1] = out_rgba[src + 1];
                            out_rgba[dst + 2] = out_rgba[src + 2];
                            out_rgba[dst + 3] = out_rgba[src + 3];
                        }
                    }
                }
            }
        }
    }
};

BitmapGlyphAtlasTextureCache::~BitmapGlyphAtlasTextureCache()
{
    Shutdown();
}

bool BitmapGlyphAtlasTextureCache::Init(const InitInfo& info, const char* debug_name)
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
    m->debug_name = debug_name ? debug_name : "BitmapGlyphAtlasTextureCache";

    if (!m->InitUploadObjects())
    {
        Shutdown();
        return false;
    }
    return true;
}

void BitmapGlyphAtlasTextureCache::Shutdown()
{
    if (m)
    {
        m->Shutdown();
        delete m;
        m = nullptr;
    }
}

void BitmapGlyphAtlasTextureCache::SetBudgetBytes(std::size_t bytes)
{
    if (!m)
        return;
    m->budget_bytes = bytes;
    // Apply immediately (best-effort): evict LRU entries if needed.
    m->EnforceBudget(/*incoming_bytes=*/0);
}

std::size_t BitmapGlyphAtlasTextureCache::BudgetBytes() const
{
    return m ? m->budget_bytes : 0;
}

std::size_t BitmapGlyphAtlasTextureCache::UsedBytes() const
{
    return m ? m->live_bytes : 0;
}

void BitmapGlyphAtlasTextureCache::SetFramesInFlight(std::uint32_t n)
{
    if (!m)
        return;
    m->frames_in_flight = std::max(1u, n);
}

std::uint32_t BitmapGlyphAtlasTextureCache::FramesInFlight() const
{
    return m ? m->frames_in_flight : 0;
}

void BitmapGlyphAtlasTextureCache::BeginFrame()
{
    if (!m)
        return;
    ++m->frame_counter;
    m->CollectGarbage();
}

static bool CanvasHasBitmapFont(const AnsiCanvas& canvas, bool& out_embedded)
{
    out_embedded = false;
    const fonts::FontInfo& finfo = fonts::Get(canvas.GetFontId());
    const AnsiCanvas::EmbeddedBitmapFont* ef = canvas.GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    if (embedded_font)
    {
        out_embedded = true;
        return true;
    }
    const bool bitmap_font = (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0);
    return bitmap_font;
}

bool BitmapGlyphAtlasTextureCache::GetBitmapGlyphAtlas(const AnsiCanvas& canvas, AnsiCanvas::BitmapGlyphAtlasView& out)
{
    out = AnsiCanvas::BitmapGlyphAtlasView{};
    if (!m)
        return false;

    bool embedded = false;
    if (!CanvasHasBitmapFont(canvas, embedded))
        return false;

    const fonts::FontInfo& finfo = fonts::Get(canvas.GetFontId());
    const AnsiCanvas::EmbeddedBitmapFont* ef = canvas.GetEmbeddedFont();

    int cell_w = 0;
    int cell_h = 0;
    int glyph_count = 0;
    bool vga_dup = false;
    std::uint64_t key = 0;

    if (embedded)
    {
        cell_w = std::max(1, ef->cell_w);
        cell_h = std::max(1, ef->cell_h);
        glyph_count = std::clamp(ef->glyph_count, 1, 512);
        vga_dup = ef->vga_9col_dup;

        key = 0xBEEFull;
        key = Mix64(key, (std::uint64_t)cell_w);
        key = Mix64(key, (std::uint64_t)cell_h);
        key = Mix64(key, (std::uint64_t)glyph_count);
        key = Mix64(key, (std::uint64_t)(vga_dup ? 1 : 0));
        // Hash the bitmap payload (only once per unique embedded font).
        key = Mix64(key, Fnv1a64(ef->bitmap.data(), ef->bitmap.size()));
    }
    else
    {
        cell_w = std::max(1, finfo.cell_w);
        cell_h = std::max(1, finfo.cell_h);
        glyph_count = 256;
        vga_dup = finfo.vga_9col_dup;

        key = 0xCAFEull;
        key = Mix64(key, (std::uint64_t)finfo.id);
        key = Mix64(key, (std::uint64_t)cell_w);
        key = Mix64(key, (std::uint64_t)cell_h);
        key = Mix64(key, (std::uint64_t)(vga_dup ? 1 : 0));
    }

    // Cache hit.
    if (auto it = m->cache.find(key); it != m->cache.end())
    {
        it->second.last_used_frame = m->frame_counter;
        out = it->second.view;
        return out.texture_id != nullptr;
    }

    // Build atlas pixels.
    std::vector<std::uint8_t> rgba;
    int atlas_w = 0, atlas_h = 0, cols = 0, rows = 0;
    const int variant_count = 4;
    const int pad = 1;
    int tile_w = 0, tile_h = 0;

    if (embedded)
    {
        auto row_bits = [&](std::uint16_t glyph_index, int yy) -> std::uint8_t
        {
            if (!ef)
                return 0;
            if (glyph_index >= (std::uint16_t)ef->glyph_count)
                return 0;
            if (yy < 0 || yy >= ef->cell_h)
                return 0;
            return ef->bitmap[(size_t)glyph_index * (size_t)ef->cell_h + (size_t)yy];
        };
        Impl::BuildAtlasRGBA(cell_w, cell_h, glyph_count, vga_dup, row_bits,
                             rgba, atlas_w, atlas_h, cols, rows, variant_count, pad, tile_w, tile_h);
    }
    else
    {
        auto row_bits = [&](std::uint16_t glyph_index, int yy) -> std::uint8_t
        {
            return fonts::BitmapGlyphRowBits(finfo.id, glyph_index, yy);
        };
        Impl::BuildAtlasRGBA(cell_w, cell_h, glyph_count, vga_dup, row_bits,
                             rgba, atlas_w, atlas_h, cols, rows, variant_count, pad, tile_w, tile_h);
    }

    if (rgba.empty() || atlas_w <= 0 || atlas_h <= 0)
        return false;

    const std::size_t atlas_bytes = Impl::AtlasBytes(atlas_w, atlas_h);
    // Enforce cache policy before allocating GPU objects.
    m->EnforceBudget(atlas_bytes);
    // Note: budget enforcement may move entries to the retired list. Collect old frees opportunistically.
    m->CollectGarbage();

    // Create GPU resources.
    Impl::Entry e;
    e.key = key;
    e.last_used_frame = m->frame_counter;
    e.bytes = atlas_bytes;
    e.view.atlas_w = atlas_w;
    e.view.atlas_h = atlas_h;
    e.view.cell_w = cell_w;
    e.view.cell_h = cell_h;
    e.view.pad = pad;
    e.view.tile_w = tile_w;
    e.view.tile_h = tile_h;
    e.view.cols = cols;
    e.view.rows = rows;
    e.view.glyph_count = glyph_count;
    e.view.variant_count = variant_count;

    VkResult err = CreateImageRGBA8(m->device, m->allocator, m->physical, atlas_w, atlas_h,
                                    e.image, e.image_mem, e.image_view);
    if (err != VK_SUCCESS)
        return false;

    e.descriptor_set = ImGui_ImplVulkan_AddTexture(m->sampler, e.image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (e.descriptor_set == VK_NULL_HANDLE)
    {
        m->DestroyEntry(e);
        return false;
    }

    if (!m->UploadRGBA(e, rgba.data(), rgba.size(), atlas_w, atlas_h))
    {
        m->DestroyEntry(e);
        return false;
    }

    e.view.texture_id = (void*)e.descriptor_set;

    // Store and return.
    m->cache.emplace(key, e);
    m->live_bytes += atlas_bytes;
    out = e.view;
    return out.texture_id != nullptr;
}


