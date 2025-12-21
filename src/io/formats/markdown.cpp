#include "io/formats/markdown.h"

#include "core/paths.h"

#include <md4c.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace formats
{
namespace markdown
{
const std::vector<std::string_view>& ImportExtensions()
{
    static const std::vector<std::string_view> exts = {"md", "markdown", "mdown", "mkd"};
    return exts;
}

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

static std::string ToLowerAscii(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static inline int ClampInt(int v, int lo, int hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static bool DecodeOneUtf8(const char* data, size_t len, size_t& i, char32_t& out_cp)
{
    out_cp = U'\0';
    if (i >= len)
        return false;

    const std::uint8_t c = (std::uint8_t)data[i];
    if ((c & 0x80u) == 0)
    {
        out_cp = (char32_t)c;
        i += 1;
        return true;
    }

    size_t remaining = 0;
    char32_t cp = 0;
    if ((c & 0xE0u) == 0xC0u) { cp = c & 0x1Fu; remaining = 1; }
    else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu; remaining = 2; }
    else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u; remaining = 3; }
    else
    {
        i += 1;
        return false;
    }

    if (i + remaining >= len)
    {
        i = len;
        return false;
    }

    for (size_t j = 0; j < remaining; ++j)
    {
        const std::uint8_t cc = (std::uint8_t)data[i + 1 + j];
        if ((cc & 0xC0u) != 0x80u)
        {
            i += 1;
            return false;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }

    i += 1 + remaining;
    out_cp = cp;
    return true;
}

static void Utf8ToCodepointsBestEffort(std::string_view s, std::vector<char32_t>& out)
{
    out.clear();
    out.reserve(s.size());

    size_t i = 0;
    while (i < s.size())
    {
        char32_t cp = U'\0';
        const size_t before = i;
        if (DecodeOneUtf8(s.data(), s.size(), i, cp))
        {
            out.push_back(cp);
        }
        else
        {
            // Skip one byte and replace with U+FFFD (visible "invalid").
            i = before + 1;
            out.push_back(U'\uFFFD');
        }
    }
}

static inline AnsiCanvas::Color32 PackImGuiCol32(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
{
    // Dear ImGui IM_COL32 is ABGR.
    return ((AnsiCanvas::Color32)a << 24) | ((AnsiCanvas::Color32)b << 16) | ((AnsiCanvas::Color32)g << 8) | (AnsiCanvas::Color32)r;
}

static bool HexToColor32(const std::string& hex, AnsiCanvas::Color32& out)
{
    std::string s = hex;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8)
        return false;

    auto to_u8 = [](const std::string& sub) -> std::uint8_t {
        return (std::uint8_t)std::strtoul(sub.c_str(), nullptr, 16);
    };

    const std::uint8_t r = to_u8(s.substr(0, 2));
    const std::uint8_t g = to_u8(s.substr(2, 2));
    const std::uint8_t b = to_u8(s.substr(4, 2));
    std::uint8_t a = 255;
    if (s.size() == 8)
        a = to_u8(s.substr(6, 2));
    out = PackImGuiCol32(r, g, b, a);
    return true;
}

// ---------------------------------------------------------------------------
// Theme model (Phosphor Markdown Style JSON)
// ---------------------------------------------------------------------------
struct StyleSpec
{
    std::optional<AnsiCanvas::Color32> fg;
    std::optional<AnsiCanvas::Color32> bg;
    AnsiCanvas::Attrs attrs = 0;

    std::optional<std::string> prefix;
    std::optional<std::string> suffix;
    std::optional<std::string> block_prefix;
    std::optional<std::string> block_suffix;
    std::optional<std::string> format;
    std::optional<int> indent;
    std::optional<int> margin;
    std::optional<std::string> indent_token;
};

struct Theme
{
    std::string name;
    std::string author;
    std::unordered_map<std::string, std::string> colors;            // token -> color string
    std::optional<StyleSpec> defaults;
    std::unordered_map<std::string, StyleSpec> elements;            // element name -> style
    // syntax map is reserved for future (code highlighting), but we ignore it for now.
};

static StyleSpec MergeStyle(const StyleSpec& base, const StyleSpec& over)
{
    StyleSpec out = base;
    if (over.fg) out.fg = over.fg;
    if (over.bg) out.bg = over.bg;
    out.attrs = (AnsiCanvas::Attrs)(out.attrs | over.attrs);

    if (over.prefix) out.prefix = over.prefix;
    if (over.suffix) out.suffix = over.suffix;
    if (over.block_prefix) out.block_prefix = over.block_prefix;
    if (over.block_suffix) out.block_suffix = over.block_suffix;
    if (over.format) out.format = over.format;
    if (over.indent) out.indent = over.indent;
    if (over.margin) out.margin = over.margin;
    if (over.indent_token) out.indent_token = over.indent_token;
    return out;
}

static bool ResolveColorString(const Theme& t, const std::string& s, AnsiCanvas::Color32& out, std::string& err, int depth = 0)
{
    if (depth > 16)
    {
        err = "Theme color alias recursion limit exceeded.";
        return false;
    }
    if (s.rfind("name:", 0) == 0)
    {
        const std::string key = s.substr(5);
        auto it = t.colors.find(key);
        if (it == t.colors.end())
        {
            err = std::string("Theme color alias not found: ") + key;
            return false;
        }
        return ResolveColorString(t, it->second, out, err, depth + 1);
    }
    if (!HexToColor32(s, out))
    {
        err = std::string("Invalid color string: ") + s;
        return false;
    }
    return true;
}

static AnsiCanvas::Attrs AttrFromName(const std::string& s)
{
    const std::string k = ToLowerAscii(s);
    if (k == "bold") return AnsiCanvas::Attr_Bold;
    if (k == "dim") return AnsiCanvas::Attr_Dim;
    if (k == "italic") return AnsiCanvas::Attr_Italic;
    if (k == "underline") return AnsiCanvas::Attr_Underline;
    if (k == "blink") return AnsiCanvas::Attr_Blink;
    if (k == "inverse") return AnsiCanvas::Attr_Reverse;
    if (k == "strike") return AnsiCanvas::Attr_Strikethrough;
    // conceal/overline not supported by AnsiCanvas (ignored).
    return 0;
}

static bool ParseStyleSpec(const Theme& theme, const Json& j, StyleSpec& out, std::string& err)
{
    err.clear();
    out = StyleSpec{};
    if (!j.is_object())
        return true;

    auto parse_color = [&](const char* key, std::optional<AnsiCanvas::Color32>& dst) -> bool {
        auto it = j.find(key);
        if (it == j.end())
            return true;
        if (!it->is_string())
            return true;
        AnsiCanvas::Color32 c = 0;
        std::string e;
        if (!ResolveColorString(theme, it->get<std::string>(), c, e))
        {
            err = e;
            return false;
        }
        dst = c;
        return true;
    };

    if (!parse_color("fg", out.fg)) return false;
    if (!parse_color("bg", out.bg)) return false;

    if (auto it = j.find("attrs"); it != j.end() && it->is_array())
    {
        for (const auto& a : *it)
        {
            if (!a.is_string())
                continue;
            out.attrs = (AnsiCanvas::Attrs)(out.attrs | AttrFromName(a.get<std::string>()));
        }
    }

    auto parse_string = [&](const char* key, std::optional<std::string>& dst)
    {
        auto it = j.find(key);
        if (it != j.end() && it->is_string())
            dst = it->get<std::string>();
    };
    parse_string("prefix", out.prefix);
    parse_string("suffix", out.suffix);
    parse_string("block_prefix", out.block_prefix);
    parse_string("block_suffix", out.block_suffix);
    parse_string("format", out.format);
    parse_string("indent_token", out.indent_token);

    auto parse_int = [&](const char* key, std::optional<int>& dst)
    {
        auto it = j.find(key);
        if (it != j.end() && it->is_number_integer())
            dst = it->get<int>();
    };
    parse_int("indent", out.indent);
    parse_int("margin", out.margin);

    return true;
}

static bool LoadThemeFromJson(const Json& j, Theme& out, std::string& err)
{
    err.clear();
    out = Theme{};

    if (!j.is_object())
    {
        err = "Theme JSON must be an object.";
        return false;
    }
    if (auto it = j.find("name"); it != j.end() && it->is_string())
        out.name = it->get<std::string>();
    if (out.name.empty())
        out.name = "(unnamed)";
    if (auto it = j.find("author"); it != j.end() && it->is_string())
        out.author = it->get<std::string>();

    if (auto it = j.find("colors"); it != j.end() && it->is_object())
    {
        for (auto it2 = it->begin(); it2 != it->end(); ++it2)
        {
            if (!it2.value().is_string())
                continue;
            out.colors[it2.key()] = it2.value().get<std::string>();
        }
    }

    // Defaults (optional)
    if (auto it = j.find("defaults"); it != j.end())
    {
        StyleSpec s;
        std::string e;
        if (!ParseStyleSpec(out, *it, s, e))
        {
            err = std::string("Theme defaults: ") + e;
            return false;
        }
        out.defaults = s;
    }

    // Elements map (required by schema, but we tolerate missing and fall back)
    if (auto it = j.find("elements"); it != j.end() && it->is_object())
    {
        for (auto it2 = it->begin(); it2 != it->end(); ++it2)
        {
            StyleSpec s;
            std::string e;
            if (!ParseStyleSpec(out, it2.value(), s, e))
            {
                err = std::string("Theme element '") + it2.key() + "': " + e;
                return false;
            }
            out.elements[it2.key()] = std::move(s);
        }
    }

    return true;
}

static bool LoadThemeFromFile(const std::string& path, Theme& out, std::string& err)
{
    err.clear();
    std::ifstream f(path);
    if (!f)
    {
        err = std::string("Failed to open theme: ") + path;
        return false;
    }
    Json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        err = e.what();
        return false;
    }
    return LoadThemeFromJson(j, out, err);
}

static Theme MinimalTheme()
{
    Theme t;
    t.name = "Minimal";
    // Empty defaults/elements: everything ends up unset (fg=0, bg=0, attrs=0).
    return t;
}

static StyleSpec ResolveElementStyle(const Theme& theme, std::string_view elem)
{
    StyleSpec s;
    if (theme.defaults)
        s = *theme.defaults;

    auto it = theme.elements.find(std::string(elem));
    if (it != theme.elements.end())
        s = MergeStyle(s, it->second);

    return s;
}

static const StyleSpec* FindElementStyleOnly(const Theme& theme, std::string_view elem)
{
    auto it = theme.elements.find(std::string(elem));
    if (it == theme.elements.end())
        return nullptr;
    return &it->second;
}

// ---------------------------------------------------------------------------
// Markdown IR
// ---------------------------------------------------------------------------
enum class BlockKind
{
    Document,
    Paragraph,
    Heading,
    ThematicBreak,
    BlockQuote,
    List,
    ListItem,
    CodeBlock,
};

enum class InlineKind
{
    Text,
    SoftBreak,
    HardBreak,
    Emph,
    Strong,
    Strike,
    CodeSpan,
    Link,
    Image,
};

struct Inline
{
    InlineKind kind = InlineKind::Text;
    std::string text;                 // for Text/CodeSpan, or URL for Link/Image
    std::vector<Inline> children;     // for container spans
};

struct Block
{
    BlockKind kind = BlockKind::Paragraph;
    int heading_level = 0;
    bool ordered = false;
    int list_start = 1;
    std::string info_string;          // code fence language (best effort)
    std::string code_text;            // code block raw text
    std::vector<Inline> inlines;      // paragraph/heading/list-item content, etc.
    std::vector<Block> children;      // nested blocks
};

// ---------------------------------------------------------------------------
// md4c parser -> IR builder
// ---------------------------------------------------------------------------
struct Parser
{
    ImportOptions opt;
    std::string error;

    Block root;
    std::vector<Block*> block_stack;
    std::vector<Inline*> inline_stack;

    std::size_t node_count = 0;
    int max_depth = 0;

    explicit Parser(const ImportOptions& o) : opt(o)
    {
        root.kind = BlockKind::Document;
        block_stack.push_back(&root);
    }

    bool Fail(const std::string& e)
    {
        if (error.empty())
            error = e;
        return false;
    }

    bool BumpNodes()
    {
        // Conservative limit to prevent pathological documents from blowing up memory.
        // This is intentionally lower than "cells" limits because IR is transient and can be nested.
        const std::size_t kMaxNodes = 200000;
        node_count++;
        if (node_count > kMaxNodes)
            return Fail("Markdown document too complex to import (node limit exceeded).");
        return true;
    }

    Block* CurBlock()
    {
        return block_stack.empty() ? nullptr : block_stack.back();
    }

    std::vector<Inline>* CurInlineList()
    {
        if (!inline_stack.empty())
            return &inline_stack.back()->children;
        Block* b = CurBlock();
        if (!b)
            return nullptr;
        return &b->inlines;
    }

    bool PushBlock(BlockKind k)
    {
        if (!BumpNodes())
            return false;
        Block* parent = CurBlock();
        if (!parent)
            return Fail("Internal parser error (no current block).");
        parent->children.emplace_back();
        Block* b = &parent->children.back();
        b->kind = k;
        block_stack.push_back(b);
        max_depth = std::max<int>(max_depth, (int)block_stack.size());
        if (max_depth > 64)
            return Fail("Markdown nesting too deep to import.");
        return true;
    }

    void PopBlock()
    {
        if (block_stack.size() > 1)
            block_stack.pop_back();
    }

    bool PushInline(InlineKind k)
    {
        if (!BumpNodes())
            return false;
        auto* list = CurInlineList();
        if (!list)
            return Fail("Internal parser error (no inline list).");
        list->emplace_back();
        Inline* n = &list->back();
        n->kind = k;
        inline_stack.push_back(n);
        return true;
    }

    void PopInline()
    {
        if (!inline_stack.empty())
            inline_stack.pop_back();
    }

    bool AppendText(InlineKind kind, std::string_view s)
    {
        if (!BumpNodes())
            return false;
        auto* list = CurInlineList();
        if (!list)
            return Fail("Internal parser error (no inline list).");
        Inline n;
        n.kind = kind;
        n.text.assign(s.begin(), s.end());
        list->push_back(std::move(n));
        return true;
    }

    bool AppendToOpenCodeSpan(std::string_view s)
    {
        if (inline_stack.empty())
            return AppendText(InlineKind::Text, s);
        Inline* top = inline_stack.back();
        if (!top || top->kind != InlineKind::CodeSpan)
            return AppendText(InlineKind::Text, s);
        if (!BumpNodes())
            return false;
        top->text.append(s.begin(), s.end());
        return true;
    }
};

static int EnterBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata)
{
    (void)detail;
    Parser& p = *(Parser*)userdata;
    if (!p.error.empty())
        return 1;

    switch (type)
    {
        case MD_BLOCK_DOC:
            return 0;
        case MD_BLOCK_P:
            return p.PushBlock(BlockKind::Paragraph) ? 0 : 1;
        case MD_BLOCK_H:
        {
            if (!p.PushBlock(BlockKind::Heading))
                return 1;
            auto* h = (MD_BLOCK_H_DETAIL*)detail;
            p.CurBlock()->heading_level = (int)h->level;
            return 0;
        }
        case MD_BLOCK_QUOTE:
            return p.PushBlock(BlockKind::BlockQuote) ? 0 : 1;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
        {
            if (!p.PushBlock(BlockKind::List))
                return 1;
            Block* b = p.CurBlock();
            if (type == MD_BLOCK_OL)
            {
                b->ordered = true;
                auto* od = (MD_BLOCK_OL_DETAIL*)detail;
                b->list_start = (int)od->start;
            }
            return 0;
        }
        case MD_BLOCK_LI:
            return p.PushBlock(BlockKind::ListItem) ? 0 : 1;
        case MD_BLOCK_HR:
            return p.PushBlock(BlockKind::ThematicBreak) ? 0 : 1;
        case MD_BLOCK_CODE:
        {
            if (!p.PushBlock(BlockKind::CodeBlock))
                return 1;
            auto* cd = (MD_BLOCK_CODE_DETAIL*)detail;
            if (cd && cd->lang.text && cd->lang.size > 0)
                p.CurBlock()->info_string.assign(cd->lang.text, cd->lang.size);
            return 0;
        }
        default:
            // Ignore other blocks for now (tables, HTML, etc).
            return p.PushBlock(BlockKind::Paragraph) ? 0 : 1;
    }
}

static int LeaveBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata)
{
    (void)detail;
    Parser& p = *(Parser*)userdata;
    if (!p.error.empty())
        return 1;

    switch (type)
    {
        case MD_BLOCK_DOC:
            return 0;
        default:
            p.PopBlock();
            return 0;
    }
}

