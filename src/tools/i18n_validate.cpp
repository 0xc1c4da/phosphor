#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <unicode/locid.h>
#include <unicode/msgfmt.h>
#include <unicode/ures.h>

namespace fs = std::filesystem;

struct Issues
{
    int missing_keys = 0;
    int msgfmt_errors = 0;
    int ellipsis_inconsistencies = 0;
    int ascii_ellipsis_in_translation = 0;
    int imgui_id_in_translation = 0;
    int file_pattern_in_translation = 0;
};

static void WalkBundle(UResourceBundle* rb,
                       std::string_view prefix,
                       std::unordered_map<std::string, std::string>& out_strings)
{
    const int32_t n = ures_getSize(rb);
    for (int32_t i = 0; i < n; ++i)
    {
        UErrorCode status = U_ZERO_ERROR;
        UResourceBundle* child = ures_getByIndex(rb, i, nullptr, &status);
        if (U_FAILURE(status) || !child)
            continue;

        const char* key = ures_getKey(child);
        const std::string child_key = key ? key : "";
        const std::string next_prefix = prefix.empty()
            ? child_key
            : (std::string(prefix) + "." + child_key);

        const UResType t = ures_getType(child);
        if (t == URES_STRING)
        {
            int32_t len = 0;
            UErrorCode s2 = U_ZERO_ERROR;
            const UChar* us = ures_getString(child, &len, &s2);
            if (!U_FAILURE(s2) && us)
            {
                icu::UnicodeString u(us, len);
                std::string utf8;
                u.toUTF8String(utf8);
                out_strings[next_prefix] = std::move(utf8);
            }
        }
        else if (t == URES_TABLE || t == URES_ARRAY)
        {
            WalkBundle(child, next_prefix, out_strings);
        }

        ures_close(child);
    }
}

struct Use
{
    std::string key;
    bool is_format = false; // PHOS_TRF(...)
};

static std::string StripCppCommentsPreservingStrings(const std::string& s)
{
    std::string out;
    out.reserve(s.size());

    bool in_str = false;
    bool in_char = false;
    bool esc = false;
    bool line_comment = false;
    bool block_comment = false;

    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        const char n = (i + 1 < s.size()) ? s[i + 1] : '\0';

        if (line_comment)
        {
            if (c == '\n')
            {
                line_comment = false;
                out.push_back(c);
            }
            continue;
        }
        if (block_comment)
        {
            if (c == '*' && n == '/')
            {
                block_comment = false;
                ++i;
            }
            continue;
        }

        if (!in_str && !in_char)
        {
            if (c == '/' && n == '/')
            {
                line_comment = true;
                ++i;
                continue;
            }
            if (c == '/' && n == '*')
            {
                block_comment = true;
                ++i;
                continue;
            }
        }

        // Track string/char literal boundaries so we don't strip comment markers inside them.
        if (in_str)
        {
            out.push_back(c);
            if (esc)
            {
                esc = false;
                continue;
            }
            if (c == '\\')
            {
                esc = true;
                continue;
            }
            if (c == '"')
                in_str = false;
            continue;
        }
        if (in_char)
        {
            out.push_back(c);
            if (esc)
            {
                esc = false;
                continue;
            }
            if (c == '\\')
            {
                esc = true;
                continue;
            }
            if (c == '\'')
                in_char = false;
            continue;
        }

        if (c == '"')
        {
            in_str = true;
            out.push_back(c);
            continue;
        }
        if (c == '\'')
        {
            in_char = true;
            out.push_back(c);
            continue;
        }

        out.push_back(c);
    }

    return out;
}

