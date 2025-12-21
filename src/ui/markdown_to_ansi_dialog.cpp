// Markdown -> AnsiCanvas import dialog implementation.

#include "ui/markdown_to_ansi_dialog.h"

#include "imgui.h"
#include "io/session/imgui_persistence.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ui/imgui_window_chrome.h"

#include <algorithm>
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

    const float max_x = std::max(vp_min.x, vp_max.x - size.x);
    const float max_y = std::max(vp_min.y, vp_max.y - size.y);
    return ImVec2(std::clamp(pos.x, vp_min.x, max_x), std::clamp(pos.y, vp_min.y, max_y));
}
} // namespace

MarkdownToAnsiDialog::~MarkdownToAnsiDialog()
{
    StopWorker();
}

void MarkdownToAnsiDialog::Open(Payload payload)
{
    StopWorker();

    payload_ = std::move(payload);

    open_ = true;
    dirty_ = true;
    dirty_since_ = -1e9;
    error_.clear();
    has_preview_ = false;
    accepted_ = false;
    preview_inflight_ = false;
    requested_gen_ = 0;
    applied_gen_ = 0;
    pending_job_.reset();
    completed_.reset();

    settings_pinned_ = true;

    // Load built-in themes (best-effort).
    themes_.clear();
    themes_error_.clear();
    theme_index_ = 0;
    {
        std::string terr;
        std::vector<formats::markdown::ThemeInfo> t;
        if (formats::markdown::ListBuiltinThemes(t, terr))
        {
            themes_ = std::move(t);
            // Try to default to the importer default theme if present.
            for (int i = 0; i < (int)themes_.size(); ++i)
            {
                if (themes_[i].path.size() >= 8 && themes_[i].path.rfind("dark.json") != std::string::npos)
                {
                    theme_index_ = i;
                    break;
                }
            }
        }
        else
        {
            themes_error_ = terr.empty() ? "No themes available." : terr;
        }
    }

    if (!themes_.empty())
        settings_.theme_path = themes_[theme_index_].path;
    else
        settings_.theme_path.clear();

    StartWorker();
}

void MarkdownToAnsiDialog::StartWorker()
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

            if (!job.payload)
            {
                res.ok = false;
                res.err = "No Markdown payload.";
            }
            else
            {
                AnsiCanvas out;
                std::string err;
                res.ok = formats::markdown::ImportMarkdownToCanvas(job.payload->markdown, out, err, job.settings);
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

void MarkdownToAnsiDialog::StopWorker()
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

void MarkdownToAnsiDialog::EnqueuePreviewJob()
{
    if (!open_)
        return;
    StartWorker();

    Job j;
    j.gen = ++requested_gen_;
    j.payload = &payload_;
    j.settings = settings_;

    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_job_ = std::move(j);
    }
    preview_inflight_ = true;
    cv_.notify_one();
}

void MarkdownToAnsiDialog::PollPreviewResult()
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

    if (r.gen != requested_gen_)
        return;

    applied_gen_ = r.gen;
    preview_inflight_ = false;

    if (!r.ok)
    {
        error_ = r.err.empty() ? "Markdown import failed." : r.err;
        has_preview_ = false;
        return;
    }

    preview_ = std::move(r.canvas);
    has_preview_ = true;
    error_.clear();
}

