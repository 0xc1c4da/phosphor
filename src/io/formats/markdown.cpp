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
    Table,
    TableRow,
    TableCell,
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
    bool list_is_tight = false;
    char ul_mark = '-';
    char ol_delim = '.';
    bool list_item_is_task = false;
    char list_item_task_mark = ' ';
    std::string info_string;          // code fence language (best effort)
    std::string code_text;            // code block raw text
    std::vector<Inline> inlines;      // paragraph/heading/list-item content, etc.
    std::vector<Block> children;      // nested blocks

    // Table cell detail.
    bool table_is_header_cell = false;
    MD_ALIGN table_align = MD_ALIGN_DEFAULT;
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
                b->list_is_tight = (od->is_tight != 0);
                b->ol_delim = (char)od->mark_delimiter;
            }
            else
            {
                auto* ud = (MD_BLOCK_UL_DETAIL*)detail;
                b->list_is_tight = (ud && ud->is_tight != 0);
                b->ul_mark = ud ? (char)ud->mark : '-';
            }
            return 0;
        }
        case MD_BLOCK_LI:
        {
            if (!p.PushBlock(BlockKind::ListItem))
                return 1;
            auto* ld = (MD_BLOCK_LI_DETAIL*)detail;
            if (ld)
            {
                p.CurBlock()->list_item_is_task = (ld->is_task != 0);
                p.CurBlock()->list_item_task_mark = (char)ld->task_mark;
            }
            return 0;
        }
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
        case MD_BLOCK_TABLE:
            return p.PushBlock(BlockKind::Table) ? 0 : 1;
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
            // We don't need separate nodes for thead/tbody; keep structure simple.
            return 0;
        case MD_BLOCK_TR:
            return p.PushBlock(BlockKind::TableRow) ? 0 : 1;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
        {
            if (!p.PushBlock(BlockKind::TableCell))
                return 1;
            Block* cell = p.CurBlock();
            cell->table_is_header_cell = (type == MD_BLOCK_TH);
            auto* td = (MD_BLOCK_TD_DETAIL*)detail;
            cell->table_align = td ? td->align : MD_ALIGN_DEFAULT;
            return 0;
        }
        default:
            // Ignore other blocks for now (HTML, math, etc.). Text callbacks will attach to
            // the nearest supported ancestor block.
            return 0;
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
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
            // We did not push a block for these container nodes.
            return 0;
        default:
            // Only pop for block types we actually pushed in EnterBlockCb.
            switch (type)
            {
                case MD_BLOCK_P:
                case MD_BLOCK_H:
                case MD_BLOCK_QUOTE:
                case MD_BLOCK_UL:
                case MD_BLOCK_OL:
                case MD_BLOCK_LI:
                case MD_BLOCK_HR:
                case MD_BLOCK_CODE:
                case MD_BLOCK_TABLE:
                case MD_BLOCK_TR:
                case MD_BLOCK_TH:
                case MD_BLOCK_TD:
                    p.PopBlock();
                    break;
                default:
                    // Ignored blocks (e.g. HTML) were not pushed; do nothing.
                    break;
            }
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

static int VisibleWidthCells(std::string_view s)
{
    std::vector<char32_t> cps;
    Utf8ToCodepointsBestEffort(s, cps);
    return (int)cps.size();
}

// Wrap-aware line builder which preserves a continuation prefix for wrapped lines.
struct WrapCtx
{
    Layout& layout;
    int width = 1;
    bool wrap = true;
    std::vector<Cell> cur;
    std::vector<Cell> cont_prefix;
    int wrap_min_index = 0; // don't wrap inside prefix

    explicit WrapCtx(Layout& l) : layout(l) {}

    void Start(const std::vector<Cell>& first_prefix, const std::vector<Cell>& continuation_prefix)
    {
        cur = first_prefix;
        cont_prefix = continuation_prefix;
        wrap_min_index = (int)cur.size();
    }

