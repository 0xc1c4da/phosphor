#include "ui/sixteen_colors_browser.h"

#include "core/i18n.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include "io/formats/ansi.h"
#include "io/formats/xbin.h"
#include "io/http_client.h"
#include "io/image_loader.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

using json = nlohmann::json;

namespace
{
static std::uint64_t Fnv1a64(const std::string& s)
{
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s)
    {
        h ^= (std::uint64_t)c;
        h *= 1099511628211ull;
    }
    return h;
}

static std::string JsonStringOrEmpty(const json& j, const char* key)
{
    if (!key || !*key)
        return {};
    auto it = j.find(key);
    if (it == j.end())
        return {};
    if (!it->is_string())
        return {};
    return it->get<std::string>();
}

static int JsonIntOrDefault(const json& j, const char* key, int def)
{
    if (!key || !*key)
        return def;
    auto it = j.find(key);
    if (it == j.end())
        return def;
    if (it->is_number_integer())
        return it->get<int>();
    if (it->is_number())
        return (int)std::lround(it->get<double>());
    if (it->is_string())
    {
        // Some endpoints occasionally serialize numbers as strings.
        try
        {
            return std::stoi(it->get<std::string>());
        }
        catch (...) {}
    }
    return def;
}

static std::string UrlEncode(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
    {
        const bool unreserved =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved)
            out.push_back((char)c);
        else if (c == ' ')
            out.push_back('+');
        else
        {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[(c >> 0) & 0xF]);
        }
    }
    return out;
}

static std::string JoinUrl(const std::string& base, const std::string& uri)
{
    if (uri.empty())
        return base;
    if (uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0)
        return uri;
    if (!base.empty() && base.back() == '/' && !uri.empty() && uri.front() == '/')
        return base.substr(0, base.size() - 1) + uri;
    if (!base.empty() && base.back() != '/' && !uri.empty() && uri.front() != '/')
        return base + "/" + uri;
    return base + uri;
}

static std::string ExtLower(const std::string& filename)
{
    auto dot = filename.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string ext = filename.substr(dot + 1);
    for (char& c : ext)
        c = (char)std::tolower((unsigned char)c);
    return ext;
}

static bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
        return true;
    if (haystack.empty())
        return false;
    auto lower = [](unsigned char c) { return (char)std::tolower(c); };
    std::string h;
    std::string n;
    h.resize(haystack.size());
    n.resize(needle.size());
    std::transform(haystack.begin(), haystack.end(), h.begin(), lower);
    std::transform(needle.begin(), needle.end(), n.begin(), lower);
    return h.find(n) != std::string::npos;
}

static std::string ApiBase() { return "https://api.16colo.rs"; }
static std::string WebBase() { return "https://16colo.rs"; }

static int GuessYearFromPackName(const std::string& pack)
{
    // Best-effort heuristic. Many packs embed YYYY or YYMM-style suffixes.
    // We keep this conservative: unknown => 0.
    if (pack.empty())
        return 0;

    // Prefer explicit 4-digit years found anywhere in the string.
    for (size_t i = 0; i + 4 <= pack.size(); ++i)
    {
        if (!std::isdigit((unsigned char)pack[i]) ||
            !std::isdigit((unsigned char)pack[i + 1]) ||
            !std::isdigit((unsigned char)pack[i + 2]) ||
            !std::isdigit((unsigned char)pack[i + 3]))
            continue;
        const int y = (pack[i] - '0') * 1000 + (pack[i + 1] - '0') * 100 + (pack[i + 2] - '0') * 10 + (pack[i + 3] - '0');
        if (y >= 1990 && y <= 2025)
            return y;
    }

    // Trailing YYMM-ish (e.g. mop-9509, ama-0717, ale-0894, ice9703a).
    // We look at the last 6 chars for a 4-digit run.
    const size_t start = (pack.size() > 6) ? (pack.size() - 6) : 0;
    for (size_t i = start; i + 4 <= pack.size(); ++i)
    {
        if (!std::isdigit((unsigned char)pack[i]) ||
            !std::isdigit((unsigned char)pack[i + 1]) ||
            !std::isdigit((unsigned char)pack[i + 2]) ||
            !std::isdigit((unsigned char)pack[i + 3]))
            continue;
        const int yy = (pack[i] - '0') * 10 + (pack[i + 1] - '0');
        // Heuristic pivot: 90-99 => 1990s, 00-25 => 2000s/2020s
        if (yy >= 90 && yy <= 99)
            return 1900 + yy;
        if (yy >= 0 && yy <= 25)
            return 2000 + yy;
    }

    return 0;
}

static std::string BuildPackListUrl(int page, int pagesize, bool groups, bool artists, const std::string& filter)
{
    return ApiBase() + "/v1/pack/?page=" + std::to_string(std::max(1, page)) +
           "&pagesize=" + std::to_string(std::clamp(pagesize, 1, 500)) +
           "&archive=true" +
           std::string("&groups=") + (groups ? "true" : "false") +
           std::string("&artists=") + (artists ? "true" : "false") +
           (filter.empty() ? "" : ("&filter=" + UrlEncode(filter)));
}

static std::string BuildPackDetailUrl(const std::string& pack)
{
    return ApiBase() + "/v1/pack/" + UrlEncode(pack) + "?sauce=false&dimensions=true&content=true&artists=true";
}

static std::string BuildGroupListUrl(int page, int pagesize, int sort, int order, const std::string& filter)
{
    const char* sort_s = (sort == 1) ? "packs" : "name";
    const char* order_s = (order == 1) ? "desc" : "asc";
    return ApiBase() + "/v1/group/?page=" + std::to_string(std::max(1, page)) +
           "&pagesize=" + std::to_string(std::clamp(pagesize, 1, 500)) +
           "&sort=" + sort_s + "&order=" + order_s +
           "&packs=false&artists=false" +
           (filter.empty() ? "" : ("&filter=" + UrlEncode(filter)));
}

static std::string BuildGroupDetailUrl(const std::string& group)
{
    return ApiBase() + "/v1/group/" + UrlEncode(group) + "?packs=true";
}

static std::string BuildArtistListUrl(int page, int pagesize, const std::string& filter)
{
    return ApiBase() + "/v1/artist/?page=" + std::to_string(std::max(1, page)) +
           "&pagesize=" + std::to_string(std::clamp(pagesize, 1, 500)) +
           "&details=true&aliases=false" +
           (filter.empty() ? "" : ("&filter=" + UrlEncode(filter)));
}

static std::string BuildArtistPacksUrl(const std::string& artist)
{
    // The /v1/artist/:name example is missing from the docs; the list endpoint can return full pack lists with details=true.
    // Use a large pagesize so "exact-ish" filter results fit in one response.
    return BuildArtistListUrl(1, 500, artist);
}

static std::string BuildYearListUrl()
{
    return ApiBase() + "/v1/year/";
}

static std::string BuildYearPacksUrl(int year, bool include_mags, const std::string& filter)
{
    const char* type_s = include_mags ? "mags" : "packs";
    return ApiBase() + "/v1/year/" + std::to_string(year) +
           "?type=" + type_s + "&groups=true&sort=pack&order=asc&pagesize=500&page=1" +
           (filter.empty() ? "" : ("&filter=" + UrlEncode(filter)));
}

static std::string BuildLatestUrl()
{
    return ApiBase() + "/v1/latest/releases";
}

// Fast coarse draw: downsample into a small grid of solid rects.
static void DrawRgbaGridExact(const unsigned char* rgba, int grid_w, int grid_h, const ImVec2& size_px)
{
    if (!rgba || grid_w <= 0 || grid_h <= 0)
        return;
    if (size_px.x <= 0.0f || size_px.y <= 0.0f)
        return;

    ImGui::InvisibleButton("##thumb_canvas", size_px);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255), 4.0f);

    const float cell_w = (p1.x - p0.x) / (float)grid_w;
    const float cell_h = (p1.y - p0.y) / (float)grid_h;

    for (int gy = 0; gy < grid_h; ++gy)
    {
        const float y0 = p0.y + gy * cell_h;
        const float y1 = y0 + cell_h;
        for (int gx = 0; gx < grid_w; ++gx)
        {
            const float x0 = p0.x + gx * cell_w;
            const float x1 = x0 + cell_w;
            const size_t base = ((size_t)gy * (size_t)grid_w + (size_t)gx) * 4u;
            const unsigned char r = rgba[base + 0];
            const unsigned char g = rgba[base + 1];
            const unsigned char b = rgba[base + 2];
            const unsigned char a = rgba[base + 3];

            // IMPORTANT: apply current style alpha (window opacity).
            const ImVec4 v((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, (float)a / 255.0f);
            const ImU32 col = ImGui::GetColorU32(v);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
        }
    }
    dl->AddRect(p0, p1, IM_COL32(90, 90, 105, 255), 4.0f);
}

static inline unsigned char ClampU8(int v)
{
    return (unsigned char)std::clamp(v, 0, 255);
}