static int EnterSpanCb(MD_SPANTYPE type, void* detail, void* userdata)
{
    Parser& p = *(Parser*)userdata;
    if (!p.error.empty())
        return 1;
    (void)detail;

    switch (type)
    {
        case MD_SPAN_EM:      return p.PushInline(InlineKind::Emph) ? 0 : 1;
        case MD_SPAN_STRONG:  return p.PushInline(InlineKind::Strong) ? 0 : 1;
        case MD_SPAN_DEL:     return p.PushInline(InlineKind::Strike) ? 0 : 1;
        case MD_SPAN_CODE:
        {
            if (!p.PushInline(InlineKind::CodeSpan))
                return 1;
            // md4c delivers code span content via MD_TEXT_CODE; we accumulate into this node's text.
            p.inline_stack.back()->text.clear();
            return 0;
        }
        case MD_SPAN_A:
        {
            auto* ad = (MD_SPAN_A_DETAIL*)detail;
            if (!p.PushInline(InlineKind::Link))
                return 1;
            if (ad && ad->href.text && ad->href.size > 0)
                p.inline_stack.back()->text.assign(ad->href.text, ad->href.size);
            return 0;
        }
        case MD_SPAN_IMG:
        {
            auto* id = (MD_SPAN_IMG_DETAIL*)detail;
            if (!p.PushInline(InlineKind::Image))
                return 1;
            if (id && id->src.text && id->src.size > 0)
                p.inline_stack.back()->text.assign(id->src.text, id->src.size);
            return 0;
        }
        default:
            return 0;
    }
}

