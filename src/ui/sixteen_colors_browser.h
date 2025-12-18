#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct SessionState;

// Minimal 16colo.rs browser window:
// - Browse/search packs via https://api.16colo.rs
// - Show a thumbnail gallery for files in a pack
// - Click to download raw bytes and import as a canvas (ANS/ASC/TXT) or image window (PNG/JPG/...)
class SixteenColorsBrowserWindow
{
public:
    struct Callbacks
    {
        std::function<void(class AnsiCanvas&&)> create_canvas;

        struct LoadedImage
        {
            std::string path; // can be URL
            int width = 0;
            int height = 0;
            std::vector<unsigned char> pixels; // RGBA8
        };
        std::function<void(LoadedImage&&)> create_image;
    };

    SixteenColorsBrowserWindow();
    ~SixteenColorsBrowserWindow();

    void Render(const char* title, bool* p_open, const Callbacks& cb,
                SessionState* session = nullptr, bool apply_placement_this_frame = false);

private:
    struct DownloadJob
    {
        std::string url;
        std::string kind; // "thumb" | "raw" | "pack_list" | "pack_detail"
        std::string pack;
        std::string filename;
    };

    struct DownloadResult
    {
        DownloadJob job;
        long status = 0;
        std::vector<std::uint8_t> bytes;
        std::string err;
    };

    void StartWorker();
    void StopWorker();

    void Enqueue(const DownloadJob& j);
    bool DequeueResult(DownloadResult& out);

    // State
    std::thread m_worker;
    bool m_worker_running = false;

    std::mutex m_mu;
    std::vector<DownloadJob> m_jobs;
    std::vector<DownloadResult> m_results;

    // UI state
    std::string m_filter;
    int m_page = 1;
    int m_pagesize = 50;
    bool m_show_groups = true;
    bool m_show_artists = false;
    bool m_loading_list = false;
    bool m_dirty_list = true;

    std::string m_selected_pack;
    bool m_loading_pack = false;
    std::string m_last_error;

    // Cached API payloads (raw JSON text)
    std::string m_pack_list_json;
    std::string m_pack_detail_json;

    // Thumbnail cache by full URL
    struct Thumb
    {
        bool requested = false;
        bool ready = false;
        bool failed = false;
        int w = 0;
        int h = 0;
        std::vector<unsigned char> rgba;
        std::string err;
    };
    std::unordered_map<std::string, Thumb> m_thumbs;

    // Network in-flight tracking (async via worker thread)
    bool m_pack_list_pending = false;
    std::string m_pack_list_pending_url;
    bool m_pack_detail_pending = false;
    std::string m_pack_detail_pending_pack;

    int m_raw_pending = 0;
};