    void FlushLine()
    {
        Line ln;
        ln.cells = std::move(cur);
        layout.lines.push_back(std::move(ln));
        cur = cont_prefix;
        wrap_min_index = (int)cur.size();
    }

    void FinishLine()
    {
        Line ln;
        ln.cells = std::move(cur);
        layout.lines.push_back(std::move(ln));
        cur.clear();
        cont_prefix.clear();
        wrap_min_index = 0;
    }
};

static void AppendRun(WrapCtx& ctx, const std::vector<Cell>& run)
{
    ctx.width = std::max(1, ctx.width);

    auto is_break_space = [](char32_t cp) -> bool { return cp == U' '; };

    size_t i = 0;
    while (i < run.size())
    {
        if (!ctx.wrap)
        {
            ctx.cur.push_back(run[i]);
            ++i;
            if ((int)ctx.cur.size() >= ctx.width)
                ctx.FlushLine();
            continue;
        }

        if ((int)ctx.cur.size() < ctx.width)
        {
            ctx.cur.push_back(run[i]);
            ++i;
            continue;
        }

        int last_space = -1;
        for (int k = (int)ctx.cur.size() - 1; k >= ctx.wrap_min_index; --k)
        {
            if (is_break_space(ctx.cur[(size_t)k].cp))
            {
                last_space = k;
                break;
            }
        }

        if (last_space >= 0)
        {
            std::vector<Cell> carry;
            carry.insert(carry.end(), ctx.cur.begin() + (size_t)last_space + 1, ctx.cur.end());
            ctx.cur.resize((size_t)last_space); // drop space + tail
            ctx.FlushLine();

            size_t drop = 0;
            while (drop < carry.size() && is_break_space(carry[drop].cp))
                drop++;
            if (drop > 0)
                carry.erase(carry.begin(), carry.begin() + (ptrdiff_t)drop);

            ctx.cur.insert(ctx.cur.end(), carry.begin(), carry.end());
        }
        else
        {
            ctx.FlushLine();
        }
    }
}

static void AppendText(WrapCtx& ctx, std::string_view text, const ResolvedStyle& style)
{
    std::vector<Cell> run;
    run.reserve(text.size());
    AppendCellsFromUtf8(run, text, style);
    AppendRun(ctx, run);
}

static void AppendInline(WrapCtx& ctx,
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
            AppendText(ctx, n.text, st);
            break;
        }
        case InlineKind::SoftBreak:
        {
            if (opt.soft_break == ImportOptions::SoftBreak::Newline)
            {
                ctx.FlushLine();
            }
            else
            {
                const ResolvedStyle st = resolve_current_style();
                AppendText(ctx, " ", st);
            }
            break;
        }
        case InlineKind::HardBreak:
        {
            ctx.FlushLine();
            break;
        }
        case InlineKind::Emph:
        {
            push_style("emph");
            for (const auto& c : n.children)
                AppendInline(ctx, theme, opt, c, style_stack);
            pop_style();
            break;
        }
        case InlineKind::Strong:
        {
            push_style("strong");
            for (const auto& c : n.children)
                AppendInline(ctx, theme, opt, c, style_stack);
            pop_style();
            break;
        }
        case InlineKind::Strike:
        {
            push_style("strikethrough");
            for (const auto& c : n.children)
                AppendInline(ctx, theme, opt, c, style_stack);
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
                AppendText(ctx, *spec.prefix, st);
            AppendText(ctx, n.text, st);
            if (spec.suffix)
                AppendText(ctx, *spec.suffix, st);
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
            AppendText(ctx, label_fmt, st_label);
            pop_style();

            if (opt.link_mode == ImportOptions::LinkMode::InlineUrl && !n.text.empty())
            {
                push_style("link");
                const ResolvedStyle st_url = resolve_current_style();
                AppendText(ctx, " (", st_url);
                AppendText(ctx, n.text, st_url);
                AppendText(ctx, ")", st_url);
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
            AppendText(ctx, text, st);
            pop_style();
            break;
        }
        default:
            break;
    }
}

