// Image -> Chafa conversion UI.
//
// Renders a normal resizable preview window (using AnsiCanvas::Render) plus a separate
// floating settings window. The settings window is "pinned" next to the preview by
// default; closing either closes the whole conversion UI.
#pragma once

#include "io/convert/chafa_convert.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class ImageToChafaDialog
{
public:
    using ImageRgba = chafa_convert::ImageRgba;
    using Settings  = chafa_convert::Settings;

    ~ImageToChafaDialog();

    // Opens the modal and takes ownership of a copy of the source pixels.
    void Open(ImageRgba src);

    // Render the attached preview + settings windows (call every frame). No-op when closed.
    void Render(struct SessionState* session, bool apply_placement_this_frame);

    // If the user pressed OK since last call, moves the resulting canvas into `out`.
    bool TakeAccepted(AnsiCanvas& out);

    // Expose settings for persistence/customization if desired.
    Settings& GetSettings() { return settings_; }
    const Settings& GetSettings() const { return settings_; }

private:
    bool open_ = false;
    bool dirty_ = true;
    bool settings_pinned_ = true;

    // Last known preview window rect (used to position the settings window when pinned).
    float preview_win_x_ = 0.0f;
    float preview_win_y_ = 0.0f;
    float preview_win_w_ = 0.0f;
    float preview_win_h_ = 0.0f;

    ImageRgba src_;
    Settings  settings_;

    AnsiCanvas preview_{80};
    bool       has_preview_ = false;
    std::string error_;

    bool accepted_ = false;
    AnsiCanvas accepted_canvas_{80};

    bool RegeneratePreview();

    // Debounced + async preview generation.
    void StartWorker();
    void StopWorker();
    void EnqueuePreviewJob();
    void PollPreviewResult();

    struct Job
    {
        uint64_t gen = 0;
        const ImageRgba* src = nullptr; // points to src_ (stable while open + worker running)
        Settings settings;
    };
    struct Result
    {
        uint64_t gen = 0;
        bool ok = false;
        AnsiCanvas canvas{80};
        std::string err;
    };

    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool worker_running_ = false;

    std::optional<Job> pending_job_;
    std::optional<Result> completed_;

    uint64_t requested_gen_ = 0; // latest enqueued generation
    uint64_t applied_gen_ = 0;   // latest applied generation
    bool preview_inflight_ = false;

    double dirty_since_ = 0.0; // ImGui time when settings last changed
};



