#include "io/io_manager.h"

#include "io/file_dialog_tags.h"
#include "io/formats/ansi.h"
#include "io/formats/image.h"
#include "io/formats/plaintext.h"
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

static std::string ToLowerAscii(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool ExtIn(std::string_view ext, const std::vector<std::string_view>& exts)
{
    for (auto e : exts)
    {
        if (ext == e)
            return true;
    }
    return false;
}

static void AppendUnique(std::vector<std::string_view>& dst, const std::vector<std::string_view>& src)
{
    for (auto e : src)
    {
        if (!ExtIn(e, dst))
            dst.push_back(e);
    }
}

static std::string JoinExtsForDialog(const std::vector<std::string_view>& exts)
{
    std::string out;
    for (size_t i = 0; i < exts.size(); ++i)
    {
        if (i) out.push_back(';');
        out.append(exts[i].begin(), exts[i].end());
    }
    return out;
}

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

    // File dialog filter strings are semicolon-separated extension lists without dots.
    std::vector<std::string_view> text_exts_v;
    AppendUnique(text_exts_v, formats::ansi::ImportExtensions());
    AppendUnique(text_exts_v, formats::plaintext::ImportExtensions());
    const std::string text_exts = JoinExtsForDialog(text_exts_v);

    const std::string image_exts = JoinExtsForDialog(std::vector<std::string_view>(formats::image::ImportExtensions().begin(),
                                                                                   formats::image::ImportExtensions().end()));

    std::vector<std::string_view> supported_exts_v;
    supported_exts_v.push_back("phos");
    AppendUnique(supported_exts_v, text_exts_v);
    // Keep the same image list for "Supported files".
    AppendUnique(supported_exts_v, formats::image::ImportExtensions());
    const std::string supported_exts = JoinExtsForDialog(supported_exts_v);

    std::vector<SdlFileDialogQueue::FilterPair> filters = {
        {"Supported files (*.phos;*.ans;*.asc;*.txt;*.nfo;*.diz;*.png;*.jpg;*.jpeg;*.gif;*.bmp)", supported_exts},
        {"Phosphor Project (*.phos)", "phos"},
        {"ANSI / Text (*.ans;*.asc;*.txt;*.nfo;*.diz)", text_exts},
        {"Images (*.png;*.jpg;*.jpeg;*.gif;*.bmp)", image_exts},
        {"All files", "*"},
    };
    dialogs.ShowOpenFileDialog(kDialog_LoadFile, window, filters, m_last_dir, false);
}

void IoManager::RequestExportAnsi(SDL_Window* window, SdlFileDialogQueue& dialogs)
{
    m_last_error.clear();

    std::vector<std::string_view> exts_v;
    AppendUnique(exts_v, formats::ansi::ExportExtensions());
    AppendUnique(exts_v, formats::plaintext::ExportExtensions());
    const std::string exts = JoinExtsForDialog(exts_v);

    std::vector<SdlFileDialogQueue::FilterPair> filters = {
        {"ANSI / Text (*.ans;*.txt;*.asc)", exts},
        {"All files", "*"},
    };
    fs::path base = m_last_dir.empty() ? fs::path(".") : fs::path(m_last_dir);
    fs::path suggested = base / "export.ans";
    dialogs.ShowSaveFileDialog(kDialog_ExportAnsi, window, filters, suggested.string());
}

void IoManager::RequestExportImage(SDL_Window* window, SdlFileDialogQueue& dialogs)
{
    m_last_error.clear();

    std::vector<std::string_view> exts_v;
    AppendUnique(exts_v, formats::image::ExportExtensions());
    const std::string exts = JoinExtsForDialog(exts_v);

    std::vector<SdlFileDialogQueue::FilterPair> filters = {
        {"Image (*.png;*.jpg;*.jpeg)", exts},
        {"All files", "*"},
    };
    fs::path base = m_last_dir.empty() ? fs::path(".") : fs::path(m_last_dir);
    fs::path suggested = base / "export.png";
    dialogs.ShowSaveFileDialog(kDialog_ExportImage, window, filters, suggested.string());
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

    if (!has_focus_canvas)
        ImGui::BeginDisabled();
    if (ImGui::MenuItem("Export Image..."))
    {
        RequestExportImage(window, dialogs);
    }
    if (!has_focus_canvas)
        ImGui::EndDisabled();

    (void)cb;
}