// Build a small, consistent preview by center-cropping to the target aspect ratio and bilinear resampling.
// This makes very tall thumbs look consistent (no squashing) and avoids expensive per-frame resampling.
static bool BuildThumbPreviewCoverBilinear(const unsigned char* src_rgba, int sw, int sh,
                                           int dw, int dh,
                                           std::vector<unsigned char>& out_rgba)
{
    if (!src_rgba || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return false;

    out_rgba.assign((size_t)dw * (size_t)dh * 4u, 0);

    const float src_w = (float)sw;
    const float src_h = (float)sh;
    const float dst_aspect = (float)dw / (float)dh;
    const float src_aspect = src_w / src_h;

    // Crop rect in source space (float) to match the destination aspect ratio.
    float crop_x0 = 0.0f, crop_y0 = 0.0f, crop_w = src_w, crop_h = src_h;
    if (src_aspect > dst_aspect)
    {
        // Too wide: crop width.
        crop_h = src_h;
        crop_w = crop_h * dst_aspect;
        crop_x0 = (src_w - crop_w) * 0.5f;
        crop_y0 = 0.0f;
    }
    else if (src_aspect < dst_aspect)
    {
        // Too tall: crop height.
        crop_w = src_w;
        crop_h = crop_w / dst_aspect;
        crop_x0 = 0.0f;
        crop_y0 = (src_h - crop_h) * 0.5f;
    }

    auto sample = [&](int x, int y, int c) -> int {
        x = std::clamp(x, 0, sw - 1);
        y = std::clamp(y, 0, sh - 1);
        return (int)src_rgba[((size_t)y * (size_t)sw + (size_t)x) * 4u + (size_t)c];
    };

    for (int y = 0; y < dh; ++y)
    {
        for (int x = 0; x < dw; ++x)
        {
            // Pixel-center mapping.
            const float u = ((float)x + 0.5f) / (float)dw;
            const float v = ((float)y + 0.5f) / (float)dh;
            const float sx = crop_x0 + u * crop_w - 0.5f;
            const float sy = crop_y0 + v * crop_h - 0.5f;

            const int x0 = (int)std::floor(sx);
            const int y0 = (int)std::floor(sy);
            const int x1 = x0 + 1;
            const int y1 = y0 + 1;
            const float tx = sx - (float)x0;
            const float ty = sy - (float)y0;

            for (int c = 0; c < 4; ++c)
            {
                const float c00 = (float)sample(x0, y0, c);
                const float c10 = (float)sample(x1, y0, c);
                const float c01 = (float)sample(x0, y1, c);
                const float c11 = (float)sample(x1, y1, c);
                const float cx0 = c00 + (c10 - c00) * tx;
                const float cx1 = c01 + (c11 - c01) * tx;
                const float cv = cx0 + (cx1 - cx0) * ty;
                out_rgba[((size_t)y * (size_t)dw + (size_t)x) * 4u + (size_t)c] = ClampU8((int)std::lround(cv));
            }
        }
    }
    return true;
}
} // namespace

SixteenColorsBrowserWindow::SixteenColorsBrowserWindow()
{
    StartWorker();
}

SixteenColorsBrowserWindow::~SixteenColorsBrowserWindow()
{
    StopWorker();
}

void SixteenColorsBrowserWindow::StartWorker()
{
    if (m_worker_running)
        return;
    m_worker_running = true;

    // Small worker pool: thumbnails are the hot path and benefit a lot from parallelism.
    const int worker_count = 4;
    m_workers.clear();
    m_workers.reserve(worker_count);
    for (int i = 0; i < worker_count; ++i)
    {
        m_workers.emplace_back([this]()
        {
            while (m_worker_running)
            {
                DownloadJob job;
                {
                    std::lock_guard<std::mutex> lock(m_mu);
                    if (!m_jobs.empty())
                    {
                        job = m_jobs.front();
                        m_jobs.erase(m_jobs.begin());
                    }
                }

                if (job.url.empty())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }

                DownloadResult res;
                res.job = job;

                http::Response r = http::Get(job.url, {}, job.cache_mode);
                res.status = r.status;
                res.err = r.err;
                res.bytes = std::move(r.body);
                res.from_cache = r.from_cache;
                res.changed = r.changed;

                {
                    std::lock_guard<std::mutex> lock(m_mu);
                    m_results.push_back(std::move(res));
                }
            }
        });
    }
}

void SixteenColorsBrowserWindow::StopWorker()
{
    if (!m_worker_running)
        return;
    m_worker_running = false;
    for (auto& t : m_workers)
    {
        if (t.joinable())
            t.join();
    }
    m_workers.clear();
}

void SixteenColorsBrowserWindow::Enqueue(const DownloadJob& j)
{
    std::lock_guard<std::mutex> lock(m_mu);
    // Priority order:
    // 1) raw (user action)
    // 2) pack_detail + lists (navigation)
    // 3) thumbnails (background)
    // 4) spider (datahoarder) (lowest priority)
    if (j.kind == "raw")
    {
        m_jobs.insert(m_jobs.begin(), j);
        return;
    }

    const bool nav =
        (j.kind == "pack_detail" || j.kind == "pack_list" ||
         j.kind == "group_list" || j.kind == "artist_list" || j.kind == "year_list" || j.kind == "latest_list" ||
         j.kind == "group_packs" || j.kind == "artist_packs" || j.kind == "year_packs");

    if (nav)
    {
        auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [](const DownloadJob& q) { return q.kind == "thumb" || q.is_spider; });
        m_jobs.insert(it, j);
        return;
    }

    if (j.kind == "thumb" && !j.is_spider)
    {
        // Keep thumbs ahead of spider work.
        auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [](const DownloadJob& q) { return q.is_spider; });
        m_jobs.insert(it, j);
        return;
    }

    // Spider or other low-priority: always at the very end.
    m_jobs.push_back(j);
}

bool SixteenColorsBrowserWindow::DequeueResult(DownloadResult& out)
{
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_results.empty())
        return false;
    out = std::move(m_results.front());
    m_results.erase(m_results.begin());
    return true;
}