static int LeaveSpanCb(MD_SPANTYPE type, void* detail, void* userdata)
{
    (void)detail;
    Parser& p = *(Parser*)userdata;
    if (!p.error.empty())
        return 1;

    switch (type)
    {
        case MD_SPAN_EM:
        case MD_SPAN_STRONG:
        case MD_SPAN_DEL:
        case MD_SPAN_CODE:
        case MD_SPAN_A:
        case MD_SPAN_IMG:
            p.PopInline();
            return 0;
        default:
            return 0;
    }
}

static int TextCb(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    Parser& p = *(Parser*)userdata;
    if (!p.error.empty())
        return 1;

    std::string_view sv(text ? text : "", (size_t)size);

    // Security/sanity: drop ESC and other C0 controls except \n and \t.
    // (md4c gives us already-normalized line breaks via SOFTBR/HARDBR, but we keep this safe anyway.)
    auto sanitize = [&](std::string_view in) -> std::string
    {
        std::string out;
        out.reserve(in.size());
        for (unsigned char ch : in)
        {
            if (ch == '\n' || ch == '\t')
            {
                out.push_back((char)ch);
                continue;
            }
            if (ch < 0x20 || ch == 0x7F)
                continue;
            out.push_back((char)ch);
        }
        return out;
    };

    // If we're inside a fenced/indented code block, md4c reports raw text chunks via MD_TEXT_CODE.
    // We capture into the current CodeBlock block node and do NOT build inline nodes for it.
    if (Block* b = p.CurBlock(); b && b->kind == BlockKind::CodeBlock)
    {
        if (type == MD_TEXT_CODE || type == MD_TEXT_NORMAL || type == MD_TEXT_ENTITY)
        {
            b->code_text += sanitize(sv);
            return 0;
        }
        if (type == MD_TEXT_SOFTBR || type == MD_TEXT_BR)
        {
            b->code_text.push_back('\n');
            return 0;
        }
        return 0;
    }

    switch (type)
    {
        case MD_TEXT_NORMAL:
        case MD_TEXT_NULLCHAR:
        case MD_TEXT_ENTITY:
            return p.AppendText(InlineKind::Text, sanitize(sv)) ? 0 : 1;
        case MD_TEXT_SOFTBR:
            return p.AppendText(InlineKind::SoftBreak, " ") ? 0 : 1;
        case MD_TEXT_BR:
            return p.AppendText(InlineKind::HardBreak, "\n") ? 0 : 1;
        case MD_TEXT_CODE:
        {
            // Code span inner text.
            const std::string s = sanitize(sv);
            return p.AppendToOpenCodeSpan(s) ? 0 : 1;
        }
        default:
            return 0;
    }
}

