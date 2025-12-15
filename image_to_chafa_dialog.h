// Image -> Chafa conversion dialog.
//
// Provides an ImGui modal that lets the user tweak Chafa settings, renders a live
// preview into an AnsiCanvas, and returns the final AnsiCanvas when accepted.
#pragma once

#include "canvas.h"

#include <cstdint>
#include <string>
#include <vector>

class ImageToChafaDialog
{
public:
    struct ImageRgba
    {
        std::string                label; // path or friendly name
        int                        width = 0;
        int                        height = 0;
        int                        rowstride = 0; // bytes per row (>= width*4)
        std::vector<std::uint8_t>  pixels; // RGBA8, unassociated alpha
    };

    struct Settings
    {
        int   out_cols = 80;
        bool  auto_rows = true;
        int   out_rows = 0; // used only when auto_rows=false

        // font_ratio = font_width / font_height (terminal cell aspect correction)
        // Typical terminals are taller than wide, so ~0.5 is a decent default.
        float font_ratio = 0.5f;
        bool  zoom = false;
        bool  stretch = false;

        // Output mode. We default to xterm-256 indexed mode because the editor
        // stores colors in an xterm-256-compatible palette.
        int   canvas_mode = 0; // 0 = indexed-256, 1 = truecolor

        // Symbols preset (subset of Chafa's symbol tags).
        int   symbol_preset = 0; // 0=All, 1=Blocks, 2=ASCII, 3=Braille

        // Dithering
        int   dither_mode = 2; // 0=None,1=Ordered,2=Diffusion,3=Noise
        float dither_intensity = 1.0f; // 0..1

        bool  preprocessing = true;
        float transparency_threshold = 0.0f; // 0..1
    };

    // Opens the modal and takes ownership of a copy of the source pixels.
    void Open(ImageRgba src);

    // Render the modal (call every frame). No-op when closed.
    void Render();

    // If the user pressed OK since last call, moves the resulting canvas into `out`.
    bool TakeAccepted(AnsiCanvas& out);

    // Expose settings for persistence/customization if desired.
    Settings& GetSettings() { return settings_; }
    const Settings& GetSettings() const { return settings_; }

private:
    bool open_ = false;
    bool open_popup_next_frame_ = false;
    bool dirty_ = true;

    ImageRgba src_;
    Settings  settings_;

    AnsiCanvas preview_{80};
    bool       has_preview_ = false;
    std::string error_;

    bool accepted_ = false;
    AnsiCanvas accepted_canvas_{80};

    bool RegeneratePreview();
};



