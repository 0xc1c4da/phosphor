#pragma once

// Shared tag IDs for routing SDL3 file-dialog results.
// Keep these stable: they may be used across multiple call sites.
enum FileDialogTag
{
    kDialog_SaveProject = 1,
    kDialog_ExportAnsi  = 4,
    // Unified loader for projects, ANSI/text, and images.
    kDialog_LoadFile    = 2,
};