static bool ParseMarkdownToIr(std::string_view markdown, Block& out_doc, std::string& err, const ImportOptions& opt)
{
    err.clear();
    Parser p(opt);

    MD_PARSER parser;
    std::memset(&parser, 0, sizeof(parser));
#ifdef MD_PARSER_ABI_VERSION
    // Some md4c versions expose an ABI version field; others don't (or don't provide the macro).
    parser.abi_version = MD_PARSER_ABI_VERSION;
#endif
    parser.flags =
        MD_FLAG_TABLES |
        MD_FLAG_STRIKETHROUGH |
        MD_FLAG_TASKLISTS;
    parser.enter_block = EnterBlockCb;
    parser.leave_block = LeaveBlockCb;
    parser.enter_span = EnterSpanCb;
    parser.leave_span = LeaveSpanCb;
    parser.text = TextCb;

    // Clamp input size defensively (callers should already limit file reads).
    if (markdown.size() > opt.max_input_bytes)
    {
        err = "Markdown input too large to import.";
        return false;
    }

    const int rc = md_parse(markdown.data(), (MD_SIZE)markdown.size(), &parser, &p);
    if (!p.error.empty())
    {
        err = p.error;
        return false;
    }
    if (rc != 0)
    {
        err = "Markdown parse failed.";
        return false;
    }
    out_doc = std::move(p.root);
    return true;
}

