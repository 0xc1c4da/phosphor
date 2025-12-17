#include "io_manager.h"

#include "ansi_importer.h"
#include "file_dialog_tags.h"
#include "sdl_file_dialog_queue.h"

#include "imgui.h"

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

static json ProjectLayerToJson(const AnsiCanvas::ProjectLayer& l)
{
    json jl;
    jl["name"] = l.name;
    jl["visible"] = l.visible;

    // Store glyphs as uint32 codepoints to keep CBOR compact and unambiguous.
    json cells = json::array();
    for (char32_t cp : l.cells)
        cells.push_back(static_cast<std::uint32_t>(cp));
    jl["cells"] = std::move(cells);

    jl["fg"] = l.fg;
    jl["bg"] = l.bg;
    return jl;
}

static bool ProjectLayerFromJson(const json& jl, AnsiCanvas::ProjectLayer& out, std::string& err)
{
    err.clear();
    if (!jl.is_object())
    {
        err = "Layer is not an object.";
        return false;
    }

    out = AnsiCanvas::ProjectLayer{};
    if (jl.contains("name") && jl["name"].is_string())
        out.name = jl["name"].get<std::string>();
    if (jl.contains("visible") && jl["visible"].is_boolean())
        out.visible = jl["visible"].get<bool>();

    if (!jl.contains("cells") || !jl["cells"].is_array())
    {
        err = "Layer missing 'cells' array.";
        return false;
    }

    const json& cells = jl["cells"];
    out.cells.clear();
    out.cells.reserve(cells.size());
    for (const auto& v : cells)
    {
        if (!v.is_number_unsigned() && !v.is_number_integer())
        {
            err = "Layer 'cells' contains a non-integer value.";
            return false;
        }
        std::uint32_t cp = 0;
        if (v.is_number_unsigned())
            cp = v.get<std::uint32_t>();
        else
        {
            const std::int64_t si = v.get<std::int64_t>();
            if (si < 0)
            {
                err = "Layer 'cells' contains a negative codepoint.";
                return false;
            }
            cp = static_cast<std::uint32_t>(si);
        }
        out.cells.push_back(static_cast<char32_t>(cp));
    }

    out.fg.clear();
    out.bg.clear();
    if (jl.contains("fg") && jl["fg"].is_array())
        out.fg = jl["fg"].get<std::vector<AnsiCanvas::Color32>>();
    if (jl.contains("bg") && jl["bg"].is_array())
        out.bg = jl["bg"].get<std::vector<AnsiCanvas::Color32>>();

    // If missing, AnsiCanvas::SetProjectState will default these to all-zero.
    return true;
}

static json ProjectSnapshotToJson(const AnsiCanvas::ProjectSnapshot& s)
{
    json js;
    js["columns"] = s.columns;
    js["rows"] = s.rows;
    js["active_layer"] = s.active_layer;
    js["caret_row"] = s.caret_row;
    js["caret_col"] = s.caret_col;
    json layers = json::array();
    for (const auto& l : s.layers)
        layers.push_back(ProjectLayerToJson(l));
    js["layers"] = std::move(layers);
    return js;
}

static bool ProjectSnapshotFromJson(const json& js, AnsiCanvas::ProjectSnapshot& out, std::string& err)
{
    err.clear();
    if (!js.is_object())
    {
        err = "Snapshot is not an object.";
        return false;
    }

    out = AnsiCanvas::ProjectSnapshot{};
    if (js.contains("columns") && js["columns"].is_number_integer())
        out.columns = js["columns"].get<int>();
    if (js.contains("rows") && js["rows"].is_number_integer())
        out.rows = js["rows"].get<int>();
    if (js.contains("active_layer") && js["active_layer"].is_number_integer())
        out.active_layer = js["active_layer"].get<int>();
    if (js.contains("caret_row") && js["caret_row"].is_number_integer())
        out.caret_row = js["caret_row"].get<int>();
    if (js.contains("caret_col") && js["caret_col"].is_number_integer())
        out.caret_col = js["caret_col"].get<int>();

    if (!js.contains("layers") || !js["layers"].is_array())
    {
        err = "Snapshot missing 'layers' array.";
        return false;
    }

    out.layers.clear();
    for (const auto& jl : js["layers"])
    {
        AnsiCanvas::ProjectLayer pl;
        if (!ProjectLayerFromJson(jl, pl, err))
            return false;
        out.layers.push_back(std::move(pl));
    }
    return true;
}

static json ProjectStateToJson(const AnsiCanvas::ProjectState& st)
{
    json j;
    j["magic"] = "utf8-art-editor";
    j["version"] = st.version;
    j["undo_limit"] = st.undo_limit;
    j["current"] = ProjectSnapshotToJson(st.current);

    json undo = json::array();
    for (const auto& s : st.undo)
        undo.push_back(ProjectSnapshotToJson(s));
    j["undo"] = std::move(undo);

    json redo = json::array();
    for (const auto& s : st.redo)
        redo.push_back(ProjectSnapshotToJson(s));
    j["redo"] = std::move(redo);
    return j;
}

