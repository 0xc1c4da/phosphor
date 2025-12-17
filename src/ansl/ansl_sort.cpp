#include "ansl/ansl_native.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ansl::sort
{
namespace
{
struct ScoredCp
{
    char32_t cp = 0;
    std::uint64_t score = 0;
    int index = 0; // stable tie-break
};

static std::uint64_t GlyphInkScoreRgba32(ImFontBaked* baked,
                                        const unsigned char* pixels_rgba,
                                        int tex_w,
                                        int tex_h,
                                        char32_t cp)
{
    if (!baked || !pixels_rgba || tex_w <= 0 || tex_h <= 0)
        return 0;

    // ImGui 1.92+: FindGlyphNoFallback() lives on ImFontBaked.
    // Requires IMGUI_USE_WCHAR32 in this repo (set in Makefile).
    const ImFontGlyph* g = baked->FindGlyphNoFallback((ImWchar)cp);
    if (!g)
        return 0;

    // Convert UV rect to pixel rect. Clamp and sum alpha channel.
    const int x0 = std::clamp((int)std::floor(g->U0 * (float)tex_w), 0, tex_w);
    const int y0 = std::clamp((int)std::floor(g->V0 * (float)tex_h), 0, tex_h);
    const int x1 = std::clamp((int)std::ceil (g->U1 * (float)tex_w), 0, tex_w);
    const int y1 = std::clamp((int)std::ceil (g->V1 * (float)tex_h), 0, tex_h);

    if (x1 <= x0 || y1 <= y0)
        return 0;

    std::uint64_t sum = 0;
    for (int y = y0; y < y1; ++y)
    {
        for (int x = x0; x < x1; ++x)
        {
            const size_t base = (size_t)(x + y * tex_w) * 4u;
            // Standard ImGui font atlas: RGB is white, A stores coverage.
            sum += (std::uint64_t)pixels_rgba[base + 3];
        }
    }
    return sum;
}
} // namespace

std::string by_brightness_utf8(const char* charset_utf8,
                               size_t len,
                               const ::ImFont* font,
                               bool ascending)
{
    if (!charset_utf8 || len == 0)
        return {};

    // Must have an active ImGui context + current font baked state.
    // This function may be called while compiling/loading scripts (outside a frame),
    // in which case touching font baked data can trigger internal ImGui asserts.
    if (ImGui::GetCurrentContext() == nullptr)
        return std::string(charset_utf8, charset_utf8 + len);

    // Caller can pass null; in the native editor we default to the current font.
    if (!font)
        font = ImGui::GetFont();
    if (!font)
        return std::string(charset_utf8, charset_utf8 + len);

    // ImGui 1.92+: ImFont owns a pointer back to its atlas.
    ImFontAtlas* atlas = font->OwnerAtlas;
    if (!atlas)
        atlas = ImGui::GetIO().Fonts;
    if (!atlas)
        return std::string(charset_utf8, charset_utf8 + len);

    // IMPORTANT for this app: our Vulkan backend asserts textures are RGBA32.
    // Do NOT request Alpha8 here; always sample RGBA32 alpha channel.
    unsigned char* pixels_rgba = nullptr;
    int tex_w = 0, tex_h = 0, bpp = 0;
    if (atlas->TexData && atlas->TexData->Pixels && atlas->TexData->Format == ImTextureFormat_RGBA32)
    {
        pixels_rgba = atlas->TexData->Pixels;
        tex_w = atlas->TexData->Width;
        tex_h = atlas->TexData->Height;
        bpp = atlas->TexData->BytesPerPixel;
    }
    if (!pixels_rgba || tex_w <= 0 || tex_h <= 0 || bpp != 4)
        atlas->GetTexDataAsRGBA32(&pixels_rgba, &tex_w, &tex_h, &bpp);
    if (!pixels_rgba || tex_w <= 0 || tex_h <= 0 || bpp != 4)
        return std::string(charset_utf8, charset_utf8 + len);

    std::vector<char32_t> cps;
    ansl::utf8::decode_to_codepoints(charset_utf8, len, cps);
    if (cps.empty())
        return std::string(charset_utf8, charset_utf8 + len);

    std::vector<ScoredCp> scored;
    scored.reserve(cps.size());

    // In ImGui 1.92+, glyph lookup is done via ImFontBaked (font data at a given size).
    // Don't rely on ImGui::GetFontBaked()/GetFontSize() being valid at script load time.
    // Use the font's legacy size (the size it was added with), which is stable in this app (Unscii 16).
    const float bake_size = (font->LegacySize > 0.0f) ? font->LegacySize : 16.0f;
    ImFontBaked* baked = const_cast<ImFont*>(font)->GetFontBaked(bake_size);
    if (!baked || baked->OwnerFont != font)
        return std::string(charset_utf8, charset_utf8 + len);

    for (size_t i = 0; i < cps.size(); ++i)
    {
        const char32_t cp = cps[i];
        ScoredCp s;
        s.cp = cp;
        s.index = (int)i;
        s.score = GlyphInkScoreRgba32(baked, pixels_rgba, tex_w, tex_h, cp);
        scored.push_back(s);
    }

    auto cmp = [&](const ScoredCp& a, const ScoredCp& b) {
        if (a.score != b.score)
            return ascending ? (a.score < b.score) : (a.score > b.score);
        return a.index < b.index;
    };
    std::stable_sort(scored.begin(), scored.end(), cmp);

    std::string out;
    out.reserve(len);
    for (const auto& s : scored)
        out += ansl::utf8::encode(s.cp);
    return out;
}
} // namespace ansl::sort