// ---------------------------------------------------------------------------
// Layout -> per-cell paint (no ANSI emission; we paint directly into a canvas grid)
// ---------------------------------------------------------------------------
struct ResolvedStyle
{
    AnsiCanvas::Color32 fg = 0;   // 0 = unset
    AnsiCanvas::Color32 bg = 0;   // 0 = unset
    AnsiCanvas::Attrs attrs = 0;
    std::string indent_token;     // optional (for indentation visuals)
    int indent = 0;              // indentation units
    int margin = 0;              // left margin in spaces (v1)
};

static ResolvedStyle ResolveStyleForElement(const Theme& theme, std::string_view elem)
{
    StyleSpec spec = ResolveElementStyle(theme, elem);
    ResolvedStyle rs;
    rs.fg = spec.fg.value_or(0);
    rs.bg = spec.bg.value_or(0);
    rs.attrs = spec.attrs;
    rs.indent = spec.indent.value_or(0);
    rs.margin = spec.margin.value_or(0);
    rs.indent_token = spec.indent_token.value_or(std::string{});
    return rs;
}

struct Cell
{
    char32_t cp = U' ';
    AnsiCanvas::Color32 fg = 0;
    AnsiCanvas::Color32 bg = 0;
    AnsiCanvas::Attrs attrs = 0;
};

struct Line
{
    std::vector<Cell> cells; // visible cells (no trailing padding stored)
};

struct Layout
{
    std::vector<Line> lines;
};

static std::string PlainTextOfInline(const Inline& n)
{
    std::string out;
    if (n.kind == InlineKind::Text || n.kind == InlineKind::CodeSpan)
        out += n.text;
    else if (n.kind == InlineKind::SoftBreak)
        out += " ";
    else if (n.kind == InlineKind::HardBreak)
        out += "\n";
    for (const auto& c : n.children)
        out += PlainTextOfInline(c);
    return out;
}

static void AppendCellsFromUtf8(std::vector<Cell>& dst, std::string_view s, const ResolvedStyle& st)
{
    std::vector<char32_t> cps;
    Utf8ToCodepointsBestEffort(s, cps);
    for (char32_t cp : cps)
    {
        Cell c;
        c.cp = cp;
        c.fg = st.fg;
        c.bg = st.bg;
        c.attrs = st.attrs;
        dst.push_back(c);
    }
}

static void EnsureIndent(Line& line, const ResolvedStyle& st)
{
    // Interpret indent as "repeat count" of indent_token when present; otherwise indent spaces.
    if (st.indent <= 0 && st.margin <= 0)
        return;

    const int margin = std::max(0, st.margin);
    for (int i = 0; i < margin; ++i)
    {
        Cell c;
        c.cp = U' ';
        c.fg = st.fg;
        c.bg = st.bg;
        c.attrs = st.attrs;
        line.cells.push_back(c);
    }

    const int indent = std::max(0, st.indent);
    if (!st.indent_token.empty())
    {
        for (int i = 0; i < indent; ++i)
            AppendCellsFromUtf8(line.cells, st.indent_token, st);
    }
    else
    {
        for (int i = 0; i < indent; ++i)
        {
            Cell c;
            c.cp = U' ';
            c.fg = st.fg;
            c.bg = st.bg;
            c.attrs = st.attrs;
            line.cells.push_back(c);
        }
    }
}

// Append a run of styled cells into layout, with optional wrapping.
// This works at the "cell" level (codepoint == 1 column) which matches the current canvas model.
static void AppendRun(Layout& layout,
                      std::vector<Cell>& cur,
                      int width,
                      bool wrap,
                      const std::vector<Cell>& run)
{
    width = std::max(1, width);
    auto flush_line = [&]() {
        Line ln;
        ln.cells = std::move(cur);
        layout.lines.push_back(std::move(ln));
        cur.clear();
    };

    // Track last whitespace position in `cur` for wrap opportunities.
    // We treat ASCII spaces only; this keeps behavior deterministic.
    auto is_break_space = [](char32_t cp) -> bool { return cp == U' '; };

    size_t i = 0;
    while (i < run.size())
    {
        if (!wrap)
        {
            cur.push_back(run[i]);
            ++i;
            if ((int)cur.size() >= width)
            {
                flush_line();
            }
            continue;
        }

        // If it fits, append.
        if ((int)cur.size() < width)
        {
            cur.push_back(run[i]);
            ++i;
            continue;
        }

        // Line is full: try to wrap at the last space in the current line.
        int last_space = -1;
        for (int k = (int)cur.size() - 1; k >= 0; --k)
        {
            if (is_break_space(cur[(size_t)k].cp))
            {
                last_space = k;
                break;
            }
        }

        if (last_space >= 0)
        {
            // Move trailing content after the space to next line.
            std::vector<Cell> carry;
            carry.insert(carry.end(), cur.begin() + (size_t)last_space + 1, cur.end());
            cur.resize((size_t)last_space); // drop space + tail
            flush_line();

            // Drop leading spaces in carry.
            size_t drop = 0;
            while (drop < carry.size() && is_break_space(carry[drop].cp))
                drop++;
            if (drop > 0)
                carry.erase(carry.begin(), carry.begin() + (ptrdiff_t)drop);

            cur.insert(cur.end(), carry.begin(), carry.end());
        }
        else
        {
            // No space to break: hard-wrap.
            flush_line();
        }
    }
}

