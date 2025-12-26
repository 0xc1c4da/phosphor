// Image -> Chafa conversion dialog implementation.

#include "ui/image_to_chafa_dialog.h"

#include "core/i18n.h"
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
    // Use a stable persistence key across language changes (session placement + chrome state).
    const char* preview_key = "chafa_preview";
    const std::string preview_title = PHOS_TR("chafa.preview_title") + "###" + preview_key;
    if (session)
        ApplyImGuiWindowPlacement(*session, preview_key, apply_placement_this_frame);
    ImGui::SetNextWindowSize(ImVec2(1100.0f, 720.0f), ImGuiCond_Appearing);

    const ImGuiWindowFlags preview_flags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        (session ? GetImGuiWindowChromeExtraFlags(*session, preview_key) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, preview_key);

    if (!ImGui::Begin(preview_title.c_str(), &open_, preview_flags))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, preview_key);
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
            CaptureImGuiWindowPlacement(*session, preview_key);
            ApplyImGuiWindowChromeZOrder(session, preview_key);
            RenderImGuiWindowChromeMenu(session, preview_key);
        }

        // Track preview window rect for settings pinning.
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        preview_win_x_ = wp.x;
        preview_win_y_ = wp.y;
        preview_win_w_ = ws.x;
        preview_win_h_ = ws.y;

        const std::string src_label = src_.label.empty() ? PHOS_TR("chafa.image_label") : src_.label;
        ImGui::TextUnformatted(PHOS_TRF("chafa.source_fmt", phos::i18n::Arg::Str(src_label)).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s",
                            PHOS_TRF("chafa.dims_fmt",
                                     phos::i18n::Arg::I64((long long)src_.width),
                                     phos::i18n::Arg::I64((long long)src_.height)).c_str());
        ImGui::Separator();

        if (preview_inflight_ || dirty_)
            ImGui::TextDisabled("%s", PHOS_TR("chafa.preview_updating_ellipsis").c_str());
        if (!error_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());

        if (has_preview_)
            preview_.Render("##chafa_preview_canvas", std::function<void(AnsiCanvas&, int)>{});
        else
            ImGui::TextUnformatted(PHOS_TR("chafa.no_preview").c_str());
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
    const std::string settings_title = PHOS_TR("chafa.settings_title") + "###chafa_settings";
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
    if (!ImGui::Begin(settings_title.c_str(), &settings_open, ImGuiWindowFlags_None))
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
    chrome_changed |= ImGui::Checkbox(PHOS_TR("chafa.pin_to_preview").c_str(), &settings_pinned_);
    ImGui::Separator();

    // Scrollable settings body (so the window can stay a reasonable size).
    const float footer_h = ImGui::GetFrameHeightWithSpacing() * 2.5f;
    if (ImGui::BeginChild("##chafa_settings_scroll", ImVec2(0.0f, -footer_h), false))
    {
        bool conversion_changed = false;

        if (ImGui::CollapsingHeader((PHOS_TR("chafa.size_layout") + "###chafa_size").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            conversion_changed |= ImGui::InputInt((PHOS_TR("chafa.columns") + "###chafa_cols").c_str(), &settings_.out_cols);
            settings_.out_cols = std::clamp(settings_.out_cols, 1, 400);

            conversion_changed |= ImGui::Checkbox(PHOS_TR("chafa.auto_rows").c_str(), &settings_.auto_rows);
            if (!settings_.auto_rows)
            {
                conversion_changed |= ImGui::InputInt((PHOS_TR("chafa.rows") + "###chafa_rows").c_str(), &settings_.out_rows);
                settings_.out_rows = std::clamp(settings_.out_rows, 1, 400);
            }
            else
            {
                ImGui::TextDisabled("%s", PHOS_TR("chafa.rows_auto").c_str());
            }

            conversion_changed |= ImGui::SliderFloat((PHOS_TR("chafa.font_ratio") + "###chafa_font_ratio").c_str(),
                                                     &settings_.font_ratio, 0.2f, 2.0f, "%.3f");
            conversion_changed |= ImGui::Checkbox(PHOS_TR("chafa.zoom").c_str(), &settings_.zoom);
            conversion_changed |= ImGui::Checkbox(PHOS_TR("chafa.stretch").c_str(), &settings_.stretch);
        }

        if (ImGui::CollapsingHeader((PHOS_TR("chafa.color_processing") + "###chafa_color").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            const std::string cm0 = PHOS_TR("chafa.color_mode_items.indexed_256");
            const std::string cm1 = PHOS_TR("chafa.color_mode_items.indexed_240");
            const std::string cm2 = PHOS_TR("chafa.color_mode_items.indexed_16");
            const std::string cm3 = PHOS_TR("chafa.color_mode_items.indexed_16_8");
            const std::string cm4 = PHOS_TR("chafa.color_mode_items.indexed_8");
            const std::string cm5 = PHOS_TR("chafa.color_mode_items.default_invert");
            const std::string cm6 = PHOS_TR("chafa.color_mode_items.default_no_codes");
            const char* mode_items[] = { cm0.c_str(), cm1.c_str(), cm2.c_str(), cm3.c_str(), cm4.c_str(), cm5.c_str(), cm6.c_str() };
            conversion_changed |= ImGui::Combo((PHOS_TR("chafa.color_mode") + "###chafa_color_mode").c_str(),
                                               &settings_.canvas_mode, mode_items, IM_ARRAYSIZE(mode_items));
            settings_.canvas_mode = std::clamp(settings_.canvas_mode, 0, (int)IM_ARRAYSIZE(mode_items) - 1);

            const std::string ce0 = PHOS_TR("chafa.color_extractor_items.average");
            const std::string ce1 = PHOS_TR("chafa.color_extractor_items.median");
            const char* extractor_items[] = { ce0.c_str(), ce1.c_str() };
            conversion_changed |= ImGui::Combo((PHOS_TR("chafa.color_extractor") + "###chafa_color_extractor").c_str(),
                                               &settings_.color_extractor, extractor_items, IM_ARRAYSIZE(extractor_items));

            const std::string cs0 = PHOS_TR("chafa.color_space_items.rgb_fast");
            const std::string cs1 = PHOS_TR("chafa.color_space_items.din99d");
            const char* space_items[] = { cs0.c_str(), cs1.c_str() };
            conversion_changed |= ImGui::Combo((PHOS_TR("chafa.color_space") + "###chafa_color_space").c_str(),
                                               &settings_.color_space, space_items, IM_ARRAYSIZE(space_items));

            conversion_changed |= ImGui::Checkbox(PHOS_TR("chafa.preprocessing").c_str(), &settings_.preprocessing);
            conversion_changed |= ImGui::SliderFloat((PHOS_TR("chafa.transparency_threshold") + "###chafa_alpha").c_str(),
                                                     &settings_.transparency_threshold, 0.0f, 1.0f, "%.2f");

            conversion_changed |= ImGui::Checkbox(PHOS_TR("chafa.foreground_only").c_str(), &settings_.fg_only);

            conversion_changed |= ImGui::Checkbox(PHOS_TR("chafa.custom_fg_bg").c_str(), &settings_.use_custom_fg_bg);
            if (settings_.use_custom_fg_bg)
            {
                conversion_changed |= ImGui::Checkbox(PHOS_TR("chafa.invert_fg_bg").c_str(), &settings_.invert_fg_bg);

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

                bool fg_changed = ImGui::ColorEdit3(PHOS_TR("chafa.fg").c_str(), fg, ImGuiColorEditFlags_NoInputs);
                bool bg_changed = ImGui::ColorEdit3(PHOS_TR("chafa.bg").c_str(), bg, ImGuiColorEditFlags_NoInputs);
                if (fg_changed) settings_.fg_rgb = f3_to_rgb(fg);
                if (bg_changed) settings_.bg_rgb = f3_to_rgb(bg);
                conversion_changed |= (fg_changed || bg_changed);
            }

            conversion_changed |= ImGui::SliderInt((PHOS_TR("chafa.work") + "###chafa_work").c_str(), &settings_.work, 1, 9, "%d");
            ImGui::TextDisabled("%s", PHOS_TR("chafa.work_help").c_str());

            conversion_changed |= ImGui::InputInt((PHOS_TR("chafa.threads") + "###chafa_threads").c_str(), &settings_.threads);
            settings_.threads = std::clamp(settings_.threads, -1, 256);
        }

        if (ImGui::CollapsingHeader((PHOS_TR("chafa.symbols") + "###chafa_symbols").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
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
                const std::string empty = PHOS_TR("common.empty_parens");
                const std::string custom = PHOS_TR("common.custom_parens");
                const char* preview = (settings_.symbols_selectors.empty()) ? empty.c_str() : (idx >= 0 ? kSelectorClasses[idx] : custom.c_str());

                if (ImGui::BeginCombo((PHOS_TR("chafa.symbols_class") + "###chafa_symbols_class").c_str(), preview))
                {
                    if (ImGui::Selectable(empty.c_str(), settings_.symbols_selectors.empty()))
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
            conversion_changed |= ImGui::InputTextWithHint(PHOS_TR("chafa.symbols_selectors").c_str(),
                                                          PHOS_TR("chafa.symbols_selectors_hint").c_str(),
                                                          &settings_.symbols_selectors);

            // Fill Class
            {
                const int idx = selector_index_for_value(settings_.fill_selectors, kSelectorClasses, IM_ARRAYSIZE(kSelectorClasses));
                const std::string same = PHOS_TR("chafa.fill_selectors_same_as_symbols");
                const std::string custom = PHOS_TR("common.custom_parens");
                const char* preview = (settings_.fill_selectors.empty()) ? same.c_str() : (idx >= 0 ? kSelectorClasses[idx] : custom.c_str());

                if (ImGui::BeginCombo((PHOS_TR("chafa.fill_class") + "###chafa_fill_class").c_str(), preview))
                {
                    if (ImGui::Selectable(same.c_str(), settings_.fill_selectors.empty()))
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
            conversion_changed |= ImGui::InputTextWithHint(PHOS_TR("chafa.fill_selectors").c_str(),
                                                          PHOS_TR("chafa.fill_selectors_hint").c_str(),
                                                          &settings_.fill_selectors);
            ImGui::TextDisabled("%s", PHOS_TR("chafa.selectors_help").c_str());
        }

        if (ImGui::CollapsingHeader((PHOS_TR("chafa.dithering") + "###chafa_dither").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            const std::string dm0 = PHOS_TR("chafa.dither_mode_items.none");
            const std::string dm1 = PHOS_TR("chafa.dither_mode_items.ordered");
            const std::string dm2 = PHOS_TR("chafa.dither_mode_items.diffusion");
            const std::string dm3 = PHOS_TR("chafa.dither_mode_items.noise");
            const char* dither_items[] = { dm0.c_str(), dm1.c_str(), dm2.c_str(), dm3.c_str() };
            conversion_changed |= ImGui::Combo((PHOS_TR("chafa.dither_mode") + "###chafa_dither_mode").c_str(),
                                               &settings_.dither_mode, dither_items, IM_ARRAYSIZE(dither_items));

            int grain_idx = 2;
            if (settings_.dither_grain <= 1) grain_idx = 0;
            else if (settings_.dither_grain == 2) grain_idx = 1;
            else if (settings_.dither_grain == 4) grain_idx = 2;
            else grain_idx = 3;
            const std::string g0 = PHOS_TR("chafa.grain_items.g1");
            const std::string g1 = PHOS_TR("chafa.grain_items.g2");
            const std::string g2 = PHOS_TR("chafa.grain_items.g4");
            const std::string g3 = PHOS_TR("chafa.grain_items.g8");
            const char* grain_items[] = { g0.c_str(), g1.c_str(), g2.c_str(), g3.c_str() };
            if (ImGui::Combo((PHOS_TR("chafa.grain") + "###chafa_grain").c_str(),
                             &grain_idx, grain_items, IM_ARRAYSIZE(grain_items)))
            {
                settings_.dither_grain = (grain_idx == 0) ? 1 : (grain_idx == 1) ? 2 : (grain_idx == 2) ? 4 : 8;
                conversion_changed = true;
            }

            conversion_changed |= ImGui::DragFloat((PHOS_TR("chafa.intensity") + "###chafa_intensity").c_str(),
                                                   &settings_.dither_intensity, 0.05f, 0.0f, 4.0f, "%.2f");
        }

        if (ImGui::CollapsingHeader((PHOS_TR("chafa.debug") + "###chafa_debug").c_str()))
        {
            conversion_changed |= ImGui::Checkbox(PHOS_TR("chafa.debug_stdout").c_str(), &settings_.debug_stdout);
            if (settings_.debug_stdout)
            {
                conversion_changed |= ImGui::Checkbox(PHOS_TR("chafa.dump_raw_ansi_danger").c_str(), &settings_.debug_dump_raw_ansi);
                ImGui::TextDisabled("%s", PHOS_TR("chafa.raw_tip").c_str());
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
    if (ImGui::Button((PHOS_TR("common.ok") + "###chafa_ok").c_str()))
    {
        accepted_canvas_ = std::move(preview_);
        accepted_ = true;
        open_ = false;
    }
    if (!can_accept)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button((PHOS_TR("common.cancel") + "###chafa_cancel").c_str()))
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