static std::optional<std::string> InlinePrefixForElement(const Theme& theme, std::string_view elem)
{
    StyleSpec spec = ResolveElementStyle(theme, elem);
    if (spec.prefix && !spec.prefix->empty())
        return *spec.prefix;
    // Many existing themes use block_prefix for inline-ish prefixes (e.g. list markers).
    if (spec.block_prefix && !spec.block_prefix->empty() && spec.block_prefix->find('\n') == std::string::npos)
        return *spec.block_prefix;
    return std::nullopt;
}

static std::optional<std::string> InlineSuffixForElement(const Theme& theme, std::string_view elem)
{
    StyleSpec spec = ResolveElementStyle(theme, elem);
    if (spec.suffix && !spec.suffix->empty())
        return *spec.suffix;
    if (spec.block_suffix && !spec.block_suffix->empty() && spec.block_suffix->find('\n') == std::string::npos)
        return *spec.block_suffix;
    return std::nullopt;
}

static std::vector<Inline> ExtractListItemInlinesBestEffort(const Block& li)
{
    if (!li.inlines.empty())
        return li.inlines;
    for (const auto& c : li.children)
    {
        if (c.kind == BlockKind::Paragraph && !c.inlines.empty())
            return c.inlines;
    }
    return {};
}

static std::vector<Inline> ExtractTableCellInlinesBestEffort(const Block& cell)
{
    if (!cell.inlines.empty())
        return cell.inlines;

    // md4c typically nests a paragraph inside TD cells.
    std::vector<Inline> out;
    bool first_para = true;
    for (const auto& c : cell.children)
    {
        if (c.kind != BlockKind::Paragraph)
            continue;
        if (c.inlines.empty())
            continue;
        if (!first_para)
        {
            Inline br;
            br.kind = InlineKind::HardBreak;
            br.text = "\n";
            out.push_back(std::move(br));
        }
        out.insert(out.end(), c.inlines.begin(), c.inlines.end());
        first_para = false;
    }
    return out;
}

static std::vector<Line> RenderInlinesToLines(const Theme& theme,
                                              const ImportOptions& opt,
                                              const std::vector<Inline>& inlines,
                                              int width,
                                              bool wrap,
                                              std::initializer_list<std::string_view> style_keys,
                                              const std::vector<Cell>& first_prefix = {},
                                              const std::vector<Cell>& cont_prefix = {})
{
    Layout tmp;
    WrapCtx ctx(tmp);
    ctx.width = std::max(1, width);
    ctx.wrap = wrap;
    ctx.Start(first_prefix, cont_prefix);

    std::vector<std::string_view> style_stack;
    for (auto k : style_keys)
        style_stack.push_back(k);

    for (const auto& inl : inlines)
        AppendInline(ctx, theme, opt, inl, style_stack);

    ctx.FinishLine();
    return std::move(tmp.lines);
}