static void AppendText(Layout& layout,
                       std::vector<Cell>& cur,
                       int width,
                       bool wrap,
                       std::string_view text,
                       const ResolvedStyle& style)
{
    std::vector<Cell> run;
    run.reserve(text.size());
    AppendCellsFromUtf8(run, text, style);
    AppendRun(layout, cur, width, wrap, run);
}

static void AppendInline(Layout& layout,
                         std::vector<Cell>& cur,
                         int width,
                         bool wrap,
                         const Theme& theme,
                         const ImportOptions& opt,
                         const Inline& n,
                         std::vector<std::string_view>& style_stack)
{
    auto push_style = [&](std::string_view s) { style_stack.push_back(s); };
    auto pop_style = [&]() { if (!style_stack.empty()) style_stack.pop_back(); };

    auto resolve_current_style = [&]() -> ResolvedStyle
    {
        StyleSpec merged;
        if (theme.defaults)
            merged = *theme.defaults;
        for (auto key : style_stack)
        {
            if (const StyleSpec* s = FindElementStyleOnly(theme, key))
                merged = MergeStyle(merged, *s);
        }

        ResolvedStyle rs;
        rs.fg = merged.fg.value_or(0);
        rs.bg = merged.bg.value_or(0);
        rs.attrs = merged.attrs;
        rs.indent = merged.indent.value_or(0);
        rs.margin = merged.margin.value_or(0);
        rs.indent_token = merged.indent_token.value_or(std::string{});
        return rs;
    };

    auto apply_format_if_any = [&](std::string_view elem_key, std::string_view fallback_text) -> std::string
    {
        StyleSpec spec = ResolveElementStyle(theme, elem_key);
        if (!spec.format)
            return std::string(fallback_text);
        std::string fmt = *spec.format;
        const std::string inner = std::string(fallback_text);
        // Simple "{text}" substitution.
        const std::string needle = "{text}";
        size_t pos = 0;
        while ((pos = fmt.find(needle, pos)) != std::string::npos)
        {
            fmt.replace(pos, needle.size(), inner);
            pos += inner.size();
        }
        return fmt;
    };

    switch (n.kind)
    {
        case InlineKind::Text:
        {
            const ResolvedStyle st = resolve_current_style();
            AppendText(layout, cur, width, wrap, n.text, st);
            break;
        }
        case InlineKind::SoftBreak:
        {
            if (opt.soft_break == ImportOptions::SoftBreak::Newline)
            {
                // Hard line break.
                Line ln;
                ln.cells = std::move(cur);
                layout.lines.push_back(std::move(ln));
                cur.clear();
            }
            else
            {
                const ResolvedStyle st = resolve_current_style();
                AppendText(layout, cur, width, wrap, " ", st);
            }
            break;
        }
        case InlineKind::HardBreak:
        {
            Line ln;
            ln.cells = std::move(cur);
            layout.lines.push_back(std::move(ln));
            cur.clear();
            break;
        }
        case InlineKind::Emph:
        {
            push_style("emph");
            for (const auto& c : n.children)
                AppendInline(layout, cur, width, wrap, theme, opt, c, style_stack);
            pop_style();
            break;
        }
        case InlineKind::Strong:
        {
            push_style("strong");
            for (const auto& c : n.children)
                AppendInline(layout, cur, width, wrap, theme, opt, c, style_stack);
            pop_style();
            break;
        }
        case InlineKind::Strike:
        {
            push_style("strikethrough");
            for (const auto& c : n.children)
                AppendInline(layout, cur, width, wrap, theme, opt, c, style_stack);
            pop_style();
            break;
        }
        case InlineKind::CodeSpan:
        {
            push_style("code_inline");
            // Apply optional prefix/suffix from the element style.
            const StyleSpec spec = ResolveElementStyle(theme, "code_inline");
            const ResolvedStyle st = resolve_current_style();
            if (spec.prefix)
                AppendText(layout, cur, width, wrap, *spec.prefix, st);
            AppendText(layout, cur, width, wrap, n.text, st);
            if (spec.suffix)
                AppendText(layout, cur, width, wrap, *spec.suffix, st);
            pop_style();
            break;
        }
        case InlineKind::Link:
        {
            // Label (link_text)
            push_style("link_text");
            const std::string label_plain = [&]() {
                std::string s;
                for (const auto& c : n.children)
                    s += PlainTextOfInline(c);
                return s;
            }();

            // Apply optional formatting.
            const std::string label_fmt = apply_format_if_any("link_text", label_plain);
            const ResolvedStyle st_label = resolve_current_style();
            AppendText(layout, cur, width, wrap, label_fmt, st_label);
            pop_style();

            if (opt.link_mode == ImportOptions::LinkMode::InlineUrl && !n.text.empty())
            {
                push_style("link");
                const ResolvedStyle st_url = resolve_current_style();
                AppendText(layout, cur, width, wrap, " (", st_url);
                AppendText(layout, cur, width, wrap, n.text, st_url);
                AppendText(layout, cur, width, wrap, ")", st_url);
                pop_style();
            }
            break;
        }
        case InlineKind::Image:
        {
            // Render as placeholder text (alt text if present).
            push_style("image_text");
            std::string alt;
            for (const auto& c : n.children)
                alt += PlainTextOfInline(c);
            if (alt.empty())
                alt = "image";
            const std::string text = apply_format_if_any("image_text", alt);
            const ResolvedStyle st = resolve_current_style();
            AppendText(layout, cur, width, wrap, text, st);
            pop_style();
            break;
        }
        default:
            break;
    }
}

