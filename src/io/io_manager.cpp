#include "io/io_manager.h"

#include "io/file_dialog_tags.h"
#include "io/formats/ansi.h"
#include "io/image_loader.h"
#include "io/project_file.h"
#include "io/sdl_file_dialog_queue.h"

#include "imgui.h"
#include "io/session/imgui_persistence.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;
} // namespace

IoManager::IoManager()
{
    // Default to current working directory when the app starts.
    try
    {
        m_last_dir = fs::current_path().string();
    }
    catch (...)
    {
        m_last_dir = ".";
    }
}

void IoManager::RequestSaveProject(SDL_Window* window, SdlFileDialogQueue& dialogs)
{
    m_last_error.clear();
    std::vector<SdlFileDialogQueue::FilterPair> filters = {
        {"Phosphor Project (*.phos)", "phos"},
        {"All files", "*"},
    };
    fs::path base = m_last_dir.empty() ? fs::path(".") : fs::path(m_last_dir);
    fs::path suggested = base / "project.phos";
    dialogs.ShowSaveFileDialog(kDialog_SaveProject, window, filters, suggested.string());
}

void IoManager::RequestLoadFile(SDL_Window* window, SdlFileDialogQueue& dialogs)
{
    m_last_error.clear();
    std::vector<SdlFileDialogQueue::FilterPair> filters = {
        {"Supported files (*.phos;*.ans;*.asc;*.txt;*.nfo;*.diz;*.png;*.jpg;*.jpeg;*.gif;*.bmp)", "phos;ans;asc;txt;nfo;diz;png;jpg;jpeg;gif;bmp"},
        {"Phosphor Project (*.phos)", "phos"},
        {"ANSI / Text (*.ans;*.asc;*.txt;*.nfo;*.diz)", "ans;asc;txt;nfo;diz"},
        {"Images (*.png;*.jpg;*.jpeg;*.gif;*.bmp)", "png;jpg;jpeg;gif;bmp"},
        {"All files", "*"},
    };
    dialogs.ShowOpenFileDialog(kDialog_LoadFile, window, filters, m_last_dir, false);
}

void IoManager::RequestExportAnsi(SDL_Window* window, SdlFileDialogQueue& dialogs)
{
    m_last_error.clear();
    std::vector<SdlFileDialogQueue::FilterPair> filters = {
        {"ANSI / Text (*.ans;*.txt)", "ans;txt"},
        {"All files", "*"},
    };
    fs::path base = m_last_dir.empty() ? fs::path(".") : fs::path(m_last_dir);
    fs::path suggested = base / "export.ans";
    dialogs.ShowSaveFileDialog(kDialog_ExportAnsi, window, filters, suggested.string());
}

void IoManager::RenderFileMenu(SDL_Window* window, SdlFileDialogQueue& dialogs, AnsiCanvas* focused_canvas, const Callbacks& cb)
{
    const bool has_focus_canvas = (focused_canvas != nullptr);

    // Save requires a focused canvas (for now).
    if (!has_focus_canvas)
        ImGui::BeginDisabled();
    if (ImGui::MenuItem("Save..."))
    {
        RequestSaveProject(window, dialogs);
    }
    if (!has_focus_canvas)
        ImGui::EndDisabled();

    if (ImGui::MenuItem("Load..."))
    {
        RequestLoadFile(window, dialogs);
    }

    if (ImGui::MenuItem("Export..."))
    {
        RequestExportAnsi(window, dialogs);
    }

    (void)cb;
}

