// Image -> Chafa conversion dialog implementation.

#include "ui/image_to_chafa_dialog.h"

#include "imgui.h"
#include "io/session/imgui_persistence.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ui/imgui_window_chrome.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
static constexpr double kPreviewDebounceSeconds = 0.15;

static ImVec2 ClampToViewportWorkArea(const ImVec2& pos, const ImVec2& size, const ImGuiViewport* vp)
{
    if (!vp)
        return pos;
    const ImVec2 vp_min = vp->WorkPos;
    const ImVec2 vp_max = ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);

    // Avoid negative ranges if the viewport is tiny.
    const float max_x = std::max(vp_min.x, vp_max.x - size.x);
    const float max_y = std::max(vp_min.y, vp_max.y - size.y);
    return ImVec2(std::clamp(pos.x, vp_min.x, max_x), std::clamp(pos.y, vp_min.y, max_y));
}
} // namespace

ImageToChafaDialog::~ImageToChafaDialog()
{
    StopWorker();
}

void ImageToChafaDialog::Open(ImageRgba src)
{
    StopWorker();

    src_ = std::move(src);
    if (src_.rowstride <= 0)
        src_.rowstride = src_.width * 4;

    open_ = true;
    dirty_ = true;
    dirty_since_ = -1e9; // enqueue immediately on first Render()
    error_.clear();
    has_preview_ = false;
    accepted_ = false;
    preview_inflight_ = false;
    requested_gen_ = 0;
    applied_gen_ = 0;
    pending_job_.reset();
    completed_.reset();

    // Default to pinned settings whenever a new conversion is opened.
    settings_pinned_ = true;

    StartWorker();
}

bool ImageToChafaDialog::RegeneratePreview()
{
    EnqueuePreviewJob();
    return true; // async
}

void ImageToChafaDialog::StartWorker()
{
    if (worker_running_)
        return;
    worker_running_ = true;

    worker_ = std::thread([this]()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&]() { return !worker_running_ || pending_job_.has_value(); });
                if (!worker_running_)
                    return;
                job = std::move(*pending_job_);
                pending_job_.reset();
            }

            Result res;
            res.gen = job.gen;

            if (!job.src)
            {
                res.ok = false;
                res.err = "No image data.";
            }
            else
            {
                AnsiCanvas out;
                std::string err;
                res.ok = chafa_convert::ConvertRgbaToAnsiCanvas(*job.src, job.settings, out, err);
                res.err = std::move(err);
                if (res.ok)
                    res.canvas = std::move(out);
            }

            {
                std::lock_guard<std::mutex> lock(mu_);
                completed_ = std::move(res);
            }
        }
    });
}

void ImageToChafaDialog::StopWorker()
{
    if (!worker_running_)
        return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        worker_running_ = false;
        pending_job_.reset();
        completed_.reset();
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    preview_inflight_ = false;
}

void ImageToChafaDialog::EnqueuePreviewJob()
{
    if (!open_)
        return;
    StartWorker();

    Job j;
    j.gen = ++requested_gen_;
    j.src = &src_;
    j.settings = settings_;

    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_job_ = std::move(j);
    }
    preview_inflight_ = true;
    cv_.notify_one();
}

void ImageToChafaDialog::PollPreviewResult()
{
    Result r;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (completed_)
        {
            r = std::move(*completed_);
            completed_.reset();
            have = true;
        }
    }
    if (!have)
        return;

    // Stale result (a newer job was requested).
    if (r.gen != requested_gen_)
        return;

    applied_gen_ = r.gen;
    preview_inflight_ = false;

    if (!r.ok)
    {
        error_ = r.err.empty() ? "Conversion failed." : r.err;
        has_preview_ = false;
        return;
    }

    preview_ = std::move(r.canvas);
    has_preview_ = true;
    error_.clear();
}

