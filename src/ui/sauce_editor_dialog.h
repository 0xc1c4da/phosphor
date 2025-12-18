#pragma once

#include "core/canvas.h"

#include <ctime>
#include <string>

// ImGui modal for editing SAUCE metadata associated with a canvas.
// Stored on the canvas as AnsiCanvas::ProjectState::SauceMeta (persisted via .phos/session).
class SauceEditorDialog
{
public:
    // Open the dialog, copying current canvas SAUCE into the dialog buffers.
    void OpenFromCanvas(const AnsiCanvas& canvas);

    // Render the dialog if open. Uses a per-canvas popup id to avoid collisions.
    void Render(AnsiCanvas& canvas, const char* popup_id);

private:
    bool m_open = false;
    bool m_open_queued = false;

    // Working copy.
    AnsiCanvas::ProjectState::SauceMeta m_meta;

    // Editable text buffers.
    std::string m_title;
    std::string m_author;
    std::string m_group;
    std::string m_tinfos;
    std::string m_comments_text; // newline separated

    // Date picker (stored as CCYYMMDD on the meta).
    std::tm m_date{}; // local date; tm_year is years since 1900, tm_mon is 0-11

    // Editable numeric buffers (kept as ints for ImGui widgets).
    int m_data_type = 1;
    int m_file_type = 1;
    int m_tinfo1 = 0;
    int m_tinfo2 = 0;
    int m_tinfo3 = 0;
    int m_tinfo4 = 0;
    int m_tflags = 0;

    static void ClampAndSanitizeForSauce(AnsiCanvas::ProjectState::SauceMeta& meta);
};


