#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace phos::i18n
{

// Lightweight formatting argument for MessageFormat "{0}" style placeholders.
struct Arg
{
    enum class Kind
    {
        String,
        Int64,
        Double,
    };

    Kind        kind = Kind::String;
    std::string s;
    long long   i = 0;
    double      d = 0.0;

    static Arg Str(std::string_view v) { Arg a; a.kind = Kind::String; a.s = std::string(v); return a; }
    static Arg Str(const char* v) { return Str(std::string_view(v ? v : "")); }
    static Arg I64(long long v) { Arg a; a.kind = Kind::Int64; a.i = v; return a; }
    static Arg F64(double v) { Arg a; a.kind = Kind::Double; a.d = v; return a; }
};

// Initializes the global i18n bundle.
//
// - `bundle_dir` should point at a directory containing ICU .res files (e.g. root.res).
// - `locale` may be empty to use ICU's default locale.
// - Returns true on success; on failure returns false and sets `error`.
bool Init(const std::string& bundle_dir, const std::string& locale, std::string& error);

// Returns whether i18n is initialized and has a loaded bundle.
bool Ready();

// Looks up a string by dotted key path (e.g. "menu.file.quit").
// If missing, returns the key itself (optionally suffixed with " (missing)" if available).
std::string T(std::string_view key);

// Looks up a string pattern by key and formats it using ICU MessageFormat with positional args.
// If formatting fails, returns the raw pattern (or key if missing).
std::string F(std::string_view key, std::initializer_list<Arg> args);

} // namespace phos::i18n

// Convenience macros for UI code.
#define PHOS_TR(key_literal) ::phos::i18n::T((key_literal))
#define PHOS_TRF(key_literal, ...) ::phos::i18n::F((key_literal), {__VA_ARGS__})


