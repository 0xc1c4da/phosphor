#include "io/session/open_canvas_cache.h"

#include "core/paths.h"
#include "io/project_file.h"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace open_canvas_cache
{
namespace fs = std::filesystem;

std::string GetSessionCanvasCacheDir()
{
    return PhosphorCachePath("session_canvases");
}

static std::string ResolveCachePath(const std::string& rel_or_abs)
{
    if (rel_or_abs.empty())
        return std::string();
    fs::path p(rel_or_abs);
    if (p.is_absolute())
        return rel_or_abs;
    return PhosphorCachePath(rel_or_abs);
}

bool SaveCanvasToSessionCachePhos(int canvas_id,
                                 const AnsiCanvas& canvas,
                                 std::string& out_rel_path,
                                 std::string& err)
{
    err.clear();
    out_rel_path.clear();

    if (canvas_id <= 0)
    {
        err = "Invalid canvas id.";
        return false;
    }

    try
    {
        fs::create_directories(GetSessionCanvasCacheDir());
    }
    catch (const std::exception& e)
    {
        err = e.what();
        return false;
    }

    out_rel_path = "session_canvases/canvas_" + std::to_string(canvas_id) + ".phos";
    const std::string abs = ResolveCachePath(out_rel_path);
    return project_file::SaveProjectToFile(abs, canvas, err);
}

bool LoadCanvasFromSessionCachePhos(const std::string& rel_or_abs_path, AnsiCanvas& out_canvas, std::string& err)
{
    err.clear();
    const std::string abs = ResolveCachePath(rel_or_abs_path);
    if (abs.empty())
    {
        err = "Empty cache path.";
        return false;
    }
    return project_file::LoadProjectFromFile(abs, out_canvas, err);
}

bool DeleteSessionCanvasCachePhos(const std::string& rel_or_abs_path, std::string& err)
{
    err.clear();
    const std::string abs = ResolveCachePath(rel_or_abs_path);
    if (abs.empty())
        return true;

    try
    {
        std::error_code ec;
        const bool removed = fs::remove(fs::path(abs), ec);
        if (ec)
        {
            err = ec.message();
            return false;
        }
        return removed || !fs::exists(fs::path(abs));
    }
    catch (const std::exception& e)
    {
        err = e.what();
        return false;
    }
}

void PruneSessionCanvasCache(const std::vector<std::string>& keep_rel_paths)
{
    std::unordered_set<std::string> keep;
    keep.reserve(keep_rel_paths.size());
    for (const auto& p : keep_rel_paths)
        keep.emplace(p);

    try
    {
        const fs::path dir(GetSessionCanvasCacheDir());
        if (!fs::exists(dir) || !fs::is_directory(dir))
            return;

        for (const auto& ent : fs::directory_iterator(dir))
        {
            if (!ent.is_regular_file())
                continue;
            const fs::path p = ent.path();
            if (p.extension() != ".phos")
                continue;

            // Compare using cache-relative paths.
            const std::string rel = std::string("session_canvases/") + p.filename().string();
            if (keep.find(rel) != keep.end())
                continue;

            std::error_code ec;
            fs::remove(p, ec);
        }
    }
    catch (...)
    {
        // Best-effort: ignore.
    }
}
} // namespace open_canvas_cache


