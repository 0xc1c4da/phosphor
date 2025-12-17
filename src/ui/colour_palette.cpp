#include "ui/colour_palette.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>

static bool HexToImVec4(const std::string& hex, ImVec4& out)
{
    std::string s = hex;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8)
        return false;

    auto to_u8 = [](const std::string& sub) -> int
    {
        return static_cast<int>(std::strtoul(sub.c_str(), nullptr, 16));
    };

    int r = to_u8(s.substr(0, 2));
    int g = to_u8(s.substr(2, 2));
    int b = to_u8(s.substr(4, 2));
    int a = 255;
    if (s.size() == 8)
        a = to_u8(s.substr(6, 2));

    out.x = r / 255.0f;
    out.y = g / 255.0f;
    out.z = b / 255.0f;
    out.w = a / 255.0f;
    return true;
}

bool LoadColourPalettesFromJson(const char* path,
                                std::vector<ColourPaletteDef>& out,
                                std::string& error)
{
    using nlohmann::json;

    std::ifstream f(path);
    if (!f)
    {
        error = std::string("Failed to open ") + path;
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    if (!j.is_array())
    {
        error = "Expected top-level JSON array in colours.json";
        return false;
    }

    out.clear();
    for (const auto& item : j)
    {
        if (!item.is_object())
            continue;

        ColourPaletteDef def;
        if (auto it = item.find("title"); it != item.end() && it->is_string())
            def.title = it->get<std::string>();
        else
            continue;

        if (auto it = item.find("colors"); it != item.end() && it->is_array())
        {
            for (const auto& c : *it)
            {
                if (!c.is_string())
                    continue;
                ImVec4 col;
                if (HexToImVec4(c.get<std::string>(), col))
                    def.colors.push_back(col);
            }
        }

        if (!def.colors.empty())
            out.push_back(std::move(def));
    }

    if (out.empty())
    {
        error = "No valid palettes found in colours.json";
        return false;
    }

    error.clear();
    return true;
}


