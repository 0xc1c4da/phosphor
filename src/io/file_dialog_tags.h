#pragma once

// Shared tag IDs for routing SDL3 file-dialog results.
// Keep these stable: they may be used across multiple call sites.
enum FileDialogTag
{
    kDialog_SaveProject = 1,
    kDialog_LoadProject = 2,
    kDialog_ImportAnsi  = 3,
    kDialog_ExportAnsi  = 4,
    kDialog_ImportImage = 5,
};


