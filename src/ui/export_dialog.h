#pragma once

#include "io/formats/ansi.h"
#include "io/formats/image.h"
#include "io/formats/plaintext.h"
#include "io/formats/xbin.h"

#include <string>

class IoManager;
class SdlFileDialogQueue;
struct SdlFileDialogResult;
struct SDL_Window;
struct SessionState;
class AnsiCanvas;

class ExportDialog
{
public:
    enum class Tab
    {
        Ansi = 0,
        Plaintext,
        Image,
        XBin,
    };

    void Open(Tab tab = Tab::Ansi)
    {
        open_ = true;
        requested_tab_ = tab;
        apply_requested_tab_ = true;
    }
    void SetOpen(bool open) { open_ = open; }
    bool IsOpen() const { return open_; }

    // Render the export window (tabbed) and allow launching native save dialogs.
    void Render(const char* title,
                SDL_Window* window,
                SdlFileDialogQueue& dialogs,
                IoManager& io,
                AnsiCanvas* focused_canvas,
                SessionState* session,
                bool apply_placement_this_frame);

    // Handle completed native file dialogs for export, run exporters, and report errors via IoManager.
    // Returns true if the dialog result was consumed by this export dialog.
    bool HandleDialogResult(const SdlFileDialogResult& r, IoManager& io, AnsiCanvas* focused_canvas);

private:
    bool open_ = false;
    bool initialized_ = false;
    bool apply_requested_tab_ = false;
    Tab requested_tab_ = Tab::Ansi;
    Tab active_tab_ = Tab::Ansi;

    // ANSI
    int ansi_preset_idx_ = 0;
    formats::ansi::ExportOptions ansi_opt_{};
    bool ansi_override_default_fg_ = false;
    bool ansi_override_default_bg_ = false;

    // Plaintext
    int text_preset_idx_ = 0;
    formats::plaintext::ExportOptions text_opt_{};

    // Image
    formats::image::ExportOptions image_opt_{};

    // XBin
    formats::xbin::ExportOptions xbin_opt_{};
};


