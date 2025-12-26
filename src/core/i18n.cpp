#include "core/i18n.h"

#include <filesystem>
#include <unordered_map>
#include <vector>

#include <unicode/locid.h>
#include <unicode/msgfmt.h>
#include <unicode/ures.h>

namespace phos::i18n
{
namespace
{
struct State
{
    std::string bundle_dir;
    std::string locale;
    UResourceBundle* bundle = nullptr; // owned

    std::string missing_suffix;

    std::unordered_map<std::string, std::string> str_cache;
    std::unordered_map<std::string, icu::UnicodeString> pattern_cache;

    ~State()
    {
        if (bundle)
            ures_close(bundle);
        bundle = nullptr;
    }
};

static State& G()
{
    static State st;
    return st;
}

static bool SplitDotted(std::string_view key, std::vector<std::string_view>& out)
{
    out.clear();
    if (key.empty())
        return false;

    size_t b = 0;
    for (;;)
    {
        const size_t p = key.find('.', b);
        const size_t e = (p == std::string_view::npos) ? key.size() : p;
        if (e == b)
            return false; // empty segment
        out.push_back(key.substr(b, e - b));
        if (p == std::string_view::npos)
            break;
        b = p + 1;
        if (b >= key.size())
            return false;
    }
    return !out.empty();
}

struct BundleHandle
{
    UResourceBundle* b = nullptr;
    BundleHandle() = default;
    explicit BundleHandle(UResourceBundle* bb) : b(bb) {}
    BundleHandle(const BundleHandle&) = delete;
    BundleHandle& operator=(const BundleHandle&) = delete;
    BundleHandle(BundleHandle&& o) noexcept : b(o.b) { o.b = nullptr; }
    BundleHandle& operator=(BundleHandle&& o) noexcept
    {
        if (this != &o)
        {
            if (b) ures_close(b);
            b = o.b;
            o.b = nullptr;
        }
        return *this;
    }
    ~BundleHandle()
    {
        if (b)
            ures_close(b);
    }
};

static bool LookupUString(std::string_view dotted_key, icu::UnicodeString& out, UErrorCode& status)
{
    out.remove();

    State& st = G();
    if (!st.bundle || dotted_key.empty())
    {
        status = U_ILLEGAL_ARGUMENT_ERROR;
        return false;
    }

    std::vector<std::string_view> segs;
    if (!SplitDotted(dotted_key, segs))
    {
        status = U_ILLEGAL_ARGUMENT_ERROR;
        return false;
    }

    // Walk nested tables using ures_getByKey.
    BundleHandle cur(nullptr);
    UResourceBundle* cur_raw = st.bundle;

    for (size_t i = 0; i < segs.size(); ++i)
    {
        const std::string seg(segs[i]);
        UErrorCode s = U_ZERO_ERROR;
        UResourceBundle* next = ures_getByKey(cur_raw, seg.c_str(), nullptr, &s);
        if (U_FAILURE(s) || !next)
        {
            status = s;
            return false;
        }

        // Close previous handle if any.
        cur = BundleHandle(next);
        cur_raw = cur.b;
    }

    int32_t len = 0;
    const UChar* us = ures_getString(cur_raw, &len, &status);
    if (U_FAILURE(status) || !us)
        return false;

    out = icu::UnicodeString(us, len);
    return true;
}

static std::string MissingSuffix()
{
    State& st = G();
    if (!st.missing_suffix.empty())
        return st.missing_suffix;

    // Try to read it lazily; if missing, leave empty and don't spam lookups.
    UErrorCode status = U_ZERO_ERROR;
    icu::UnicodeString us;
    if (LookupUString("app_strings.missing_suffix", us, status))
    {
        std::string tmp;
        us.toUTF8String(tmp);
        st.missing_suffix = std::move(tmp);
    }
    return st.missing_suffix;
}
} // namespace

bool Init(const std::string& bundle_dir, const std::string& locale, std::string& error)
{
    error.clear();
    State& st = G();

    if (st.bundle)
    {
        ures_close(st.bundle);
        st.bundle = nullptr;
    }
    st.str_cache.clear();
    st.pattern_cache.clear();
    st.missing_suffix.clear();

    st.bundle_dir = bundle_dir;
    st.locale = locale;

    namespace fs = std::filesystem;
    try
    {
        if (st.bundle_dir.empty() || !fs::exists(fs::path(st.bundle_dir)))
        {
            error = "i18n bundle dir not found: " + st.bundle_dir;
            return false;
        }
    }
    catch (...)
    {
        // Best-effort existence check only.
    }

    const std::string chosen_locale = st.locale.empty()
        ? std::string(icu::Locale::getDefault().getName())
        : st.locale;

    // ICU resource bundle files are named by locale: root.res, en.res, fr.res, ...
    // We try the chosen locale first, then fall back to "root".
    {
        UErrorCode status = U_ZERO_ERROR;
        st.bundle = ures_openDirect(st.bundle_dir.c_str(), chosen_locale.c_str(), &status);
        if (U_FAILURE(status) || !st.bundle)
            st.bundle = nullptr;
    }
    if (!st.bundle)
    {
        UErrorCode status = U_ZERO_ERROR;
        st.bundle = ures_openDirect(st.bundle_dir.c_str(), "root", &status);
        if (U_FAILURE(status) || !st.bundle)
        {
            error = "Failed to open ICU resource bundle (dir=" + st.bundle_dir + ", locale=" + chosen_locale + ", fallback=root)";
            st.bundle = nullptr;
            return false;
        }
    }

    // Cache the missing suffix if present.
    (void)MissingSuffix();
    return true;
}

bool Ready()
{
    return G().bundle != nullptr;
}

std::string T(std::string_view key)
{
    State& st = G();
    if (!st.bundle)
        return std::string(key);

    const std::string k(key);
    if (auto it = st.str_cache.find(k); it != st.str_cache.end())
        return it->second;

    UErrorCode status = U_ZERO_ERROR;
    icu::UnicodeString us;
    if (!LookupUString(key, us, status))
    {
        std::string out = std::string(key);
        const std::string suff = MissingSuffix();
        if (!suff.empty())
            out += suff;
        st.str_cache.emplace(k, out);
        return out;
    }

    std::string out;
    us.toUTF8String(out);
    st.str_cache.emplace(k, out);
    return out;
}

std::string F(std::string_view key, std::initializer_list<Arg> args)
{
    State& st = G();
    if (!st.bundle)
        return std::string(key);

    icu::UnicodeString pattern;
    {
        const std::string k(key);
        auto it = st.pattern_cache.find(k);
        if (it != st.pattern_cache.end())
        {
            pattern = it->second;
        }
        else
        {
            UErrorCode status = U_ZERO_ERROR;
            if (!LookupUString(key, pattern, status))
                return T(key); // includes missing suffix handling/caching
            st.pattern_cache.emplace(k, pattern);
        }
    }

    UErrorCode status = U_ZERO_ERROR;
    icu::MessageFormat mf(pattern, icu::Locale::getDefault(), status);
    if (U_FAILURE(status))
    {
        std::string out;
        pattern.toUTF8String(out);
        return out;
    }

    std::vector<icu::Formattable> fargs;
    fargs.reserve(args.size());
    for (const Arg& a : args)
    {
        switch (a.kind)
        {
            case Arg::Kind::String:
                fargs.emplace_back(icu::UnicodeString::fromUTF8(a.s));
                break;
            case Arg::Kind::Int64:
                fargs.emplace_back((int64_t)a.i);
                break;
            case Arg::Kind::Double:
                fargs.emplace_back(a.d);
                break;
        }
    }

    icu::UnicodeString out_us;
    icu::FieldPosition fp(0);
    mf.format(fargs.data(), (int32_t)fargs.size(), out_us, fp, status);
    if (U_FAILURE(status))
    {
        std::string out;
        pattern.toUTF8String(out);
        return out;
    }

    std::string out;
    out_us.toUTF8String(out);
    return out;
}

} // namespace phos::i18n


