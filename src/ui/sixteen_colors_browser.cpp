#include "ui/sixteen_colors_browser.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include "io/ansi_importer.h"
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

// Fast coarse draw: downsample into a small grid of solid rects.
static void DrawRgbaPreviewGrid(const unsigned char* rgba, int w, int h, const ImVec2& size_px, int max_grid_dim = 32)
{
    if (!rgba || w <= 0 || h <= 0)
        return;
    if (size_px.x <= 0.0f || size_px.y <= 0.0f)
        return;

    int grid_w = std::min(w, max_grid_dim);
    int grid_h = std::min(h, max_grid_dim);
    if (w >= h)
        grid_h = std::max(1, (int)std::lround((double)h * ((double)grid_w / (double)w)));
    else
        grid_w = std::max(1, (int)std::lround((double)w * ((double)grid_h / (double)h)));

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
        int src_y = (int)std::floor(((gy + 0.5f) * (float)h) / (float)grid_h);
        src_y = std::clamp(src_y, 0, h - 1);
        for (int gx = 0; gx < grid_w; ++gx)
        {
            const float x0 = p0.x + gx * cell_w;
            const float x1 = x0 + cell_w;
            int src_x = (int)std::floor(((gx + 0.5f) * (float)w) / (float)grid_w);
            src_x = std::clamp(src_x, 0, w - 1);
            const size_t base = ((size_t)src_y * (size_t)w + (size_t)src_x) * 4u;
            const unsigned char r = rgba[base + 0];
            const unsigned char g = rgba[base + 1];
            const unsigned char b = rgba[base + 2];
            const unsigned char a = rgba[base + 3];
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(r, g, b, a));
        }
    }
    dl->AddRect(p0, p1, IM_COL32(90, 90, 105, 255), 4.0f);
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
    m_worker = std::thread([this]()
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

            http::Response r = http::Get(job.url);
            res.status = r.status;
            res.err = r.err;
            res.bytes = std::move(r.body);

            {
                std::lock_guard<std::mutex> lock(m_mu);
                m_results.push_back(std::move(res));
            }
        }
    });
}

void SixteenColorsBrowserWindow::StopWorker()
{
    if (!m_worker_running)
        return;
    m_worker_running = false;
    if (m_worker.joinable())
        m_worker.join();
}

