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

        // Called when a Markdown file is selected for import.
        // The app is expected to open a preview+settings import dialog and only create
        // a canvas after the user accepts.
        struct MarkdownPayload
        {
            std::string path;     // original path (for window title + recent tracking)
            std::string markdown; // UTF-8 bytes (best-effort; control chars should be filtered later)
        };
        std::function<void(MarkdownPayload&&)> open_markdown_import_dialog;
    };

    IoManager();

    // Programmatic triggers (used by keybindings, not just menu clicks).
    // Save: if the canvas has a local file path, writes immediately; otherwise falls back to Save As.
    void SaveProject(SDL_Window* window, SdlFileDialogQueue& dialogs, AnsiCanvas* target_canvas);
    // Save As: always opens a save dialog and writes to the chosen path.
    void SaveProjectAs(SDL_Window* window, SdlFileDialogQueue& dialogs, AnsiCanvas* target_canvas);
    // Targeted save: ensures the dialog result applies to `target_canvas` even if focus changes
    // before the file dialog returns.
    void RequestSaveProject(SDL_Window* window, SdlFileDialogQueue& dialogs, AnsiCanvas* target_canvas);
    void RequestLoadFile(SDL_Window* window, SdlFileDialogQueue& dialogs);
    void RequestExportAnsi(SDL_Window* window, SdlFileDialogQueue& dialogs);
    void RequestExportImage(SDL_Window* window, SdlFileDialogQueue& dialogs);

    // Call from within the "File" menu.
    using ShortcutProvider = std::function<std::string(std::string_view action_id)>;
    void RenderFileMenu(SDL_Window* window,
                        SdlFileDialogQueue& dialogs,
                        AnsiCanvas* focused_canvas,
                        const Callbacks& cb,
                        const ShortcutProvider& shortcut_for_action = {});

    // Handle a completed SDL file dialog (polled from SdlFileDialogQueue).
    void HandleDialogResult(const SdlFileDialogResult& r, AnsiCanvas* focused_canvas, const Callbacks& cb);

    // Open a path directly (used by File -> Recent).
    // Returns true if the path was handled (successfully opened OR failed with an error message).
    bool OpenPath(const std::string& path, const Callbacks& cb);

    // Optional UI helpers (ImGui) to show last status / error.
    void RenderStatusWindows(SessionState* session = nullptr, bool apply_placement_this_frame = false);

    // Open/import outcome reporting (used to update File -> Recent lists).
    enum class OpenEventKind
    {
        None = 0,
        Canvas,
        Image,
    };
    struct OpenEvent
    {
        OpenEventKind kind = OpenEventKind::None;
        std::string path;
        std::string error;
    };
    bool TakeLastOpenEvent(OpenEvent& out);

    // Save dialog outcome reporting (used by close-confirm UX).
    enum class SaveEventKind
    {
        None = 0,
        Success,
        Failed,
        Canceled,
    };
    struct SaveEvent
    {
        SaveEventKind kind = SaveEventKind::None;
        AnsiCanvas* canvas = nullptr; // not owned
        std::string path;             // chosen path for success
        std::string error;            // failure reason (if any)
    };
    bool TakeLastSaveEvent(SaveEvent& out);

    // Sync with persisted session state.
    void SetLastDir(const std::string& dir) { m_last_dir = dir; }
    const std::string& GetLastDir() const { return m_last_dir; }

    // Allow other UI subsystems (e.g. Export dialog) to surface IO errors in the standard place.
    void SetLastError(const std::string& err) { m_last_error = err; }
    void ClearLastError() { m_last_error.clear(); }

private:
    bool SaveProjectToPath(const std::string& path, AnsiCanvas& canvas, std::string& err);

    std::string m_last_dir;
    std::string m_last_error;

    // Pending target canvas for the Save Project dialog (not owned).
    AnsiCanvas* m_pending_save_canvas = nullptr;

    SaveEvent m_last_save_event;
    OpenEvent m_last_open_event;
};

