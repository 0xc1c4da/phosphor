#include "io/io_manager.h"

#include "io/file_dialog_tags.h"
#include "io/formats/ansi.h"
#include "io/formats/image.h"
#include "io/formats/plaintext.h"
#include "io/formats/xbin.h"
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

bool IoManager::TakeLastSaveEvent(SaveEvent& out)
{
    if (m_last_save_event.kind == SaveEventKind::None)
        return false;
    out = m_last_save_event;
    m_last_save_event = SaveEvent{};
    return true;
}

bool IoManager::TakeLastOpenEvent(OpenEvent& out)
{
    if (m_last_open_event.kind == OpenEventKind::None)
        return false;
    out = m_last_open_event;
    m_last_open_event = OpenEvent{};
    return true;
}

bool IoManager::SaveProjectToPath(const std::string& path, AnsiCanvas& canvas, std::string& err)
{
    if (project_file::SaveProjectToFile(path, canvas, err))
    {
        canvas.SetFilePath(path);
        canvas.MarkSaved();
        m_last_error.clear();

        SaveEvent ev;
        ev.kind = SaveEventKind::Success;
        ev.canvas = &canvas;
        ev.path = path;
        m_last_save_event = std::move(ev);
        return true;
    }
    return false;
}

void IoManager::SaveProject(SDL_Window* window, SdlFileDialogQueue& dialogs, AnsiCanvas* target_canvas)
{
    if (!target_canvas)
    {
        m_last_error = "No canvas to save.";
        return;
    }

    auto is_uri = [](const std::string& s) -> bool
    {
        return s.find("://") != std::string::npos;
    };

    // If we have a local path, save directly. Otherwise fall back to Save As.
    if (target_canvas->HasFilePath() && !is_uri(target_canvas->GetFilePath()))
    {
        std::string err;
        if (SaveProjectToPath(target_canvas->GetFilePath(), *target_canvas, err))
            return;
        m_last_error = err.empty() ? "Save failed." : err;

        SaveEvent ev;
        ev.kind = SaveEventKind::Failed;
        ev.canvas = target_canvas;
        ev.error = m_last_error;
        m_last_save_event = std::move(ev);
        return;
    }

    SaveProjectAs(window, dialogs, target_canvas);
}

void IoManager::SaveProjectAs(SDL_Window* window, SdlFileDialogQueue& dialogs, AnsiCanvas* target_canvas)
{
    RequestSaveProject(window, dialogs, target_canvas);
}

