#include "io/session/project_state_json.h"

#include <algorithm>
#include <cstdint>

namespace project_state_json
{
static json SauceMetaToJson(const AnsiCanvas::ProjectState::SauceMeta& s)
{
    json js;
    js["present"] = s.present;
    js["title"] = s.title;
    js["author"] = s.author;
    js["group"] = s.group;
    js["date"] = s.date;
    js["file_size"] = s.file_size;
    js["data_type"] = s.data_type;
    js["file_type"] = s.file_type;
    js["tinfo1"] = s.tinfo1;
    js["tinfo2"] = s.tinfo2;
    js["tinfo3"] = s.tinfo3;
    js["tinfo4"] = s.tinfo4;
    js["tflags"] = s.tflags;
    js["tinfos"] = s.tinfos;
    js["comments"] = s.comments;
    return js;
}

static void SauceMetaFromJson(const json& js, AnsiCanvas::ProjectState::SauceMeta& out)
{
    out = AnsiCanvas::ProjectState::SauceMeta{};
    if (!js.is_object())
        return;
    if (js.contains("present") && js["present"].is_boolean())
        out.present = js["present"].get<bool>();
    if (js.contains("title") && js["title"].is_string())
        out.title = js["title"].get<std::string>();
    if (js.contains("author") && js["author"].is_string())
        out.author = js["author"].get<std::string>();
    if (js.contains("group") && js["group"].is_string())
        out.group = js["group"].get<std::string>();
    if (js.contains("date") && js["date"].is_string())
        out.date = js["date"].get<std::string>();
    if (js.contains("file_size") && (js["file_size"].is_number_unsigned() || js["file_size"].is_number_integer()))
        out.file_size = js["file_size"].get<std::uint32_t>();
    if (js.contains("data_type") && (js["data_type"].is_number_unsigned() || js["data_type"].is_number_integer()))
        out.data_type = js["data_type"].get<std::uint8_t>();
    if (js.contains("file_type") && (js["file_type"].is_number_unsigned() || js["file_type"].is_number_integer()))
        out.file_type = js["file_type"].get<std::uint8_t>();
    if (js.contains("tinfo1") && (js["tinfo1"].is_number_unsigned() || js["tinfo1"].is_number_integer()))
        out.tinfo1 = js["tinfo1"].get<std::uint16_t>();
    if (js.contains("tinfo2") && (js["tinfo2"].is_number_unsigned() || js["tinfo2"].is_number_integer()))
        out.tinfo2 = js["tinfo2"].get<std::uint16_t>();
    if (js.contains("tinfo3") && (js["tinfo3"].is_number_unsigned() || js["tinfo3"].is_number_integer()))
        out.tinfo3 = js["tinfo3"].get<std::uint16_t>();
    if (js.contains("tinfo4") && (js["tinfo4"].is_number_unsigned() || js["tinfo4"].is_number_integer()))
        out.tinfo4 = js["tinfo4"].get<std::uint16_t>();
    if (js.contains("tflags") && (js["tflags"].is_number_unsigned() || js["tflags"].is_number_integer()))
        out.tflags = js["tflags"].get<std::uint8_t>();
    if (js.contains("tinfos") && js["tinfos"].is_string())
        out.tinfos = js["tinfos"].get<std::string>();
    if (js.contains("comments") && js["comments"].is_array())
        out.comments = js["comments"].get<std::vector<std::string>>();
}

static json ProjectLayerToJson(const AnsiCanvas::ProjectLayer& l)
{
    json jl;
    jl["name"] = l.name;
    jl["visible"] = l.visible;
    jl["lock_transparency"] = l.lock_transparency;

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
    if (jl.contains("lock_transparency") && jl["lock_transparency"].is_boolean())
        out.lock_transparency = jl["lock_transparency"].get<bool>();

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
    if (!st.colour_palette_title.empty())
        j["colour_palette_title"] = st.colour_palette_title;
    j["sauce"] = SauceMetaToJson(st.sauce);
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
        // 0 (or negative) = unlimited.
        out.undo_limit = (v > 0) ? static_cast<size_t>(v) : 0;
    }

    // Optional SAUCE metadata (safe default if absent).
    if (j.contains("sauce"))
        SauceMetaFromJson(j["sauce"], out.sauce);

    // Optional UI colour palette identity.
    if (j.contains("colour_palette_title") && j["colour_palette_title"].is_string())
        out.colour_palette_title = j["colour_palette_title"].get<std::string>();

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