static void AppendBlock(Layout& layout,
                        const Theme& theme,
                        const ImportOptions& opt,
                        const Block& b,
                        int width,
                        int quote_depth = 0)
{
    auto push_line = [&](Line&& ln) {
        layout.lines.push_back(std::move(ln));
    };

    auto flush_cur = [&](std::vector<Cell>& cur) {
        Line ln;
        ln.cells = std::move(cur);
        layout.lines.push_back(std::move(ln));
        cur.clear();
    };

    auto append_blank_line = [&]() {
        if (opt.preserve_blank_lines)
            layout.lines.push_back(Line{});
    };

    auto apply_block_prefix_suffix = [&](std::string_view elem, bool prefix) {
        StyleSpec spec = ResolveElementStyle(theme, elem);
        const std::optional<std::string>& s = prefix ? spec.block_prefix : spec.block_suffix;
        if (!s || s->empty())
            return;
        ResolvedStyle st = ResolveStyleForElement(theme, elem);

        // Split on '\n' for multi-line block prefixes/suffixes.
        size_t start = 0;
        while (start <= s->size())
        {
            const size_t nl = s->find('\n', start);
            const size_t end = (nl == std::string::npos) ? s->size() : nl;
            Line ln;
            AppendCellsFromUtf8(ln.cells, std::string_view(*s).substr(start, end - start), st);
            push_line(std::move(ln));
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
    };

    switch (b.kind)
    {
        case BlockKind::Document:
            for (const auto& c : b.children)
                AppendBlock(layout, theme, opt, c, width, quote_depth);
            break;

        case BlockKind::ThematicBreak:
        {
            ResolvedStyle st = ResolveStyleForElement(theme, "hr");
            Line ln;
            const int cols = std::max(1, width);
            ln.cells.reserve((size_t)cols);
            for (int i = 0; i < cols; ++i)
            {
                Cell c;
                c.cp = opt.hr_glyph ? opt.hr_glyph : U'-';
                c.fg = st.fg;
                c.bg = st.bg;
                c.attrs = st.attrs;
                ln.cells.push_back(c);
            }
            push_line(std::move(ln));
            append_blank_line();
            break;
        }

        case BlockKind::CodeBlock:
        {
            ResolvedStyle st = ResolveStyleForElement(theme, "code_block");
            apply_block_prefix_suffix("code_block", true);

            // Optional language header line (v1, simple).
            if (opt.show_code_language && !b.info_string.empty())
            {
                Line ln;
                AppendCellsFromUtf8(ln.cells, "```", st);
                AppendCellsFromUtf8(ln.cells, b.info_string, st);
                push_line(std::move(ln));
            }

            // Split raw code text into lines.
            size_t start = 0;
            while (start <= b.code_text.size())
            {
                const size_t nl = b.code_text.find('\n', start);
                const size_t end = (nl == std::string::npos) ? b.code_text.size() : nl;
                std::string_view line_sv(b.code_text.data() + start, end - start);

                Line ln;
                // Apply indentation (if any) requested by the style.
                // For code blocks, indent/margin typically come from theme.defaults or code_block.
                EnsureIndent(ln, st);
                AppendCellsFromUtf8(ln.cells, line_sv, st);
                push_line(std::move(ln));

                if (nl == std::string::npos)
                    break;
                start = nl + 1;
            }

            apply_block_prefix_suffix("code_block", false);
            append_blank_line();
            break;
        }

        case BlockKind::Heading:
        case BlockKind::Paragraph:
        case BlockKind::ListItem:
        {
            const bool is_heading = (b.kind == BlockKind::Heading);
            const bool is_item = (b.kind == BlockKind::ListItem);

            std::string elem = "paragraph";
            if (is_heading)
            {
                const int lvl = ClampInt(b.heading_level, 1, 6);
                elem = "h" + std::to_string(lvl);
            }
            else if (is_item)
            {
                elem = "item";
            }

            ResolvedStyle st = ResolveStyleForElement(theme, elem);
            apply_block_prefix_suffix(elem, true);

            std::vector<Cell> cur;
            // Apply block indentation/margin.
            {
                Line tmp;
                EnsureIndent(tmp, st);
                cur.insert(cur.end(), tmp.cells.begin(), tmp.cells.end());
            }

            // Quote prefix (simple: "> " per nesting level).
            if (quote_depth > 0)
            {
                ResolvedStyle qst = ResolveStyleForElement(theme, "block_quote");
                for (int i = 0; i < quote_depth; ++i)
                {
                    AppendCellsFromUtf8(cur, "> ", qst);
                }
            }

            // List bullet prefix (simple).
            if (is_item)
            {
                // Use either enumeration (ordered) or list bullet.
                const bool ordered = false; // v1: md4c provides ordered at List block, but item doesn't carry number; keep simple.
                ResolvedStyle bst = ResolveStyleForElement(theme, ordered ? "enumeration" : "list");
                AppendCellsFromUtf8(cur, ordered ? "1. " : "- ", bst);
            }

            // Inline content
            std::vector<std::string_view> style_stack;
            style_stack.push_back(elem);
            const bool wrap = opt.wrap_paragraphs && (b.kind != BlockKind::Heading);

            for (const auto& inl : b.inlines)
                AppendInline(layout, cur, width, wrap, theme, opt, inl, style_stack);

            flush_cur(cur);
            apply_block_prefix_suffix(elem, false);
            append_blank_line();
            break;
        }

        case BlockKind::BlockQuote:
        {
            apply_block_prefix_suffix("block_quote", true);
            for (const auto& c : b.children)
                AppendBlock(layout, theme, opt, c, width, quote_depth + 1);
            apply_block_prefix_suffix("block_quote", false);
            break;
        }

        case BlockKind::List:
        {
            // Render children list items.
            apply_block_prefix_suffix("list", true);
            for (const auto& c : b.children)
                AppendBlock(layout, theme, opt, c, width, quote_depth);
            apply_block_prefix_suffix("list", false);
            break;
        }

        default:
            break;
    }
}

static bool LayoutAndPaint(const Block& doc, const Theme& theme, const ImportOptions& opt, AnsiCanvas& out, std::string& err)
{
    err.clear();
    const int cols = ClampInt(opt.columns, 1, 4096);
    const int max_rows = ClampInt(opt.max_rows, 1, 200000);

    Layout layout;
    AppendBlock(layout, theme, opt, doc, cols, 0);

    if (layout.lines.empty())
        layout.lines.push_back(Line{});

    const int rows = std::min<int>((int)layout.lines.size(), max_rows);

    // Build a single-layer project state, like other importers.
    AnsiCanvas::ProjectState st;
    st.version = 6;
    st.undo_limit = 0;
    st.current.columns = cols;
    st.current.rows = std::max(1, rows);
    st.current.active_layer = 0;
    st.current.caret_row = 0;
    st.current.caret_col = 0;
    st.current.layers.clear();
    st.current.layers.resize(1);
    st.current.layers[0].name = "Base";
    st.current.layers[0].visible = true;

    const size_t total = (size_t)st.current.rows * (size_t)st.current.columns;
    st.current.layers[0].cells.assign(total, U' ');
    st.current.layers[0].fg.assign(total, 0);
    st.current.layers[0].bg.assign(total, 0);
    st.current.layers[0].attrs.assign(total, 0);

    auto at = [&](int r, int c) -> size_t {
        return (size_t)r * (size_t)cols + (size_t)c;
    };

    for (int r = 0; r < rows; ++r)
    {
        const Line& ln = layout.lines[(size_t)r];
        const int n = std::min<int>((int)ln.cells.size(), cols);
        for (int c = 0; c < n; ++c)
        {
            const Cell& cell = ln.cells[(size_t)c];
            const size_t idx = at(r, c);
            st.current.layers[0].cells[idx] = cell.cp;
            st.current.layers[0].fg[idx] = cell.fg;
            st.current.layers[0].bg[idx] = cell.bg;
            st.current.layers[0].attrs[idx] = cell.attrs;
        }
    }

    AnsiCanvas canvas(cols);
    std::string apply_err;
    if (!canvas.SetProjectState(st, apply_err))
    {
        err = apply_err.empty() ? "Failed to apply Markdown import state." : apply_err;
        return false;
    }
    out = std::move(canvas);
    return true;
}

static std::string DefaultThemePath()
{
    return PhosphorAssetPath("md-styles/dark.json");
}

} // namespace

