#include "fonts/textmode_font_registry.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
static void AppendUtf8(char32_t cp, std::string& out)
{
    // Basic UTF-8 encoder (no normalization). Invalid values become U+FFFD.
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
        cp = 0xFFFDu;

    if (cp <= 0x7Fu)
    {
        out.push_back((char)cp);
    }
    else if (cp <= 0x7FFu)
    {
        out.push_back((char)(0xC0u | ((cp >> 6) & 0x1Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
    else if (cp <= 0xFFFFu)
    {
        out.push_back((char)(0xE0u | ((cp >> 12) & 0x0Fu)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
    else
    {
        out.push_back((char)(0xF0u | ((cp >> 18) & 0x07u)));
        out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
}

static bool IsBlank(char32_t cp)
{
    // Treat NUL/space/NBSP as blank "ink".
    return cp == U'\0' || cp == U' ' || cp == U'\u00A0';
}

static std::string BitmapToUtf8Text(const textmode_font::Bitmap& bmp)
{
    std::string out;
    if (bmp.w <= 0 || bmp.h <= 0)
        return out;

    // Rough reserve: average 1-3 bytes per cell + newlines.
    out.reserve((size_t)bmp.w * (size_t)bmp.h * 2 + (size_t)bmp.h);

    for (int y = 0; y < bmp.h; ++y)
    {
        for (int x = 0; x < bmp.w; ++x)
        {
            const char32_t cp = bmp.cp[(size_t)y * (size_t)bmp.w + (size_t)x];
            AppendUtf8(IsBlank(cp) ? U' ' : cp, out);
        }
        out.push_back('\n');
    }
    return out;
}

static std::string SanitizeFilename(std::string s)
{
    for (char& c : s)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '_' || c == '.' || c == '#';
        if (!ok)
            c = '_';
    }
    // Keep paths short-ish for convenience.
    if (s.size() > 160)
        s.resize(160);
    return s;
}

struct CheckResult
{
    bool ok = false;
    std::vector<std::string> reasons;
    int w = 0;
    int h = 0;
    int non_blank = 0;
    int total = 0;
};

static void AddReason(CheckResult& r, std::string why)
{
    if (!why.empty())
        r.reasons.push_back(std::move(why));
}

static CheckResult CheckRenderedBitmap(std::string_view font_id,
                                      const textmode_font::FontMeta& meta,
                                      const textmode_font::Bitmap& bmp)
{
    (void)meta;

    CheckResult res;
    res.w = bmp.w;
    res.h = bmp.h;

    if (bmp.w <= 0 || bmp.h <= 0)
    {
        AddReason(res, "empty dimensions");
        return res;
    }

    const size_t expected = (size_t)bmp.w * (size_t)bmp.h;
    res.total = (int)expected;
    if (bmp.cp.size() != expected)
        AddReason(res, "cp size mismatch");
    if (!bmp.fg.empty() && bmp.fg.size() != expected)
        AddReason(res, "fg size mismatch");
    if (!bmp.bg.empty() && bmp.bg.size() != expected)
        AddReason(res, "bg size mismatch");

    // Guard against pathological results (usually indicates parsing/render bug).
    // "test" should never be huge; allow generous limits.
    if (bmp.w > 1000)
        AddReason(res, "suspiciously wide (w>1000)");
    if (bmp.h > 200)
        AddReason(res, "suspiciously tall (h>200)");
    if (expected > 200000)
        AddReason(res, "suspiciously large (w*h>200k)");

    // Count ink + detect invalid Unicode.
    int non_blank = 0;
    int invalid = 0;
    for (char32_t cp : bmp.cp)
    {
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
            ++invalid;
        if (!IsBlank(cp))
            ++non_blank;
    }
    res.non_blank = non_blank;
    if (invalid > 0)
        AddReason(res, "contains invalid unicode codepoints");

    if (non_blank == 0)
        AddReason(res, "renders blank (no ink)");
    else if (non_blank < 8)
        AddReason(res, "very low ink (<8 non-blank cells)");

    // FIGlet-specific-ish heuristics: endmark/hardblank leaks often show up as lots of '@'/'$'
    // (or a consistent junk char at the far right of most rows).
    //
    // We keep these as "likely broken" rather than hard errors: false positives are possible.
    auto count_cp = [&](char32_t c) -> int {
        int n = 0;
        for (char32_t cp : bmp.cp)
            if (cp == c)
                ++n;
        return n;
    };

    const int at_count = count_cp(U'@');
    const int dollar_count = count_cp(U'$');

    auto suspicious_ratio = [&](int cnt) -> bool {
        if (non_blank <= 0)
            return false;
        // "If >35% of ink is a single ASCII punctuation, it's probably leakage."
        return cnt > 0 && (double)cnt / (double)non_blank > 0.35;
    };

    if (meta.kind == textmode_font::Kind::Figlet)
    {
        if (suspicious_ratio(at_count))
            AddReason(res, "likely endmark leak ('@' dominates ink)");
        if (suspicious_ratio(dollar_count))
            AddReason(res, "likely hardblank leak ('$' dominates ink)");
    }

    // Right-edge junk detector (FIGlet only): find the last non-blank char per row; if it's the
    // same ASCII punctuation for most rows and appears at the max x, suspect untrimmed endmarks.
    //
    // Note: This is NOT a safe heuristic for TDF fonts; many TDF designs legitimately use
    // repeated '|' or '_' strokes on the right edge.
    if (meta.kind == textmode_font::Kind::Figlet && bmp.w >= 3 && bmp.h >= 3 && non_blank > 0)
    {
        std::vector<char32_t> last_cp((size_t)bmp.h, U'\0');
        std::vector<int> last_x((size_t)bmp.h, -1);
        for (int y = 0; y < bmp.h; ++y)
        {
            for (int x = bmp.w - 1; x >= 0; --x)
            {
                const char32_t cp = bmp.cp[(size_t)y * (size_t)bmp.w + (size_t)x];
                if (!IsBlank(cp))
                {
                    last_cp[(size_t)y] = cp;
                    last_x[(size_t)y] = x;
                    break;
                }
            }
        }

        // Count the most common last_cp among rows that have ink.
        // Compute mode manually (small N).
        char32_t mode_cp = U'\0';
        int mode_count = 0;
        for (int y = 0; y < bmp.h; ++y)
        {
            if (last_x[(size_t)y] < 0)
                continue;
            const char32_t c = last_cp[(size_t)y];
            int cnt = 0;
            for (int yy = 0; yy < bmp.h; ++yy)
                if (last_x[(size_t)yy] >= 0 && last_cp[(size_t)yy] == c)
                    ++cnt;
            if (cnt > mode_count)
            {
                mode_count = cnt;
                mode_cp = c;
            }
        }

        const bool is_ascii_punct =
            (mode_cp >= 33 && mode_cp <= 126) &&
            !((mode_cp >= '0' && mode_cp <= '9') || (mode_cp >= 'A' && mode_cp <= 'Z') ||
              (mode_cp >= 'a' && mode_cp <= 'z'));

        int mode_at_right_edge = 0;
        int ink_rows = 0;
        for (int y = 0; y < bmp.h; ++y)
        {
            if (last_x[(size_t)y] < 0)
                continue;
            ++ink_rows;
            if (last_cp[(size_t)y] == mode_cp && last_x[(size_t)y] >= bmp.w - 2)
                ++mode_at_right_edge;
        }

        if (ink_rows >= 3 && is_ascii_punct && mode_count >= (int)(0.8 * ink_rows) &&
            mode_at_right_edge >= (int)(0.8 * ink_rows))
        {
            std::string why = "likely right-edge junk leak (rows end with '";
            why.push_back((char)mode_cp);
            why += "')";
            AddReason(res, std::move(why));
        }
    }

    // If we have no reasons, consider it OK.
    res.ok = res.reasons.empty();

    // A few issues are soft; if the only issue is "very low ink" but it isn't blank and the
    // bitmap is small, keep it OK to avoid false positives on tiny fonts.
    if (!res.ok && res.reasons.size() == 1 && res.reasons[0].find("very low ink") != std::string::npos)
    {
        if (bmp.w <= 8 && bmp.h <= 8 && non_blank > 0)
            res.ok = true;
    }

    (void)font_id;
    return res;
}

static void PrintUsage(const char* argv0)
{
    std::cerr << "Usage: " << argv0 << " [--assets <dir>] [--dump <dir>] [--only flf|tdf] [--limit N]\n"
              << "               [--emit-broken-ids <path>]\n"
              << "               [--move-broken-flf <dir>]\n"
              << "\n"
              << "Scans assets/fonts/{flf,tdf}, renders \"test\" in each font, and flags likely-broken fonts.\n"
              << "\n"
              << "Options:\n"
              << "  --assets <dir>  Project assets dir (default: ./assets)\n"
              << "  --dump <dir>    If set, write a UTF-8 preview for broken fonts into this directory\n"
              << "  --only flf|tdf  Restrict scan to one family\n"
              << "  --limit N       Stop after N fonts (debug)\n"
              << "  --emit-broken-ids <path>\n"
              << "                 Write newline-separated broken font ids (stable ids: flf:..., tdf:...#N)\n"
              << "  --move-broken-flf <dir>\n"
              << "                 Move broken FIGlet .flf files into <dir> (quarantine).\n";
}
} // namespace

int main(int argc, char** argv)
{
    fs::path assets_dir = fs::path("assets");
    fs::path dump_dir;
    fs::path emit_broken_ids_path;
    fs::path move_broken_flf_dir;
    std::string only_family;
    int limit = -1;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        auto need = [&](const char* opt) -> std::string_view {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for " << opt << "\n";
                PrintUsage(argv[0]);
                std::exit(2);
            }
            return std::string_view(argv[++i]);
        };

        if (a == "--help" || a == "-h")
        {
            PrintUsage(argv[0]);
            return 0;
        }
        else if (a == "--assets")
        {
            assets_dir = fs::path(std::string(need("--assets")));
        }
        else if (a == "--dump")
        {
            dump_dir = fs::path(std::string(need("--dump")));
        }
        else if (a == "--only")
        {
            only_family = std::string(need("--only"));
            if (!(only_family == "flf" || only_family == "tdf"))
            {
                std::cerr << "Invalid --only value (expected flf|tdf)\n";
                return 2;
            }
        }
        else if (a == "--emit-broken-ids")
        {
            emit_broken_ids_path = fs::path(std::string(need("--emit-broken-ids")));
        }
        else if (a == "--move-broken-flf")
        {
            move_broken_flf_dir = fs::path(std::string(need("--move-broken-flf")));
        }
        else if (a == "--limit")
        {
            limit = std::stoi(std::string(need("--limit")));
        }
        else
        {
            std::cerr << "Unknown arg: " << a << "\n";
            PrintUsage(argv[0]);
            return 2;
        }
    }

    if (!dump_dir.empty())
        fs::create_directories(dump_dir);

    textmode_font::Registry reg;
    std::string scan_err;
    if (!reg.Scan(assets_dir.string(), scan_err))
    {
        std::cerr << "Font scan failed: " << scan_err << "\n";
        for (const auto& e : reg.Errors())
            std::cerr << "  - " << e << "\n";
        return 1;
    }

    if (!scan_err.empty())
        std::cerr << scan_err << "\n";

    textmode_font::RenderOptions ro;
    ro.mode = textmode_font::RenderMode::Display;
    ro.outline_style = 0;
    ro.use_font_colours = true;
    ro.icecolours = true;

    struct Row
    {
        std::string id;
        std::string label;
        CheckResult check;
        textmode_font::Bitmap bmp;
        textmode_font::FontMeta meta;
    };
    std::vector<Row> broken;

    int checked = 0;
    for (const auto& entry : reg.List())
    {
        if (!only_family.empty())
        {
            if (only_family == "flf" && entry.id.rfind("flf:", 0) != 0)
                continue;
            if (only_family == "tdf" && entry.id.rfind("tdf:", 0) != 0)
                continue;
        }

        textmode_font::Bitmap bmp;
        std::string err;
        bool ok = reg.Render(entry.id, "test", ro, bmp, err);
        ++checked;

        Row row;
        row.id = entry.id;
        row.label = entry.label;
        row.meta = entry.meta;
        row.bmp = std::move(bmp);

        if (!ok)
        {
            row.check.ok = false;
            AddReason(row.check, err.empty() ? "render failed" : ("render failed: " + err));
            broken.push_back(std::move(row));
        }
        else
        {
            row.check = CheckRenderedBitmap(entry.id, entry.meta, row.bmp);
            if (!row.check.ok)
                broken.push_back(std::move(row));
        }

        if (limit > 0 && checked >= limit)
            break;
    }

    std::sort(broken.begin(), broken.end(), [](const Row& a, const Row& b) { return a.id < b.id; });

    std::cout << "Checked: " << checked << " fonts\n";
    std::cout << "Broken:  " << broken.size() << " fonts\n";

    std::ofstream broken_ids_out;
    if (!emit_broken_ids_path.empty())
    {
        broken_ids_out.open(emit_broken_ids_path, std::ios::binary);
        if (!broken_ids_out)
        {
            std::cerr << "Failed to open --emit-broken-ids output: " << emit_broken_ids_path.string() << "\n";
            return 2;
        }
    }

    for (const auto& b : broken)
    {
        if (broken_ids_out)
            broken_ids_out << b.id << "\n";

        std::cout << "\nBROKEN " << b.id << "\n";
        std::cout << "  " << b.label << "\n";
        std::cout << "  kind=" << ((b.meta.kind == textmode_font::Kind::Tdf) ? "tdf" : "flf")
                  << " w=" << b.check.w << " h=" << b.check.h << " ink=" << b.check.non_blank << "\n";

        // Print source asset path (handy because TDF bundles contain multiple fonts per file).
        if (b.id.rfind("flf:", 0) == 0)
        {
            const std::string rel = b.id.substr(4);
            std::cout << "  source=" << (assets_dir / "fonts" / "flf" / (rel + ".flf")).string() << "\n";
        }
        else if (b.id.rfind("tdf:", 0) == 0)
        {
            const std::string rest = b.id.substr(4);
            const size_t hash = rest.find('#');
            const std::string rel = (hash == std::string::npos) ? rest : rest.substr(0, hash);
            std::cout << "  source=" << (assets_dir / "fonts" / "tdf" / (rel + ".tdf")).string();
            if (hash != std::string::npos)
                std::cout << " (bundle_index=" << rest.substr(hash + 1) << ")";
            std::cout << "\n";
        }

        for (const auto& r : b.check.reasons)
            std::cout << "  - " << r << "\n";

        if (!dump_dir.empty() && b.bmp.w > 0 && b.bmp.h > 0 && b.bmp.cp.size() == (size_t)b.bmp.w * (size_t)b.bmp.h)
        {
            const std::string text = BitmapToUtf8Text(b.bmp);
            const fs::path out = dump_dir / (SanitizeFilename(b.id) + ".txt");
            std::ofstream f(out, std::ios::binary);
            if (f)
                f.write(text.data(), (std::streamsize)text.size());
        }
    }

    // Optional cleanup action: move broken FIGlet fonts out of the scan directory.
    if (!move_broken_flf_dir.empty())
    {
        // Safety: only act on FIGlet ids.
        std::vector<std::string> broken_flf_ids;
        broken_flf_ids.reserve(broken.size());
        for (const auto& b : broken)
            if (b.id.rfind("flf:", 0) == 0)
                broken_flf_ids.push_back(b.id);

        if (broken_flf_ids.empty())
        {
            std::cout << "\nNo broken FIGlet fonts to move.\n";
        }
        else
        {
            fs::create_directories(move_broken_flf_dir);

            int moved = 0;
            int missing = 0;
            int failed = 0;

            for (const auto& id : broken_flf_ids)
            {
                const std::string rel = id.substr(4);
                const fs::path src = assets_dir / "fonts" / "flf" / (rel + ".flf");

                // Preserve leaf filename; sanitize just in case.
                fs::path dst = move_broken_flf_dir / (SanitizeFilename(fs::path(rel).filename().generic_string()) + ".flf");

                std::error_code ec;
                if (!fs::exists(src, ec) || ec)
                {
                    ++missing;
                    continue;
                }

                // Avoid clobbering existing file.
                if (fs::exists(dst, ec) && !ec)
                {
                    for (int i = 1; i < 1000; ++i)
                    {
                        fs::path alt = move_broken_flf_dir / (dst.stem().generic_string() + ".dup" + std::to_string(i) + dst.extension().generic_string());
                        if (!fs::exists(alt, ec) || ec)
                        {
                            dst = alt;
                            break;
                        }
                    }
                }

                fs::rename(src, dst, ec);
                if (ec)
                {
                    ++failed;
                    continue;
                }
                ++moved;
            }

            std::cout << "\nMoved broken FIGlet fonts:\n";
            std::cout << "  moved=" << moved << " missing=" << missing << " failed=" << failed << "\n";
            std::cout << "  quarantine=" << move_broken_flf_dir.string() << "\n";
        }
    }

    // Non-zero exit if anything looks broken (useful for CI / bulk cleanup scripts).
    return broken.empty() ? 0 : 3;
}


