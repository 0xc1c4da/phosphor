#pragma once

// Shared tag IDs for routing SDL3 file-dialog results.
// Keep these stable: they may be used across multiple call sites.
enum FileDialogTag
{
    kDialog_SaveProject = 1,
    kDialog_ExportAnsi  = 4,
    kDialog_ExportImage = 5,
    // Unified loader for projects, ANSI/text, and images.
    kDialog_LoadFile    = 2,

    // Unified export dialog (UI-driven; handled by ui::ExportDialog, not IoManager).
    kDialog_ExportDlg_Ansi      = 20,
    kDialog_ExportDlg_Plaintext = 21,
    kDialog_ExportDlg_Image     = 22,
    kDialog_ExportDlg_XBin      = 23,
};