void ImageToChafaDialog::Render(SessionState* session, bool apply_placement_this_frame)
{
    if (!open_)
        return;

    StartWorker();
    PollPreviewResult();

    // Debounced conversion scheduling.
    const double now = ImGui::GetTime();
    if (dirty_ && (now - dirty_since_) >= kPreviewDebounceSeconds)
    {
        EnqueuePreviewJob();
        dirty_ = false;
    }

    // ---------------------------------------------------------------------
    // Preview window (regular resizable window; the canvas itself handles scrolling)
    // ---------------------------------------------------------------------
    const char* preview_title = "Image \xE2\x86\x92 ANSI (Chafa)##chafa_preview";
    if (session)
        ApplyImGuiWindowPlacement(*session, preview_title, apply_placement_this_frame);
    ImGui::SetNextWindowSize(ImVec2(1100.0f, 720.0f), ImGuiCond_Appearing);

    const ImGuiWindowFlags preview_flags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        (session ? GetImGuiWindowChromeExtraFlags(*session, preview_title) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, preview_title);

    if (!ImGui::Begin(preview_title, &open_, preview_flags))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, preview_title);
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        // If the window was closed via the titlebar, also close the attached settings.
        if (!open_)
        {
            StopWorker();
            src_ = ImageRgba{};
            has_preview_ = false;
            error_.clear();
            dirty_ = true;
        }
        return;
    }
    {
        if (session)
        {
            CaptureImGuiWindowPlacement(*session, preview_title);
            ApplyImGuiWindowChromeZOrder(session, preview_title);
            RenderImGuiWindowChromeMenu(session, preview_title);
        }

        // Track preview window rect for settings pinning.
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        preview_win_x_ = wp.x;
        preview_win_y_ = wp.y;
        preview_win_w_ = ws.x;
        preview_win_h_ = ws.y;

        ImGui::Text("Source: %s", src_.label.empty() ? "(image)" : src_.label.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%dx%d)", src_.width, src_.height);
        ImGui::Separator();

        if (preview_inflight_ || dirty_)
            ImGui::TextDisabled("Preview updating\u2026");
        if (!error_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());

        if (has_preview_)
            preview_.Render("##chafa_preview_canvas", std::function<void(AnsiCanvas&, int)>{});
        else
            ImGui::TextUnformatted("(no preview)");
    }
    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);

    // If the preview window was closed, also close settings and drop state.
    if (!open_)
    {
        StopWorker();
        src_ = ImageRgba{};
        has_preview_ = false;
        error_.clear();
        dirty_ = true;
        return;
    }

    // ---------------------------------------------------------------------
    // Settings window (separate floating window, pinned next to preview by default)
    // ---------------------------------------------------------------------
    const char* settings_title = "Chafa Settings##chafa_settings";
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 8.0f;

    // Approximate size for pinning/clamping before we know the actual size.
    // Make settings panel ~20% larger (requested) while keeping pinning logic the same.
    const ImVec2 approx_size(504.0f, 768.0f);
    ImVec2 desired(preview_win_x_ + preview_win_w_ + pad, preview_win_y_);
    if (vp)
    {
        const float vp_right = vp->WorkPos.x + vp->WorkSize.x;
        if (desired.x + approx_size.x > vp_right)
            desired.x = preview_win_x_ - pad - approx_size.x;
    }
    desired = ClampToViewportWorkArea(desired, approx_size, vp);

    if (settings_pinned_)
        ImGui::SetNextWindowPos(desired, ImGuiCond_Always);
    else
        ImGui::SetNextWindowPos(desired, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(approx_size, ImGuiCond_Appearing);

    bool settings_open = open_;
    if (!ImGui::Begin(settings_title, &settings_open, ImGuiWindowFlags_None))
    {
        ImGui::End();
        open_ = settings_open;
        return;
    }

    // Closing the settings window closes the whole conversion UI.
    open_ = settings_open;
    if (!open_)
    {
        StopWorker();
        ImGui::End();
        src_ = ImageRgba{};
        has_preview_ = false;
        error_.clear();
        dirty_ = true;
        return;
    }

    bool chrome_changed = false;
    chrome_changed |= ImGui::Checkbox("Pin to preview", &settings_pinned_);
    ImGui::Separator();

    // Scrollable settings body (so the window can stay a reasonable size).
    const float footer_h = ImGui::GetFrameHeightWithSpacing() * 2.5f;
    if (ImGui::BeginChild("##chafa_settings_scroll", ImVec2(0.0f, -footer_h), false))
    {
        bool conversion_changed = false;

        if (ImGui::CollapsingHeader("Size & layout", ImGuiTreeNodeFlags_DefaultOpen))
        {
            conversion_changed |= ImGui::InputInt("Columns", &settings_.out_cols);
            settings_.out_cols = std::clamp(settings_.out_cols, 1, 400);

            conversion_changed |= ImGui::Checkbox("Auto rows", &settings_.auto_rows);
            if (!settings_.auto_rows)
            {
                conversion_changed |= ImGui::InputInt("Rows", &settings_.out_rows);
                settings_.out_rows = std::clamp(settings_.out_rows, 1, 400);
            }
            else
            {
                ImGui::TextDisabled("Rows: auto");
            }

            conversion_changed |= ImGui::SliderFloat("Font ratio (w/h)", &settings_.font_ratio, 0.2f, 2.0f, "%.3f");
            conversion_changed |= ImGui::Checkbox("Zoom", &settings_.zoom);
            conversion_changed |= ImGui::Checkbox("Stretch", &settings_.stretch);
        }

        if (ImGui::CollapsingHeader("Color & processing", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* mode_items[] =
            {
                "Indexed 256 (xterm)",
                "Indexed 240",
                "Indexed 16",
                "Indexed 16/8",
                "Indexed 8",
                "Default fg/bg + invert",
                "Default fg/bg (no codes)",
            };
            conversion_changed |= ImGui::Combo("Color mode", &settings_.canvas_mode, mode_items, IM_ARRAYSIZE(mode_items));
            settings_.canvas_mode = std::clamp(settings_.canvas_mode, 0, (int)IM_ARRAYSIZE(mode_items) - 1);

            const char* extractor_items[] = {"Average", "Median"};
            conversion_changed |= ImGui::Combo("Color extractor", &settings_.color_extractor, extractor_items, IM_ARRAYSIZE(extractor_items));

            const char* space_items[] = {"RGB (fast)", "DIN99d (perceptual)"};
            conversion_changed |= ImGui::Combo("Color space", &settings_.color_space, space_items, IM_ARRAYSIZE(space_items));

            conversion_changed |= ImGui::Checkbox("Preprocessing", &settings_.preprocessing);
            conversion_changed |= ImGui::SliderFloat("Transparency threshold", &settings_.transparency_threshold, 0.0f, 1.0f, "%.2f");

            conversion_changed |= ImGui::Checkbox("Foreground-only (fg-only)", &settings_.fg_only);

            conversion_changed |= ImGui::Checkbox("Custom fg/bg colors", &settings_.use_custom_fg_bg);
            if (settings_.use_custom_fg_bg)
            {
                conversion_changed |= ImGui::Checkbox("Invert fg/bg", &settings_.invert_fg_bg);

                auto rgb_to_f3 = [](uint32_t rgb, float out[3])
                {
                    out[0] = ((rgb >> 16) & 0xFF) / 255.0f;
                    out[1] = ((rgb >> 8) & 0xFF) / 255.0f;
                    out[2] = (rgb & 0xFF) / 255.0f;
                };
                auto f3_to_rgb = [](const float in[3]) -> uint32_t
                {
                    const uint32_t r = (uint32_t)std::clamp((int)std::lround(in[0] * 255.0f), 0, 255);
                    const uint32_t g = (uint32_t)std::clamp((int)std::lround(in[1] * 255.0f), 0, 255);
                    const uint32_t b = (uint32_t)std::clamp((int)std::lround(in[2] * 255.0f), 0, 255);
                    return (r << 16) | (g << 8) | b;
                };

                float fg[3] = {0, 0, 0};
                float bg[3] = {0, 0, 0};
                rgb_to_f3(settings_.fg_rgb, fg);
                rgb_to_f3(settings_.bg_rgb, bg);

                bool fg_changed = ImGui::ColorEdit3("FG", fg, ImGuiColorEditFlags_NoInputs);
                bool bg_changed = ImGui::ColorEdit3("BG", bg, ImGuiColorEditFlags_NoInputs);
                if (fg_changed) settings_.fg_rgb = f3_to_rgb(fg);
                if (bg_changed) settings_.bg_rgb = f3_to_rgb(bg);
                conversion_changed |= (fg_changed || bg_changed);
            }

            conversion_changed |= ImGui::SliderInt("Work", &settings_.work, 1, 9, "%d");
            ImGui::TextDisabled("Higher work improves quality at the cost of CPU/time (maps to libchafa work_factor 0..1).");

            conversion_changed |= ImGui::InputInt("Threads (-1=auto)", &settings_.threads);
            settings_.threads = std::clamp(settings_.threads, -1, 256);
        }

        if (ImGui::CollapsingHeader("Symbols", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Chafa selector helper: selecting from a dropdown writes into the text field so
            // the active selector is always visible, and editing the text triggers re-render.
            auto selector_index_for_value = [](const std::string& v, const char* const* items, int n_items) -> int
            {
                for (int i = 0; i < n_items; ++i)
                    if (v == items[i])
                        return i;
                return -1;
            };

            // Accepted selector classes (from chafa CLI help): can be combined with + and -.
            static const char* kSelectorClasses[] = {
                "all", "ascii", "braille", "extra", "narrow", "solid",
                "alnum", "bad", "diagonal", "geometric", "inverted", "none", "space", "vhalf",
                "alpha", "block", "digit", "half", "latin", "quad", "stipple", "wedge",
                "ambiguous", "border", "dot", "hhalf", "legacy", "sextant", "technical", "wide",
            };

            // Symbols Class
            {
                const int idx = selector_index_for_value(settings_.symbols_selectors, kSelectorClasses, IM_ARRAYSIZE(kSelectorClasses));
                const char* preview = (settings_.symbols_selectors.empty()) ? "(empty)" : (idx >= 0 ? kSelectorClasses[idx] : "(custom)");

                if (ImGui::BeginCombo("Symbols Class", preview))
                {
                    if (ImGui::Selectable("(empty)", settings_.symbols_selectors.empty()))
                    {
                        settings_.symbols_selectors.clear();
                        conversion_changed = true;
                    }
                    for (int i = 0; i < (int)IM_ARRAYSIZE(kSelectorClasses); ++i)
                    {
                        const bool selected = (!settings_.symbols_selectors.empty() && settings_.symbols_selectors == kSelectorClasses[i]);
                        if (ImGui::Selectable(kSelectorClasses[i], selected))
                        {
                            settings_.symbols_selectors = kSelectorClasses[i];
                            conversion_changed = true;
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            // Symbols Selectors
            conversion_changed |= ImGui::InputTextWithHint("Symbols Selectors", "e.g. block+border-diagonal", &settings_.symbols_selectors);

            // Fill Class
            {
                const int idx = selector_index_for_value(settings_.fill_selectors, kSelectorClasses, IM_ARRAYSIZE(kSelectorClasses));
                const char* preview = (settings_.fill_selectors.empty()) ? "(same as symbols)" : (idx >= 0 ? kSelectorClasses[idx] : "(custom)");

                if (ImGui::BeginCombo("Fill Class", preview))
                {
                    if (ImGui::Selectable("(same as symbols)", settings_.fill_selectors.empty()))
                    {
                        settings_.fill_selectors.clear();
                        conversion_changed = true;
                    }
                    for (int i = 0; i < (int)IM_ARRAYSIZE(kSelectorClasses); ++i)
                    {
                        const bool selected = (!settings_.fill_selectors.empty() && settings_.fill_selectors == kSelectorClasses[i]);
                        if (ImGui::Selectable(kSelectorClasses[i], selected))
                        {
                            settings_.fill_selectors = kSelectorClasses[i];
                            conversion_changed = true;
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            // Fill Selectors
            conversion_changed |= ImGui::InputTextWithHint("Fill Selectors", "(blank = same as symbols)", &settings_.fill_selectors);
            ImGui::TextDisabled("Selectors follow chafa CLI syntax: combine with + and -, e.g. all-wide or block+border-diagonal.");
        }

        if (ImGui::CollapsingHeader("Dithering", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* dither_items[] = {"None", "Ordered", "Diffusion", "Noise"};
            conversion_changed |= ImGui::Combo("Mode", &settings_.dither_mode, dither_items, IM_ARRAYSIZE(dither_items));

            int grain_idx = 2;
            if (settings_.dither_grain <= 1) grain_idx = 0;
            else if (settings_.dither_grain == 2) grain_idx = 1;
            else if (settings_.dither_grain == 4) grain_idx = 2;
            else grain_idx = 3;
            const char* grain_items[] = {"1x1", "2x2", "4x4", "8x8"};
            if (ImGui::Combo("Grain", &grain_idx, grain_items, IM_ARRAYSIZE(grain_items)))
            {
                settings_.dither_grain = (grain_idx == 0) ? 1 : (grain_idx == 1) ? 2 : (grain_idx == 2) ? 4 : 8;
                conversion_changed = true;
            }

            conversion_changed |= ImGui::DragFloat("Intensity", &settings_.dither_intensity, 0.05f, 0.0f, 4.0f, "%.2f");
        }

        if (ImGui::CollapsingHeader("Debug"))
        {
            conversion_changed |= ImGui::Checkbox("Debug to stdout", &settings_.debug_stdout);
            if (settings_.debug_stdout)
            {
                conversion_changed |= ImGui::Checkbox("Dump RAW ANSI (danger)", &settings_.debug_dump_raw_ansi);
                ImGui::TextDisabled("Tip: keep RAW off; use escaped dump to avoid terminal garbage.");
            }
        }

        if (conversion_changed)
        {
            dirty_ = true;
            dirty_since_ = ImGui::GetTime();
        }
        ImGui::EndChild();
    }

    // Footer actions
    ImGui::Separator();
    const bool up_to_date =
        (requested_gen_ > 0) &&
        (applied_gen_ == requested_gen_) &&
        (!dirty_) &&
        (!preview_inflight_);
    const bool can_accept = has_preview_ && error_.empty() && up_to_date;
    if (!can_accept)
        ImGui::BeginDisabled();
    if (ImGui::Button("OK"))
    {
        accepted_canvas_ = std::move(preview_);
        accepted_ = true;
        open_ = false;
    }
    if (!can_accept)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        open_ = false;
    }

    ImGui::End();

    if (!open_)
    {
        // Drop any heavy state immediately when closed.
        StopWorker();
        src_ = ImageRgba{};
        has_preview_ = false;
        error_.clear();
        dirty_ = true;
    }
}

bool ImageToChafaDialog::TakeAccepted(AnsiCanvas& out)
{
    if (!accepted_)
        return false;
    out = std::move(accepted_canvas_);
    accepted_ = false;
    return true;
}