bool ListBuiltinThemes(std::vector<ThemeInfo>& out, std::string& err)
{
    err.clear();
    out.clear();

    const std::string dir = PhosphorAssetPath("md-styles");
    std::error_code ec;
    if (!fs::exists(dir, ec))
    {
        err = "Markdown themes directory not found in assets.";
        return false;
    }

    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;
        fs::path p = entry.path();
        if (ToLowerAscii(p.extension().string()) != ".json")
            continue;

        Theme t;
        std::string terr;
        if (!LoadThemeFromFile(p.string(), t, terr))
            continue;

        ThemeInfo info;
        info.path = p.string();
        info.name = t.name;
        info.author = t.author;
        out.push_back(std::move(info));
    }

    if (out.empty())
    {
        err = "No Markdown themes found.";
        return false;
    }

    std::sort(out.begin(), out.end(), [](const ThemeInfo& a, const ThemeInfo& b) {
        return a.name < b.name;
    });
    return true;
}

bool ImportMarkdownToCanvas(std::string_view markdown_utf8,
                           AnsiCanvas& out_canvas,
                           std::string& err,
                           const ImportOptions& opt)
{
    err.clear();

    // Parse Markdown -> IR
    Block doc;
    std::string perr;
    if (!ParseMarkdownToIr(markdown_utf8, doc, perr, opt))
    {
        err = perr.empty() ? "Failed to parse Markdown." : perr;
        return false;
    }

    // Load theme
    Theme theme = MinimalTheme();
    {
        std::string path = opt.theme_path.empty() ? DefaultThemePath() : opt.theme_path;
        std::string terr;
        Theme loaded;
        if (LoadThemeFromFile(path, loaded, terr))
            theme = std::move(loaded);
        // else: silently fall back to MinimalTheme (dialog shows error from conversion if desired).
    }

    // Layout -> paint
    std::string rerr;
    if (!LayoutAndPaint(doc, theme, opt, out_canvas, rerr))
    {
        err = rerr.empty() ? "Failed to render Markdown." : rerr;
        return false;
    }

    return true;
}
} // namespace markdown
} // namespace formats


