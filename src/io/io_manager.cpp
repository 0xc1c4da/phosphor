#include "io/io_manager.h"

#include "io/ansi_importer.h"
#include "io/file_dialog_tags.h"
#include "io/image_loader.h"
#include "io/sdl_file_dialog_queue.h"
#include "io/session/project_state_json.h"

#include "imgui.h"
#include "io/session/imgui_persistence.h"

#include <nlohmann/json.hpp>
#include <zstd.h>

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
using json = nlohmann::json;

static constexpr unsigned char kPhosZstdMagic[4] = { 'U', '8', 'P', 'Z' };
static constexpr std::uint32_t kPhosZstdVersion = 1;

static void AppendU32LE(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back((std::uint8_t)((v >> 0) & 0xFF));
    out.push_back((std::uint8_t)((v >> 8) & 0xFF));
    out.push_back((std::uint8_t)((v >> 16) & 0xFF));
    out.push_back((std::uint8_t)((v >> 24) & 0xFF));
}

static void AppendU64LE(std::vector<std::uint8_t>& out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back((std::uint8_t)((v >> (8 * i)) & 0xFF));
}

static bool ReadU32LE(const std::vector<std::uint8_t>& in, size_t off, std::uint32_t& out)
{
    if (off + 4 > in.size()) return false;
    out = (std::uint32_t)in[off + 0]
        | ((std::uint32_t)in[off + 1] << 8)
        | ((std::uint32_t)in[off + 2] << 16)
        | ((std::uint32_t)in[off + 3] << 24);
    return true;
}

static bool ReadU64LE(const std::vector<std::uint8_t>& in, size_t off, std::uint64_t& out)
{
    if (off + 8 > in.size()) return false;
    out = 0;
    for (int i = 0; i < 8; ++i)
        out |= ((std::uint64_t)in[off + (size_t)i]) << (8 * i);
    return true;
}

static bool HasPhosZstdHeader(const std::vector<std::uint8_t>& bytes)
{
    return bytes.size() >= 4 &&
           bytes[0] == kPhosZstdMagic[0] &&
           bytes[1] == kPhosZstdMagic[1] &&
           bytes[2] == kPhosZstdMagic[2] &&
           bytes[3] == kPhosZstdMagic[3];
}

static bool ZstdCompress(const std::vector<std::uint8_t>& in,
                         std::vector<std::uint8_t>& out,
                         std::string& err)
{
    err.clear();
    out.clear();

    const size_t bound = ZSTD_compressBound(in.size());
    out.resize(bound);

    const int level = 3; // fast default; tweak later if needed
    const size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), level);
    if (ZSTD_isError(n))
    {
        err = std::string("zstd compress failed: ") + ZSTD_getErrorName(n);
        out.clear();
        return false;
    }
    out.resize(n);
    return true;
}

static bool ZstdDecompressKnownSize(const std::vector<std::uint8_t>& in,
                                    std::uint64_t uncompressed_size,
                                    std::vector<std::uint8_t>& out,
                                    std::string& err)
{
    err.clear();
    out.clear();

    if (uncompressed_size > (std::uint64_t)std::numeric_limits<size_t>::max())
    {
        err = "zstd decompress failed: uncompressed size too large for this platform.";
        return false;
    }
    out.resize((size_t)uncompressed_size);

    const size_t n = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
    if (ZSTD_isError(n))
    {
        err = std::string("zstd decompress failed: ") + ZSTD_getErrorName(n);
        out.clear();
        return false;
    }
    if (n != out.size())
    {
        err = "zstd decompress failed: size mismatch.";
        out.clear();
        return false;
    }
    return true;
}

static std::vector<std::uint8_t> ReadAllBytes(const std::string& path, std::string& err)
{
    err.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        err = "Failed to open file for reading.";
        return {};
    }
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    if (sz < 0)
    {
        err = "Failed to read file size.";
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<size_t>(sz));
    if (sz > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), sz);
    if (!in && sz > 0)
    {
        err = "Failed to read file contents.";
        return {};
    }
    return bytes;
}

static bool WriteAllBytes(const std::string& path, const std::vector<std::uint8_t>& bytes, std::string& err)
{
    err.clear();
    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        err = "Failed to open file for writing.";
        return false;
    }
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    if (!out)
    {
        err = "Failed to write file contents.";
        return false;
    }
    return true;
}

