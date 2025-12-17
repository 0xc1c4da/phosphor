#include "core/paths.h"

#include "io/session/session_state.h"

#include <filesystem>

std::string GetPhosphorAssetsDir()
{
    namespace fs = std::filesystem;
    return (fs::path(GetPhosphorConfigDir()) / "assets").string();
}

std::string PhosphorAssetPath(const std::string& relative)
{
    namespace fs = std::filesystem;
    if (relative.empty())
        return GetPhosphorAssetsDir();
    return (fs::path(GetPhosphorAssetsDir()) / relative).string();
}


