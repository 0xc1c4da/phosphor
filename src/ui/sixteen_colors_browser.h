#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <atomic>
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
    enum class BrowseMode
    {
        Packs = 0,
        Groups,
        Artists,
        Years,
        Latest,
    };

    enum class LeftView
    {
        RootList = 0, // groups/artists/years, or packs when mode==Packs/Latest
        PacksList,    // drill-down pack list for selected group/artist/year
    };

    struct DownloadJob
    {
        std::string url;
        std::string kind; // "thumb" | "raw" | "pack_list" | "pack_detail" | "group_list" | "artist_list" | "year_list" | "latest_list" | "group_packs" | "artist_packs" | "year_packs"
        std::string pack;
        std::string filename;
        int page = 0; // for paged list endpoints (pack/group/artist)
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
    std::vector<std::thread> m_workers;
    bool m_worker_running = false;

    std::mutex m_mu;
    std::vector<DownloadJob> m_jobs;
    std::vector<DownloadResult> m_results;

    // UI state
    BrowseMode m_mode = BrowseMode::Latest;
    LeftView m_left_view = LeftView::RootList;
    bool m_auto_selected_latest = false;

    // Shared query/filter state
    std::string m_filter;
    int m_page = 1;
    int m_pagesize = 50;
    bool m_show_groups = true;
    bool m_show_artists = false;
    bool m_loading_list = false;
    bool m_dirty_list = true;

    // Root list paging/sorting for groups/artists
    int m_root_pagesize = 80;
    int m_group_sort = 1;  // 0=name, 1=packs
    int m_group_order = 1; // 0=asc, 1=desc
    int m_artist_sort = 1;  // 0=name, 1=releases
    int m_artist_order = 1; // 0=asc, 1=desc

    // Years mode
    bool m_year_include_mags = false;

    // Gallery filters (Pack view)
    std::string m_file_filter;
    std::string m_tag_filter;
    int m_ext_filter = 0; // 0=All, else index into dynamic list each frame

    std::string m_selected_pack;
    std::string m_selected_group;
    std::string m_selected_artist;
    int m_selected_year = 0;
    bool m_loading_pack = false;
    std::string m_last_error;

    // Cached API payloads (raw JSON text)
    std::string m_pack_list_json;
    std::string m_pack_detail_json;
    std::string m_group_list_json;
    std::string m_artist_list_json;
    std::string m_year_list_json;
    std::string m_latest_list_json;
    std::string m_drill_packs_json; // packs list derived from group/artist/year selection (normalized where possible)

    // Parsed + accumulated root lists (for infinite scrolling)
    struct PackRow { std::string name; int year = 0; };
    struct GroupRow { std::string name; int releases = 0; };
    struct ArtistRow { std::string name; int releases = 0; };

    std::vector<PackRow> m_pack_rows;
    std::vector<GroupRow> m_group_rows;
    std::vector<ArtistRow> m_artist_rows;

    int m_pack_pages = 0;
    int m_pack_next_page = 1;
    int m_group_pages = 0;
    int m_group_next_page = 1;
    int m_artist_pages = 0;
    int m_artist_next_page = 1;

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
    bool m_root_list_pending = false;
    std::string m_root_list_pending_url;
    bool m_drill_packs_pending = false;
    std::string m_drill_packs_pending_key; // group/artist/year identifier for staleness checks
    bool m_auto_selected_drill_pack = false;

    int m_raw_pending = 0;
};


