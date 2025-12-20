#include "fonts/textmode_font_registry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace textmode_font
{
static bool ReadFileBytes(const fs::path& path, std::vector<std::uint8_t>& out, std::string& err)
{
    err.clear();
    out.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        err = "Failed to open: " + path.string();
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    if (sz < 0)
    {
        err = "Failed to stat: " + path.string();
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    if (!out.empty())
        f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)out.size());
    if (!f && !out.empty())
    {
        err = "Failed to read: " + path.string();
        out.clear();
        return false;
    }
    return true;
}

static std::string ToLowerAscii(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string KindToString(Kind k)
{
    return (k == Kind::Tdf) ? "TDF" : "FIGlet";
}

static std::string TdfTypeToString(TdfFontType t)
{
    switch (t)
    {
    case TdfFontType::Outline: return "outline";
    case TdfFontType::Block:   return "block";
    case TdfFontType::Color:   return "color";
    }
    return "block";
}

static std::string MakeBaseLabel(const FontMeta& meta)
{
    std::string name = meta.name;
    if (name.empty())
        name = "(unnamed)";
    if (meta.kind == Kind::Tdf)
        return name + " [" + TdfTypeToString(meta.tdf_type) + "]";
    return name;
}

static std::string RelNoExt(const fs::path& root, const fs::path& p)
{
    fs::path rel;
    try { rel = fs::relative(p, root); }
    catch (...) { rel = p.filename(); }
    rel = rel.lexically_normal();
    rel.replace_extension();
    return rel.generic_string();
}

bool Registry::Scan(const std::string& assets_dir, std::string& out_error)
{
    out_error.clear();
    entries_.clear();
    fonts_by_id_.clear();
    errors_.clear();

    const fs::path root = fs::path(assets_dir) / "fonts";
    const fs::path flf_dir = root / "flf";
    const fs::path tdf_dir = root / "tdf";

    auto scan_dir = [&](const fs::path& dir, const char* ext_lower, const char* prefix) {
        if (!fs::exists(dir))
            return;

        for (const auto& it : fs::directory_iterator(dir))
        {
            if (!it.is_regular_file())
                continue;
            const fs::path p = it.path();
            const std::string ext = ToLowerAscii(p.extension().string());
            if (ext != ext_lower)
                continue;

            std::vector<std::uint8_t> bytes;
            std::string ferr;
            if (!ReadFileBytes(p, bytes, ferr))
            {
                errors_.push_back(ferr);
                continue;
            }

            std::vector<Font> loaded;
            std::string lerr;
            if (!LoadFontsFromBytes(bytes, loaded, lerr))
            {
                // Some collections include "empty" bundles that only contain the TDF header.
                // Treat those as ignorable for discovery (but keep strict errors for real failures).
                if (lerr.find("bundle contains no fonts") != std::string::npos)
                    continue;
                errors_.push_back("Failed to parse " + p.string() + ": " + lerr);
                continue;
            }

            for (size_t i = 0; i < loaded.size(); ++i)
            {
                FontMeta meta = GetMeta(loaded[i]);
                const std::string rel = RelNoExt(dir, p);
                std::string id = std::string(prefix) + ":" + rel;
                if (meta.kind == Kind::Tdf)
                    id += "#" + std::to_string(i);

                // If meta.name is empty (or duplicates), make it stable by including file stem.
                std::string label = MakeBaseLabel(meta);
                if (meta.kind == Kind::Tdf)
                {
                    label += " — " + rel;
                    label += " (" + KindToString(meta.kind) + ")";
                }
                else
                {
                    label += " (" + KindToString(meta.kind) + ")";
                }

                // Keep the loaded font in memory for fast renders.
                fonts_by_id_[id] = loaded[i];
                entries_.push_back(RegistryEntry{std::move(id), std::move(label), std::move(meta)});
            }
        }
    };

    scan_dir(flf_dir, ".flf", "flf");
    scan_dir(tdf_dir, ".tdf", "tdf");

    std::sort(entries_.begin(), entries_.end(),
              [](const RegistryEntry& a, const RegistryEntry& b) {
                  return a.label < b.label;
              });

    if (entries_.empty())
    {
        std::ostringstream oss;
        oss << "No fonts found under " << root.string();
        if (!errors_.empty())
            oss << " (" << errors_.size() << " errors)";
        out_error = oss.str();
        return false;
    }

    if (!errors_.empty())
    {
        // Non-fatal: tools can still function; expose errors via ansl.font if needed.
        out_error = "Font scan: " + std::to_string(errors_.size()) + " errors";
    }

    return true;
}

bool Registry::Render(std::string_view id,
                      std::string_view utf8_text,
                      const RenderOptions& options,
                      Bitmap& out,
                      std::string& err) const
{
    err.clear();
    out = Bitmap{};

    const auto it = fonts_by_id_.find(std::string(id));
    if (it == fonts_by_id_.end())
    {
        err = "Unknown font id: " + std::string(id);
        return false;
    }
    return RenderText(it->second, utf8_text, options, out, err);
}
} // namespace textmode_font