void IoManager::HandleDialogResult(const SdlFileDialogResult& r, AnsiCanvas* focused_canvas, const Callbacks& cb)
{
    // Ignore dialogs not owned by IoManager.
    if (r.tag != kDialog_SaveProject &&
        r.tag != kDialog_LoadFile &&
        r.tag != kDialog_ExportAnsi &&
        r.tag != kDialog_ExportImage)
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
        ext = ToLowerAscii(ext);

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
        const bool looks_plaintext = ExtIn(ext, formats::plaintext::ImportExtensions());
        const bool looks_ansi = ExtIn(ext, formats::ansi::ImportExtensions());
        const bool looks_image = ExtIn(ext, formats::image::ImportExtensions());

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

        auto try_import_plaintext = [&]() -> bool
        {
            if (!cb.create_canvas)
            {
                m_last_error = "Internal error: create_canvas callback not set.";
                return true; // handled (as error)
            }
            AnsiCanvas imported;
            std::string ierr;
            formats::plaintext::ImportOptions opt;
            // If the user picked .asc, default to ASCII; otherwise assume UTF-8.
            if (ext == "asc")
                opt.text_encoding = formats::plaintext::ImportOptions::TextEncoding::Ascii;
            if (formats::plaintext::ImportFileToCanvas(chosen, imported, ierr, opt))
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
        else if (looks_plaintext)
            handled = try_import_plaintext();
        else if (looks_ansi)
            handled = try_import_ansi();
        else if (looks_image)
            handled = try_load_image();
        else
        {
            // Unknown extension (or URI). Try in descending order of likelihood.
            handled = try_load_project() || try_import_ansi() || try_import_plaintext() || try_load_image();
        }

        if (!handled)
        {
            // Use the most recent decoder error if we have one.
            m_last_error = err.empty() ? "Unsupported file type or failed to load file." : err;
        }
    }
    else if (r.tag == kDialog_ExportAnsi)
    {
        if (!focused_canvas)
        {
            m_last_error = "No focused canvas to export.";
            return;
        }

        std::string path = chosen;
        if (!is_uri(path))
        {
            fs::path p(path);
            // Default to .ans if user omitted extension.
            if (p.extension().empty())
                path += ".ans";
        }

        // Default export preset for now (until we add an Export dialog UI).
        // If user explicitly chose .txt/.asc, emit plain UTF-8 text (no ANSI escape sequences).
        std::string ext;
        if (!is_uri(path))
        {
            try
            {
                fs::path p(path);
                ext = p.extension().string();
            }
            catch (...) {}
        }
        if (!ext.empty() && ext[0] == '.')
            ext.erase(ext.begin());
        ext = ToLowerAscii(ext);

        bool ok = false;
        if (ExtIn(ext, formats::plaintext::ExportExtensions()))
        {
            formats::plaintext::ExportOptions opt;
            if (const auto* preset = formats::plaintext::FindPreset(formats::plaintext::PresetId::PlainUtf8))
                opt = preset->export_;
            ok = formats::plaintext::ExportCanvasToFile(path, *focused_canvas, err, opt);
        }
        else
        {
            // Goal: reasonable terminal-friendly output with xterm256 colors.
            formats::ansi::ExportOptions opt;
            if (const auto* preset = formats::ansi::FindPreset(formats::ansi::PresetId::ModernUtf8_256))
                opt = preset->export_;
            ok = formats::ansi::ExportCanvasToFile(path, *focused_canvas, err, opt);
        }

        if (ok)
        {
            m_last_error.clear();
        }
        else
        {
            m_last_error = err.empty() ? "Export failed." : err;
        }
    }
    else if (r.tag == kDialog_ExportImage)
    {
        if (!focused_canvas)
        {
            m_last_error = "No focused canvas to export.";
            return;
        }

        std::string path = chosen;
        if (!is_uri(path))
        {
            fs::path p(path);
            // Default to .png if user omitted extension.
            if (p.extension().empty())
                path += ".png";
        }

        formats::image::ExportOptions opt;
        opt.scale = 2;
        opt.transparent_unset_bg = false;
        opt.png_bit_depth = 32;
        opt.png_compression = 6;
        opt.jpg_quality = 95;

        const bool ok = formats::image::ExportCanvasToFile(path, *focused_canvas, err, opt);
        if (ok)
            m_last_error.clear();
        else
            m_last_error = err.empty() ? "Export failed." : err;
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

