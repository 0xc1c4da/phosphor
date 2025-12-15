#pragma once

#include "canvas.h"

#include <functional>
#include <string>

// IO manager:
// - owns File menu items (Save/Load/Import/Export)
// - provides an in-app ImGui file picker (open/save)
// - serializes projects as CBOR via nlohmann::json (implemented in io_manager.cpp)
class IoManager
{
public:
    struct Callbacks
    {
        // Called when Load/Import produces a new canvas.
        std::function<void(AnsiCanvas&&)> create_canvas;
    };

    IoManager();

    // Call from within the "File" menu.
    void RenderFileMenu(AnsiCanvas* focused_canvas, const Callbacks& cb);

    // Call once per frame (after menus) to render any active popups/modals.
    void RenderPopups(AnsiCanvas* focused_canvas, const Callbacks& cb);

private:
    enum class DialogKind
    {
        None = 0,
        SaveProject,
        LoadProject,
        ImportAnsi,
        ExportAnsi,
    };

    struct FileDialogState
    {
        DialogKind kind = DialogKind::None;
        bool       request_open = false; // triggers ImGui::OpenPopup next frame

        std::string title;
        std::string ok_label;
        bool        is_save = false;

        std::string current_dir;
        std::string selected_name;
        char        filename_buf[256] = {};

        std::string error;
        std::string info;
    };

    FileDialogState m_dialog;
    std::string     m_last_status;

    void OpenDialog(DialogKind kind);
    void RenderDialogContents(AnsiCanvas* focused_canvas, const Callbacks& cb);
};

