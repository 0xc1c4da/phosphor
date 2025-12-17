#pragma once

#include <SDL3/SDL_dialog.h>

#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct SdlFileDialogResult
{
    int tag = 0;
    int filter_index = -1;

    // If `error` is non-empty, the dialog failed (see SDL_GetError()).
    std::string error;

    // True if the user canceled / chose nothing.
    bool canceled = false;

    // UTF-8 paths. On Android these may be content:// URIs.
    std::vector<std::string> paths;
};

// Small helper that turns SDL3's async file dialogs into a pollable queue.
// NOTE: SDL may invoke the callback on another thread, so this class is thread-safe.
class SdlFileDialogQueue
{
public:
    using FilterPair = std::pair<std::string, std::string>; // (label, pattern)

    // Show an "Open File" dialog.
    void ShowOpenFileDialog(int tag,
                            SDL_Window* window,
                            const std::vector<FilterPair>& filters,
                            const std::string& default_location,
                            bool allow_many);

    // Show a "Save File" dialog.
    void ShowSaveFileDialog(int tag,
                            SDL_Window* window,
                            const std::vector<FilterPair>& filters,
                            const std::string& default_location);

    // Poll for completed dialog results.
    bool Poll(SdlFileDialogResult& out);

private:
    struct Request
    {
        SdlFileDialogQueue* self = nullptr;
        int tag = 0;

        std::string default_location;

        // Filters must remain valid until callback returns.
        std::vector<std::string> filter_labels;
        std::vector<std::string> filter_patterns;
        std::vector<SDL_DialogFileFilter> sdl_filters;
    };

    static void BuildRequestFilters(Request& req, const std::vector<FilterPair>& filters);
    static void SDLCALL DialogCallback(void* userdata, const char* const* filelist, int filter);

    void Push(SdlFileDialogResult&& r);

    std::mutex mtx_;
    std::deque<SdlFileDialogResult> queue_;
};