void SixteenColorsBrowserWindow::Render(const char* title, bool* p_open, const Callbacks& cb,
                                       SessionState* session, bool apply_placement_this_frame)
{
    if (!p_open || !*p_open)
        return;

    const char* win_name = title ? title : "16colo.rs Browser";
    if (session)
        ApplyImGuiWindowPlacement(*session, win_name, apply_placement_this_frame);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_None |
        (session ? GetImGuiWindowChromeExtraFlags(*session, win_name) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, win_name);
    if (!ImGui::Begin(win_name, p_open, flags))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, win_name);
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return;
    }
    if (session)
        CaptureImGuiWindowPlacement(*session, win_name);
    if (session)
    {
        ApplyImGuiWindowChromeZOrder(session, win_name);
        RenderImGuiWindowChromeMenu(session, win_name);
    }

    // Drain download results (thumbs + raw opens).
    DownloadResult dr;
    while (DequeueResult(dr))
    {
        if (dr.job.is_spider)
        {
            DatahoarderOnResult(dr);
            continue;
        }

        if (dr.job.kind == "thumb")
        {
            // If the user navigated away, don't let stale thumbs repopulate the cache or waste work on decoding.
            if (!dr.job.pack.empty() && dr.job.pack != m_selected_pack)
                continue;
            Thumb& t = m_thumbs[dr.job.url];
            if (!dr.err.empty())
            {
                t.failed = true;
                t.err = dr.err;
                continue;
            }
            int w = 0, h = 0;
            std::vector<unsigned char> rgba;
            std::string ierr;
            if (!image_loader::LoadImageFromMemoryAsRgba32(dr.bytes, w, h, rgba, ierr))
            {
                t.failed = true;
                t.err = ierr;
                continue;
            }

            // Precompute a consistent, good-looking preview:
            // - center-crop to the thumbnail box aspect ratio (so tall thumbs don't get squashed)
            // - bilinear resample to a small fixed grid (so draw cost is bounded)
            constexpr int kPreviewW = 32;
            constexpr int kPreviewH = 21; // ~1.52 aspect, close to 170x110 (~1.545)

            std::vector<unsigned char> preview;
            if (!BuildThumbPreviewCoverBilinear(rgba.data(), w, h, kPreviewW, kPreviewH, preview))
            {
                t.failed = true;
                t.err = "Failed to build thumbnail preview.";
                continue;
            }

            t.ready = true;
            t.failed = false;
            t.w = w;
            t.h = h;
            t.preview_w = kPreviewW;
            t.preview_h = kPreviewH;
            t.preview_rgba = std::move(preview);
            t.err.clear();
        }
        else if (dr.job.kind == "raw")
        {
            if (m_raw_pending > 0)
                m_raw_pending--;
            if (!dr.err.empty())
            {
                m_last_error = dr.err;
                continue;
            }

            const std::string ext = ExtLower(dr.job.filename);
            const std::string display_path = dr.job.url; // store URL in created window

            // Heuristic: treat these as images.
            const bool is_image =
                (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "bmp");
            const bool is_textish =
                (ext == "ans" || ext == "asc" || ext == "txt" || ext == "nfo" || ext == "diz" || ext == "xb");

            if (is_image)
            {
                if (!cb.create_image)
                {
                    m_last_error = "Internal error: create_image callback not set.";
                    continue;
                }

                int w = 0, h = 0;
                std::vector<unsigned char> rgba;
                std::string ierr;
                if (!image_loader::LoadImageFromMemoryAsRgba32(dr.bytes, w, h, rgba, ierr))
                {
                    m_last_error = ierr;
                    continue;
                }

                Callbacks::LoadedImage li;
                li.path = display_path;
                li.width = w;
                li.height = h;
                li.pixels = std::move(rgba);
                cb.create_image(std::move(li));
                m_last_error.clear();
            }
            else if (is_textish)
            {
                if (!cb.create_canvas)
                {
                    m_last_error = "Internal error: create_canvas callback not set.";
                    continue;
                }

                AnsiCanvas imported;
                std::string ierr;
                const bool ok =
                    (ext == "xb")
                        ? formats::xbin::ImportBytesToCanvas(dr.bytes, imported, ierr)
                        : formats::ansi::ImportBytesToCanvas(dr.bytes, imported, ierr);
                if (!ok)
                {
                    m_last_error = ierr;
                    continue;
                }
                // Use the URL as a stable "path" identity for window titles and session restore.
                imported.SetFilePath(display_path);
                cb.create_canvas(std::move(imported));
                m_last_error.clear();
            }
            else
            {
                m_last_error = "Unsupported file type for import: " + dr.job.filename;
            }
        }
        else if (dr.job.kind == "pack_list")
        {
            // Foreground "display" jobs drive loading state; background refresh jobs should be silent.
            if (!dr.job.is_background_refresh)
            {
                // CacheOnly miss: keep waiting for the network refresh job.
                if (dr.job.cache_mode == http::CacheMode::CacheOnly && !dr.err.empty())
                    continue;

                m_pack_list_pending = false;
                m_pack_list_pending_url.clear();
                m_loading_list = false;
                if (!dr.err.empty())
                {
                    m_last_error = dr.err;
                    m_pack_list_json.clear();
                    m_pack_rows.clear();
                    m_pack_pages = 0;
                    m_pack_next_page = 1;
                    continue;
                }
            }
            else
            {
                // Background refresh: ignore failures and only apply changes when the response differs.
                if (!dr.err.empty() || !dr.changed)
                    continue;
                // Only refresh page 1 to avoid reordering/duplication mid-infinite-scroll.
                if (dr.job.page != 1)
                    continue;
                // If the user already loaded more pages, don't reshuffle under them.
                if (m_pack_next_page > 2)
                    continue;

                // If there was no cached display (cache miss), this network refresh is the initial load.
                if (m_pack_list_pending && m_pack_list_pending_url == dr.job.url)
                {
                    m_pack_list_pending = false;
                    m_pack_list_pending_url.clear();
                    m_loading_list = false;
                }
            }

            m_pack_list_json.assign((const char*)dr.bytes.data(),
                                    (const char*)dr.bytes.data() + dr.bytes.size());

            // Parse + append for infinite scrolling
            json j;
            try { j = json::parse(m_pack_list_json); } catch (...) { j = json(); }
            if (j.is_object())
            {
                if (j.contains("page") && j["page"].is_object())
                    m_pack_pages = JsonIntOrDefault(j["page"], "pages", m_pack_pages);
                if (j.contains("results") && j["results"].is_array())
                {
                    if (dr.job.is_background_refresh)
                    {
                        m_pack_rows.clear();
                        m_pack_next_page = 2;
                    }
                    for (const auto& it : j["results"])
                    {
                        if (!it.is_object())
                            continue;
                        const std::string name = JsonStringOrEmpty(it, "name");
                        if (name.empty())
                            continue;
                        const int year = JsonIntOrDefault(it, "year", 0);
                        m_pack_rows.push_back(PackRow{name, year});
                    }
                }
            }
        }
        else if (dr.job.kind == "pack_detail")
        {
            m_pack_detail_pending = false;
            m_pack_detail_pending_pack.clear();
            m_loading_pack = false;
            if (!dr.err.empty())
            {
                m_last_error = dr.err;
                m_pack_detail_json.clear();
                continue;
            }
            m_pack_detail_json.assign((const char*)dr.bytes.data(), (const char*)dr.bytes.data() + dr.bytes.size());
        }
        else if (dr.job.kind == "group_list")
        {
            if (!dr.job.is_background_refresh)
            {
                // CacheOnly miss: keep waiting for the network refresh job.
                if (dr.job.cache_mode == http::CacheMode::CacheOnly && !dr.err.empty())
                    continue;

                m_root_list_pending = false;
                m_root_list_pending_url.clear();
                m_loading_list = false;
                if (!dr.err.empty())
                {
                    m_last_error = dr.err;
                    m_group_list_json.clear();
                    m_group_rows.clear();
                    m_group_pages = 0;
                    m_group_next_page = 1;
                    continue;
                }
            }
            else
            {
                if (!dr.err.empty() || !dr.changed)
                    continue;
                if (dr.job.page != 1)
                    continue;
                if (m_group_next_page > 2)
                    continue;
            }

            // Even for background refresh, clear spinner if this was the only in-flight root list.
            if (m_root_list_pending && m_root_list_pending_url == dr.job.url && dr.err.empty())
            {
                m_root_list_pending = false;
                m_root_list_pending_url.clear();
                m_loading_list = false;
            }

            m_group_list_json.assign((const char*)dr.bytes.data(),
                                     (const char*)dr.bytes.data() + dr.bytes.size());

            json j;
            try { j = json::parse(m_group_list_json); } catch (...) { j = json(); }
            if (j.is_object())
            {
                if (j.contains("page") && j["page"].is_object())
                    m_group_pages = JsonIntOrDefault(j["page"], "pages", m_group_pages);
                if (j.contains("results") && j["results"].is_array())
                {
                    if (dr.job.is_background_refresh)
                    {
                        m_group_rows.clear();
                        m_group_next_page = 2;
                    }
                    for (const auto& it : j["results"])
                    {
                        if (!it.is_object())
                            continue;
                        std::string name;
                        int releases = 0;
                        if (it.contains("name") && it["name"].is_string())
                        {
                            name = it["name"].get<std::string>();
                            releases = JsonIntOrDefault(it, "releases", 0);
                        }
                        else if (it.size() == 1)
                        {
                            auto f = it.begin();
                            name = f.key();
                            if (f.value().is_object())
                                releases = JsonIntOrDefault(f.value(), "releases", 0);
                        }
                        if (!name.empty())
                            m_group_rows.push_back(GroupRow{name, releases});
                    }
                }
            }
        }
        else if (dr.job.kind == "artist_list")
        {
            if (!dr.job.is_background_refresh)
            {
                // CacheOnly miss: keep waiting for the network refresh job.
                if (dr.job.cache_mode == http::CacheMode::CacheOnly && !dr.err.empty())
                    continue;

                m_root_list_pending = false;
                m_root_list_pending_url.clear();
                m_loading_list = false;
                if (!dr.err.empty())
                {
                    m_last_error = dr.err;
                    m_artist_list_json.clear();
                    m_artist_rows.clear();
                    m_artist_pages = 0;
                    m_artist_next_page = 1;
                    continue;
                }
            }
            else
            {
                if (!dr.err.empty() || !dr.changed)
                    continue;
                if (dr.job.page != 1)
                    continue;
                if (m_artist_next_page > 2)
                    continue;
            }

            if (m_root_list_pending && m_root_list_pending_url == dr.job.url && dr.err.empty())
            {
                m_root_list_pending = false;
                m_root_list_pending_url.clear();
                m_loading_list = false;
            }

            m_artist_list_json.assign((const char*)dr.bytes.data(),
                                      (const char*)dr.bytes.data() + dr.bytes.size());

            json j;
            try { j = json::parse(m_artist_list_json); } catch (...) { j = json(); }
            if (j.is_object())
            {
                if (j.contains("page") && j["page"].is_object())
                    m_artist_pages = JsonIntOrDefault(j["page"], "pages", m_artist_pages);
                if (j.contains("results") && j["results"].is_array())
                {
                    if (dr.job.is_background_refresh)
                    {
                        m_artist_rows.clear();
                        m_artist_next_page = 2;
                    }
                    for (const auto& it : j["results"])
                    {
                        if (!it.is_object())
                            continue;
                        json a;
                        if (it.contains("artist") && it["artist"].is_object())
                            a = it["artist"];
                        else if (it.size() == 1 && it.begin().value().is_object())
                            a = it.begin().value();
                        else
                            a = it;
                        const std::string name = JsonStringOrEmpty(a, "name");
                        const int releases = JsonIntOrDefault(a, "releases", 0);
                        if (!name.empty())
                            m_artist_rows.push_back(ArtistRow{name, releases});
                    }
                }
            }
        }
        else if (dr.job.kind == "year_list")
        {
            m_root_list_pending = false;
            m_root_list_pending_url.clear();
            m_loading_list = false;
            if (!dr.err.empty())
            {
                m_last_error = dr.err;
                m_year_list_json.clear();
                continue;
            }
            m_year_list_json.assign((const char*)dr.bytes.data(), (const char*)dr.bytes.data() + dr.bytes.size());
        }
        else if (dr.job.kind == "latest_list")
        {
            if (!dr.job.is_background_refresh)
            {
                // CacheOnly miss: keep waiting for the network refresh job.
                if (dr.job.cache_mode == http::CacheMode::CacheOnly && !dr.err.empty())
                    continue;

                m_root_list_pending = false;
                m_root_list_pending_url.clear();
                m_loading_list = false;
                if (!dr.err.empty())
                {
                    m_last_error = dr.err;
                    m_latest_list_json.clear();
                    continue;
                }
            }
            else
            {
                // Background refresh: ignore failures and only apply changes when different.
                if (!dr.err.empty() || !dr.changed)
                    continue;
                if (m_root_list_pending && m_root_list_pending_url == dr.job.url)
                {
                    m_root_list_pending = false;
                    m_root_list_pending_url.clear();
                    m_loading_list = false;
                }
            }

            m_latest_list_json.assign((const char*)dr.bytes.data(),
                                      (const char*)dr.bytes.data() + dr.bytes.size());
        }
        else if (dr.job.kind == "group_packs" || dr.job.kind == "artist_packs")
        {
            m_drill_packs_pending = false;
            m_loading_list = false;
            if (!dr.err.empty())
            {
                m_last_error = dr.err;
                m_drill_packs_json.clear();
                m_auto_selected_drill_pack = false;
                continue;
            }
            // Drop stale drill responses (e.g. user clicked a different group/artist).
            if (!m_drill_packs_pending_key.empty() && dr.job.pack != m_drill_packs_pending_key)
                continue;
            m_drill_packs_json.assign((const char*)dr.bytes.data(), (const char*)dr.bytes.data() + dr.bytes.size());
            // Allow auto-select of the top pack for this drill context.
            m_auto_selected_drill_pack = false;
        }
        else if (dr.job.kind == "year_packs")
        {
            if (!dr.job.is_background_refresh)
            {
                // CacheOnly miss: keep waiting for the network refresh job.
                if (dr.job.cache_mode == http::CacheMode::CacheOnly && !dr.err.empty())
                    continue;

                m_drill_packs_pending = false;
                m_loading_list = false;
                if (!dr.err.empty())
                {
                    m_last_error = dr.err;
                    m_drill_packs_json.clear();
                    m_auto_selected_drill_pack = false;
                    continue;
                }
            }
            else
            {
                if (!dr.err.empty() || !dr.changed)
                    continue;
                if (m_drill_packs_pending)
                {
                    m_drill_packs_pending = false;
                    m_loading_list = false;
                }
            }

            m_drill_packs_json.assign((const char*)dr.bytes.data(),
                                      (const char*)dr.bytes.data() + dr.bytes.size());
            m_auto_selected_drill_pack = false;
        }
    }

    // Top controls
    ImGui::Separator();

    bool list_settings_changed = false;

    // Mode switcher
    int mode_i = (int)m_mode;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo("Browse", &mode_i, "Packs\0Groups\0Artists\0Years\0Latest\0"))
    {
        m_mode = (BrowseMode)mode_i;
        m_left_view = LeftView::RootList;
        m_selected_group.clear();
        m_selected_artist.clear();
        m_selected_year = 0;
        m_drill_packs_json.clear();
        m_drill_packs_pending = false;
        m_drill_packs_pending_key.clear();
        m_dirty_list = true;
        list_settings_changed = true;

        // Interrupt background thumb floods + stale navigation work.
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_jobs.erase(
                std::remove_if(
                    m_jobs.begin(),
                    m_jobs.end(),
                    [&](const DownloadJob& j) { return j.kind != "raw"; }),
                m_jobs.end());
            m_results.erase(
                std::remove_if(
                    m_results.begin(),
                    m_results.end(),
                    [&](const DownloadResult& r) { return r.job.kind != "raw"; }),
                m_results.end());
        }

        if (m_mode == BrowseMode::Latest)
            m_auto_selected_latest = false;
        m_auto_selected_drill_pack = false;
    }

    // Top-right: Datahoarder toggle (background cache fill)
    {
        const char* label = "Datahoarder";
        const ImGuiStyle& st = ImGui::GetStyle();
        // Checkbox approximate width: label + frame + spacing.
        const float w = ImGui::CalcTextSize(label).x + st.FramePadding.x * 2.0f + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x;
        const float right_x = ImGui::GetWindowContentRegionMax().x;
        const float x = std::max(ImGui::GetCursorPosX(), right_x - w);
        ImGui::SameLine(x);
        if (ImGui::Checkbox(label, &m_datahoarder_enabled))
        {
            // Start immediately when enabled; keep progress when paused.
            if (m_datahoarder_enabled)
                m_datahoarder_next_network_allowed = std::chrono::steady_clock::time_point{};
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            const std::string tip = PHOS_TRF("sixteen_colors.datahoarder_tooltip",
                                             phos::i18n::Arg::I64((long long)m_datahoarder_enqueued),
                                             phos::i18n::Arg::I64((long long)m_datahoarder_completed),
                                             phos::i18n::Arg::I64((long long)m_datahoarder_errors),
                                             phos::i18n::Arg::I64((long long)m_datahoarder_todo.size()));
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }

    if (m_left_view == LeftView::PacksList && (m_mode == BrowseMode::Groups || m_mode == BrowseMode::Artists || m_mode == BrowseMode::Years))
    {
        ImGui::SameLine();
        if (ImGui::Button((PHOS_TR("sixteen_colors.back") + "##16c_back").c_str()))
        {
            m_left_view = LeftView::RootList;
            m_drill_packs_json.clear();
            m_drill_packs_pending = false;
            m_drill_packs_pending_key.clear();
            m_dirty_list = true;
            m_auto_selected_drill_pack = false;
        }
    }

    // Search/filter (used by most endpoints as ?filter=...)
    std::string filter_hint = PHOS_TR("sixteen_colors.filter_hint_default");
    if (m_mode == BrowseMode::Packs)
        filter_hint = PHOS_TR("sixteen_colors.filter_hint_packs");
    else if (m_mode == BrowseMode::Groups)
        filter_hint = PHOS_TR("sixteen_colors.filter_hint_groups");
    else if (m_mode == BrowseMode::Artists)
        filter_hint = PHOS_TR("sixteen_colors.filter_hint_artists");
    else if (m_mode == BrowseMode::Years)
        filter_hint = PHOS_TR("sixteen_colors.filter_hint_years_optional");

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##filter", filter_hint.c_str(), &m_filter))
        list_settings_changed = true;

    // View-specific controls
    if (m_mode == BrowseMode::Packs && m_left_view == LeftView::RootList)
    {
        if (ImGui::Checkbox(PHOS_TR("sixteen_colors.include_groups").c_str(), &m_show_groups))
            list_settings_changed = true;
        ImGui::SameLine();
        if (ImGui::Checkbox(PHOS_TR("sixteen_colors.include_artists").c_str(), &m_show_artists))
            list_settings_changed = true;
    }
    else if ((m_mode == BrowseMode::Groups || m_mode == BrowseMode::Artists) && m_left_view == LeftView::RootList)
    {
        if (m_mode == BrowseMode::Groups)
        {
            const std::string sort_lbl = PHOS_TR("sixteen_colors.sort") + "##16c_group_sort";
            if (ImGui::Combo(sort_lbl.c_str(), &m_group_sort, "name\0packs\0"))
                list_settings_changed = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            const std::string order_lbl = PHOS_TR("sixteen_colors.order") + "##16c_group_order";
            if (ImGui::Combo(order_lbl.c_str(), &m_group_order, "asc\0desc\0"))
                list_settings_changed = true;
        }
        else
        {
            const std::string sort_lbl = PHOS_TR("sixteen_colors.sort") + "##16c_artist_sort";
            if (ImGui::Combo(sort_lbl.c_str(), &m_artist_sort, "name\0releases\0"))
                list_settings_changed = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            const std::string order_lbl = PHOS_TR("sixteen_colors.order") + "##16c_artist_order";
            if (ImGui::Combo(order_lbl.c_str(), &m_artist_order, "asc\0desc\0"))
                list_settings_changed = true;
        }
    }
    else if (m_mode == BrowseMode::Years && m_left_view == LeftView::RootList)
    {
        if (ImGui::Checkbox(PHOS_TR("sixteen_colors.include_mags").c_str(), &m_year_include_mags))
            list_settings_changed = true;
    }

    if (list_settings_changed)
        m_dirty_list = true;

    // Auto-fetch based on mode/current left view.
    if (m_mode == BrowseMode::Packs && m_left_view == LeftView::RootList)
    {
        if (m_dirty_list)
        {
            m_pack_rows.clear();
            m_pack_pages = 0;
            m_pack_next_page = 1;
            m_pack_list_json.clear();
        }

        if ((m_pack_rows.empty() || m_dirty_list) && !m_pack_list_pending)
        {
            const std::string url = BuildPackListUrl(1, m_pagesize, m_show_groups, m_show_artists, m_filter);
            m_pack_list_pending = true;
            m_pack_list_pending_url = url;
            m_loading_list = true;
            m_dirty_list = false;
            m_pack_next_page = 2;
            if (!m_filter.empty())
            {
                // Stale-while-revalidate for filtered searches (page 1 only).
                DownloadJob j{url, "pack_list", "", "", 1};
                j.cache_mode = http::CacheMode::CacheOnly;
                j.is_background_refresh = false;
                Enqueue(j);

                DownloadJob r{url, "pack_list", "", "", 1};
                r.cache_mode = http::CacheMode::NetworkOnly;
                r.is_background_refresh = true;
                Enqueue(r);
            }
            else
            {
                Enqueue(DownloadJob{url, "pack_list", "", "", 1});
            }
        }
    }
    else if (m_mode == BrowseMode::Groups && m_left_view == LeftView::RootList)
    {
        if (m_dirty_list)
        {
            m_group_rows.clear();
            m_group_pages = 0;
            m_group_next_page = 1;
            m_group_list_json.clear();
        }

        if ((m_group_rows.empty() || m_dirty_list) && !m_root_list_pending)
        {
            const std::string url = BuildGroupListUrl(1, m_root_pagesize, m_group_sort, m_group_order, m_filter);
            m_root_list_pending = true;
            m_root_list_pending_url = url;
            m_loading_list = true;
            m_dirty_list = false;
            m_group_next_page = 2;
            if (!m_filter.empty())
            {
                DownloadJob j{url, "group_list", "", "", 1};
                j.cache_mode = http::CacheMode::CacheOnly;
                j.is_background_refresh = false;
                Enqueue(j);

                DownloadJob r{url, "group_list", "", "", 1};
                r.cache_mode = http::CacheMode::NetworkOnly;
                r.is_background_refresh = true;
                Enqueue(r);
            }
            else
            {
                Enqueue(DownloadJob{url, "group_list", "", "", 1});
            }
        }
    }
    else if (m_mode == BrowseMode::Artists && m_left_view == LeftView::RootList)
    {
        if (m_dirty_list)
        {
            m_artist_rows.clear();
            m_artist_pages = 0;
            m_artist_next_page = 1;
            m_artist_list_json.clear();
        }

        if ((m_artist_rows.empty() || m_dirty_list) && !m_root_list_pending)
        {
            const std::string url = BuildArtistListUrl(1, m_root_pagesize, m_filter);
            m_root_list_pending = true;
            m_root_list_pending_url = url;
            m_loading_list = true;
            m_dirty_list = false;
            m_artist_next_page = 2;
            if (!m_filter.empty())
            {
                DownloadJob j{url, "artist_list", "", "", 1};
                j.cache_mode = http::CacheMode::CacheOnly;
                j.is_background_refresh = false;
                Enqueue(j);

                DownloadJob r{url, "artist_list", "", "", 1};
                r.cache_mode = http::CacheMode::NetworkOnly;
                r.is_background_refresh = true;
                Enqueue(r);
            }
            else
            {
                Enqueue(DownloadJob{url, "artist_list", "", "", 1});
            }
        }
    }
    else if (m_mode == BrowseMode::Years && m_left_view == LeftView::RootList)
    {
        // Year index list has no pagination and is not filterable.
        if ((m_year_list_json.empty() || m_dirty_list) && !m_root_list_pending)
        {
            const std::string url = BuildYearListUrl();
            m_root_list_pending = true;
            m_root_list_pending_url = url;
            m_loading_list = true;
            m_dirty_list = false;
            Enqueue(DownloadJob{url, "year_list", "", ""});
        }
    }
    else if (m_mode == BrowseMode::Latest && m_left_view == LeftView::RootList)
    {
        if ((m_latest_list_json.empty() || m_dirty_list) && !m_root_list_pending)
        {
            const std::string url = BuildLatestUrl();
            m_root_list_pending = true;
            m_root_list_pending_url = url;
            m_loading_list = true;
            m_dirty_list = false;

            // Stale-while-revalidate:
            // - show cached response immediately (if present)
            // - refresh in background; only apply if changed
            {
                DownloadJob j{url, "latest_list", "", ""};
                j.cache_mode = http::CacheMode::CacheOnly;
                j.is_background_refresh = false;
                Enqueue(j);
            }
            {
                DownloadJob j{url, "latest_list", "", ""};
                j.cache_mode = http::CacheMode::NetworkOnly;
                j.is_background_refresh = true;
                Enqueue(j);
            }
        }
    }

    const bool show_loading =
        m_loading_list || m_loading_pack || m_pack_list_pending || m_root_list_pending ||
        m_drill_packs_pending || (m_raw_pending > 0);

    if (show_loading)
    {
        ImGui::SameLine();
        if (m_raw_pending > 0)
            ImGui::TextUnformatted(PHOS_TRF("sixteen_colors.downloading_n_fmt",
                                            phos::i18n::Arg::I64((long long)m_raw_pending)).c_str());
        else if (m_loading_pack)
            ImGui::TextUnformatted(PHOS_TR("sixteen_colors.loading_pack").c_str());
        else
            ImGui::TextUnformatted(PHOS_TR("sixteen_colors.loading").c_str());
    }

    if (!m_last_error.empty())
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                           PHOS_TRF("sixteen_colors.error_fmt", phos::i18n::Arg::Str(m_last_error)).c_str());
    }

    ImGui::Separator();

    // Two-column layout: pack list (left) + pack contents grid (right)
    ImGui::Columns(2, "##16c_cols", true);
    {
        const float w = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
        const float left = std::clamp(w * 0.22f, 200.0f, 420.0f);
        ImGui::SetColumnWidth(0, left);
    }

    auto select_pack = [&](const std::string& name)
    {
        if (name.empty())
            return;
        m_selected_pack = name;
        m_pack_detail_json.clear();
        m_thumbs.clear();
        m_file_filter.clear();
        m_tag_filter.clear();
        m_ext_filter = 0;
        if (m_mode == BrowseMode::Latest)
            m_auto_selected_latest = true;

        // Drop queued thumb/detail work from previous pack so the worker doesn't get starved.
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_jobs.erase(
                std::remove_if(
                    m_jobs.begin(),
                    m_jobs.end(),
                    [&](const DownloadJob& j) {
                        if (j.kind == "raw")
                            return false; // never drop a user-initiated open
                        return (j.kind == "thumb" || j.kind == "pack_detail");
                    }),
                m_jobs.end());

            m_results.erase(
                std::remove_if(
                    m_results.begin(),
                    m_results.end(),
                    [&](const DownloadResult& r) {
                        if (r.job.kind == "raw")
                            return false;
                        return (r.job.kind == "thumb" || r.job.kind == "pack_detail");
                    }),
                m_results.end());
        }

        const std::string url = BuildPackDetailUrl(m_selected_pack);
        m_pack_detail_pending = true;
        m_pack_detail_pending_pack = m_selected_pack;
        m_loading_pack = true;
        Enqueue(DownloadJob{url, "pack_detail", m_selected_pack, ""});
    };

    // Left: navigation (root list) or pack list (drill-down)
    const char* left_title = "Packs";
    if (m_left_view == LeftView::RootList)
    {
        if (m_mode == BrowseMode::Packs) left_title = "Packs";
        else if (m_mode == BrowseMode::Groups) left_title = "Groups";
        else if (m_mode == BrowseMode::Artists) left_title = "Artists";
        else if (m_mode == BrowseMode::Years) left_title = "Years";
        else if (m_mode == BrowseMode::Latest) left_title = "Latest packs";
    }
    else
    {
        left_title = "Packs";
    }

    ImGui::TextUnformatted(left_title);
    ImGui::Separator();

    ImGui::BeginChild("##left_list", ImVec2(0, 0), true);
    if (m_left_view == LeftView::RootList)
    {
        if (m_mode == BrowseMode::Packs)
        {
            if (m_pack_rows.empty())
            {
                ImGui::TextUnformatted(PHOS_TR("sixteen_colors.no_pack_list_yet").c_str());
            }
            else
            {
                for (const auto& it : m_pack_rows)
                {
                    char label[256];
                    if (it.year > 0)
                        std::snprintf(label, sizeof(label), "%s (%d)", it.name.c_str(), it.year);
                    else
                        std::snprintf(label, sizeof(label), "%s", it.name.c_str());
                    if (ImGui::Selectable(label, it.name == m_selected_pack))
                        select_pack(it.name);
                }
            }

            // Infinite scroll: fetch next page near bottom.
            if (!m_pack_list_pending && m_pack_pages > 0 && m_pack_next_page <= m_pack_pages)
            {
                const float y = ImGui::GetScrollY();
                const float ymax = ImGui::GetScrollMaxY();
                if (ymax > 0.0f && y >= (ymax - 120.0f))
                {
                    const int page = m_pack_next_page++;
                    const std::string url = BuildPackListUrl(page, m_pagesize, m_show_groups, m_show_artists, m_filter);
                    m_pack_list_pending = true;
                    m_pack_list_pending_url = url;
                    m_loading_list = true;
                    Enqueue(DownloadJob{url, "pack_list", "", "", page});
                }
            }
        }
        else if (m_mode == BrowseMode::Latest)
        {
            if (m_latest_list_json.empty())
            {
                ImGui::TextUnformatted(PHOS_TR("sixteen_colors.no_latest_list_yet").c_str());
            }
            else
            {
                json j;
                try { j = json::parse(m_latest_list_json); }
                catch (const std::exception& e)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "JSON parse failed: %s", e.what());
                    j = json();
                }

                if (j.is_object() && j.contains("results") && j["results"].is_array())
                {
                    // Auto-select the most recent pack once so the right gallery isn't empty on launch.
                    if (!m_auto_selected_latest && m_selected_pack.empty() && !j["results"].empty())
                    {
                        const auto& it0 = j["results"][0];
                        if (it0.is_object())
                        {
                            std::string name = JsonStringOrEmpty(it0, "pack");
                            if (name.empty())
                                name = JsonStringOrEmpty(it0, "name");
                            if (!name.empty())
                                select_pack(name);
                        }
                    }

                    for (const auto& it : j["results"])
                    {
                        if (!it.is_object())
                            continue;
                        const int year = JsonIntOrDefault(it, "year", 0);
                        std::string name = JsonStringOrEmpty(it, "pack");
                        if (name.empty())
                            name = JsonStringOrEmpty(it, "name");
                        if (name.empty())
                            continue;

                        char label[256];
                        if (year > 0)
                            std::snprintf(label, sizeof(label), "%s (%d)", name.c_str(), year);
                        else
                            std::snprintf(label, sizeof(label), "%s", name.c_str());
                        const bool selected = (name == m_selected_pack);
                        if (ImGui::Selectable(label, selected))
                            select_pack(name);
                    }
                }
                else
                {
                    ImGui::TextUnformatted(PHOS_TR("sixteen_colors.unexpected_response").c_str());
                }
            }
        }
        else if (m_mode == BrowseMode::Groups)
        {
            if (m_group_rows.empty())
            {
                ImGui::TextUnformatted(PHOS_TR("sixteen_colors.no_group_list_yet").c_str());
            }
            else
            {
                // Groups are already sorted server-side; we still can present as-is.
                for (const auto& gr : m_group_rows)
                {
                    char label[256];
                    if (gr.releases > 0)
                        std::snprintf(label, sizeof(label), "%s (%d)", gr.name.c_str(), gr.releases);
                    else
                        std::snprintf(label, sizeof(label), "%s", gr.name.c_str());
                    if (ImGui::Selectable(label, gr.name == m_selected_group))
                    {
                        m_selected_group = gr.name;
                        m_selected_artist.clear();
                        m_selected_year = 0;
                        m_left_view = LeftView::PacksList;
                        m_drill_packs_json.clear();
                        m_last_error.clear();
                        m_selected_pack.clear();
                        m_pack_detail_json.clear();
                        m_auto_selected_drill_pack = false;
                        const std::string url = BuildGroupDetailUrl(m_selected_group);
                        m_drill_packs_pending = true;
                        m_drill_packs_pending_key = m_selected_group;
                        m_loading_list = true;
                        Enqueue(DownloadJob{url, "group_packs", m_selected_group, "", 0});
                    }
                }
            }

            if (!m_root_list_pending && m_group_pages > 0 && m_group_next_page <= m_group_pages)
            {
                const float y = ImGui::GetScrollY();
                const float ymax = ImGui::GetScrollMaxY();
                if (ymax > 0.0f && y >= (ymax - 120.0f))
                {
                    const int page = m_group_next_page++;
                    const std::string url = BuildGroupListUrl(page, m_root_pagesize, m_group_sort, m_group_order, m_filter);
                    m_root_list_pending = true;
                    m_root_list_pending_url = url;
                    m_loading_list = true;
                    Enqueue(DownloadJob{url, "group_list", "", "", page});
                }
            }
        }
        else if (m_mode == BrowseMode::Artists)
        {
            if (m_artist_rows.empty())
            {
                ImGui::TextUnformatted(PHOS_TR("sixteen_colors.no_artist_list_yet").c_str());
            }
            else
            {
                // Sort client-side (API sorts by name only).
                std::vector<ArtistRow> rows = m_artist_rows;
                std::sort(rows.begin(), rows.end(), [&](const ArtistRow& a, const ArtistRow& b) {
                    const bool asc = (m_artist_order == 0);
                    if (m_artist_sort == 0)
                        return asc ? (a.name < b.name) : (a.name > b.name);
                    if (a.releases != b.releases)
                        return asc ? (a.releases < b.releases) : (a.releases > b.releases);
                    return a.name < b.name;
                });

                for (const auto& r : rows)
                {
                    char label[256];
                    if (r.releases > 0)
                        std::snprintf(label, sizeof(label), "%s (%d)", r.name.c_str(), r.releases);
                    else
                        std::snprintf(label, sizeof(label), "%s", r.name.c_str());

                    if (ImGui::Selectable(label, r.name == m_selected_artist))
                    {
                        m_selected_artist = r.name;
                        m_selected_group.clear();
                        m_selected_year = 0;
                        m_left_view = LeftView::PacksList;
                        m_drill_packs_json.clear();
                        m_last_error.clear();
                        m_selected_pack.clear();
                        m_pack_detail_json.clear();
                        m_auto_selected_drill_pack = false;
                        const std::string url = BuildArtistPacksUrl(m_selected_artist);
                        m_drill_packs_pending = true;
                        m_drill_packs_pending_key = m_selected_artist;
                        m_loading_list = true;
                        Enqueue(DownloadJob{url, "artist_packs", m_selected_artist, "", 0});
                    }
                }
            }

            if (!m_root_list_pending && m_artist_pages > 0 && m_artist_next_page <= m_artist_pages)
            {
                const float y = ImGui::GetScrollY();
                const float ymax = ImGui::GetScrollMaxY();
                if (ymax > 0.0f && y >= (ymax - 120.0f))
                {
                    const int page = m_artist_next_page++;
                    const std::string url = BuildArtistListUrl(page, m_root_pagesize, m_filter);
                    m_root_list_pending = true;
                    m_root_list_pending_url = url;
                    m_loading_list = true;
                    Enqueue(DownloadJob{url, "artist_list", "", "", page});
                }
            }
        }
        else if (m_mode == BrowseMode::Years)
        {
            if (m_year_list_json.empty())
            {
                ImGui::TextUnformatted(PHOS_TR("sixteen_colors.no_year_index_yet").c_str());
            }
            else
            {
                json j;
                try { j = json::parse(m_year_list_json); }
                catch (const std::exception& e)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "JSON parse failed: %s", e.what());
                    j = json();
                }

                if (j.is_object())
                {
                    // Sort years descending for easier browsing.
                    std::vector<int> years;
                    years.reserve(j.size());
                    for (auto it = j.begin(); it != j.end(); ++it)
                    {
                        try { years.push_back(std::stoi(it.key())); }
                        catch (...) {}
                    }
                    std::sort(years.begin(), years.end(), std::greater<int>());

                    for (int y : years)
                    {
                        const auto key = std::to_string(y);
                        int packs = 0;
                        int mags = 0;
                        if (j.contains(key) && j[key].is_object())
                        {
                            packs = JsonIntOrDefault(j[key], "packs", 0);
                            mags = JsonIntOrDefault(j[key], "mags", 0);
                        }
                        char label[256];
                        std::snprintf(label, sizeof(label), "%d (packs %d, mags %d)", y, packs, mags);
                        if (ImGui::Selectable(label, y == m_selected_year))
                        {
                            m_selected_year = y;
                            m_selected_group.clear();
                            m_selected_artist.clear();
                            m_left_view = LeftView::PacksList;
                            m_drill_packs_json.clear();
                            m_last_error.clear();
                            m_selected_pack.clear();
                            m_pack_detail_json.clear();
                            m_auto_selected_drill_pack = false;
                            const std::string url = BuildYearPacksUrl(m_selected_year, m_year_include_mags, m_filter);
                            m_drill_packs_pending = true;
                            m_loading_list = true;
                            if (!m_filter.empty())
                            {
                                DownloadJob j{url, "year_packs", std::to_string(m_selected_year), "", 0};
                                j.cache_mode = http::CacheMode::CacheOnly;
                                j.is_background_refresh = false;
                                Enqueue(j);

                                DownloadJob r{url, "year_packs", std::to_string(m_selected_year), "", 0};
                                r.cache_mode = http::CacheMode::NetworkOnly;
                                r.is_background_refresh = true;
                                Enqueue(r);
                            }
                            else
                            {
                                Enqueue(DownloadJob{url, "year_packs", std::to_string(m_selected_year), "", 0});
                            }
                        }
                    }
                }
                else
                {
                    ImGui::TextUnformatted(PHOS_TR("sixteen_colors.unexpected_response").c_str());
                }
            }
        }
    }
    else // PacksList (drill-down)
    {
        if (m_drill_packs_json.empty())
        {
            ImGui::TextUnformatted(PHOS_TR("sixteen_colors.no_packs_loaded_yet").c_str());
        }
        else if (m_mode == BrowseMode::Groups)
        {
            json j;
            try { j = json::parse(m_drill_packs_json); }
            catch (...) { j = json(); }

            // Actual shape (as of API docs/observed):
            // { "results": { "packs": { "1998": ["pack1", ...], ... } } }
            // Older/alternate shapes are tolerated.
            std::vector<std::pair<int, std::string>> packs;
            json packs_obj;
            if (j.is_object() && j.contains("results") && j["results"].is_object())
            {
                const json& r = j["results"];
                if (r.contains("packs") && r["packs"].is_object())
                    packs_obj = r["packs"];
            }
            if (packs_obj.is_null() && j.is_object() && j.contains("results") && j["results"].is_array())
            {
                // Tolerate the older parser expectation:
                // results[0][group].packs = { ... }
                for (const auto& r : j["results"])
                {
                    if (!r.is_object())
                        continue;
                    if (r.contains(m_selected_group) && r[m_selected_group].is_object())
                    {
                        const json& g = r[m_selected_group];
                        if (g.contains("packs") && g["packs"].is_object())
                            packs_obj = g["packs"];
                    }
                }
            }

            if (packs_obj.is_object())
            {
                for (auto it = packs_obj.begin(); it != packs_obj.end(); ++it)
                {
                    int y = 0;
                    try { y = std::stoi(it.key()); } catch (...) { y = 0; }
                    if (!it.value().is_array())
                        continue;
                    for (const auto& pn : it.value())
                    {
                        if (!pn.is_string())
                            continue;
                        packs.push_back({y, pn.get<std::string>()});
                    }
                }
            }
            std::sort(packs.begin(), packs.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first > b.first;
                return a.second < b.second;
            });

            if (!m_auto_selected_drill_pack && m_selected_pack.empty() && !packs.empty())
            {
                m_auto_selected_drill_pack = true;
                select_pack(packs.front().second);
            }

            for (const auto& p : packs)
            {
                char label[256];
                if (p.first > 0)
                    std::snprintf(label, sizeof(label), "%s (%d)", p.second.c_str(), p.first);
                else
                    std::snprintf(label, sizeof(label), "%s", p.second.c_str());
                if (ImGui::Selectable(label, p.second == m_selected_pack))
                    select_pack(p.second);
            }
        }
        else if (m_mode == BrowseMode::Artists)
        {
            json j;
            try { j = json::parse(m_drill_packs_json); }
            catch (...) { j = json(); }

            std::vector<std::string> packs;
            if (j.is_object() && j.contains("results") && j["results"].is_array())
            {
                for (const auto& it : j["results"])
                {
                    if (!it.is_object())
                        continue;
                    json a;
                    if (it.contains("artist") && it["artist"].is_object())
                        a = it["artist"];
                    else if (it.size() == 1 && it.begin().value().is_object())
                        a = it.begin().value();
                    else
                        a = it;
                    const std::string name = JsonStringOrEmpty(a, "name");
                    if (!ContainsCaseInsensitive(name, m_selected_artist))
                        continue;
                    if (a.contains("packs") && a["packs"].is_array())
                    {
                        for (const auto& pn : a["packs"])
                        {
                            if (pn.is_string())
                                packs.push_back(pn.get<std::string>());
                        }
                    }
                }
            }
            // Best-effort: sort by inferred year desc, then name asc.
            std::sort(packs.begin(), packs.end(), [](const std::string& a, const std::string& b) {
                const int ya = GuessYearFromPackName(a);
                const int yb = GuessYearFromPackName(b);
                if (ya != yb) return ya > yb;
                return a < b;
            });
            packs.erase(std::unique(packs.begin(), packs.end()), packs.end());
            if (!m_auto_selected_drill_pack && m_selected_pack.empty() && !packs.empty())
            {
                m_auto_selected_drill_pack = true;
                select_pack(packs.front());
            }
            for (const auto& pn : packs)
            {
                if (ImGui::Selectable(pn.c_str(), pn == m_selected_pack))
                    select_pack(pn);
            }
        }
        else
        {
            // Years (single/range) produces the same shape as pack list: results[].name/year
            json j;
            try { j = json::parse(m_drill_packs_json); }
            catch (...) { j = json(); }
            if (j.is_object() && j.contains("results") && j["results"].is_array())
            {
                // Compute "top" pack once for auto-select (stable ordering: name asc).
                std::string top_name;
                if (!m_auto_selected_drill_pack && m_selected_pack.empty())
                {
                    std::vector<std::string> names;
                    for (const auto& it : j["results"])
                    {
                        if (!it.is_object())
                            continue;
                        std::string name = JsonStringOrEmpty(it, "name");
                        if (name.empty())
                            name = JsonStringOrEmpty(it, "pack");
                        if (!name.empty())
                            names.push_back(name);
                    }
                    std::sort(names.begin(), names.end());
                    names.erase(std::unique(names.begin(), names.end()), names.end());
                    if (!names.empty())
                        top_name = names.front();
                }

                for (const auto& it : j["results"])
                {
                    if (!it.is_object())
                        continue;
                    const int year = JsonIntOrDefault(it, "year", 0);
                    std::string name = JsonStringOrEmpty(it, "name");
                    if (name.empty())
                        name = JsonStringOrEmpty(it, "pack");
                    if (name.empty())
                        continue;
                    char label[256];
                    if (year > 0)
                        std::snprintf(label, sizeof(label), "%s (%d)", name.c_str(), year);
                    else
                        std::snprintf(label, sizeof(label), "%s", name.c_str());
                    if (ImGui::Selectable(label, name == m_selected_pack))
                        select_pack(name);
                }

                if (!top_name.empty())
                {
                    m_auto_selected_drill_pack = true;
                    select_pack(top_name);
                }
            }
            else
            {
                ImGui::TextUnformatted(PHOS_TR("sixteen_colors.unexpected_response").c_str());
            }
        }
    }
    ImGui::EndChild();

    ImGui::NextColumn();

    // Right: pack contents
    const std::string pack_name = m_selected_pack.empty() ? PHOS_TR("sixteen_colors.pack_none") : m_selected_pack;
    ImGui::TextUnformatted(PHOS_TRF("sixteen_colors.pack_fmt", phos::i18n::Arg::Str(pack_name)).c_str());
    ImGui::Separator();

    if (!m_selected_pack.empty())
    {
        // Gallery controls (client-side filtering)
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##file_filter", PHOS_TR("sixteen_colors.filter_files_hint").c_str(), &m_file_filter);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##tag_filter", PHOS_TR("sixteen_colors.filter_tag_hint").c_str(), &m_tag_filter);

        if (!m_pack_detail_json.empty())
        {
            json j;
            try { j = json::parse(m_pack_detail_json); }
            catch (const std::exception& e)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "JSON parse failed: %s", e.what());
                j = json();
            }

            // Expect: results[0].files is an object keyed by filename.
            if (j.is_object() && j.contains("results") && j["results"].is_array() && !j["results"].empty())
            {
                const json& r0 = j["results"][0];
                if (r0.contains("files") && r0["files"].is_object())
                {
                    // Extension filter options (derived from the pack contents).
                    std::vector<std::string> ext_opts;
                    ext_opts.push_back("All");
                    {
                        std::vector<std::string> exts;
                        exts.reserve((size_t)r0["files"].size());
                        for (auto it = r0["files"].begin(); it != r0["files"].end(); ++it)
                        {
                            const std::string ext = ExtLower(it.key());
                            if (!ext.empty())
                                exts.push_back(ext);
                        }
                        std::sort(exts.begin(), exts.end());
                        exts.erase(std::unique(exts.begin(), exts.end()), exts.end());
                        for (auto& e : exts)
                            ext_opts.push_back(e);
                    }

                    if (m_ext_filter < 0 || m_ext_filter >= (int)ext_opts.size())
                        m_ext_filter = 0;
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::BeginCombo("##ext_filter", ext_opts[m_ext_filter].c_str()))
                    {
                        for (int i = 0; i < (int)ext_opts.size(); ++i)
                        {
                            const bool is_sel = (m_ext_filter == i);
                            if (ImGui::Selectable(ext_opts[i].c_str(), is_sel))
                                m_ext_filter = i;
                            if (is_sel)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }

                    // Grid sizing
                    const float thumb_w = 170.0f;
                    const float thumb_h = 110.0f;
                    const float pad = 10.0f;
                    const float avail = ImGui::GetContentRegionAvail().x;
                    const int cols = std::max(1, (int)std::floor((avail + pad) / (thumb_w + pad)));

                    int idx = 0;
                    for (auto it = r0["files"].begin(); it != r0["files"].end(); ++it)
                    {
                        const std::string filename = it.key();
                        const json& frec = it.value();

                        if (!m_file_filter.empty() && !ContainsCaseInsensitive(filename, m_file_filter))
                            continue;
                        if (m_ext_filter > 0)
                        {
                            const std::string ext = ExtLower(filename);
                            if (ext != ext_opts[m_ext_filter])
                                continue;
                        }

                        // Build thumbnail URL if present.
                        std::string tn_url;
                        try
                        {
                            if (frec.contains("file") && frec["file"].is_object() &&
                                frec["file"].contains("tn") && frec["file"]["tn"].is_object())
                            {
                                const json& tn = frec["file"]["tn"];
                                const std::string uri = JsonStringOrEmpty(tn, "uri");
                                if (!uri.empty())
                                    tn_url = JoinUrl(WebBase(), uri);
                                else
                                {
                                    const std::string file = JsonStringOrEmpty(tn, "file");
                                    if (!file.empty())
                                        tn_url = WebBase() + "/pack/" + UrlEncode(m_selected_pack) + "/tn/" + UrlEncode(file);
                                }
                            }
                            else if (frec.contains("tn") && frec["tn"].is_object())
                            {
                                const json& tn = frec["tn"];
                                const std::string uri = JsonStringOrEmpty(tn, "uri");
                                if (!uri.empty())
                                    tn_url = JoinUrl(WebBase(), uri);
                                else
                                {
                                    const std::string file = JsonStringOrEmpty(tn, "file");
                                    if (!file.empty())
                                        tn_url = WebBase() + "/pack/" + UrlEncode(m_selected_pack) + "/tn/" + UrlEncode(file);
                                }
                            }
                        }
                        catch (...) {}

                        // Content tag filter (if tags exist in the record)
                        if (!m_tag_filter.empty())
                        {
                            bool tag_ok = false;
                            try
                            {
                                if (frec.contains("content") && frec["content"].is_array())
                                {
                                    for (const auto& t : frec["content"])
                                    {
                                        if (t.is_string() && ContainsCaseInsensitive(t.get<std::string>(), m_tag_filter))
                                        {
                                            tag_ok = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            catch (...) {}
                            if (!tag_ok)
                                continue;
                        }

                        ImGui::PushID(idx++);
                        if ((idx - 1) % cols != 0)
                            ImGui::SameLine();

                        ImGui::BeginGroup();

                        // Thumbnail area
                        if (!tn_url.empty())
                        {
                            Thumb& t = m_thumbs[tn_url];
                            if (!t.ready && !t.failed && !t.requested)
                            {
                                // Enqueue thumbnail download once.
                                t.requested = true;
                                Enqueue(DownloadJob{tn_url, "thumb", m_selected_pack, filename});
                            }

                            if (t.ready)
                            {
                                DrawRgbaGridExact(t.preview_rgba.data(), t.preview_w, t.preview_h, ImVec2(thumb_w, thumb_h));
                            }
                            else
                            {
                                ImGui::InvisibleButton("##thumb_canvas", ImVec2(thumb_w, thumb_h));
                                ImDrawList* dl = ImGui::GetWindowDrawList();
                                dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(20, 20, 24, 255), 4.0f);
                                dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(90, 90, 105, 255), 4.0f);
                                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - thumb_h + 8.0f);
                                ImGui::TextUnformatted(t.failed
                                    ? PHOS_TR("sixteen_colors.thumb_failed").c_str()
                                    : PHOS_TR("sixteen_colors.thumb_loading").c_str());
                            }
                        }
                        else
                        {
                            ImGui::InvisibleButton("##thumb_canvas", ImVec2(thumb_w, thumb_h));
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(20, 20, 24, 255), 4.0f);
                            dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(90, 90, 105, 255), 4.0f);
                            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - thumb_h + 8.0f);
                            ImGui::TextUnformatted(PHOS_TR("sixteen_colors.no_thumbnail").c_str());
                        }

                        // Click behavior: download raw and import.
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                        {
                            const std::string raw_url =
                                "https://16colo.rs/pack/" + UrlEncode(m_selected_pack) + "/raw/" + UrlEncode(filename);
                            m_raw_pending++;
                            Enqueue(DownloadJob{raw_url, "raw", m_selected_pack, filename});
                        }

                        // Filename
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumb_w);
                        ImGui::TextUnformatted(filename.c_str());
                        ImGui::PopTextWrapPos();

                        ImGui::EndGroup();
                        ImGui::PopID();
                    }
                }
                else
                {
                    ImGui::TextUnformatted(PHOS_TR("sixteen_colors.no_files_for_pack").c_str());
                }
            }
            else
            {
                ImGui::TextUnformatted(PHOS_TR("sixteen_colors.unexpected_pack_details").c_str());
            }
        }
        else
        {
            ImGui::TextUnformatted(PHOS_TR("sixteen_colors.no_pack_details_yet").c_str());
        }
    }
    else
    {
        ImGui::TextUnformatted(PHOS_TR("sixteen_colors.select_pack_left").c_str());
    }

    ImGui::Columns(1);

    // Tick after UI so user-driven jobs for this frame have already been queued.
    // Must happen before ImGui::End() to keep ImGui's stack valid.
    DatahoarderTick();

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
}