static std::vector<Use> ScanUsesInFile(const fs::path& p)
{
    std::vector<Use> out;
    std::ifstream in(p);
    if (!in)
        return out;
    const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string s = StripCppCommentsPreservingStrings(raw);

    // Very simple scan: PHOS_TR("...") and PHOS_TRF("...")
    // Note: raw string delimiter must avoid appearing inside the regex itself.
    static const std::regex re(R"phos(\bPHOS_TR(F?)\(\s*"([^"]+)")phos");
    for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it)
    {
        Use u;
        u.is_format = ((*it)[1].matched && (*it)[1].str() == "F");
        u.key = (*it)[2].str();
        out.push_back(std::move(u));
    }
    return out;
}

static bool EndsWith(std::string_view s, std::string_view suf)
{
    return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

static bool ContainsDisallowedAsciiEllipsis(std::string_view v)
{
    // We prefer the Unicode ellipsis "…" for UI strings.
    // ASCII "..." is allowed only in a few clearly-technical contexts (not a UI continuation marker).
    //
    // Current allowed cases (see i18n/root.txt notes):
    // - API filter syntax: "filter=..."
    // - Pablo/Icy truecolor token: "...t" / "(...t)"
    if (v.find("...") == std::string::npos)
        return false;
    if (v.find("filter=...") != std::string::npos)
        return false;
    if (v.find("...t") != std::string::npos)
        return false;
    return true;
}

int main(int argc, char** argv)
{
    // Default expected location from this repo's Makefile: build/i18n/root.res
    std::string res_path = "build/i18n/root.res";
    std::string bundle_dir = "build/i18n";
    std::string locale = "root";

    if (argc >= 2)
        bundle_dir = argv[1];
    if (argc >= 3)
        locale = argv[2];

    Issues issues;

    // Load bundle (use absolute path; ICU file loaders can be picky about relative paths).
    try
    {
        bundle_dir = fs::absolute(fs::path(bundle_dir)).string();
    }
    catch (...)
    {
        // Best effort only; keep original.
    }

    UErrorCode status = U_ZERO_ERROR;
    UResourceBundle* rb = ures_openDirect(bundle_dir.c_str(), locale.c_str(), &status);
    if (U_FAILURE(status) || !rb)
    {
        std::cerr << "i18n_validate: failed to open bundle (dir=" << bundle_dir
                  << " locale=" << locale
                  << " status=" << (int)status
                  << ")\n";
        return 2;
    }

    std::unordered_map<std::string, std::string> strings;
    WalkBundle(rb, "", strings);
    ures_close(rb);

    // Scan uses in src/
    std::vector<Use> uses;
    for (const auto& it : fs::recursive_directory_iterator("src"))
    {
        if (!it.is_regular_file())
            continue;
        if (it.path().extension() != ".cpp")
            continue;
        auto v = ScanUsesInFile(it.path());
        uses.insert(uses.end(), v.begin(), v.end());
    }

    std::unordered_set<std::string> used_keys;
    std::unordered_set<std::string> used_fmt_keys;
    for (const auto& u : uses)
    {
        used_keys.insert(u.key);
        if (u.is_format)
            used_fmt_keys.insert(u.key);
    }

    // Missing keys
    for (const auto& k : used_keys)
    {
        if (strings.find(k) == strings.end())
        {
            ++issues.missing_keys;
            std::cerr << "MISSING_KEY " << k << "\n";
        }
    }

    // Translator safety checks: ImGui IDs and file-pattern blobs should not appear in translations.
    for (const auto& [k, v] : strings)
    {
        if (v.find("##") != std::string::npos)
        {
            ++issues.imgui_id_in_translation;
            std::cerr << "IMGUI_ID_IN_TRANSLATION " << k << " = " << v << "\n";
        }
        // Flag common "label (*.ext;...)" blobs. We intentionally allow patterns like "filter=..." elsewhere.
        if (v.find("(*.") != std::string::npos)
        {
            ++issues.file_pattern_in_translation;
            std::cerr << "FILE_PATTERN_IN_TRANSLATION " << k << " = " << v << "\n";
        }

        // Guardrail: avoid ASCII "..." for UI ellipsis outside known technical contexts.
        // Note: *_ellipsis is handled separately below for clearer error messages.
        if (!EndsWith(k, "_ellipsis") && ContainsDisallowedAsciiEllipsis(v))
        {
            ++issues.ascii_ellipsis_in_translation;
            std::cerr << "ELLIPSIS_ASCII " << k << " = " << v << "\n";
        }
    }

    // Ellipsis consistency: *_ellipsis should use Unicode ellipsis (…)
    for (const auto& [k, v] : strings)
    {
        if (!EndsWith(k, "_ellipsis"))
            continue;
        if (v.find("...") != std::string::npos)
        {
            ++issues.ellipsis_inconsistencies;
            std::cerr << "ELLIPSIS_ASCII " << k << " = " << v << "\n";
        }
    }

    // MessageFormat parse validation:
    // - Always validate keys used via PHOS_TRF
    // - Also validate any key ending in _fmt (even if not currently used)
    std::unordered_set<std::string> fmt_keys = used_fmt_keys;
    for (const auto& [k, _v] : strings)
    {
        if (EndsWith(k, "_fmt"))
            fmt_keys.insert(k);
    }
    for (const auto& k : fmt_keys)
    {
        auto it = strings.find(k);
        if (it == strings.end())
            continue; // already counted missing
        const std::string& pat_utf8 = it->second;
        UErrorCode s = U_ZERO_ERROR;
        icu::UnicodeString pat = icu::UnicodeString::fromUTF8(pat_utf8);
        icu::MessageFormat mf(pat, icu::Locale::getDefault(), s);
        if (U_FAILURE(s))
        {
            ++issues.msgfmt_errors;
            std::cerr << "MSGFMT_PARSE_ERROR " << k << " = " << pat_utf8 << "\n";
        }
        (void)mf;
    }

    if (issues.missing_keys ||
        issues.msgfmt_errors ||
        issues.ellipsis_inconsistencies ||
        issues.ascii_ellipsis_in_translation ||
        issues.imgui_id_in_translation ||
        issues.file_pattern_in_translation)
    {
        std::cerr << "i18n_validate: FAIL"
                  << " missing_keys=" << issues.missing_keys
                  << " msgfmt_errors=" << issues.msgfmt_errors
                  << " ellipsis_ascii=" << issues.ellipsis_inconsistencies
                  << " ellipsis_ascii_in_translation=" << issues.ascii_ellipsis_in_translation
                  << " imgui_id_in_translation=" << issues.imgui_id_in_translation
                  << " file_pattern_in_translation=" << issues.file_pattern_in_translation
                  << "\n";
        return 1;
    }

    std::cout << "i18n_validate: OK"
              << " (keys=" << strings.size()
              << " used=" << used_keys.size()
              << " fmt_used=" << used_fmt_keys.size()
              << ")\n";
    return 0;
}