static bool ProjectStateFromJson(const json& j, AnsiCanvas::ProjectState& out, std::string& err)
{
    err.clear();
    if (!j.is_object())
    {
        err = "Project file root is not an object.";
        return false;
    }

    if (j.contains("magic") && j["magic"].is_string())
    {
        const std::string magic = j["magic"].get<std::string>();
        if (magic != "utf8-art-editor")
        {
            err = "Not a utf8-art-editor project file.";
            return false;
        }
    }

    out = AnsiCanvas::ProjectState{};
    if (j.contains("version") && j["version"].is_number_integer())
        out.version = j["version"].get<int>();
    if (j.contains("undo_limit") && j["undo_limit"].is_number_unsigned())
        out.undo_limit = j["undo_limit"].get<size_t>();
    else if (j.contains("undo_limit") && j["undo_limit"].is_number_integer())
    {
        const int v = j["undo_limit"].get<int>();
        out.undo_limit = (v > 0) ? static_cast<size_t>(v) : 256;
    }

    if (!j.contains("current"))
    {
        err = "Project missing 'current' snapshot.";
        return false;
    }
    if (!ProjectSnapshotFromJson(j["current"], out.current, err))
        return false;

    out.undo.clear();
    if (j.contains("undo") && j["undo"].is_array())
    {
        for (const auto& s : j["undo"])
        {
            AnsiCanvas::ProjectSnapshot snap;
            if (!ProjectSnapshotFromJson(s, snap, err))
                return false;
            out.undo.push_back(std::move(snap));
        }
    }

    out.redo.clear();
    if (j.contains("redo") && j["redo"].is_array())
    {
        for (const auto& s : j["redo"])
        {
            AnsiCanvas::ProjectSnapshot snap;
            if (!ProjectSnapshotFromJson(s, snap, err))
                return false;
            out.redo.push_back(std::move(snap));
        }
    }

    return true;
}

static bool SaveProjectToFile(const std::string& path, const AnsiCanvas& canvas, std::string& err)
{
    err.clear();
    const auto st = canvas.GetProjectState();
    const json j = ProjectStateToJson(st);

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
    if (!ProjectStateFromJson(j, st, err))
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

void IoManager::RenderFileMenu(SDL_Window* window, SdlFileDialogQueue& dialogs, AnsiCanvas* focused_canvas, const Callbacks& cb)
{
    const bool has_focus_canvas = (focused_canvas != nullptr);

    // Save requires a focused canvas (for now).
    if (!has_focus_canvas)
        ImGui::BeginDisabled();
    if (ImGui::MenuItem("Save..."))
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
    if (!has_focus_canvas)
        ImGui::EndDisabled();

    if (ImGui::MenuItem("Load..."))
    {
        m_last_error.clear();
        std::vector<SdlFileDialogQueue::FilterPair> filters = {
            {"Phosphor Project (*.phos)", "phos"},
            {"All files", "*"},
        };
        dialogs.ShowOpenFileDialog(kDialog_LoadProject, window, filters, m_last_dir, false);
    }

    if (ImGui::MenuItem("Import..."))
    {
        m_last_error.clear();
        std::vector<SdlFileDialogQueue::FilterPair> filters = {
            {"ANSI / Text (*.ans;*.asc;*.txt)", "ans;asc;txt"},
            {"All files", "*"},
        };
        dialogs.ShowOpenFileDialog(kDialog_ImportAnsi, window, filters, m_last_dir, false);
    }

    if (ImGui::MenuItem("Export..."))
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

    (void)cb;
}

void IoManager::HandleDialogResult(const SdlFileDialogResult& r, AnsiCanvas* focused_canvas, const Callbacks& cb)
{
    // Ignore dialogs not owned by IoManager.
    if (r.tag != kDialog_SaveProject &&
        r.tag != kDialog_LoadProject &&
        r.tag != kDialog_ImportAnsi &&
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
    else if (r.tag == kDialog_LoadProject)
    {
        if (!cb.create_canvas)
        {
            m_last_error = "Internal error: create_canvas callback not set.";
            return;
        }

        AnsiCanvas loaded;
        if (LoadProjectFromFile(chosen, loaded, err))
        {
            cb.create_canvas(std::move(loaded));
            m_last_error.clear();
        }
        else
        {
            m_last_error = err.empty() ? "Load failed." : err;
        }
    }
    else if (r.tag == kDialog_ImportAnsi)
    {
        if (!cb.create_canvas)
        {
            m_last_error = "Internal error: create_canvas callback not set.";
            return;
        }

        AnsiCanvas imported;
        std::string ierr;
        if (!ansi_importer::ImportAnsiFileToCanvas(chosen, imported, ierr))
        {
            m_last_error = ierr.empty() ? "Failed to import ANSI file." : ierr;
        }
        else
        {
            cb.create_canvas(std::move(imported));
            m_last_error.clear();
        }
    }
    else if (r.tag == kDialog_ExportAnsi)
    {
        // Stub: UI contract only (we'll implement proper export later).
        (void)chosen;
        m_last_error = "Export is not implemented yet.";
    }
}

void IoManager::RenderStatusWindows()
{
    if (!m_last_error.empty())
    {
        ImGui::Begin("File Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_last_error.c_str());
        if (ImGui::Button("Dismiss"))
            m_last_error.clear();
        ImGui::End();
    }
}

