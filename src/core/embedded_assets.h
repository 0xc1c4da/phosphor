#pragma once

#include <string>

// Ensures the built-in (embedded) assets have been extracted into the user's config directory.
//
// Extraction is "non-destructive": existing files are not overwritten, so user edits persist.
// Returns true if assets are available after the call; on failure returns false and sets `error`.
bool EnsureBundledAssetsExtracted(std::string& error);


