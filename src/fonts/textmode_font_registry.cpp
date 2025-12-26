#include "fonts/textmode_font_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unordered_set>
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

static std::string TrimAscii(std::string s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b]))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace((unsigned char)s[e - 1]))
        --e;
    if (b == 0 && e == s.size())
        return s;
    return s.substr(b, e - b);
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
    case TdfFontType::Colour:   return "colour";
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

namespace
{
static std::uint64_t Fnv1a64Update(std::uint64_t h, const void* data, size_t n)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (size_t i = 0; i < n; ++i)
    {
        h ^= (std::uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static std::uint64_t Fnv1a64UpdateString(std::uint64_t h, const std::string& s)
{
    return Fnv1a64Update(h, s.data(), s.size());
}

static std::uint64_t ComputeFontsFingerprint(const fs::path& flf_dir, const fs::path& tdf_dir)
{
    // Fingerprint includes filename + size + last_write_time for all .flf/.tdf files.
    // This avoids reading file contents (fast) and is good enough to invalidate on updates.
    std::uint64_t h = 1469598103934665603ull; // FNV offset basis

    auto hash_dir = [&](const fs::path& dir, const char* ext_lower) {
        std::error_code ec;
        if (!fs::exists(dir, ec) || ec)
            return;
        for (const auto& it : fs::directory_iterator(dir, ec))
        {
            if (ec)
                break;
            if (!it.is_regular_file())
                continue;
            const fs::path p = it.path();
            const std::string ext = ToLowerAscii(p.extension().string());
            if (ext != ext_lower)
                continue;

            // Use relative path (not just filename) in case font packs gain subdirectories later.
            std::string rel;
            try { rel = fs::relative(p, dir).generic_string(); }
            catch (...) { rel = p.filename().generic_string(); }
            h = Fnv1a64UpdateString(h, rel);
            const char zero = '\0';
            h = Fnv1a64Update(h, &zero, 1);

            std::uint64_t sz = 0;
            std::int64_t wt = 0;
            {
                std::error_code sec;
                sz = (std::uint64_t)fs::file_size(p, sec);
                if (sec) sz = 0;
            }
            {
                std::error_code tec;
                const auto ft = fs::last_write_time(p, tec);
                if (!tec)
                    wt = (std::int64_t)ft.time_since_epoch().count();
            }

            h = Fnv1a64Update(h, &sz, sizeof(sz));
            h = Fnv1a64Update(h, &wt, sizeof(wt));
        }
    };

    hash_dir(flf_dir, ".flf");
    hash_dir(tdf_dir, ".tdf");
    return h;
}

static bool IsBlankCell(char32_t cp)
{
    return cp == U'\0' || cp == U' ' || cp == U'\u00A0';
}

static bool LooksBrokenQuick(const FontMeta& meta, const Bitmap& bmp)
{
    (void)meta;
    if (bmp.w <= 0 || bmp.h <= 0)
        return true;
    const size_t expected = (size_t)bmp.w * (size_t)bmp.h;
    if (bmp.cp.size() != expected)
        return true;
    if (!bmp.fg.empty() && bmp.fg.size() != expected)
        return true;
    if (!bmp.bg.empty() && bmp.bg.size() != expected)
        return true;

    // A render that produces zero "ink" is effectively unusable for stamping.
    int non_blank = 0;
    for (char32_t cp : bmp.cp)
        if (!IsBlankCell(cp))
            ++non_blank;
    if (non_blank == 0)
        return true;

    // Guard against pathological outputs (usually corruption).
    if (bmp.w > 2000 || bmp.h > 500)
        return true;

    return false;
}
} // namespace

bool Registry::Scan(const std::string& assets_dir, std::string& out_error)
{
    ScanOptions opt;
    return Scan(assets_dir, out_error, opt, nullptr);
}

bool Registry::Scan(const std::string& assets_dir, std::string& out_error, const ScanOptions& options, SanityCache* cache)
{
    out_error.clear();
    entries_.clear();
    entry_index_by_id_.clear();
    id_aliases_.clear();
    fonts_by_id_.clear();
    errors_.clear();
    broken_ids_.clear();

    const fs::path root = fs::path(assets_dir) / "fonts";
    const fs::path flf_dir = root / "flf";
    const fs::path tdf_dir = root / "tdf";

    const std::uint64_t fingerprint = ComputeFontsFingerprint(flf_dir, tdf_dir);

    const bool cache_valid =
        cache && cache->schema_version == 1 && cache->complete && cache->fonts_fingerprint == fingerprint;

    std::unordered_set<std::string> cached_broken;
    if (cache_valid)
    {
        cached_broken.reserve(cache->broken_ids.size());
        for (const auto& id : cache->broken_ids)
            cached_broken.insert(id);
        broken_ids_ = cache->broken_ids;
    }

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

                // FIGlet fonts often don't contain a reliable human-readable name in-file.
                // Our FIGlet parser currently defaults meta.name to "figlet", which makes the
                // UI drop-down unusable. Prefer the file base name (without extension).
                if (meta.kind == Kind::Figlet)
                {
                    const std::string name_lower = ToLowerAscii(meta.name);
                    if (meta.name.empty() || name_lower == "figlet" || meta.name == "(unnamed)")
                    {
                        meta.name = fs::path(rel).filename().generic_string();
                    }
                }

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

                if (options.filter_broken_fonts && cache_valid && cached_broken.find(id) != cached_broken.end())
                {
                    // Skip known-broken fonts (keep errors_ separate from broken list).
                    continue;
                }

                // Keep the loaded font in memory for fast renders.
                fonts_by_id_[id] = loaded[i];
                entries_.push_back(RegistryEntry{std::move(id), std::move(label), std::move(meta)});
            }
        }
    };

    scan_dir(flf_dir, ".flf", "flf");
    scan_dir(tdf_dir, ".tdf", "tdf");

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

    // If requested, validate + populate cache on miss.
    if (options.validate_if_cache_miss && (!cache_valid))
    {
        RenderOptions ro;
        ro.mode = RenderMode::Display;
        ro.outline_style = 0;
        ro.use_font_colours = true;
        ro.icecolours = true;

        std::vector<std::string> broken;
        broken.reserve(256);

        for (const auto& e : entries_)
        {
            const auto it = fonts_by_id_.find(e.id);
            if (it == fonts_by_id_.end())
                continue;
            Bitmap bmp;
            std::string rerr;
            if (!RenderText(it->second, options.validate_text, ro, bmp, rerr) || LooksBrokenQuick(e.meta, bmp))
                broken.push_back(e.id);
        }

        std::sort(broken.begin(), broken.end());
        broken.erase(std::unique(broken.begin(), broken.end()), broken.end());
        broken_ids_ = broken;

        if (cache)
        {
            cache->schema_version = 1;
            cache->fonts_fingerprint = fingerprint;
            cache->complete = true;
            cache->broken_ids = broken;
        }

        if (options.filter_broken_fonts && !broken.empty())
        {
            std::unordered_set<std::string> broken_set;
            broken_set.reserve(broken.size());
            for (const auto& id : broken)
                broken_set.insert(id);

            // Filter entries_
            entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                          [&](const RegistryEntry& re) {
                                              return broken_set.find(re.id) != broken_set.end();
                                          }),
                          entries_.end());
            // Filter fonts_by_id_
            for (const auto& id : broken)
                fonts_by_id_.erase(id);
        }
    }

    // Dedupe entries_ by normalized title (meta.name) + kind, to avoid UI/Lua list spam.
    // Keep aliases so older saved ids can still resolve/render and show a friendly name.
    auto dedupe_key = [&](const RegistryEntry& e) -> std::string {
        std::string title = TrimAscii(e.meta.name);
        if (title.empty())
            title = TrimAscii(e.label);
        title = ToLowerAscii(title);
        title += "|";
        title += (e.meta.kind == Kind::Tdf) ? "tdf" : "flf";
        return title;
    };

    if (!entries_.empty())
    {
        std::vector<size_t> order(entries_.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) {
                      const auto& ea = entries_[a];
                      const auto& eb = entries_[b];
                      const std::string ka = dedupe_key(ea);
                      const std::string kb = dedupe_key(eb);
                      if (ka != kb) return ka < kb;
                      return ea.id < eb.id;
                  });

        std::unordered_map<std::string, std::string> canonical_by_key;
        canonical_by_key.reserve(entries_.size());

        std::vector<RegistryEntry> deduped;
        deduped.reserve(entries_.size());
        std::vector<std::string> dup_ids;
        dup_ids.reserve(64);

        for (size_t idx : order)
        {
            auto& e = entries_[idx];
            const std::string k = dedupe_key(e);
            auto itc = canonical_by_key.find(k);
            if (itc == canonical_by_key.end())
            {
                canonical_by_key.emplace(k, e.id);
                deduped.push_back(std::move(e));
            }
            else
            {
                id_aliases_.emplace(e.id, itc->second);
                dup_ids.push_back(e.id);
            }
        }

        // Drop duplicate font payloads (aliases will resolve to the canonical id).
        for (const auto& id : dup_ids)
            fonts_by_id_.erase(id);

        entries_ = std::move(deduped);
    }

    // Stable presentation order.
    std::sort(entries_.begin(), entries_.end(),
              [](const RegistryEntry& a, const RegistryEntry& b) {
                  return a.label < b.label;
              });

    // Build fast id->entry index for Find() and alias resolution.
    entry_index_by_id_.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i)
        entry_index_by_id_[entries_[i].id] = i;

    return true;
}

std::string Registry::ResolveId(std::string_view id) const
{
    std::string cur(id);
    // Follow alias chains defensively (should be 0-1 hop in practice).
    for (int i = 0; i < 4; ++i)
    {
        const auto it = id_aliases_.find(cur);
        if (it == id_aliases_.end())
            break;
        cur = it->second;
    }
    return cur;
}

const RegistryEntry* Registry::Find(std::string_view id) const
{
    const std::string rid = ResolveId(id);
    const auto it = entry_index_by_id_.find(rid);
    if (it == entry_index_by_id_.end())
        return nullptr;
    const size_t idx = it->second;
    if (idx >= entries_.size())
        return nullptr;
    return &entries_[idx];
}

bool Registry::Render(std::string_view id,
                      std::string_view utf8_text,
                      const RenderOptions& options,
                      Bitmap& out,
                      std::string& err) const
{
    err.clear();
    out = Bitmap{};

    const std::string rid = ResolveId(id);
    const auto it = fonts_by_id_.find(rid);
    if (it == fonts_by_id_.end())
    {
        err = "Unknown font id: " + std::string(id);
        return false;
    }
    return RenderText(it->second, utf8_text, options, out, err);
}
} // namespace textmode_font


