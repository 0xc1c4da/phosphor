#pragma once

#include <string>

class AnsiCanvas;

namespace app
{
// Copy the current selection (composited "what you see") to the OS clipboard as UTF-8 text.
// Returns false if there is no selection or clipboard backend unavailable.
bool CopySelectionToSystemClipboardText(const AnsiCanvas& canvas);

// Paste OS clipboard UTF-8 text at (x,y) in the active layer.
// - If the canvas has a selection, replaces it and pastes at selection top-left.
// - Detects ANSI escape sequences and preserves colours/attrs when present.
// - Always selects the pasted region on success.
// Returns false if clipboard empty/unavailable or parse failed.
bool PasteSystemClipboardText(AnsiCanvas& canvas, int x, int y);
} // namespace app