void SixteenColorsBrowserWindow::Enqueue(const DownloadJob& j)
{
    std::lock_guard<std::mutex> lock(m_mu);
    // Prioritize "raw" downloads so clicking an item opens quickly even while thumbs stream in.
    if (j.kind == "raw")
        m_jobs.insert(m_jobs.begin(), j);
    else
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
        if (dr.job.kind == "thumb")
        {
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
            t.ready = true;
            t.failed = false;
            t.w = w;
            t.h = h;
            t.rgba = std::move(rgba);
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
                (ext == "ans" || ext == "asc" || ext == "txt");

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
                if (!ansi_importer::ImportAnsiBytesToCanvas(dr.bytes, imported, ierr))
                {
                    m_last_error = ierr;
                    continue;
                }
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
            m_pack_list_pending = false;
            m_pack_list_pending_url.clear();
            m_loading_list = false;
            if (!dr.err.empty())
            {
                m_last_error = dr.err;
                m_pack_list_json.clear();
                continue;
            }
            m_pack_list_json.assign((const char*)dr.bytes.data(), (const char*)dr.bytes.data() + dr.bytes.size());
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
    }

    // Top controls
    ImGui::Separator();

    bool list_settings_changed = false;

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##filter", "Search packs (filter=...)", &m_filter))
        list_settings_changed = true;

    ImGui::SetNextItemWidth(110.0f);
    int page_before = m_page;
    ImGui::InputInt("Page", &m_page);
    m_page = std::max(1, m_page);
    if (m_page != page_before)
        list_settings_changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    int pagesize_before = m_pagesize;
    ImGui::InputInt("Page Size", &m_pagesize);
    m_pagesize = std::clamp(m_pagesize, 1, 500);
    if (m_pagesize != pagesize_before)
        list_settings_changed = true;

    if (ImGui::Checkbox("Include groups", &m_show_groups))
        list_settings_changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Include artists", &m_show_artists))
        list_settings_changed = true;

    if (list_settings_changed)
        m_dirty_list = true;

    // Auto-fetch pack list on first open and whenever settings change.
    if ((m_pack_list_json.empty() || m_dirty_list) && !m_pack_list_pending)
    {
        const std::string url =
            "https://api.16colo.rs/v1/pack/?page=" + std::to_string(m_page) +
            "&pagesize=" + std::to_string(m_pagesize) +
            "&archive=true" +
            std::string("&groups=") + (m_show_groups ? "true" : "false") +
            std::string("&artists=") + (m_show_artists ? "true" : "false") +
            (m_filter.empty() ? "" : ("&filter=" + UrlEncode(m_filter)));

        m_pack_list_pending = true;
        m_pack_list_pending_url = url;
        m_loading_list = true;
        m_dirty_list = false;
        Enqueue(DownloadJob{url, "pack_list", "", ""});
    }

    if (m_loading_list || m_loading_pack || m_raw_pending > 0)
    {
        ImGui::SameLine();
        if (m_raw_pending > 0)
            ImGui::Text("Downloading %d...", m_raw_pending);
        else if (m_loading_pack)
            ImGui::TextUnformatted("Loading pack...");
        else
            ImGui::TextUnformatted("Loading...");
    }

    if (!m_last_error.empty())
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", m_last_error.c_str());
    }

    ImGui::Separator();

    // Two-column layout: pack list (left) + pack contents grid (right)
    ImGui::Columns(2, "##16c_cols", true);
    {
        const float w = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
        const float left = std::clamp(w * 0.22f, 200.0f, 420.0f);
        ImGui::SetColumnWidth(0, left);
    }

    // Left: pack list
    ImGui::TextUnformatted("Packs");
    ImGui::Separator();
    if (m_pack_list_json.empty())
    {
        ImGui::TextUnformatted("No pack list loaded yet.");
    }
    else
    {
        json j;
        try { j = json::parse(m_pack_list_json); }
        catch (const std::exception& e)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "JSON parse failed: %s", e.what());
            j = json();
        }

        if (j.is_object() && j.contains("results") && j["results"].is_array())
        {
            ImGui::BeginChild("##packs_list", ImVec2(0, 0), true);
            for (const auto& it : j["results"])
            {
                if (!it.is_object())
                    continue;
                const int year = JsonIntOrDefault(it, "year", 0);
                const std::string name = JsonStringOrEmpty(it, "name");
                if (name.empty())
                    continue;

                char label[256];
                std::snprintf(label, sizeof(label), "%s (%d)", name.c_str(), year);
                const bool selected = (name == m_selected_pack);
                if (ImGui::Selectable(label, selected))
                {
                    m_selected_pack = name;
                    m_pack_detail_json.clear();
                    m_thumbs.clear();

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

                    // Auto-fetch pack details on selection.
                    const std::string url =
                        "https://api.16colo.rs/v1/pack/" + UrlEncode(m_selected_pack) +
                        "?sauce=false&dimensions=true&content=true&artists=true";
                    m_pack_detail_pending = true;
                    m_pack_detail_pending_pack = m_selected_pack;
                    m_loading_pack = true;
                    Enqueue(DownloadJob{url, "pack_detail", m_selected_pack, ""});
                }
            }
            ImGui::EndChild();
        }
        else
        {
            ImGui::TextUnformatted("Unexpected response.");
        }
    }

    ImGui::NextColumn();

    // Right: pack contents
    ImGui::Text("Pack: %s", m_selected_pack.empty() ? "(none)" : m_selected_pack.c_str());
    ImGui::Separator();

    if (!m_selected_pack.empty())
    {
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

                        // Build thumbnail URL if present.
                        std::string tn_url;
                        try
                        {
                            if (frec.contains("file") && frec["file"].is_object() &&
                                frec["file"].contains("tn") && frec["file"]["tn"].is_object())
                            {
                                const json& tn = frec["file"]["tn"];
                                const std::string uri = JsonStringOrEmpty(tn, "uri");
                                tn_url = JoinUrl("https://16colo.rs", uri);
                            }
                        }
                        catch (...) {}

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
                                DrawRgbaPreviewGrid(t.rgba.data(), t.w, t.h, ImVec2(thumb_w, thumb_h), 28);
                            }
                            else
                            {
                                ImGui::InvisibleButton("##thumb_canvas", ImVec2(thumb_w, thumb_h));
                                ImDrawList* dl = ImGui::GetWindowDrawList();
                                dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(20, 20, 24, 255), 4.0f);
                                dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(90, 90, 105, 255), 4.0f);
                                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - thumb_h + 8.0f);
                                ImGui::TextUnformatted(t.failed ? "thumb failed" : "loading...");
                            }
                        }
                        else
                        {
                            ImGui::InvisibleButton("##thumb_canvas", ImVec2(thumb_w, thumb_h));
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(20, 20, 24, 255), 4.0f);
                            dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(90, 90, 105, 255), 4.0f);
                            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - thumb_h + 8.0f);
                            ImGui::TextUnformatted("no thumbnail");
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
                    ImGui::TextUnformatted("No files for this pack.");
                }
            }
            else
            {
                ImGui::TextUnformatted("Unexpected pack details response.");
            }
        }
        else
        {
            ImGui::TextUnformatted("No pack details loaded yet.");
        }
    }
    else
    {
        ImGui::TextUnformatted("Select a pack on the left.");
    }

    ImGui::Columns(1);

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
}


