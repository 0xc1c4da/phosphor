// Markdown -> AnsiCanvas import UI.
//
// Pattern-matched from ImageToChafaDialog:
// - preview window renders an AnsiCanvas
// - separate settings window (pinned by default)
// - debounced async worker regenerates preview on settings changes
#pragma once

#include "io/io_manager.h"
#include "io/formats/markdown.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class MarkdownToAnsiDialog
{
public:
    using Payload  = IoManager::Callbacks::MarkdownPayload;
    using Settings = formats::markdown::ImportOptions;

    ~MarkdownToAnsiDialog();

    // Opens the dialog and takes ownership of the payload (path + markdown bytes).
    void Open(Payload payload);

    // Render the attached preview + settings windows (call every frame). No-op when closed.
    void Render(struct SessionState* session, bool apply_placement_this_frame);

    // If the user pressed OK since last call, moves the resulting canvas into `out`.
    bool TakeAccepted(AnsiCanvas& out);

    Settings& GetSettings() { return settings_; }
    const Settings& GetSettings() const { return settings_; }

    // Returns the source path for the currently-open dialog (empty if closed).
    const std::string& SourcePath() const { return payload_.path; }

private:
    bool open_ = false;
    bool dirty_ = true;
    bool settings_pinned_ = true;

    // Last known preview window rect (used to position the settings window when pinned).
    float preview_win_x_ = 0.0f;
    float preview_win_y_ = 0.0f;
    float preview_win_w_ = 0.0f;
    float preview_win_h_ = 0.0f;

    Payload  payload_;
    Settings settings_;

    std::vector<formats::markdown::ThemeInfo> themes_;
    int theme_index_ = 0;
    std::string themes_error_;

    AnsiCanvas preview_{80};
    bool       has_preview_ = false;
    std::string error_;

    bool accepted_ = false;
    AnsiCanvas accepted_canvas_{80};

    // Debounced + async preview generation.
    void StartWorker();
    void StopWorker();
    void EnqueuePreviewJob();
    void PollPreviewResult();

    struct Job
    {
        uint64_t gen = 0;
        const Payload* payload = nullptr; // points to payload_ (stable while open + worker running)
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

    uint64_t requested_gen_ = 0;
    uint64_t applied_gen_ = 0;
    bool preview_inflight_ = false;

    double dirty_since_ = 0.0;
};


