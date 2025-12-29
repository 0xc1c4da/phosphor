#include "ui/character_picker.h"

#include "app/focus_router.h"
#include "core/key_bindings.h"
#include "core/i18n.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"
#include "misc/cpp/imgui_stdlib.h"

#include <unicode/uchar.h>
#include <unicode/uniset.h>
#include <unicode/unistr.h>
#include <unicode/uspoof.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>

using icu::UnicodeSet;
using icu::UnicodeString;

namespace {

struct SpoofDeleter
{
    void operator()(USpoofChecker* p) const noexcept
    {
        if (p) uspoof_close(p);
    }
};
using SpoofPtr = std::unique_ptr<USpoofChecker, SpoofDeleter>;

static SpoofPtr MakeSpoofChecker()
{
    UErrorCode status = U_ZERO_ERROR;
    USpoofChecker* sc = uspoof_open(&status);
    if (U_FAILURE(status) || !sc)
        return SpoofPtr(nullptr);

    // Make intent explicit: we only need confusables/skeleton computations.
    uspoof_setChecks(sc, USPOOF_CONFUSABLE, &status);
    if (U_FAILURE(status))
    {
        uspoof_close(sc);
        return SpoofPtr(nullptr);
    }
    return SpoofPtr(sc);
}

static UnicodeString SkeletonOf(const USpoofChecker* sc, const UnicodeString& s)
{
    UErrorCode status = U_ZERO_ERROR;
    UnicodeString skel;
    // 'type' is deprecated in newer ICU; 0 is fine in ICU67.
    uspoof_getSkeletonUnicodeString(sc, 0, s, skel, &status);
    if (U_FAILURE(status))
        return UnicodeString();
    return skel;
}

static std::string ToUtf8(const UnicodeString& s)
{
    std::string out;
    s.toUTF8String(out);
    return out;
}

static std::string TrimCopy(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

static int CeilDivInt(int a, int b)
{
    return (a + b - 1) / b;
}

} // namespace

void CharacterPicker::MarkSelectionChanged()
{
    selection_changed_ = true;
    request_focus_selected_ = true;
    scroll_to_selected_ = true;
}

bool CharacterPicker::TakeSelectionChanged(uint32_t& out_cp)
{
    if (!selection_changed_)
        return false;
    selection_changed_ = false;
    out_cp = selected_cp_;
    return true;
}

bool CharacterPicker::TakeDoubleClicked(uint32_t& out_cp)
{
    if (!double_clicked_)
        return false;
    double_clicked_ = false;
    out_cp = double_clicked_cp_;
    double_clicked_cp_ = 0;
    return out_cp != 0;
}

void CharacterPicker::JumpToCodePoint(uint32_t cp)
{
    if (!IsScalarValue(cp))
        return;

    // Clear search so the view is deterministic (plane/block based).
    ClearSearch();

    // Programmatic jumps are typically followed by keyboard navigation in the grid.
    nav_region_ = NavRegion::Grid;
    tab_stop_ = TabStop::Grid;
    request_focus_grid_ = true;

    block_index_ = 0;
    subpage_index_ = static_cast<int>(cp / 0x10000u);
    subpage_index_ = std::clamp(subpage_index_, 0, 16);
    SyncRangeFromSelection();

    selected_cp_ = cp;
    ClampSelectionToCurrentView();
    confusables_for_cp_ = 0xFFFFFFFFu;
    scroll_to_selected_ = true;
    MarkSelectionChanged();
}

void CharacterPicker::RestoreSelectedCodePoint(uint32_t cp)
{
    if (!IsScalarValue(cp))
        return;

    // Restore is "silent": don't emit selection_changed_.
    selection_changed_ = false;
    double_clicked_ = false;
    double_clicked_cp_ = 0;

    ClearSearch();

    // Treat restores like programmatic jumps: keep keyboard focus deterministic.
    nav_region_ = NavRegion::Grid;
    tab_stop_ = TabStop::Grid;
    request_focus_grid_ = true;

    block_index_ = 0;
    subpage_index_ = static_cast<int>(cp / 0x10000u);
    subpage_index_ = std::clamp(subpage_index_, 0, 16);
    SyncRangeFromSelection();

    selected_cp_ = cp;
    ClampSelectionToCurrentView();
    confusables_for_cp_ = 0xFFFFFFFFu;
    scroll_to_selected_ = true;          // UX: scroll into view on activation
    request_focus_selected_ = true;      // keep nav highlight synced
}

// -------------------- ICU helpers --------------------

bool CharacterPicker::IsScalarValue(uint32_t cp)
{
    return (cp <= 0x10FFFFu) && !(cp >= 0xD800u && cp <= 0xDFFFu);
}

std::string CharacterPicker::CodePointHex(uint32_t cp)
{
    std::ostringstream oss;
    oss << "U+"
        << std::uppercase << std::hex << std::setfill('0')
        << std::setw(cp <= 0xFFFFu ? 4 : 6) << cp;
    return oss.str();
}

std::string CharacterPicker::GlyphUtf8(uint32_t cp)
{
    if (!IsScalarValue(cp))
        return std::string();
    UnicodeString s;
    s.append(static_cast<UChar32>(cp));
    return ToUtf8(s);
}

std::string CharacterPicker::CharName(uint32_t cp)
{
    char buf[256];
    UErrorCode status = U_ZERO_ERROR;
    int32_t len = u_charName(static_cast<UChar32>(cp), U_UNICODE_CHAR_NAME, buf, sizeof(buf), &status);
    if (U_FAILURE(status) || len <= 0)
        return std::string();
    return std::string(buf, static_cast<size_t>(len));
}

std::string CharacterPicker::BlockNameFor(uint32_t cp)
{
    int32_t block_val = u_getIntPropertyValue(static_cast<UChar32>(cp), UCHAR_BLOCK);
    const char* nm = u_getPropertyValueName(UCHAR_BLOCK, block_val, U_LONG_PROPERTY_NAME);
    return nm ? std::string(nm) : PHOS_TR("character_picker.unknown_block");
}

std::vector<std::string> CharacterPicker::TokenizeUpperASCII(const std::string& q)
{
    std::vector<std::string> toks;
    std::string cur;

    auto flush = [&]()
    {
        if (!cur.empty())
        {
            toks.push_back(cur);
            cur.clear();
        }
    };

    for (unsigned char uc : q)
    {
        if (std::isalnum(uc))
            cur.push_back(static_cast<char>(std::toupper(uc)));
        else
            flush();
    }
    flush();
    return toks;
}

// -------------------- omit/visibility helpers --------------------

void CharacterPicker::InitDefaultOmitRanges()
{
    // Known missing-glyph spans for Unscii (Unicode 13).
    // Add more here as you discover them; ranges are inclusive.
    omit_ranges_.clear();
    AddOmitRange(0x0000u, 0x0010u);
    AddOmitRange(0x0870u, 0x0890u);
    AddOmitRange(0x08C0u, 0x08C0u);
    AddOmitRange(0x1AC0u, 0x1AF0u);
    AddOmitRange(0x2450u, 0x2450u);
    AddOmitRange(0x2E50u, 0x2E70u);
    AddOmitRange(0x9FF0u, 0x9FF0u);
    AddOmitRange(0xE390u, 0xE3A0u);
    AddOmitRange(0xE400u, 0xE460u);
    AddOmitRange(0xE4D0u, 0xE5B0u);
    AddOmitRange(0xE5E0u, 0xE620u);
    AddOmitRange(0xE6D0u, 0xE6E0u);
    AddOmitRange(0xEB40u, 0xEBF0u);
    AddOmitRange(0xECE0u, 0xECF0u);
    AddOmitRange(0xED40u, 0xF4B0u);
    AddOmitRange(0xFAE0u, 0xFAF0u);
    AddOmitRange(0xFD40u, 0xFD40u);
    AddOmitRange(0xFFF0u, 0xFFF0u);

    NormalizeOmitRanges();
}

void CharacterPicker::AddOmitRange(uint32_t start_inclusive, uint32_t end_inclusive)
{
    if (end_inclusive < start_inclusive)
        std::swap(start_inclusive, end_inclusive);
    omit_ranges_.push_back(OmitRange{start_inclusive, end_inclusive});
    omit_revision_++;
}

void CharacterPicker::NormalizeOmitRanges()
{
    if (omit_ranges_.empty())
        return;

    std::sort(omit_ranges_.begin(), omit_ranges_.end(),
              [](const OmitRange& a, const OmitRange& b)
              {
                  if (a.start != b.start) return a.start < b.start;
                  return a.end < b.end;
              });

    std::vector<OmitRange> merged;
    merged.reserve(omit_ranges_.size());
    OmitRange cur = omit_ranges_.front();

    for (size_t i = 1; i < omit_ranges_.size(); ++i)
    {
        const OmitRange& r = omit_ranges_[i];
        if (r.start <= cur.end + 1u)
        {
            cur.end = std::max(cur.end, r.end);
        }
        else
        {
            merged.push_back(cur);
            cur = r;
        }
    }
    merged.push_back(cur);
    omit_ranges_ = std::move(merged);
}

bool CharacterPicker::IsOmitted(uint32_t cp) const
{
    if (omit_ranges_.empty())
        return false;

    // Binary search for the last range with start <= cp, then check if cp <= end.
    size_t lo = 0;
    size_t hi = omit_ranges_.size();
    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo) / 2;
        if (omit_ranges_[mid].start <= cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return false;
    const OmitRange& r = omit_ranges_[lo - 1];
    return (cp >= r.start && cp <= r.end);
}

bool CharacterPicker::IsRangeFullyOmitted(uint32_t start, uint32_t end) const
{
    if (end < start)
        return true;
    if (omit_ranges_.empty())
        return false;

    uint32_t cur = start;
    for (const auto& r : omit_ranges_)
    {
        if (r.end < cur)
            continue;
        if (r.start > cur)
            return false; // gap
        // r.start <= cur <= r.end
        if (r.end >= end)
            return true;
        cur = r.end + 1u;
        if (cur == 0) // wrapped
            return true;
    }
    return false;
}

bool CharacterPicker::HasGlyph(const ImFont* font, uint32_t cp)
{
    if (!font)
        return true; // best-effort: if no font, don't hide anything here
    if (!IsScalarValue(cp))
        return false;

    // Dear ImGui commonly uses 16-bit ImWchar unless IMGUI_USE_WCHAR32 is enabled.
    if (sizeof(ImWchar) == 2 && cp > 0xFFFFu)
        return false;

    // IMPORTANT: do not force-load/bake glyphs here (doing so in big loops can explode atlas size
    // and cause Vulkan allocator failures). Use non-loading presence checks only.

    // Fast path: if glyph already loaded for current baked font, it's definitely drawable.
    if (ImFontBaked* baked = ImGui::GetFontBaked())
        if (baked->IsGlyphLoaded(static_cast<ImWchar>(cp)))
            return true;

    // Presence check in font sources (non-const in 1.91.4 but logically a query).
    return const_cast<ImFont*>(font)->IsGlyphInFont(static_cast<ImWchar>(cp));
}

std::optional<uint32_t> CharacterPicker::FirstVisibleInRange(uint32_t start, uint32_t end, const ImFont* /*font*/) const
{
    if (end < start)
        return std::nullopt;
    for (uint32_t cp = start;; ++cp)
    {
        if (!IsScalarValue(cp))
        {
            if (cp == end)
                break;
            continue;
        }
        if (IsOmitted(cp))
        {
            if (cp == end)
                break;
            continue;
        }
        return cp;
        // unreachable
    }
    return std::nullopt;
}

void CharacterPicker::RebuildVisibleCache(uint32_t view_start, uint32_t view_end, const ImFont* font)
{
    if (visible_cache_start_ == view_start &&
        visible_cache_end_ == view_end &&
        visible_cache_font_ == font &&
        visible_cache_omit_revision_ == omit_revision_)
    {
        return;
    }

    visible_cache_start_ = view_start;
    visible_cache_end_ = view_end;
    visible_cache_font_ = font;
    visible_cache_omit_revision_ = omit_revision_;

    visible_cps_cache_.clear();
    if (view_end < view_start)
        return;

    for (uint32_t cp = view_start;; ++cp)
    {
        if (!IsScalarValue(cp))
        {
            if (cp == view_end)
                break;
            continue;
        }
        if (IsOmitted(cp))
        {
            if (cp == view_end)
                break;
            continue;
        }
        visible_cps_cache_.push_back(cp);

        if (cp == view_end)
            break;
    }
}

void CharacterPicker::RebuildAvailablePlanes(const ImFont* font)
{
    if (plane_cache_font_ == font && plane_cache_omit_revision_ == omit_revision_)
        return;

    plane_cache_font_ = font;
    plane_cache_omit_revision_ = omit_revision_;

    available_planes_.clear();
    available_planes_.reserve(17);

    // Requirement: hide planes only when the omit ranges cover the *entire* plane.
    // Don't scan plane contents or query glyphs here (can be extremely expensive).
    for (int p = 0; p <= 16; ++p)
    {
        const uint32_t ps = static_cast<uint32_t>(p) * 0x10000u;
        const uint32_t pe = std::min(ps + 0xFFFFu, 0x10FFFFu);
        if (!IsRangeFullyOmitted(ps, pe))
            available_planes_.push_back(p);
    }

    // Ensure we always have something selectable to avoid weird UI states.
    if (available_planes_.empty())
        available_planes_.push_back(0);

    const int cur_plane = std::clamp(subpage_index_, 0, 16);
    if (std::find(available_planes_.begin(), available_planes_.end(), cur_plane) == available_planes_.end())
        subpage_index_ = available_planes_.front();
}

// -------------------- blocks --------------------

CharacterPicker::CharacterPicker()
{
    InitDefaultOmitRanges();
    EnsureBlocksLoaded();
    SyncRangeFromSelection();
}

CharacterPicker::~CharacterPicker() = default;

void CharacterPicker::EnsureBlocksLoaded()
{
    if (blocks_loaded_)
        return;

    blocks_.clear();

    const int32_t minV = u_getIntPropertyMinValue(UCHAR_BLOCK);
    const int32_t maxV = u_getIntPropertyMaxValue(UCHAR_BLOCK);

    for (int32_t v = minV; v <= maxV; ++v)
    {
        const char* nm = u_getPropertyValueName(UCHAR_BLOCK, v, U_LONG_PROPERTY_NAME);
        if (!nm)
            continue;

        // Skip synthetic/empty buckets.
        if (std::strcmp(nm, "No_Block") == 0 || std::strcmp(nm, "No Block") == 0)
            continue;

        UErrorCode ec = U_ZERO_ERROR;
        UnicodeSet set;
        set.applyIntPropertyValue(UCHAR_BLOCK, v, ec);
        if (U_FAILURE(ec) || set.isEmpty())
            continue;

        // Unicode blocks should be contiguous; if not, skip to keep UI simple.
        if (set.getRangeCount() != 1)
            continue;

        BlockInfo bi;
        bi.start = static_cast<uint32_t>(set.getRangeStart(0));
        bi.end   = static_cast<uint32_t>(set.getRangeEnd(0));
        bi.value = v;
        bi.name  = nm;
        blocks_.push_back(std::move(bi));
    }

    std::sort(blocks_.begin(), blocks_.end(),
              [](const BlockInfo& a, const BlockInfo& b) { return a.start < b.start; });

    blocks_loaded_ = true;
}

void CharacterPicker::SyncRangeFromSelection()
{
    if (block_index_ == 0)
    {
        // "All Unicode": subpage = plane.
        const uint32_t plane = static_cast<uint32_t>(std::clamp(subpage_index_, 0, 16));
        range_start_ = plane * 0x10000u;
        range_end_   = std::min(plane * 0x10000u + 0xFFFFu, 0x10FFFFu);
        return;
    }

    const int bi = block_index_ - 1;
    if (bi < 0 || bi >= static_cast<int>(blocks_.size()))
    {
        block_index_ = 0;
        subpage_index_ = 0;
        SyncRangeFromSelection();
        return;
    }

    const BlockInfo& b = blocks_[bi];
    // Full block range. (Subpage is used as a "jump-to" control in the UI.)
    range_start_ = b.start;
    range_end_   = b.end;
}

void CharacterPicker::ClampSelectionToCurrentView()
{
    if (search_active_)
    {
        auto cps = FilteredSearchCpsForCurrentBlock();
        if (cps.empty())
            return;
        if (std::find(cps.begin(), cps.end(), selected_cp_) == cps.end())
            selected_cp_ = cps.front();
        return;
    }

    if (selected_cp_ < range_start_)
        selected_cp_ = range_start_;
    if (selected_cp_ > range_end_)
        selected_cp_ = range_end_;

    // Avoid landing on omitted codepoints.
    if (IsOmitted(selected_cp_))
    {
        // Prefer scanning forward, then backward.
        for (uint32_t cp = selected_cp_;; ++cp)
        {
            if (cp > range_end_)
                break;
            if (IsScalarValue(cp) && !IsOmitted(cp))
            {
                selected_cp_ = cp;
                return;
            }
            if (cp == range_end_)
                break;
        }
        for (uint32_t cp = selected_cp_;; --cp)
        {
            if (cp < range_start_)
                break;
            if (IsScalarValue(cp) && !IsOmitted(cp))
            {
                selected_cp_ = cp;
                return;
            }
            if (cp == range_start_)
                break;
        }
    }
}

// -------------------- search --------------------

struct SearchCtx
{
    std::vector<std::string> tokensUpper;
    int32_t limit = 512;
    int32_t found = 0;
    std::vector<CharacterPicker::SearchResult>* out = nullptr;
};

static UBool U_CALLCONV EnumNamesCallback(void* context,
                                         UChar32 code,
                                         UCharNameChoice /*nameChoice*/,
                                         const char* name,
                                         int32_t length)
{
    auto* ctx = static_cast<SearchCtx*>(context);
    if (!ctx || !ctx->out)
        return false;

    // ICU names are ASCII uppercase; require all tokens to appear as substrings.
    std::string_view nm{name, static_cast<size_t>(length)};
    for (const auto& tok : ctx->tokensUpper)
    {
        if (tok.empty())
            continue;
        if (nm.find(tok) == std::string_view::npos)
            return true; // keep enumerating
    }

    CharacterPicker::SearchResult r;
    r.cp = static_cast<uint32_t>(code);
    r.name = std::string(nm);
    r.block = CharacterPicker::BlockNameFor(r.cp);
    ctx->out->push_back(std::move(r));

    ctx->found++;
    if (ctx->found >= ctx->limit)
        return false; // stop enumeration
    return true;
}

void CharacterPicker::PerformSearch()
{
    search_results_.clear();
    search_active_ = false;

    const std::string q = TrimCopy(search_query_);
    if (q.empty())
        return;

    SearchCtx ctx;
    ctx.tokensUpper = TokenizeUpperASCII(q);
    ctx.limit = std::max(1, search_limit_);
    ctx.out = &search_results_;

    if (ctx.tokensUpper.empty())
        return;

    UErrorCode status = U_ZERO_ERROR;
    u_enumCharNames(0, 0x110000, EnumNamesCallback, &ctx, U_UNICODE_CHAR_NAME, &status);
    (void)status; // ICU may report out-of-sync errors; we don't treat as fatal here.

    search_active_ = !search_results_.empty();
    search_dirty_ = false;
    if (search_active_)
    {
        selected_cp_ = search_results_.front().cp;
        MarkSelectionChanged();
    }
}

void CharacterPicker::ClearSearch()
{
    search_query_.clear();
    search_results_.clear();
    search_active_ = false;
    search_dirty_ = false;
}

std::vector<uint32_t> CharacterPicker::FilteredSearchCpsForCurrentBlock() const
{
    std::vector<uint32_t> cps;
    cps.reserve(search_results_.size());

    uint32_t block_start = 0;
    uint32_t block_end = 0x10FFFFu;
    if (block_index_ > 0)
    {
        const int bi = block_index_ - 1;
        if (bi >= 0 && bi < static_cast<int>(blocks_.size()))
        {
            block_start = blocks_[bi].start;
            block_end = blocks_[bi].end;
        }
    }

    for (const auto& r : search_results_)
    {
        if (r.cp >= block_start && r.cp <= block_end && !IsOmitted(r.cp))
            cps.push_back(r.cp);
    }
    return cps;
}

// -------------------- confusables --------------------

void CharacterPicker::UpdateConfusablesIfNeeded()
{
    if (confusables_for_cp_ == selected_cp_)
        return;

    confusable_cps_.clear();
    confusables_for_cp_ = selected_cp_;
    ComputeConfusables(selected_cp_, confusables_limit_);
}

void CharacterPicker::ComputeConfusables(uint32_t base_cp, int limit)
{
    if (!IsScalarValue(base_cp))
        return;
    if (IsOmitted(base_cp))
        return;

    auto sc = MakeSpoofChecker();
    if (!sc)
        return;

    UnicodeString input;
    input.append(static_cast<UChar32>(base_cp));
    UnicodeString target_skel = SkeletonOf(sc.get(), input);
    if (target_skel.isEmpty())
        return;

    UErrorCode status = U_ZERO_ERROR;
    const UnicodeSet* cand = uspoof_getInclusionUnicodeSet(&status);
    if (U_FAILURE(status) || cand == nullptr)
        return;

    int printed = 0;
    for (int32_t i = 0; i < cand->getRangeCount(); ++i)
    {
        UChar32 start = cand->getRangeStart(i);
        UChar32 end = cand->getRangeEnd(i);
        for (UChar32 cp = start; cp <= end; ++cp)
        {
            if (static_cast<uint32_t>(cp) == base_cp)
                continue;

            UnicodeString s;
            s.append(cp);
            UnicodeString sk = SkeletonOf(sc.get(), s);
            if (sk == target_skel)
            {
                const uint32_t ucp = static_cast<uint32_t>(cp);
                if (!IsOmitted(ucp))
                {
                    confusable_cps_.push_back(ucp);
                    printed++;
                    if (printed >= limit)
                        return;
                }
            }
        }
    }
}

// -------------------- UI --------------------

bool CharacterPicker::Render(const char* window_title, bool* p_open,
                             SessionState* session, bool apply_placement_this_frame,
                             app::FocusRouter* focus_router,
                             kb::KeyBindingsEngine* keybinds)
{
    EnsureBlocksLoaded();
    // Unicode picker is Unicode-only: always render with the UI font (Unscii / ImGui default),
    // not whatever font happens to be pushed by other UI.
    ImFont* ui_font = ImGui::GetIO().FontDefault ? ImGui::GetIO().FontDefault : ImGui::GetFont();
    const bool font_pushed = (ui_font != nullptr);
    if (font_pushed)
        ImGui::PushFont(ui_font);

    RebuildAvailablePlanes(ui_font);

    if (session)
        ApplyImGuiWindowPlacement(*session, window_title, apply_placement_this_frame);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoSavedSettings |
        (session ? GetImGuiWindowChromeExtraFlags(*session, window_title) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, window_title);
    const std::string win_title = PHOS_TR("menu.window.unicode_character_picker") + "###" + std::string(window_title);
    if (!ImGui::Begin(win_title.c_str(), p_open, flags))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, window_title);
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        if (font_pushed)
            ImGui::PopFont();
        return (p_open == nullptr) ? true : *p_open;
    }
    const bool window_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focus_router)
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        ImGuiWindow* root = (w && w->RootWindow) ? w->RootWindow : w;
        const std::uint32_t root_id = root ? (std::uint32_t)root->ID : 0u;
        focus_router->NoteWindowTarget(app::TargetKind::CharacterPicker, root_id, window_focused);

        // Ctrl+Tab / docking focus can land the window without a valid NavId.
        // Ensure the selected cell is immediately keyboard-navigable when the router targets this window.
        const app::Target kb = focus_router->KeyboardTarget();
        if (kb.kind == app::TargetKind::CharacterPicker && window_focused)
            request_focus_selected_ = true;

        // Region focus seeding:
        // Ctrl+Tab / docking focus can land the window without a valid NavId.
        // When this happens, restore focus to the last active picker region so keyboard behavior is deterministic.
        if (kb.kind == app::TargetKind::CharacterPicker && window_focused)
        {
            ImGuiContext& g = *GImGui;
            if (!ImGui::GetIO().WantTextInput && g.NavId == 0)
            {
                request_focus_topbar_  = (nav_region_ == NavRegion::TopBar);
                request_focus_grid_    = (nav_region_ == NavRegion::Grid);
                request_focus_sidebar_ = (nav_region_ == NavRegion::Sidebar);
            }
        }
    }
    if (session)
        CaptureImGuiWindowPlacement(*session, window_title);
    if (session)
    {
        ApplyImGuiWindowChromeZOrder(session, window_title);
        RenderImGuiWindowChromeMenu(session, window_title);
    }

    // Title-bar ⋮ popup: holds the former right sidebar (Selected + Copy + Confusables).
    // This keeps the main picker UI grid-focused and reduces horizontal clutter.
    {
        ImVec2 kebab_min(0.0f, 0.0f), kebab_max(0.0f, 0.0f);
        const bool has_close = (p_open != nullptr);
        const bool has_collapse = (flags & ImGuiWindowFlags_NoCollapse) == 0;
        if (RenderImGuiWindowChromeTitleBarButton("##charpick_kebab", "\xE2\x8B\xAE", has_close, has_collapse,
                                                  &kebab_min, &kebab_max,
                                                  /*button_index_from_right=*/0))
        {
            ImGui::OpenPopup("##charpick_sidebar");
        }

        if (ImGui::IsPopupOpen("##charpick_sidebar"))
            ImGui::SetNextWindowPos(ImVec2(kebab_min.x, kebab_max.y), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f, 0.0f), ImVec2(720.0f, 720.0f));
        if (ImGui::BeginPopup("##charpick_sidebar"))
        {
            // Former sidebar contents:
            // Selected info + copy buttons + confusables list.
            ImGui::TextUnformatted(PHOS_TR("character_picker.selected").c_str());
            ImGui::Separator();

            const std::string hex = CodePointHex(selected_cp_);
            const std::string glyph = GlyphUtf8(selected_cp_);
            const std::string nm = CharName(selected_cp_);
            const std::string blk = BlockNameFor(selected_cp_);

            ImGui::TextUnformatted(hex.c_str());
            if (!glyph.empty())
            {
                const std::string s = PHOS_TRF("character_picker.glyph_prefix", phos::i18n::Arg::Str(glyph));
                ImGui::TextUnformatted(s.c_str());
            }
            if (!nm.empty())
            {
                const std::string s = PHOS_TRF("character_picker.name_prefix", phos::i18n::Arg::Str(nm));
                ImGui::TextWrapped("%s", s.c_str());
            }
            {
                const std::string s = PHOS_TRF("character_picker.block_prefix", phos::i18n::Arg::Str(blk));
                ImGui::TextWrapped("%s", s.c_str());
            }

            if (ImGui::Button(PHOS_TR("character_picker.copy_character").c_str()) && !glyph.empty())
                ImGui::SetClipboardText(glyph.c_str());
            ImGui::SameLine();
            if (ImGui::Button(PHOS_TR("character_picker.copy_u_plus").c_str()))
                ImGui::SetClipboardText(hex.c_str());

            ImGui::Separator();

            ImGui::TextUnformatted(PHOS_TR("character_picker.confusables_header").c_str());
            ImGui::SameLine();
            {
                const std::string s = PHOS_TRF("character_picker.limit_fmt", phos::i18n::Arg::I64((long long)confusables_limit_));
                ImGui::TextDisabled("%s", s.c_str());
            }

            // Scrollable list so the popup stays a reasonable size.
            ImGui::BeginChild("##charpick_conf_scroll", ImVec2(0.0f, 360.0f), false, ImGuiWindowFlags_None);
            if (confusable_cps_.empty())
            {
                ImGui::TextDisabled("%s", PHOS_TR("character_picker.no_confusables").c_str());
            }
            else
            {
                for (size_t i = 0; i < confusable_cps_.size(); ++i)
                {
                    const uint32_t cp = confusable_cps_[i];
                    const std::string g = GlyphUtf8(cp);
                    const std::string h = CodePointHex(cp);
                    const std::string n = CharName(cp);

                    std::string label = h;
                    if (!g.empty())
                        label += "  " + g;
                    if (!n.empty())
                        label += "  " + n;

                    if (ImGui::Selectable(label.c_str(), false))
                    {
                        selected_cp_ = cp;
                        confusables_for_cp_ = 0xFFFFFFFFu;
                        // Keep the view consistent with the clicked cp when browsing "All Unicode".
                        if (block_index_ == 0)
                        {
                            subpage_index_ = static_cast<int>(cp / 0x10000u);
                            SyncRangeFromSelection();
                            scroll_to_selected_ = true;
                        }
                        MarkSelectionChanged();
                    }
                }
            }
            ImGui::EndChild();

            ImGui::Separator();
            if (ImGui::Button(PHOS_TR("common.close").c_str()))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    bool allow_keyboard_nav = window_focused;
    if (focus_router)
        allow_keyboard_nav = window_focused && (focus_router->KeyboardTarget().kind == app::TargetKind::CharacterPicker);

    // Picker-level keyboard shortcuts (focus/search/escape).
    // Keep these independent of ImGui global nav settings and avoid interfering with text editing/popups.
    if (allow_keyboard_nav &&
        !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        // Ctrl+F: focus search.
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, ImGuiInputFlags_RouteFocused))
        {
            nav_region_ = NavRegion::TopBar;
            request_tab_stop_ = TabStop::Search;
            request_tab_focus_ = true;
        }

        // Escape: clear search if active, otherwise return to grid; if already on grid, close window if possible.
        if (ImGui::Shortcut(ImGuiKey_Escape, ImGuiInputFlags_RouteFocused))
        {
            if (search_active_ || !search_query_.empty())
            {
                ClearSearch();
                subpage_index_ = 0;
                SyncRangeFromSelection();
                ClampSelectionToCurrentView();
                MarkSelectionChanged();
                nav_region_ = NavRegion::Grid;
                tab_stop_ = TabStop::Grid;
                request_focus_grid_ = true;
            }
            else if (nav_region_ != NavRegion::Grid)
            {
                nav_region_ = NavRegion::Grid;
                tab_stop_ = TabStop::Grid;
                request_focus_grid_ = true;
            }
            else if (p_open)
            {
                *p_open = false;
            }
        }
    }

    // Explicit Tab/Shift+Tab cycling inside the picker (avoids relying on ImGui nav global settings).
    // Do not interfere with text editing or popups.
    if (allow_keyboard_nav &&
        !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        const bool tab_fwd = ImGui::Shortcut(ImGuiKey_Tab, ImGuiInputFlags_RouteFocused);
        const bool tab_back = ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Tab, ImGuiInputFlags_RouteFocused);
        if (tab_fwd || tab_back)
        {
            auto next_stop = [&](TabStop cur, int dir) -> TabStop
            {
                static constexpr TabStop order[] = {
                    TabStop::Block,
                    TabStop::Subpage,
                    TabStop::Search,
                    TabStop::Grid,
                };
                constexpr int n = (int)(sizeof(order) / sizeof(order[0]));
                int idx = 0;
                for (int i = 0; i < n; ++i)
                    if (order[i] == cur) { idx = i; break; }
                idx = (idx + dir) % n;
                if (idx < 0) idx += n;
                return order[idx];
            };

            request_tab_stop_ = next_stop(tab_stop_, tab_back ? -1 : +1);
            request_tab_focus_ = true;

            // Switching tab stops also implies switching regions (used for arrow key policy + borders).
            switch (request_tab_stop_)
            {
                case TabStop::Block:
                case TabStop::Subpage:
                case TabStop::Search:
                    nav_region_ = NavRegion::TopBar;
                    request_focus_topbar_ = true;
                    break;
                case TabStop::Grid:
                    nav_region_ = NavRegion::Grid;
                    request_focus_grid_ = true;
                    break;
            }
        }
    }

    RenderTopBar();
    ImGui::Separator();
    RenderGridAndSidePanel(keybinds, allow_keyboard_nav);

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
    if (font_pushed)
        ImGui::PopFont();
    return (p_open == nullptr) ? true : *p_open;
}

