#pragma once

#include <string>

// Returns the base assets directory used by phosphor.
//
// Defaults to "<config_dir>/assets" where config_dir is returned by GetPhosphorConfigDir().
std::string GetPhosphorAssetsDir();

// Joins the assets dir and a relative path within it.
// Example: PhosphorAssetPath("tools") -> "<assets_dir>/tools"
std::string PhosphorAssetPath(const std::string& relative);

// Returns the base cache directory used by phosphor.
//
// Defaults to "<config_dir>/cache" where config_dir is returned by GetPhosphorConfigDir().
// Intended for cached network content (packs, thumbnails, downloaded artwork, etc).
std::string GetPhosphorCacheDir();

// Joins the cache dir and a relative path within it.
// Example: PhosphorCachePath("http/abcd.bin") -> "<cache_dir>/http/abcd.bin"
std::string PhosphorCachePath(const std::string& relative);


