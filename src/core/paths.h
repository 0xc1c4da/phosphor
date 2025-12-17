#pragma once

#include <string>

// Returns the base assets directory used by phosphor.
//
// Defaults to "<config_dir>/assets" where config_dir is returned by GetPhosphorConfigDir().
std::string GetPhosphorAssetsDir();

// Joins the assets dir and a relative path within it.
// Example: PhosphorAssetPath("tools") -> "<assets_dir>/tools"
std::string PhosphorAssetPath(const std::string& relative);