void IoManager::HandleDialogResult(const SdlFileDialogResult& r, AnsiCanvas* focused_canvas, const Callbacks& cb)
{
    // Ignore dialogs not owned by IoManager.
    if (r.tag != kDialog_SaveProject &&
        r.tag != kDialog_LoadFile &&
        r.tag != kDialog_ExportAnsi)
        return;

    if (!r.error.empty())
    {
        m_last_error = r.error;
        return;
    }
    if (r.canceled || r.paths.empty())
        return;

    const std::string chosen = r.paths[0];

    auto is_uri = [](const std::string& s) -> bool
    {
        return s.find("://") != std::string::npos;
    };

    if (!is_uri(chosen))
    {
        try
        {
            fs::path p(chosen);
            if (p.has_parent_path())
                m_last_dir = p.parent_path().string();
        }
        catch (...) {}
    }

    std::string err;

    if (r.tag == kDialog_SaveProject)
    {
        if (!focused_canvas)
        {
            m_last_error = "No focused canvas to save.";
            return;
        }

        std::string path = chosen;
        if (!is_uri(path))
        {
            fs::path p(path);
            if (p.extension().empty())
                path += ".phos";
        }

        if (project_file::SaveProjectToFile(path, *focused_canvas, err))
        {
            // Treat successful save as establishing the document's canonical file path.
            focused_canvas->SetFilePath(path);
            m_last_error.clear();
        }
        else
        {
            m_last_error = err.empty() ? "Save failed." : err;
        }
    }
    else if (r.tag == kDialog_LoadFile)
    {
        auto to_lower = [](std::string s) -> std::string
        {
            for (char& c : s)
                c = (char)std::tolower((unsigned char)c);
            return s;
        };

        std::string ext;
        if (!is_uri(chosen))
        {
            try
            {
                fs::path p(chosen);
                ext = p.extension().string();
            }
            catch (...) {}
        }
        if (!ext.empty() && ext[0] == '.')
            ext.erase(ext.begin());
        ext = to_lower(ext);

        auto is_ext = [&](const std::initializer_list<const char*>& exts) -> bool
        {
            for (const char* e : exts)
            {
                if (ext == e)
                    return true;
            }
            return false;
        };

        // Fast-path by file extension.
        const bool looks_project = is_ext({"phos"});
        const bool looks_ansi = is_ext({"ans", "asc", "txt", "nfo", "diz"});
        const bool looks_image = is_ext({"png", "jpg", "jpeg", "gif", "bmp"});

        auto try_load_project = [&]() -> bool
        {
            if (!cb.create_canvas)
            {
                m_last_error = "Internal error: create_canvas callback not set.";
                return true; // handled (as error)
            }
            AnsiCanvas loaded;
            if (project_file::LoadProjectFromFile(chosen, loaded, err))
            {
                loaded.SetFilePath(chosen);
                cb.create_canvas(std::move(loaded));
                m_last_error.clear();
                return true;
            }
            return false;
        };

        auto try_import_ansi = [&]() -> bool
        {
            if (!cb.create_canvas)
            {
                m_last_error = "Internal error: create_canvas callback not set.";
                return true; // handled (as error)
            }
            AnsiCanvas imported;
            std::string ierr;
            if (formats::ansi::ImportFileToCanvas(chosen, imported, ierr))
            {
                imported.SetFilePath(chosen);
                cb.create_canvas(std::move(imported));
                m_last_error.clear();
                return true;
            }
            err = ierr;
            return false;
        };

        auto try_load_image = [&]() -> bool
        {
            if (!cb.create_image)
            {
                m_last_error = "Internal error: create_image callback not set.";
                return true; // handled (as error)
            }
            int iw = 0, ih = 0;
            std::vector<unsigned char> rgba;
            std::string ierr;
            if (image_loader::LoadImageAsRgba32(chosen, iw, ih, rgba, ierr))
            {
                Callbacks::LoadedImage li;
                li.path = chosen;
                li.width = iw;
                li.height = ih;
                li.pixels = std::move(rgba);
                cb.create_image(std::move(li));
                m_last_error.clear();
                return true;
            }
            err = ierr;
            return false;
        };

        bool handled = false;
        if (looks_project)
            handled = try_load_project();
        else if (looks_ansi)
            handled = try_import_ansi();
        else if (looks_image)
            handled = try_load_image();
        else
        {
            // Unknown extension (or URI). Try in descending order of likelihood.
            handled = try_load_project() || try_import_ansi() || try_load_image();
        }

        if (!handled)
        {
            // Use the most recent decoder error if we have one.
            m_last_error = err.empty() ? "Unsupported file type or failed to load file." : err;
        }
    }
    else if (r.tag == kDialog_ExportAnsi)
    {
        // Stub: UI contract only (we'll implement proper export later).
        (void)chosen;
        m_last_error = "Export is not implemented yet.";
    }
}

void IoManager::RenderStatusWindows(SessionState* session, bool apply_placement_this_frame)
{
    if (!m_last_error.empty())
    {
        const char* wname = "File Error";
        if (session)
            ApplyImGuiWindowPlacement(*session, wname, apply_placement_this_frame);

        ImGui::Begin(wname, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        if (session)
            CaptureImGuiWindowPlacement(*session, wname);

        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_last_error.c_str());
        if (ImGui::Button("Dismiss"))
            m_last_error.clear();
        ImGui::End();
    }
}