void CharacterPicker::RenderTopBar()
{
    // If picker focus was programmatically restored to the top bar, place keyboard focus on the first item.
    if (request_focus_topbar_)
    {
        ImGui::SetKeyboardFocusHere();
        request_focus_topbar_ = false;
    }

    // Block dropdown
    {
        if (request_tab_focus_ && request_tab_stop_ == TabStop::Block)
        {
            ImGui::SetKeyboardFocusHere();
            request_tab_focus_ = false;
        }

        std::string preview = PHOS_TR("character_picker.all_unicode_by_plane");
        if (block_index_ > 0)
        {
            const int bi = block_index_ - 1;
            if (bi >= 0 && bi < static_cast<int>(blocks_.size()))
                preview = blocks_[bi].name;
        }

        ImGui::SetNextItemWidth(280.0f);
        const std::string block_lbl = PHOS_TR("character_picker.block") + "###charpick_block";
        if (ImGui::BeginCombo(block_lbl.c_str(), preview.c_str()))
        {
            nav_region_ = NavRegion::TopBar;
            bool sel_all = (block_index_ == 0);
            if (ImGui::Selectable(PHOS_TR("character_picker.all_unicode_by_plane").c_str(), sel_all))
            {
                block_index_ = 0;
                RebuildAvailablePlanes(ImGui::GetFont());
                subpage_index_ = std::clamp(subpage_index_, 0, 16);
                SyncRangeFromSelection();
                ClampSelectionToCurrentView();
                MarkSelectionChanged();
            }
            if (sel_all)
                ImGui::SetItemDefaultFocus();

            for (int i = 0; i < static_cast<int>(blocks_.size()); ++i)
            {
                const bool sel = (block_index_ == (i + 1));
                std::string label = blocks_[i].name + "  (" +
                                    CodePointHex(blocks_[i].start) + ".." + CodePointHex(blocks_[i].end) + ")";
                if (ImGui::Selectable(label.c_str(), sel))
                {
                    block_index_ = i + 1;
                    subpage_index_ = 0;
                    SyncRangeFromSelection();
                    ClampSelectionToCurrentView();
                    MarkSelectionChanged();
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemFocused() || ImGui::IsItemActive())
            nav_region_ = NavRegion::TopBar;
        if (ImGui::IsItemFocused() || ImGui::IsItemActive())
            tab_stop_ = TabStop::Block;
    }

    ImGui::SameLine();

    // Subpage dropdown (planes for All, pages for block; pages for search results when active)
    {
        if (search_active_)
        {
            auto cps = FilteredSearchCpsForCurrentBlock();
            const int page_size = 256; // 16x16
            const int page_count = std::max(1, CeilDivInt(static_cast<int>(cps.size()), page_size));
            subpage_index_ = std::clamp(subpage_index_, 0, page_count - 1);

            const int start_i = subpage_index_ * page_size;
            const int end_i = std::min(static_cast<int>(cps.size()), start_i + page_size) - 1;
                std::string preview = (cps.empty())
                    ? PHOS_TR("character_picker.no_results")
                    : PHOS_TRF("character_picker.results_range_of_total_fmt",
                               phos::i18n::Arg::I64((long long)start_i + 1),
                               phos::i18n::Arg::I64((long long)end_i + 1),
                               phos::i18n::Arg::I64((long long)cps.size()));

            ImGui::SetNextItemWidth(260.0f);
                const std::string page_lbl = PHOS_TR("character_picker.page") + "###charpick_page";
                if (request_tab_focus_ && request_tab_stop_ == TabStop::Subpage)
                {
                    ImGui::SetKeyboardFocusHere();
                    request_tab_focus_ = false;
                }
                if (ImGui::BeginCombo(page_lbl.c_str(), preview.c_str()))
            {
                nav_region_ = NavRegion::TopBar;
                for (int p = 0; p < page_count; ++p)
                {
                    const int s = p * page_size;
                    const int e = std::min(static_cast<int>(cps.size()), s + page_size) - 1;
                        std::string label = PHOS_TRF("character_picker.results_range_fmt",
                                                     phos::i18n::Arg::I64((long long)s + 1),
                                                     phos::i18n::Arg::I64((long long)e + 1));
                    bool sel = (p == subpage_index_);
                    if (ImGui::Selectable(label.c_str(), sel))
                        subpage_index_ = p;
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemFocused() || ImGui::IsItemActive())
                nav_region_ = NavRegion::TopBar;
            if (ImGui::IsItemFocused() || ImGui::IsItemActive())
                tab_stop_ = TabStop::Subpage;
        }
        else if (block_index_ == 0)
        {
            // Planes 0..16, but hide planes with no visible glyphs in the current font.
            RebuildAvailablePlanes(ImGui::GetFont());
            const int plane = std::clamp(subpage_index_, 0, 16);
                const std::string range =
                    CodePointHex(static_cast<uint32_t>(plane) * 0x10000u) + ".." +
                    CodePointHex(std::min(static_cast<uint32_t>(plane) * 0x10000u + 0xFFFFu, 0x10FFFFu));
                std::string preview = PHOS_TRF("character_picker.plane_preview_fmt",
                                               phos::i18n::Arg::I64((long long)plane),
                                               phos::i18n::Arg::Str(range));

            ImGui::SetNextItemWidth(260.0f);
                const std::string subpage_lbl = PHOS_TR("character_picker.subpage") + "###charpick_subpage";
                if (request_tab_focus_ && request_tab_stop_ == TabStop::Subpage)
                {
                    ImGui::SetKeyboardFocusHere();
                    request_tab_focus_ = false;
                }
                if (ImGui::BeginCombo(subpage_lbl.c_str(), preview.c_str()))
            {
                nav_region_ = NavRegion::TopBar;
                for (int p : available_planes_)
                {
                    const uint32_t ps = static_cast<uint32_t>(p) * 0x10000u;
                    const uint32_t pe = std::min(ps + 0xFFFFu, 0x10FFFFu);
                        const std::string pr = CodePointHex(ps) + ".." + CodePointHex(pe);
                        std::string label = PHOS_TRF("character_picker.plane_preview_fmt",
                                                     phos::i18n::Arg::I64((long long)p),
                                                     phos::i18n::Arg::Str(pr));
                    const bool sel = (p == plane);
                    if (ImGui::Selectable(label.c_str(), sel))
                    {
                        subpage_index_ = p;
                        SyncRangeFromSelection();
                        ClampSelectionToCurrentView();
                        scroll_to_selected_ = true;
                        MarkSelectionChanged();
                    }
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemFocused() || ImGui::IsItemActive())
                nav_region_ = NavRegion::TopBar;
            if (ImGui::IsItemFocused() || ImGui::IsItemActive())
                tab_stop_ = TabStop::Subpage;
        }
        else
        {
            // Block pages (chunked to 256 code points)
            const int bi = block_index_ - 1;
            const BlockInfo& b = blocks_[bi];
            constexpr uint32_t kPageSize = 256u;
            const uint32_t block_len = (b.end >= b.start) ? (b.end - b.start + 1u) : 0u;
            const int page_count = std::max(1, static_cast<int>((block_len + (kPageSize - 1u)) / kPageSize));
            subpage_index_ = std::clamp(subpage_index_, 0, page_count - 1);

            const uint32_t ps = b.start + static_cast<uint32_t>(subpage_index_) * kPageSize;
            const uint32_t pe = std::min(ps + (kPageSize - 1u), b.end);
            std::string preview = CodePointHex(ps) + ".." + CodePointHex(pe);

            ImGui::SetNextItemWidth(260.0f);
            const std::string jump_lbl = PHOS_TR("character_picker.jump") + "###charpick_jump";
            if (request_tab_focus_ && request_tab_stop_ == TabStop::Subpage)
            {
                ImGui::SetKeyboardFocusHere();
                request_tab_focus_ = false;
            }
            if (ImGui::BeginCombo(jump_lbl.c_str(), preview.c_str()))
            {
                nav_region_ = NavRegion::TopBar;
                for (int p = 0; p < page_count; ++p)
                {
                    const uint32_t s = b.start + static_cast<uint32_t>(p) * kPageSize;
                    const uint32_t e = std::min(s + (kPageSize - 1u), b.end);
                    std::string label = CodePointHex(s) + ".." + CodePointHex(e);
                    const bool sel = (p == subpage_index_);
                    if (ImGui::Selectable(label.c_str(), sel))
                    {
                        subpage_index_ = p;
                        const ImFont* font = ImGui::GetFont();
                        if (auto first = FirstVisibleInRange(s, e, font))
                            selected_cp_ = *first;
                        else
                            selected_cp_ = s; // fallback; will be clamped later
                        confusables_for_cp_ = 0xFFFFFFFFu;
                        scroll_to_selected_ = true;
                        MarkSelectionChanged();
                    }
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemFocused() || ImGui::IsItemActive())
                nav_region_ = NavRegion::TopBar;
            if (ImGui::IsItemFocused() || ImGui::IsItemActive())
                tab_stop_ = TabStop::Subpage;
        }
    }

    // Search row (no Go/Clear buttons; press Enter to search, Esc clears when active).
    {
        // Put search on its own line under the dropdowns.
        ImGui::Spacing();
        const float w = std::max(240.0f, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(w);
        const std::string search_lbl = PHOS_TR("common.search") + "###charpick_search";
        const std::string hint = PHOS_TR("character_picker.search_hint");
        if (request_tab_focus_ && request_tab_stop_ == TabStop::Search)
        {
            ImGui::SetKeyboardFocusHere();
            request_tab_focus_ = false;
        }
        if (ImGui::InputTextWithHint(search_lbl.c_str(), hint.c_str(), &search_query_,
                                     ImGuiInputTextFlags_EnterReturnsTrue))
        {
            nav_region_ = NavRegion::TopBar;
            search_dirty_ = true;
            PerformSearch();
            subpage_index_ = 0;
            SyncRangeFromSelection();
            ClampSelectionToCurrentView();
        }
        if (ImGui::IsItemFocused() || ImGui::IsItemActive())
            nav_region_ = NavRegion::TopBar;
        if (ImGui::IsItemFocused() || ImGui::IsItemActive())
            tab_stop_ = TabStop::Search;

        // If the user deletes the query text to empty without pressing Enter, auto-exit search mode
        // so the picker never gets "stuck" showing stale results.
        if (search_active_ && TrimCopy(search_query_).empty())
        {
            ClearSearch();
            subpage_index_ = 0;
            SyncRangeFromSelection();
            ClampSelectionToCurrentView();
            MarkSelectionChanged();
        }
    }
}

void CharacterPicker::RenderGridAndSidePanel(kb::KeyBindingsEngine* keybinds, bool allow_keyboard_nav)
{
    UpdateConfusablesIfNeeded();

    const bool any_popup_open =
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool want_text_input = ImGui::GetIO().WantTextInput;
    const bool grid_region_active =
        allow_keyboard_nav && !want_text_input && !any_popup_open && (nav_region_ == NavRegion::Grid);

    // When the grid region is active, arrow keys should move the glyph selection only,
    // not also navigate the surrounding ImGui widgets.
    if (grid_region_active)
    {
        const ImGuiID owner = ImGui::GetCurrentWindow()->ID;
        ImGui::SetKeyOwner(ImGuiKey_LeftArrow, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_RightArrow, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_UpArrow, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_DownArrow, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_Enter, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_KeypadEnter, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_Space, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_Home, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_End, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_PageUp, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_PageDown, owner, ImGuiInputFlags_LockThisFrame);
    }

    // While grid region is active: Enter/Space commits the selected codepoint (equivalent to double-click).
    if (grid_region_active && selected_cp_ != 0)
    {
        const bool pressed =
            ImGui::Shortcut(ImGuiKey_Enter, ImGuiInputFlags_RouteFocused) ||
            ImGui::Shortcut(ImGuiKey_KeypadEnter, ImGuiInputFlags_RouteFocused) ||
            ImGui::Shortcut(ImGuiKey_Space, ImGuiInputFlags_RouteFocused);
        if (pressed)
        {
            double_clicked_ = true;
            double_clicked_cp_ = selected_cp_;
        }
    }

    // Keyboard navigation (keybindings-driven):
    // Use the shared KeyBindingsEngine so user remaps (and repeat) apply consistently.
    // Canvas/tools won't see these arrow intents because InputDispatcher gates them on FocusRouter
    // (only canvases that own keyboard receive injected nav events).
    auto maybe_nav = [&](const std::vector<uint32_t>& cps)
    {
        if (!grid_region_active || !keybinds)
            return;
        if (cps.empty())
            return;

        kb::EvalContext kctx;
        kctx.global = true;
        kctx.editor = true;      // nav.caret_* defaults are "editor" bindings
        kctx.canvas = false;
        kctx.selection = false;
        kctx.platform = kb::RuntimePlatform();

        const bool left  = keybinds->ActionPressed("nav.caret_left", kctx);
        const bool right = keybinds->ActionPressed("nav.caret_right", kctx);
        const bool up    = keybinds->ActionPressed("nav.caret_up", kctx);
        const bool down  = keybinds->ActionPressed("nav.caret_down", kctx);
        const bool home = keybinds->ActionPressed("nav.home", kctx);
        const bool end  = keybinds->ActionPressed("nav.end", kctx);
        const bool page_up   = keybinds->ActionPressed("nav.page_up", kctx);
        const bool page_down = keybinds->ActionPressed("nav.page_down", kctx);
        const bool doc_top    = keybinds->ActionPressed("nav.doc_top", kctx);
        const bool doc_bottom = keybinds->ActionPressed("nav.doc_bottom", kctx);

        if (!(left || right || up || down || home || end || page_up || page_down || doc_top || doc_bottom))
            return;

        int idx = 0;
        for (int i = 0; i < (int)cps.size(); ++i)
        {
            if (cps[(size_t)i] == selected_cp_)
            {
                idx = i;
                break;
            }
        }

        constexpr int kCols = 16;
        int new_idx = idx;
        if (left)  new_idx = std::max(0, new_idx - 1);
        if (right) new_idx = std::min((int)cps.size() - 1, new_idx + 1);
        if (up)    new_idx = std::max(0, new_idx - kCols);
        if (down)  new_idx = std::min((int)cps.size() - 1, new_idx + kCols);
        if (home)  new_idx = (new_idx / kCols) * kCols;
        if (end)   new_idx = std::min((int)cps.size() - 1, (new_idx / kCols) * kCols + (kCols - 1));
        // Page-up/down: jump by several rows (heuristic; deterministic across platforms).
        constexpr int kPageRows = 8;
        if (page_up)   new_idx = std::max(0, new_idx - kCols * kPageRows);
        if (page_down) new_idx = std::min((int)cps.size() - 1, new_idx + kCols * kPageRows);
        if (doc_top)    new_idx = 0;
        if (doc_bottom) new_idx = std::max(0, (int)cps.size() - 1);

        if (new_idx != idx)
        {
            selected_cp_ = cps[(size_t)new_idx];
            confusables_for_cp_ = 0xFFFFFFFFu;
            scroll_to_selected_ = true;
            MarkSelectionChanged();
        }
    };

    // Single layout: render directly in the main window (no full-height child window).
    // Full-height child windows can cover the window resize grip and make the window feel "stuck".

    // Visual cue: when the grid region is the active keyboard surface, draw a highlight border
    // around the remaining content region.
    if (allow_keyboard_nav && nav_region_ == NavRegion::Grid)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_NavHighlight);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 p1(p0.x + std::max(1.0f, avail.x), p0.y + std::max(1.0f, avail.y));
        dl->AddRect(p0, p1, col, 0.0f, 0, 2.0f);
    }

    // Focus anchor:
    // Make the grid region reachable via Tab/Shift+Tab and allow programmatic focus restore.
    // If a programmatic selection change requested focus sync, prefer focusing the grid only when the grid is
    // the current nav region.
    if (request_focus_selected_ && nav_region_ == NavRegion::Grid)
    {
        request_focus_grid_ = true;
        request_focus_selected_ = false;
    }
    if (request_focus_grid_)
    {
        ImGui::SetKeyboardFocusHere();
        request_focus_grid_ = false;
    }
    if (request_tab_focus_ && request_tab_stop_ == TabStop::Grid)
    {
        ImGui::SetKeyboardFocusHere();
        request_tab_focus_ = false;
    }
    ImGui::InvisibleButton("##grid_focus_anchor", ImVec2(1.0f, 1.0f), ImGuiButtonFlags_EnableNav);
    if (ImGui::IsItemFocused() || ImGui::IsItemActive())
    {
        nav_region_ = NavRegion::Grid;
        tab_stop_ = TabStop::Grid;
    }

    if (search_active_)
    {
        auto cps = FilteredSearchCpsForCurrentBlock();
        if (!cps.empty() && std::find(cps.begin(), cps.end(), selected_cp_) == cps.end())
            selected_cp_ = cps.front();
        maybe_nav(cps);
        RenderGrid(0, 0, &cps);
    }
    else
    {
        const ImFont* font = ImGui::GetFont();
        SyncRangeFromSelection();
        RebuildVisibleCache(range_start_, range_end_, font);
        if (!visible_cps_cache_.empty())
        {
            if (std::find(visible_cps_cache_.begin(), visible_cps_cache_.end(), selected_cp_) == visible_cps_cache_.end())
                selected_cp_ = visible_cps_cache_.front();
            maybe_nav(visible_cps_cache_);
            RenderGrid(0, 0, &visible_cps_cache_);
        }
        else
        {
            // No visible glyphs in this view.
            ImGui::TextDisabled("%s", PHOS_TR("character_picker.no_drawable_glyphs").c_str());
        }
    }

    // Mouse affordance: clicking anywhere in the grid/table area should activate the grid region,
    // even if the click didn't land on the 1x1 focus anchor.
    if (allow_keyboard_nav &&
        !want_text_input &&
        !any_popup_open &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        nav_region_ = NavRegion::Grid;
        tab_stop_ = TabStop::Grid;
    }
}

void CharacterPicker::RenderGrid(uint32_t view_start, uint32_t view_end,
                                 const std::vector<uint32_t>* explicit_cps)
{
    constexpr int kCols = 16;

    // Table sizing: keep cells square-ish.
    const float cell_w = 26.0f;
    const float rowhdr_w = 70.0f;

    const int total_cols = 1 + kCols; // row header + glyph columns
    ImGuiTableFlags flags =
        ImGuiTableFlags_BordersInner | ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY;

    // Give the table a fixed outer height so ScrollY works (fill remaining grid space).
    ImVec2 outer_size(0.0f, std::max(1.0f, ImGui::GetContentRegionAvail().y));

    // We handle arrow-key navigation via KeyBindingsEngine (nav.caret_*) at the picker level.
    // Disable ImGui nav for the table so it doesn't also move focus/selection on arrow presses.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (ImGui::BeginTable("##unicode_table", total_cols, flags, outer_size))
    {
        const std::string row_col = PHOS_TR("character_picker.row_col") + "###charpick_row";
        ImGui::TableSetupColumn(row_col.c_str(), ImGuiTableColumnFlags_WidthFixed, rowhdr_w);
        for (int c = 0; c < kCols; ++c)
        {
            char hdr[8];
            std::snprintf(hdr, sizeof(hdr), "%X", c);
            ImGui::TableSetupColumn(hdr, ImGuiTableColumnFlags_WidthFixed, cell_w);
        }
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();

        const int total_items = explicit_cps
            ? static_cast<int>(explicit_cps->size())
            : static_cast<int>(view_end >= view_start ? (view_end - view_start + 1u) : 0u);
        const int row_count = std::max(0, CeilDivInt(total_items, kCols));

        auto cpAt = [&](int r, int c) -> std::optional<uint32_t>
        {
            if (explicit_cps)
            {
                const int idx = r * kCols + c;
                if (idx < 0 || idx >= static_cast<int>(explicit_cps->size()))
                    return std::nullopt;
                return (*explicit_cps)[static_cast<size_t>(idx)];
            }
            const uint32_t base = view_start + static_cast<uint32_t>(r) * static_cast<uint32_t>(kCols);
            const uint32_t cp = base + static_cast<uint32_t>(c);
            if (cp < view_start || cp > view_end || !IsScalarValue(cp) || IsOmitted(cp))
                return std::nullopt;
            return cp;
        };

        ImGuiListClipper clipper;
        clipper.Begin(row_count);
        while (clipper.Step())
        {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
            {
                ImGui::TableNextRow();

                // Row header
                ImGui::TableSetColumnIndex(0);
                uint32_t row_base = 0;
                if (explicit_cps)
                {
                    const int idx0 = r * kCols;
                    if (idx0 < static_cast<int>(explicit_cps->size()))
                        row_base = (*explicit_cps)[static_cast<size_t>(idx0)];
                }
                else
                {
                    row_base = view_start + static_cast<uint32_t>(r) * static_cast<uint32_t>(kCols);
                }
                std::string row_lbl = CodePointHex(row_base);
                ImGui::TextUnformatted(row_lbl.c_str());

                for (int c = 0; c < kCols; ++c)
                {
                    ImGui::TableSetColumnIndex(1 + c);

                    const auto cp_opt = cpAt(r, c);
                    if (!cp_opt.has_value())
                    {
                        ImGui::TextUnformatted("");
                        continue;
                    }
                    const uint32_t cp = *cp_opt;

                    ImGui::PushID(static_cast<int>(cp));

                    const bool is_sel = (cp == selected_cp_);
                    std::string glyph = GlyphUtf8(cp);
                    if (glyph.empty())
                        glyph = " ";

                    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
                    // UX robustness: SelectOnClick (mouse-down) so selection works even if mouse-up is
                    // consumed by other layers (e.g. focus transitions / popup close policies).
                    if (ImGui::Selectable(glyph.c_str(), is_sel, ImGuiSelectableFlags_SelectOnClick,
                                          ImVec2(cell_w, cell_w)))
                    {
                        nav_region_ = NavRegion::Grid;
                        selected_cp_ = cp;
                        confusables_for_cp_ = 0xFFFFFFFFu;
                        MarkSelectionChanged();
                    }
                    ImGui::PopStyleVar();

                    // Double-click inserts into the canvas caret (handled by app-level wiring).
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        double_clicked_ = true;
                        double_clicked_cp_ = cp;
                    }

                    if (is_sel && scroll_to_selected_)
                    {
                        ImGui::SetScrollHereY(0.5f);
                        scroll_to_selected_ = false;
                    }

                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary))
                    {
                        const std::string h = CodePointHex(cp);
                        const std::string n = CharName(cp);
                        ImGui::BeginTooltip();
                        ImGui::Text("%s", h.c_str());
                        if (!n.empty())
                            ImGui::TextWrapped("%s", n.c_str());
                        ImGui::EndTooltip();
                    }

                    ImGui::PopID();
                }
            }
        }

        ImGui::EndTable();
    }
    ImGui::PopItemFlag();
}