void MarkdownToAnsiDialog::Render(SessionState* session, bool apply_placement_this_frame)
{
    if (!open_)
        return;

    StartWorker();
    PollPreviewResult();

    const double now = ImGui::GetTime();
    if (dirty_ && (now - dirty_since_) >= kPreviewDebounceSeconds)
    {
        EnqueuePreviewJob();
        dirty_ = false;
    }

    // ---------------------------------------------------------------------
    // Preview window
    // ---------------------------------------------------------------------
    const char* preview_title = "Markdown \xE2\x86\x92 Canvas##md_preview";
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
        if (!open_)
        {
            StopWorker();
            if (!accepted_)
                payload_ = Payload{};
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

        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        preview_win_x_ = wp.x;
        preview_win_y_ = wp.y;
        preview_win_w_ = ws.x;
        preview_win_h_ = ws.y;

        ImGui::Text("Source: %s", payload_.path.empty() ? "(markdown)" : payload_.path.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu bytes)", payload_.markdown.size());
        ImGui::Separator();

        if (preview_inflight_ || dirty_)
            ImGui::TextDisabled("Preview updating\u2026");
        if (!themes_error_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Themes: %s", themes_error_.c_str());
        if (!error_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());

        if (has_preview_)
            preview_.Render("##md_preview_canvas", std::function<void(AnsiCanvas&, int)>{});
        else
            ImGui::TextUnformatted("(no preview)");
    }
    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);

    if (!open_)
    {
        StopWorker();
        if (!accepted_)
            payload_ = Payload{};
        has_preview_ = false;
        error_.clear();
        dirty_ = true;
        return;
    }

    // ---------------------------------------------------------------------
    // Settings window
    // ---------------------------------------------------------------------
    const char* settings_title = "Markdown Import Settings##md_settings";
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 8.0f;

    const ImVec2 approx_size(540.0f, 780.0f);
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

    open_ = settings_open;
    if (!open_)
    {
        StopWorker();
        ImGui::End();
        if (!accepted_)
            payload_ = Payload{};
        has_preview_ = false;
        error_.clear();
        dirty_ = true;
        return;
    }

    bool chrome_changed = false;
    chrome_changed |= ImGui::Checkbox("Pin to preview", &settings_pinned_);
    (void)chrome_changed;
    ImGui::Separator();

    const float footer_h = ImGui::GetFrameHeightWithSpacing() * 2.5f;
    if (ImGui::BeginChild("##md_settings_scroll", ImVec2(0.0f, -footer_h), false))
    {
        bool changed = false;

        if (ImGui::CollapsingHeader("Canvas", ImGuiTreeNodeFlags_DefaultOpen))
        {
            changed |= ImGui::InputInt("Width (columns)", &settings_.columns);
            settings_.columns = std::clamp(settings_.columns, 20, 400);

            changed |= ImGui::InputInt("Max rows", &settings_.max_rows);
            settings_.max_rows = std::clamp(settings_.max_rows, 100, 200000);

            changed |= ImGui::Checkbox("Preserve blank lines", &settings_.preserve_blank_lines);
        }

        if (ImGui::CollapsingHeader("Wrapping", ImGuiTreeNodeFlags_DefaultOpen))
        {
            changed |= ImGui::Checkbox("Wrap paragraphs", &settings_.wrap_paragraphs);
            const char* sb_items[] = {"Space", "Newline"};
            int sb = (settings_.soft_break == Settings::SoftBreak::Space) ? 0 : 1;
            if (ImGui::Combo("Soft breaks", &sb, sb_items, IM_ARRAYSIZE(sb_items)))
            {
                settings_.soft_break = (sb == 0) ? Settings::SoftBreak::Space : Settings::SoftBreak::Newline;
                changed = true;
            }
        }

        if (ImGui::CollapsingHeader("Theme##md_theme_hdr", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (themes_.empty())
            {
                ImGui::TextDisabled("(no themes loaded)");
            }
            else
            {
                std::string preview = themes_[theme_index_].name;
                if (!themes_[theme_index_].author.empty())
                {
                    preview += " \xE2\x80\x94 ";
                    preview += themes_[theme_index_].author;
                }
                if (ImGui::BeginCombo("Theme##md_theme_combo", preview.c_str()))
                {
                    for (int i = 0; i < (int)themes_.size(); ++i)
                    {
                        const bool selected = (i == theme_index_);
                        std::string label = themes_[i].name;
                        if (!themes_[i].author.empty())
                        {
                            label += " \xE2\x80\x94 ";
                            label += themes_[i].author;
                        }
                        // Ensure stable unique ImGui IDs even if multiple themes share the same name/author.
                        ImGui::PushID(i);
                        if (ImGui::Selectable(label.c_str(), selected))
                        {
                            theme_index_ = i;
                            settings_.theme_path = themes_[i].path;
                            changed = true;
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("%s", settings_.theme_path.c_str());
            }
        }

        if (ImGui::CollapsingHeader("Links", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* link_items[] = {"Text only", "Inline URL"};
            int lm = (settings_.link_mode == Settings::LinkMode::TextOnly) ? 0 : 1;
            if (ImGui::Combo("Render mode", &lm, link_items, IM_ARRAYSIZE(link_items)))
            {
                settings_.link_mode = (lm == 0) ? Settings::LinkMode::TextOnly : Settings::LinkMode::InlineUrl;
                changed = true;
            }
        }

        if (ImGui::CollapsingHeader("Code blocks", ImGuiTreeNodeFlags_DefaultOpen))
        {
            changed |= ImGui::Checkbox("Show language label", &settings_.show_code_language);
        }

        if (changed)
        {
            dirty_ = true;
            dirty_since_ = ImGui::GetTime();
        }

        ImGui::EndChild();
    }

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
        StopWorker();
        if (!accepted_)
            payload_ = Payload{};
        has_preview_ = false;
        error_.clear();
        dirty_ = true;
    }
}

bool MarkdownToAnsiDialog::TakeAccepted(AnsiCanvas& out)
{
    if (!accepted_)
        return false;
    out = std::move(accepted_canvas_);
    accepted_ = false;
    // Drop source payload memory once the app has consumed the accepted canvas.
    payload_ = Payload{};
    return true;
}


