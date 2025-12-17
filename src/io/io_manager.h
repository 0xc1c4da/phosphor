#pragma once

#include "core/canvas.h"

#include <functional>
#include <string>
#include <vector>

struct SdlFileDialogResult;
class SdlFileDialogQueue;
struct SDL_Window;
struct SessionState;

// IO manager:
// - owns File menu items (Save/Load/Import/Export)
// - serializes projects as CBOR via nlohmann::json (implemented in io_manager.cpp)
class IoManager
{
public:
    struct Callbacks
    {
        // Called when Load/Import produces a new canvas.
        std::function<void(AnsiCanvas&&)> create_canvas;

        struct LoadedImage
        {
            std::string path;
            int width = 0;
            int height = 0;
            std::vector<unsigned char> pixels; // RGBA8
        };

        // Called when Load produces a new image window payload.
        std::function<void(LoadedImage&&)> create_image;
    };

    IoManager();

    // Call from within the "File" menu.
    void RenderFileMenu(SDL_Window* window, SdlFileDialogQueue& dialogs, AnsiCanvas* focused_canvas, const Callbacks& cb);

    // Handle a completed SDL file dialog (polled from SdlFileDialogQueue).
    void HandleDialogResult(const SdlFileDialogResult& r, AnsiCanvas* focused_canvas, const Callbacks& cb);

    // Optional UI helpers (ImGui) to show last status / error.
    void RenderStatusWindows(SessionState* session = nullptr, bool apply_placement_this_frame = false);

    // Sync with persisted session state.
    void SetLastDir(const std::string& dir) { m_last_dir = dir; }
    const std::string& GetLastDir() const { return m_last_dir; }

private:
    std::string m_last_dir;
    std::string m_last_error;
};