bool SixteenColorsBrowserWindow::DatahoarderShouldYieldToUser() const
{
    if (m_raw_pending > 0)
        return true;
    if (m_loading_list || m_loading_pack)
        return true;
    if (m_pack_list_pending || m_pack_detail_pending || m_root_list_pending || m_drill_packs_pending)
        return true;

    // If there are queued non-spider navigation jobs, let them drain first.
    {
        std::lock_guard<std::mutex> lock(m_mu);
        for (const auto& j : m_jobs)
        {
            if (j.is_spider)
                continue;
            if (j.kind == "thumb")
                continue;
            // Anything else is user-visible navigation / open.
            return true;
        }
    }

    return false;
}

void SixteenColorsBrowserWindow::DatahoarderEnqueueUnique(DownloadJob j)
{
    if (j.url.empty())
        return;
    const std::uint64_t h = Fnv1a64(j.url);
    if (!m_datahoarder_seen.insert(h).second)
        return;

    j.is_spider = true;
    j.is_background_refresh = true;
    j.cache_mode = http::CacheMode::Default; // cache miss => fetch once; cache hit => disk
    m_datahoarder_todo.push_back(std::move(j));
    m_datahoarder_enqueued++;
}

void SixteenColorsBrowserWindow::DatahoarderSeedIfNeeded()
{
    if (m_datahoarder_seeded)
        return;
    m_datahoarder_seeded = true;

    // Seed the crawl frontier. Use large pagesize (<=500) to reduce API request count.
    DatahoarderEnqueueUnique(DownloadJob{BuildLatestUrl(), "latest_list", "", ""});
    DatahoarderEnqueueUnique(DownloadJob{BuildYearListUrl(), "year_list", "", ""});

    // Packs/groups/artists root lists (page 1); subsequent pages are discovered from the response.
    DatahoarderEnqueueUnique(DownloadJob{BuildPackListUrl(1, 500, false, false, ""), "pack_list", "", "", 1});
    DatahoarderEnqueueUnique(DownloadJob{BuildGroupListUrl(1, 500, 0, 0, ""), "group_list", "", "", 1});
    DatahoarderEnqueueUnique(DownloadJob{BuildArtistListUrl(1, 500, ""), "artist_list", "", "", 1});
}