static bool SaveProjectToFile(const std::string& path, const AnsiCanvas& canvas, std::string& err)
{
    err.clear();
    const auto st = canvas.GetProjectState();
    const json j = project_state_json::ToJson(st);

    std::vector<std::uint8_t> bytes;
    try
    {
        bytes = json::to_cbor(j);
    }
    catch (const std::exception& e)
    {
        err = std::string("CBOR encode failed: ") + e.what();
        return false;
    }

    // Compress CBOR payload with zstd.
    std::vector<std::uint8_t> compressed;
    std::string zerr;
    if (!ZstdCompress(bytes, compressed, zerr))
    {
        err = zerr;
        return false;
    }

    // File format:
    //   4 bytes  magic: "U8PZ"
    //   4 bytes  version (LE): 1
    //   8 bytes  uncompressed size (LE): CBOR byte length
    //   ...      zstd-compressed CBOR
    std::vector<std::uint8_t> out;
    out.reserve(4 + 4 + 8 + compressed.size());
    out.insert(out.end(), std::begin(kPhosZstdMagic), std::end(kPhosZstdMagic));
    AppendU32LE(out, kPhosZstdVersion);
    AppendU64LE(out, (std::uint64_t)bytes.size());
    out.insert(out.end(), compressed.begin(), compressed.end());

    return WriteAllBytes(path, out, err);
}

static bool LoadProjectFromFile(const std::string& path, AnsiCanvas& out_canvas, std::string& err)
{
    err.clear();
    std::string read_err;
    const auto bytes = ReadAllBytes(path, read_err);
    if (!read_err.empty())
    {
        err = read_err;
        return false;
    }

    json j;
    // New format: zstd-wrapped CBOR with U8PZ header.
    if (HasPhosZstdHeader(bytes))
    {
        std::uint32_t ver = 0;
        std::uint64_t ulen = 0;
        if (!ReadU32LE(bytes, 4, ver) || !ReadU64LE(bytes, 8, ulen))
        {
            err = "Invalid project header.";
            return false;
        }
        if (ver != kPhosZstdVersion)
        {
            err = "Unsupported project version.";
            return false;
        }
        if (bytes.size() < 16)
        {
            err = "Invalid project header (truncated).";
            return false;
        }
        const std::vector<std::uint8_t> comp(bytes.begin() + 16, bytes.end());
        std::vector<std::uint8_t> cbor;
        std::string zerr;
        if (!ZstdDecompressKnownSize(comp, ulen, cbor, zerr))
        {
            err = zerr;
            return false;
        }
        try
        {
            j = json::from_cbor(cbor);
        }
        catch (const std::exception& e)
        {
            err = std::string("CBOR decode failed: ") + e.what();
            return false;
        }
    }
    else
    {
        // Backward compatibility: older uncompressed CBOR files.
        try
        {
            j = json::from_cbor(bytes);
        }
        catch (const std::exception& e)
        {
            err = std::string("CBOR decode failed: ") + e.what();
            return false;
        }
    }

    AnsiCanvas::ProjectState st;
    if (!project_state_json::FromJson(j, st, err))
        return false;

    std::string apply_err;
    if (!out_canvas.SetProjectState(st, apply_err))
    {
        err = apply_err.empty() ? "Failed to apply project state." : apply_err;
        return false;
    }
    return true;
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
    std::vector<SdlFileDialogQueue::FilterPair> filters = {
        {"Supported files (*.phos;*.ans;*.asc;*.txt;*.png;*.jpg;*.jpeg;*.gif;*.bmp)", "phos;ans;asc;txt;png;jpg;jpeg;gif;bmp"},
        {"Phosphor Project (*.phos)", "phos"},
        {"ANSI / Text (*.ans;*.asc;*.txt)", "ans;asc;txt"},
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

        if (SaveProjectToFile(path, *focused_canvas, err))
        {
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
        const bool looks_ansi = is_ext({"ans", "asc", "txt"});
        const bool looks_image = is_ext({"png", "jpg", "jpeg", "gif", "bmp"});

        auto try_load_project = [&]() -> bool
        {
            if (!cb.create_canvas)
            {
                m_last_error = "Internal error: create_canvas callback not set.";
                return true; // handled (as error)
            }
            AnsiCanvas loaded;
            if (LoadProjectFromFile(chosen, loaded, err))
            {
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
            if (ansi_importer::ImportAnsiFileToCanvas(chosen, imported, ierr))
            {
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

