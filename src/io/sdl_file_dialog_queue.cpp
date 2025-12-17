#include "io/sdl_file_dialog_queue.h"

#include <SDL3/SDL_error.h>

void SdlFileDialogQueue::BuildRequestFilters(Request& req, const std::vector<FilterPair>& filters)
{
    req.filter_labels.clear();
    req.filter_patterns.clear();
    req.sdl_filters.clear();

    req.filter_labels.reserve(filters.size());
    req.filter_patterns.reserve(filters.size());
    req.sdl_filters.reserve(filters.size());

    for (const auto& f : filters)
    {
        req.filter_labels.push_back(f.first);
        req.filter_patterns.push_back(f.second);
    }

    // Take pointers after vectors are populated (no more reallocations).
    for (size_t i = 0; i < filters.size(); ++i)
    {
        SDL_DialogFileFilter sdl{};
        sdl.name = req.filter_labels[i].c_str();
        sdl.pattern = req.filter_patterns[i].c_str();
        req.sdl_filters.push_back(sdl);
    }
}

void SdlFileDialogQueue::ShowOpenFileDialog(int tag,
                                            SDL_Window* window,
                                            const std::vector<FilterPair>& filters,
                                            const std::string& default_location,
                                            bool allow_many)
{
    Request* req = new Request();
    req->self = this;
    req->tag = tag;
    req->default_location = default_location;
    BuildRequestFilters(*req, filters);

    const SDL_DialogFileFilter* sdl_filters = req->sdl_filters.empty() ? nullptr : req->sdl_filters.data();
    const int nfilters = (int)req->sdl_filters.size();
    const char* loc = req->default_location.empty() ? nullptr : req->default_location.c_str();

    SDL_ShowOpenFileDialog(&SdlFileDialogQueue::DialogCallback, req, window, sdl_filters, nfilters, loc, allow_many);
}

void SdlFileDialogQueue::ShowSaveFileDialog(int tag,
                                            SDL_Window* window,
                                            const std::vector<FilterPair>& filters,
                                            const std::string& default_location)
{
    Request* req = new Request();
    req->self = this;
    req->tag = tag;
    req->default_location = default_location;
    BuildRequestFilters(*req, filters);

    const SDL_DialogFileFilter* sdl_filters = req->sdl_filters.empty() ? nullptr : req->sdl_filters.data();
    const int nfilters = (int)req->sdl_filters.size();
    const char* loc = req->default_location.empty() ? nullptr : req->default_location.c_str();

    SDL_ShowSaveFileDialog(&SdlFileDialogQueue::DialogCallback, req, window, sdl_filters, nfilters, loc);
}

bool SdlFileDialogQueue::Poll(SdlFileDialogResult& out)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty())
        return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void SDLCALL SdlFileDialogQueue::DialogCallback(void* userdata, const char* const* filelist, int filter)
{
    Request* req = static_cast<Request*>(userdata);
    if (!req || !req->self)
        return;

    SdlFileDialogResult r;
    r.tag = req->tag;
    r.filter_index = filter;

    if (filelist == nullptr)
    {
        r.error = SDL_GetError();
    }
    else if (filelist[0] == nullptr)
    {
        r.canceled = true;
    }
    else
    {
        for (int i = 0; filelist[i] != nullptr; ++i)
            r.paths.emplace_back(filelist[i]);
    }

    req->self->Push(std::move(r));
    delete req;
}

void SdlFileDialogQueue::Push(SdlFileDialogResult&& r)
{
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.push_back(std::move(r));
}