void SixteenColorsBrowserWindow::DatahoarderTick()
{
    if (!m_datahoarder_enabled)
        return;

    DatahoarderSeedIfNeeded();

    if (m_datahoarder_inflight > 0)
        return;

    if (DatahoarderShouldYieldToUser())
        return;

    // Don't add background work when the shared queue already has work.
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_jobs.size() > 8)
            return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_datahoarder_todo.empty())
        return;

    // If the next job is a cache miss, it will hit the network; respect the network rate limit.
    // Cache hits are allowed to run fast.
    const DownloadJob& peek = m_datahoarder_todo.front();
    const bool cached = http::HasCached(peek.url, {});
    if (!cached)
    {
        if (m_datahoarder_next_network_allowed != std::chrono::steady_clock::time_point{} && now < m_datahoarder_next_network_allowed)
            return;
    }

    DownloadJob j = std::move(m_datahoarder_todo.front());
    m_datahoarder_todo.pop_front();
    m_datahoarder_inflight = 1;
    m_datahoarder_last_url_hash = Fnv1a64(j.url);
    Enqueue(j);
}

void SixteenColorsBrowserWindow::DatahoarderOnResult(const DownloadResult& dr)
{
    if (!dr.job.is_spider)
        return;

    if (m_datahoarder_inflight > 0 && m_datahoarder_last_url_hash == Fnv1a64(dr.job.url))
        m_datahoarder_inflight = 0;
    else
        m_datahoarder_inflight = 0; // best-effort; scheduler keeps this at 1 anyway

    m_datahoarder_completed++;

    const bool ok = dr.err.empty() && dr.status >= 200 && dr.status < 300;
    const bool rate_limited = (dr.status == 429);
    const bool server_error = (dr.status >= 500 && dr.status < 600);
    if (!ok)
        m_datahoarder_errors++;

    // Pacing (network only):
    // - cache hits can run fast (disk is fine to hammer)
    // - cache misses are network hits and are rate-limited
    // - errors back off aggressively (and start high) to avoid hammering the API
    constexpr std::uint64_t kSpiderMinNetworkIntervalMs = 1000; // 1 req/sec max (network)
    constexpr std::uint64_t kSpiderBackoffStartMs = 10000; // start at 10s on errors
    constexpr std::uint64_t kSpiderBackoffCapMs = 10ull * 60ull * 1000ull; // 10 minutes

    const auto now = std::chrono::steady_clock::now();
    std::uint64_t delay_ms = 0;
    const bool did_network = !dr.from_cache;
    if (did_network && (!ok || rate_limited || server_error))
    {
        if (m_datahoarder_backoff_ms == 0)
            m_datahoarder_backoff_ms = kSpiderBackoffStartMs;
        else
            m_datahoarder_backoff_ms = std::min<std::uint64_t>(m_datahoarder_backoff_ms * 2, kSpiderBackoffCapMs);
        delay_ms = m_datahoarder_backoff_ms;
    }
    else if (did_network)
    {
        m_datahoarder_backoff_ms = 0;
        delay_ms = kSpiderMinNetworkIntervalMs;
    }
    // Cache hit: do not adjust network pacing.
    if (delay_ms > 0)
        m_datahoarder_next_network_allowed = now + std::chrono::milliseconds((long long)delay_ms);

    if (!ok)
        return;

    // Expand frontier from successful API responses.
    // NOTE: We only parse JSON endpoints. Thumbs are just cached by http::Get and ignored.
    const auto parse_json = [&](json& out) -> bool {
        try
        {
            out = json::parse((const char*)dr.bytes.data(), (const char*)dr.bytes.data() + dr.bytes.size());
            return true;
        }
        catch (...) { out = json(); return false; }
    };

    if (dr.job.kind == "thumb")
    {
        // Never decode/store thumbs for the spider: it would explode memory.
        return;
    }

    json j;
    if (!parse_json(j))
        return;

    if (dr.job.kind == "pack_list")
    {
        int pages = 0;
        if (j.is_object() && j.contains("page") && j["page"].is_object())
            pages = JsonIntOrDefault(j["page"], "pages", 0);
        if (dr.job.page == 1 && pages > 1)
        {
            for (int p = 2; p <= pages; ++p)
                DatahoarderEnqueueUnique(DownloadJob{BuildPackListUrl(p, 500, false, false, ""), "pack_list", "", "", p});
        }
        if (j.is_object() && j.contains("results") && j["results"].is_array())
        {
            for (const auto& it : j["results"])
            {
                if (!it.is_object())
                    continue;
                const std::string name = JsonStringOrEmpty(it, "name");
                if (!name.empty())
                    DatahoarderEnqueueUnique(DownloadJob{BuildPackDetailUrl(name), "pack_detail", name, ""});
            }
        }
    }
    else if (dr.job.kind == "group_list")
    {
        int pages = 0;
        if (j.is_object() && j.contains("page") && j["page"].is_object())
            pages = JsonIntOrDefault(j["page"], "pages", 0);
        if (dr.job.page == 1 && pages > 1)
        {
            for (int p = 2; p <= pages; ++p)
                DatahoarderEnqueueUnique(DownloadJob{BuildGroupListUrl(p, 500, 0, 0, ""), "group_list", "", "", p});
        }
        if (j.is_object() && j.contains("results") && j["results"].is_array())
        {
            for (const auto& it : j["results"])
            {
                if (!it.is_object())
                    continue;
                std::string name;
                if (it.contains("name") && it["name"].is_string())
                    name = it["name"].get<std::string>();
                else if (it.size() == 1)
                    name = it.begin().key();
                if (!name.empty())
                    DatahoarderEnqueueUnique(DownloadJob{BuildGroupDetailUrl(name), "group_packs", name, ""});
            }
        }
    }
    else if (dr.job.kind == "artist_list")
    {
        int pages = 0;
        if (j.is_object() && j.contains("page") && j["page"].is_object())
            pages = JsonIntOrDefault(j["page"], "pages", 0);
        if (dr.job.page == 1 && pages > 1)
        {
            for (int p = 2; p <= pages; ++p)
                DatahoarderEnqueueUnique(DownloadJob{BuildArtistListUrl(p, 500, ""), "artist_list", "", "", p});
        }
        if (j.is_object() && j.contains("results") && j["results"].is_array())
        {
            for (const auto& it : j["results"])
            {
                if (!it.is_object())
                    continue;
                json a;
                if (it.contains("artist") && it["artist"].is_object())
                    a = it["artist"];
                else if (it.size() == 1 && it.begin().value().is_object())
                    a = it.begin().value();
                else
                    a = it;
                const std::string name = JsonStringOrEmpty(a, "name");
                if (!name.empty())
                    DatahoarderEnqueueUnique(DownloadJob{BuildArtistPacksUrl(name), "artist_packs", name, ""});
            }
        }
    }
    else if (dr.job.kind == "year_list")
    {
        if (j.is_object())
        {
            for (auto it = j.begin(); it != j.end(); ++it)
            {
                int y = 0;
                try { y = std::stoi(it.key()); } catch (...) { y = 0; }
                if (y <= 0)
                    continue;
                // Cache both variants (packs + mags) so the Years mode toggle works offline.
                DatahoarderEnqueueUnique(DownloadJob{BuildYearPacksUrl(y, false, ""), "year_packs", std::to_string(y), ""});
                DatahoarderEnqueueUnique(DownloadJob{BuildYearPacksUrl(y, true, ""), "year_packs", std::to_string(y), ""});
            }
        }
    }
    else if (dr.job.kind == "latest_list")
    {
        if (j.is_object() && j.contains("results") && j["results"].is_array())
        {
            for (const auto& it : j["results"])
            {
                if (!it.is_object())
                    continue;
                std::string name = JsonStringOrEmpty(it, "pack");
                if (name.empty())
                    name = JsonStringOrEmpty(it, "name");
                if (!name.empty())
                    DatahoarderEnqueueUnique(DownloadJob{BuildPackDetailUrl(name), "pack_detail", name, ""});
            }
        }
    }
    else if (dr.job.kind == "year_packs")
    {
        // Same shape as pack list: results[].name/year
        if (j.is_object() && j.contains("results") && j["results"].is_array())
        {
            for (const auto& it : j["results"])
            {
                if (!it.is_object())
                    continue;
                std::string name = JsonStringOrEmpty(it, "name");
                if (name.empty())
                    name = JsonStringOrEmpty(it, "pack");
                if (!name.empty())
                    DatahoarderEnqueueUnique(DownloadJob{BuildPackDetailUrl(name), "pack_detail", name, ""});
            }
        }
    }
    else if (dr.job.kind == "group_packs")
    {
        // Best-effort: extract pack names too, so we can reach pack_detail even if pack_list misses something.
        // Expected: { "results": { "packs": { "1998": ["pack1", ...] } } }
        json packs_obj;
        if (j.is_object() && j.contains("results") && j["results"].is_object())
        {
            const json& r = j["results"];
            if (r.contains("packs") && r["packs"].is_object())
                packs_obj = r["packs"];
        }
        if (packs_obj.is_object())
        {
            for (auto it = packs_obj.begin(); it != packs_obj.end(); ++it)
            {
                if (!it.value().is_array())
                    continue;
                for (const auto& pn : it.value())
                {
                    if (pn.is_string())
                    {
                        const std::string name = pn.get<std::string>();
                        if (!name.empty())
                            DatahoarderEnqueueUnique(DownloadJob{BuildPackDetailUrl(name), "pack_detail", name, ""});
                    }
                }
            }
        }
    }
    else if (dr.job.kind == "pack_detail")
    {
        // Cache pack-detail + thumbnails for offline browsing.
        if (!(j.is_object() && j.contains("results") && j["results"].is_array() && !j["results"].empty()))
            return;
        const json& r0 = j["results"][0];
        if (!(r0.contains("files") && r0["files"].is_object()))
            return;

        const std::string pack = dr.job.pack;
        for (auto it = r0["files"].begin(); it != r0["files"].end(); ++it)
        {
            const std::string filename = it.key();
            const json& frec = it.value();
            std::string tn_url;
            try
            {
                if (frec.contains("file") && frec["file"].is_object() &&
                    frec["file"].contains("tn") && frec["file"]["tn"].is_object())
                {
                    const json& tn = frec["file"]["tn"];
                    const std::string uri = JsonStringOrEmpty(tn, "uri");
                    if (!uri.empty())
                        tn_url = JoinUrl(WebBase(), uri);
                    else
                    {
                        const std::string file = JsonStringOrEmpty(tn, "file");
                        if (!file.empty())
                            tn_url = WebBase() + "/pack/" + UrlEncode(pack) + "/tn/" + UrlEncode(file);
                    }
                }
                else if (frec.contains("tn") && frec["tn"].is_object())
                {
                    const json& tn = frec["tn"];
                    const std::string uri = JsonStringOrEmpty(tn, "uri");
                    if (!uri.empty())
                        tn_url = JoinUrl(WebBase(), uri);
                    else
                    {
                        const std::string file = JsonStringOrEmpty(tn, "file");
                        if (!file.empty())
                            tn_url = WebBase() + "/pack/" + UrlEncode(pack) + "/tn/" + UrlEncode(file);
                    }
                }
            }
            catch (...) {}

            if (!tn_url.empty())
                DatahoarderEnqueueUnique(DownloadJob{tn_url, "thumb", pack, filename});
        }
    }
}


