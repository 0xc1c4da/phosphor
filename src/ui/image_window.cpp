#include "ui/image_window.h"

#include "ui/image_to_chafa_dialog.h"

#include "imgui.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"

#include <algorithm>

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

            // IMPORTANT: apply current style alpha (which includes per-window opacity via PushImGuiWindowChromeAlpha()).
            // Using raw IM_COL32 would bypass style.Alpha and make content ignore the window opacity setting.
            const ImVec4 v(static_cast<float>(r) / 255.0f,
                           static_cast<float>(g) / 255.0f,
                           static_cast<float>(b) / 255.0f,
                           static_cast<float>(a) / 255.0f);
            const ImU32 col = ImGui::GetColorU32(v);
            draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
        }
    }
}

bool RenderImageWindow(const char* title, ImageWindow& image, ImageToChafaDialog& dialog,
                       SessionState* session, bool apply_placement_this_frame)
{
    if (!title || !*title)
        title = "Image";

    if (!image.open)
        return false;

    // Keep IDs stable even if multiple image windows share common widget names.
    ImGui::PushID(image.id);

    if (session)
        ApplyImGuiWindowPlacement(*session, title, apply_placement_this_frame);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_None |
        (session ? GetImGuiWindowChromeExtraFlags(*session, title) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, title);
    if (!ImGui::Begin(title, &image.open, flags))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, title);
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        ImGui::PopID();
        return true;
    }
    if (session)
        CaptureImGuiWindowPlacement(*session, title);
    if (session)
    {
        ApplyImGuiWindowChromeZOrder(session, title);
        RenderImGuiWindowChromeMenu(session, title);
    }

    // Scalable preview (context menu is on the preview region).
    RenderImageWindowContents(image, dialog);

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
    ImGui::PopID();
    return true;
}