static void AppendTable(Layout& layout,
                        const Theme& theme,
                        const ImportOptions& opt,
                        const Block& table,
                        int width,
                        int quote_depth)
{
    // Gather rows/cells.
    std::vector<const Block*> rows;
    for (const auto& c : table.children)
    {
        if (c.kind == BlockKind::TableRow)
            rows.push_back(&c);
    }
    if (rows.empty())
        return;

    int col_count = 0;
    for (auto* r : rows)
        col_count = std::max<int>(col_count, (int)r->children.size());
    if (col_count <= 0)
        return;

    // Build quote prefix cells (best-effort; matches existing quote rendering style).
    std::vector<Cell> quote_prefix;
    if (quote_depth > 0)
    {
        ResolvedStyle qst = ResolveStyleForElement(theme, "block_quote");
        for (int i = 0; i < quote_depth; ++i)
            AppendCellsFromUtf8(quote_prefix, "> ", qst);
    }

    // Compute natural column widths from plain text.
    std::vector<int> colw((size_t)col_count, 1);
    for (auto* r : rows)
    {
        for (int c = 0; c < col_count; ++c)
        {
            if (c >= (int)r->children.size())
                continue;
            const Block& cell = r->children[(size_t)c];
            std::string plain;
            const std::vector<Inline> inls = ExtractTableCellInlinesBestEffort(cell);
            for (const auto& inl : inls)
                plain += PlainTextOfInline(inl);
            for (char& ch : plain)
            {
                if (ch == '\n' || ch == '\r')
                    ch = ' ';
            }
            colw[(size_t)c] = std::max(colw[(size_t)c], VisibleWidthCells(plain));
        }
    }

    // Fit to available width (accounting for borders/padding and quote prefix).
    const int border_overhead = 1 + col_count * 3; // '|' + (space+cell+space+'|') per col
    const int avail = std::max(8, width - (int)quote_prefix.size());
    int total = border_overhead;
    for (int w : colw) total += w;

    if (total > avail)
    {
        // Greedy shrink widest columns first, keep minimum 3.
        while (total > avail)
        {
            int best = -1;
            for (int i = 0; i < col_count; ++i)
            {
                if (colw[(size_t)i] > 3 && (best < 0 || colw[(size_t)i] > colw[(size_t)best]))
                    best = i;
            }
            if (best < 0)
                break;
            colw[(size_t)best]--;
            total--;
        }
    }

    ResolvedStyle border_st = ResolveStyleForElement(theme, "table");

    auto push_border_line = [&](char left, char mid, char right, char fill)
    {
        Line ln;
        ln.cells.insert(ln.cells.end(), quote_prefix.begin(), quote_prefix.end());
        AppendCellsFromUtf8(ln.cells, std::string(1, left), border_st);
        for (int c = 0; c < col_count; ++c)
        {
            AppendCellsFromUtf8(ln.cells, std::string((size_t)colw[(size_t)c] + 2, fill), border_st);
            AppendCellsFromUtf8(ln.cells, std::string(1, (c == col_count - 1) ? right : mid), border_st);
        }
        layout.lines.push_back(std::move(ln));
    };

    auto push_pipe_row = [&](const std::vector<std::vector<Line>>& cell_lines, bool is_header_row)
    {
        // Determine max wrapped line count.
        size_t max_lines = 1;
        for (const auto& cl : cell_lines)
            max_lines = std::max(max_lines, cl.size());

        for (size_t li = 0; li < max_lines; ++li)
        {
            Line out;
            out.cells.insert(out.cells.end(), quote_prefix.begin(), quote_prefix.end());
            AppendCellsFromUtf8(out.cells, "|", border_st);
            for (int c = 0; c < col_count; ++c)
            {
                AppendCellsFromUtf8(out.cells, " ", border_st);

                const bool have = (c < (int)cell_lines.size() && li < cell_lines[(size_t)c].size());
                const std::vector<Cell>* src_cells = have ? &cell_lines[(size_t)c][li].cells : nullptr;

                int pad_width = colw[(size_t)c];
                int used = 0;
                if (src_cells)
                {
                    const int take = std::min<int>((int)src_cells->size(), pad_width);
                    out.cells.insert(out.cells.end(), src_cells->begin(), src_cells->begin() + take);
                    used = take;
                }
                if (used < pad_width)
                {
                    std::vector<Cell> spaces;
                    spaces.reserve((size_t)(pad_width - used));
                    for (int k = 0; k < pad_width - used; ++k)
                    {
                        Cell sc;
                        sc.cp = U' ';
                        sc.fg = is_header_row ? border_st.fg : border_st.fg;
                        sc.bg = border_st.bg;
                        sc.attrs = border_st.attrs;
                        spaces.push_back(sc);
                    }
                    out.cells.insert(out.cells.end(), spaces.begin(), spaces.end());
                }

                AppendCellsFromUtf8(out.cells, " |", border_st);
            }
            layout.lines.push_back(std::move(out));
        }
    };

    // Render top border.
    push_border_line('+', '+', '+', '-');

    // Render each row with wrapped cells.
    for (size_t ri = 0; ri < rows.size(); ++ri)
    {
        const Block& row = *rows[ri];
        bool header_row = false;
        for (const auto& cell : row.children)
            header_row = header_row || cell.table_is_header_cell;

        std::vector<std::vector<Line>> cell_rendered;
        cell_rendered.resize((size_t)col_count);
        for (int c = 0; c < col_count; ++c)
        {
            if (c >= (int)row.children.size())
            {
                cell_rendered[(size_t)c] = {Line{}};
                continue;
            }
            const Block& cell = row.children[(size_t)c];
            const std::vector<Inline> inls = ExtractTableCellInlinesBestEffort(cell);
            // Cells get their own base style role; header cells can override with table_head if present.
            const char* base = header_row ? "table_head" : "table_row";
            const auto lines = RenderInlinesToLines(theme, opt, inls, colw[(size_t)c], true, {base});
            cell_rendered[(size_t)c] = lines;
        }

        push_pipe_row(cell_rendered, header_row);
        // Separator after each row.
        push_border_line('+', '+', '+', '-');
    }

    if (opt.preserve_blank_lines)
        layout.lines.push_back(Line{});
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

    auto append_blank_line = [&]() {
        if (opt.preserve_blank_lines)
            layout.lines.push_back(Line{});
    };

    auto apply_block_prefix_suffix = [&](std::string_view elem, bool prefix) {
        StyleSpec spec = ResolveElementStyle(theme, elem);
        const std::optional<std::string>& s = prefix ? spec.block_prefix : spec.block_suffix;
        if (!s || s->empty())
            return;
        // Heuristic: if the block prefix/suffix has no newline, treat it as an inline prefix/suffix
        // (used by current bundled themes for list markers) and do not emit it as standalone lines.
        if (s->find('\n') == std::string::npos)
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
        {
            const bool is_heading = (b.kind == BlockKind::Heading);

            std::string elem = "paragraph";
            if (is_heading)
            {
                const int lvl = ClampInt(b.heading_level, 1, 6);
                elem = "h" + std::to_string(lvl);
            }

            ResolvedStyle st = ResolveStyleForElement(theme, elem);
            apply_block_prefix_suffix(elem, true);

            // Prefixes (indent + quote + style prefix).
            std::vector<Cell> first_prefix;
            std::vector<Cell> cont_prefix;

            {
                Line tmp;
                EnsureIndent(tmp, st);
                first_prefix.insert(first_prefix.end(), tmp.cells.begin(), tmp.cells.end());
                cont_prefix.insert(cont_prefix.end(), tmp.cells.begin(), tmp.cells.end());
            }

            // Quote prefix (simple: "> " per nesting level).
            if (quote_depth > 0)
            {
                ResolvedStyle qst = ResolveStyleForElement(theme, "block_quote");
                for (int i = 0; i < quote_depth; ++i)
                {
                    AppendCellsFromUtf8(first_prefix, "> ", qst);
                    AppendCellsFromUtf8(cont_prefix, "> ", qst);
                }
            }

            // Element inline prefix (e.g. heading hashes in bundled themes).
            {
                if (auto pre = InlinePrefixForElement(theme, elem))
                {
                    const ResolvedStyle pst = ResolveStyleForElement(theme, elem);
                    AppendCellsFromUtf8(first_prefix, *pre, pst);
                }
            }

            WrapCtx ctx(layout);
            ctx.width = std::max(1, width);
            ctx.wrap = opt.wrap_paragraphs && !is_heading;
            ctx.Start(first_prefix, cont_prefix);

            std::vector<std::string_view> style_stack;
            if (is_heading)
                style_stack.push_back("heading");
            style_stack.push_back(elem);

            for (const auto& inl : b.inlines)
                AppendInline(ctx, theme, opt, inl, style_stack);

            if (auto suf = InlineSuffixForElement(theme, elem))
            {
                const ResolvedStyle sst = ResolveStyleForElement(theme, elem);
                AppendText(ctx, *suf, sst);
            }

            ctx.FinishLine();
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
            // Render list items with proper marker + continuation indentation.
            apply_block_prefix_suffix("list", true);

            ResolvedStyle list_st = ResolveStyleForElement(theme, "list");
            std::vector<Cell> list_indent;
            {
                Line tmp;
                EnsureIndent(tmp, list_st);
                list_indent = std::move(tmp.cells);
            }

            for (size_t i = 0; i < b.children.size(); ++i)
            {
                const Block& li = b.children[i];
                if (li.kind != BlockKind::ListItem)
                {
                    AppendBlock(layout, theme, opt, li, width, quote_depth);
                    continue;
                }

                const int ordinal = b.list_start + (int)i;
                std::string marker_text;
                std::string marker_elem;
                if (li.list_item_is_task)
                {
                    const bool checked = (li.list_item_task_mark == 'x' || li.list_item_task_mark == 'X');
                    marker_elem = checked ? "task_checked" : "task_unchecked";
                    StyleSpec ms = ResolveElementStyle(theme, marker_elem);
                    marker_text = ms.block_prefix.value_or(checked ? "[✓] " : "[ ] ");
                }
                else if (b.ordered)
                {
                    marker_elem = "enumeration";
                    StyleSpec ms = ResolveElementStyle(theme, marker_elem);
                    marker_text = std::to_string(ordinal) + ms.block_prefix.value_or(std::string(1, b.ol_delim) + " ");
                }
                else
                {
                    marker_elem = "item";
                    StyleSpec ms = ResolveElementStyle(theme, marker_elem);
                    marker_text = ms.block_prefix.value_or("• ");
                }

                ResolvedStyle marker_st = ResolveStyleForElement(theme, marker_elem);

                std::vector<Cell> quote_prefix;
                if (quote_depth > 0)
                {
                    ResolvedStyle qst = ResolveStyleForElement(theme, "block_quote");
                    for (int q = 0; q < quote_depth; ++q)
                        AppendCellsFromUtf8(quote_prefix, "> ", qst);
                }

                std::vector<Cell> first_prefix;
                std::vector<Cell> cont_prefix;
                first_prefix.insert(first_prefix.end(), list_indent.begin(), list_indent.end());
                first_prefix.insert(first_prefix.end(), quote_prefix.begin(), quote_prefix.end());
                cont_prefix.insert(cont_prefix.end(), list_indent.begin(), list_indent.end());
                cont_prefix.insert(cont_prefix.end(), quote_prefix.begin(), quote_prefix.end());

                AppendCellsFromUtf8(first_prefix, marker_text, marker_st);

                const int marker_w = VisibleWidthCells(marker_text);
                for (int s = 0; s < marker_w; ++s)
                {
                    Cell c;
                    c.cp = U' ';
                    c.fg = marker_st.fg;
                    c.bg = marker_st.bg;
                    c.attrs = marker_st.attrs;
                    cont_prefix.push_back(c);
                }

                const std::vector<Inline> content = ExtractListItemInlinesBestEffort(li);
                const auto lines = RenderInlinesToLines(theme, opt, content, width, true, {"paragraph"}, first_prefix, cont_prefix);
                for (const auto& ln : lines)
                    layout.lines.push_back(ln);

                // Tight lists typically don't have blank lines between items.
                if (!b.list_is_tight && opt.preserve_blank_lines)
                    layout.lines.push_back(Line{});
            }

            apply_block_prefix_suffix("list", false);
            append_blank_line();
            break;
        }

        case BlockKind::Table:
        {
            AppendTable(layout, theme, opt, b, width, quote_depth);
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


