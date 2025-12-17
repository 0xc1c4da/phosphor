#include "project_state_json.h"

#include <algorithm>
#include <cstdint>

namespace project_state_json
{
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

json ToJson(const AnsiCanvas::ProjectState& st)
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

bool FromJson(const json& j, AnsiCanvas::ProjectState& out, std::string& err)
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
} // namespace project_state_json