void IoManager::RequestSaveProject(SDL_Window* window, SdlFileDialogQueue& dialogs, AnsiCanvas* target_canvas)
{
    m_last_error.clear();
    m_pending_save_canvas = target_canvas;
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

    std::vector<std::string_view> xbin_exts_v;
    AppendUnique(xbin_exts_v, formats::xbin::ImportExtensions());
    const std::string xbin_exts = JoinExtsForDialog(xbin_exts_v);

    const std::string image_exts = JoinExtsForDialog(std::vector<std::string_view>(formats::image::ImportExtensions().begin(),
                                                                                   formats::image::ImportExtensions().end()));

    std::vector<std::string_view> supported_exts_v;
    supported_exts_v.push_back("phos");
    AppendUnique(supported_exts_v, text_exts_v);
    AppendUnique(supported_exts_v, xbin_exts_v);
    // Keep the same image list for "Supported files".
    AppendUnique(supported_exts_v, formats::image::ImportExtensions());
    const std::string supported_exts = JoinExtsForDialog(supported_exts_v);

    std::vector<SdlFileDialogQueue::FilterPair> filters = {
        {"Supported files (*.phos;*.ans;*.asc;*.txt;*.nfo;*.diz;*.xb;*.png;*.jpg;*.jpeg;*.gif;*.bmp)", supported_exts},
        {"Phosphor Project (*.phos)", "phos"},
        {"ANSI / Text (*.ans;*.asc;*.txt;*.nfo;*.diz)", text_exts},
        {"XBin (*.xb)", xbin_exts},
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
    AppendUnique(exts_v, formats::xbin::ExportExtensions());
    const std::string exts = JoinExtsForDialog(exts_v);

    std::vector<SdlFileDialogQueue::FilterPair> filters = {
        {"Export (*.ans;*.txt;*.asc;*.xb)", exts},
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

void IoManager::RenderFileMenu(SDL_Window* window,
                               SdlFileDialogQueue& dialogs,
                               AnsiCanvas* focused_canvas,
                               const Callbacks& cb,
                               const ShortcutProvider& shortcut_for_action)
{
    const bool has_focus_canvas = (focused_canvas != nullptr);

    // Save requires a focused canvas (for now).
    if (!has_focus_canvas)
        ImGui::BeginDisabled();
    const std::string sc_save = shortcut_for_action ? shortcut_for_action("app.file.save") : std::string{};
    if (ImGui::MenuItem("Save", sc_save.empty() ? nullptr : sc_save.c_str()))
        SaveProject(window, dialogs, focused_canvas);
    if (!has_focus_canvas)
        ImGui::EndDisabled();

    if (has_focus_canvas)
    {
        const std::string sc_save_as = shortcut_for_action ? shortcut_for_action("app.file.save_as") : std::string{};
        if (ImGui::MenuItem("Save As...", sc_save_as.empty() ? nullptr : sc_save_as.c_str()))
            SaveProjectAs(window, dialogs, focused_canvas);
    }

    const std::string sc_load = shortcut_for_action ? shortcut_for_action("app.file.open") : std::string{};
    if (ImGui::MenuItem("Load...", sc_load.empty() ? nullptr : sc_load.c_str()))
    {
        RequestLoadFile(window, dialogs);
    }

    (void)cb;
}

bool IoManager::OpenPath(const std::string& path, const Callbacks& cb)
{
    m_last_error.clear();

    auto is_uri = [](const std::string& s) -> bool
    {
        return s.find("://") != std::string::npos;
    };

    // Sync last dir for future dialogs.
    if (!is_uri(path))
    {
        try
        {
            fs::path p(path);
            if (p.has_parent_path())
                m_last_dir = p.parent_path().string();
        }
        catch (...) {}
    }

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

    std::string err;

    auto is_ext = [&](const std::initializer_list<const char*>& exts) -> bool
    {
        for (const char* e : exts)
        {
            if (ext == e)
                return true;
        }
        return false;
    };

    const bool looks_project = is_ext({"phos"});
    const bool looks_plaintext = ExtIn(ext, formats::plaintext::ImportExtensions());
    const bool looks_ansi = ExtIn(ext, formats::ansi::ImportExtensions());
    const bool looks_image = ExtIn(ext, formats::image::ImportExtensions());
    const bool looks_xbin = ExtIn(ext, formats::xbin::ImportExtensions());

    auto try_load_project = [&]() -> bool
    {
        if (!cb.create_canvas)
        {
            m_last_error = "Internal error: create_canvas callback not set.";
            return true;
        }
        AnsiCanvas loaded;
        if (project_file::LoadProjectFromFile(path, loaded, err))
        {
            loaded.SetFilePath(path);
            loaded.MarkSaved();
            cb.create_canvas(std::move(loaded));
            m_last_error.clear();
            m_last_open_event = OpenEvent{OpenEventKind::Canvas, path, {}};
            return true;
        }
        return false;
    };

    auto try_import_ansi = [&]() -> bool
    {
        if (!cb.create_canvas)
        {
            m_last_error = "Internal error: create_canvas callback not set.";
            return true;
        }
        AnsiCanvas imported;
        std::string ierr;
        if (formats::ansi::ImportFileToCanvas(path, imported, ierr))
        {
            imported.SetFilePath(path);
            imported.MarkSaved();
            cb.create_canvas(std::move(imported));
            m_last_error.clear();
            m_last_open_event = OpenEvent{OpenEventKind::Canvas, path, {}};
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
            return true;
        }
        AnsiCanvas imported;
        std::string ierr;
        formats::plaintext::ImportOptions opt;
        if (ext == "asc")
            opt.text_encoding = formats::plaintext::ImportOptions::TextEncoding::Ascii;
        if (formats::plaintext::ImportFileToCanvas(path, imported, ierr, opt))
        {
            imported.SetFilePath(path);
            imported.MarkSaved();
            cb.create_canvas(std::move(imported));
            m_last_error.clear();
            m_last_open_event = OpenEvent{OpenEventKind::Canvas, path, {}};
            return true;
        }
        err = ierr;
        return false;
    };

    auto try_import_xbin = [&]() -> bool
    {
        if (!cb.create_canvas)
        {
            m_last_error = "Internal error: create_canvas callback not set.";
            return true;
        }
        AnsiCanvas imported;
        std::string ierr;
        if (formats::xbin::ImportFileToCanvas(path, imported, ierr))
        {
            imported.SetFilePath(path);
            imported.MarkSaved();
            cb.create_canvas(std::move(imported));
            m_last_error.clear();
            m_last_open_event = OpenEvent{OpenEventKind::Canvas, path, {}};
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
            return true;
        }
        int iw = 0, ih = 0;
        std::vector<unsigned char> rgba;
        std::string ierr;
        if (image_loader::LoadImageAsRgba32(path, iw, ih, rgba, ierr))
        {
            Callbacks::LoadedImage li;
            li.path = path;
            li.width = iw;
            li.height = ih;
            li.pixels = std::move(rgba);
            cb.create_image(std::move(li));
            m_last_error.clear();
            m_last_open_event = OpenEvent{OpenEventKind::Image, path, {}};
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
    else if (looks_xbin)
        handled = try_import_xbin();
    else if (looks_image)
        handled = try_load_image();
    else
        handled = try_load_project() || try_import_ansi() || try_import_xbin() || try_import_plaintext() || try_load_image();

    if (!handled)
    {
        m_last_error = err.empty() ? "Unsupported file type or failed to load file." : err;
        m_last_open_event = OpenEvent{OpenEventKind::None, path, m_last_error};
        return true;
    }
    return true;
}

void IoManager::HandleDialogResult(const SdlFileDialogResult& r, AnsiCanvas* focused_canvas, const Callbacks& cb)
{
    // Ignore dialogs not owned by IoManager.
    if (r.tag != kDialog_SaveProject &&
        r.tag != kDialog_LoadFile &&
        r.tag != kDialog_ExportAnsi &&
        r.tag != kDialog_ExportImage)
        return;

    // If the user canceled (or the dialog errored), clear any pending Save target and emit an event.
    if (r.tag == kDialog_SaveProject && (r.canceled || !r.error.empty()))
    {
        SaveEvent ev;
        ev.canvas = m_pending_save_canvas ? m_pending_save_canvas : focused_canvas;
        if (!r.error.empty())
        {
            ev.kind = SaveEventKind::Failed;
            ev.error = r.error;
        }
        else
        {
            ev.kind = SaveEventKind::Canceled;
        }
        m_last_save_event = std::move(ev);
        m_pending_save_canvas = nullptr;
    }

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
        AnsiCanvas* target = m_pending_save_canvas ? m_pending_save_canvas : focused_canvas;
        m_pending_save_canvas = nullptr;

        if (!target)
        {
            m_last_error = "No focused canvas to save.";
            SaveEvent ev;
            ev.kind = SaveEventKind::Failed;
            ev.canvas = nullptr;
            ev.error = m_last_error;
            m_last_save_event = std::move(ev);
            return;
        }

        std::string path = chosen;
        if (!is_uri(path))
        {
            fs::path p(path);
            if (p.extension().empty())
                path += ".phos";
        }

        if (project_file::SaveProjectToFile(path, *target, err))
        {
            // Treat successful save as establishing the document's canonical file path.
            target->SetFilePath(path);
            target->MarkSaved();
            m_last_error.clear();

            SaveEvent ev;
            ev.kind = SaveEventKind::Success;
            ev.canvas = target;
            ev.path = path;
            m_last_save_event = std::move(ev);
        }
        else
        {
            m_last_error = err.empty() ? "Save failed." : err;

            SaveEvent ev;
            ev.kind = SaveEventKind::Failed;
            ev.canvas = target;
            ev.error = m_last_error;
            m_last_save_event = std::move(ev);
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
        const bool looks_xbin = ExtIn(ext, formats::xbin::ImportExtensions());

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
                loaded.MarkSaved();
                cb.create_canvas(std::move(loaded));
                m_last_error.clear();
                m_last_open_event = OpenEvent{OpenEventKind::Canvas, chosen, {}};
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
                imported.MarkSaved();
                cb.create_canvas(std::move(imported));
                m_last_error.clear();
                m_last_open_event = OpenEvent{OpenEventKind::Canvas, chosen, {}};
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
                imported.MarkSaved();
                cb.create_canvas(std::move(imported));
                m_last_error.clear();
                m_last_open_event = OpenEvent{OpenEventKind::Canvas, chosen, {}};
                return true;
            }
            err = ierr;
            return false;
        };

        auto try_import_xbin = [&]() -> bool
        {
            if (!cb.create_canvas)
            {
                m_last_error = "Internal error: create_canvas callback not set.";
                return true; // handled (as error)
            }
            AnsiCanvas imported;
            std::string ierr;
            if (formats::xbin::ImportFileToCanvas(chosen, imported, ierr))
            {
                imported.SetFilePath(chosen);
                imported.MarkSaved();
                cb.create_canvas(std::move(imported));
                m_last_error.clear();
                m_last_open_event = OpenEvent{OpenEventKind::Canvas, chosen, {}};
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
                m_last_open_event = OpenEvent{OpenEventKind::Image, chosen, {}};
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
        else if (looks_xbin)
            handled = try_import_xbin();
        else if (looks_image)
            handled = try_load_image();
        else
        {
            // Unknown extension (or URI). Try in descending order of likelihood.
            handled = try_load_project() || try_import_ansi() || try_import_xbin() || try_import_plaintext() || try_load_image();
        }

        if (!handled)
        {
            // Use the most recent decoder error if we have one.
            m_last_error = err.empty() ? "Unsupported file type or failed to load file." : err;
            m_last_open_event = OpenEvent{OpenEventKind::None, chosen, m_last_error};
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
        else if (ExtIn(ext, formats::xbin::ExportExtensions()))
        {
            formats::xbin::ExportOptions opt;
            opt.source = formats::xbin::ExportOptions::Source::Composite;
            opt.include_palette = true;
            opt.compress = true;
            opt.nonblink = true;
            opt.write_sauce = false;
            ok = formats::xbin::ExportCanvasToFile(path, *focused_canvas, err, opt);
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
        opt.png_format = formats::image::ExportOptions::PngFormat::Indexed8;
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

