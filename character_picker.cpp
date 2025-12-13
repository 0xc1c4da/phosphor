#include "character_picker.h"

#include "imgui.h"
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
    return nm ? std::string(nm) : std::string("Unknown_Block");
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

// -------------------- blocks --------------------

CharacterPicker::CharacterPicker()
{
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
        selected_cp_ = search_results_.front().cp;
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
        if (r.cp >= block_start && r.cp <= block_end)
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
                confusable_cps_.push_back(static_cast<uint32_t>(cp));
                printed++;
                if (printed >= limit)
                    return;
            }
        }
    }
}

// -------------------- UI --------------------

bool CharacterPicker::Render(const char* window_title, bool* p_open)
{
    EnsureBlocksLoaded();

    if (!ImGui::Begin(window_title, p_open, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::End();
        return (p_open == nullptr) ? true : *p_open;
    }

    RenderTopBar();
    ImGui::Separator();
    RenderGridAndSidePanel();

    ImGui::End();
    return (p_open == nullptr) ? true : *p_open;
}

void CharacterPicker::RenderTopBar()
{
    // Block dropdown
    {
        const char* preview = "All Unicode (by Plane)";
        if (block_index_ > 0)
        {
            const int bi = block_index_ - 1;
            if (bi >= 0 && bi < static_cast<int>(blocks_.size()))
                preview = blocks_[bi].name.c_str();
        }

        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::BeginCombo("Block", preview))
        {
            bool sel_all = (block_index_ == 0);
            if (ImGui::Selectable("All Unicode (by Plane)", sel_all))
            {
                block_index_ = 0;
                subpage_index_ = std::clamp(subpage_index_, 0, 16);
                SyncRangeFromSelection();
                ClampSelectionToCurrentView();
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
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
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
                ? std::string("No results")
                : ("Results " + std::to_string(start_i + 1) + ".." + std::to_string(end_i + 1) +
                   " / " + std::to_string(cps.size()));

            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::BeginCombo("Page", preview.c_str()))
            {
                for (int p = 0; p < page_count; ++p)
                {
                    const int s = p * page_size;
                    const int e = std::min(static_cast<int>(cps.size()), s + page_size) - 1;
                    std::string label = "Results " + std::to_string(s + 1) + ".." + std::to_string(e + 1);
                    bool sel = (p == subpage_index_);
                    if (ImGui::Selectable(label.c_str(), sel))
                        subpage_index_ = p;
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        else if (block_index_ == 0)
        {
            // Planes 0..16
            const int plane = std::clamp(subpage_index_, 0, 16);
            std::string preview = "Plane " + std::to_string(plane) + "  (" +
                                  CodePointHex(static_cast<uint32_t>(plane) * 0x10000u) + ".." +
                                  CodePointHex(std::min(static_cast<uint32_t>(plane) * 0x10000u + 0xFFFFu, 0x10FFFFu)) + ")";

            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::BeginCombo("Subpage", preview.c_str()))
            {
                for (int p = 0; p <= 16; ++p)
                {
                    const uint32_t ps = static_cast<uint32_t>(p) * 0x10000u;
                    const uint32_t pe = std::min(ps + 0xFFFFu, 0x10FFFFu);
                    std::string label = "Plane " + std::to_string(p) + "  (" +
                                        CodePointHex(ps) + ".." + CodePointHex(pe) + ")";
                    const bool sel = (p == plane);
                    if (ImGui::Selectable(label.c_str(), sel))
                    {
                        subpage_index_ = p;
                        SyncRangeFromSelection();
                        ClampSelectionToCurrentView();
                        scroll_to_selected_ = true;
                    }
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
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
            if (ImGui::BeginCombo("Jump", preview.c_str()))
            {
                for (int p = 0; p < page_count; ++p)
                {
                    const uint32_t s = b.start + static_cast<uint32_t>(p) * kPageSize;
                    const uint32_t e = std::min(s + (kPageSize - 1u), b.end);
                    std::string label = CodePointHex(s) + ".." + CodePointHex(e);
                    const bool sel = (p == subpage_index_);
                    if (ImGui::Selectable(label.c_str(), sel))
                    {
                        subpage_index_ = p;
                        selected_cp_ = s;
                        confusables_for_cp_ = 0xFFFFFFFFu;
                        scroll_to_selected_ = true;
                    }
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
    }

    ImGui::SameLine();

    // Search
    {
        ImGui::SetNextItemWidth(340.0f);
        if (ImGui::InputTextWithHint("Search", "unicode name (e.g. greek small letter eta)", &search_query_,
                                     ImGuiInputTextFlags_EnterReturnsTrue))
        {
            search_dirty_ = true;
            PerformSearch();
            subpage_index_ = 0;
            SyncRangeFromSelection();
            ClampSelectionToCurrentView();
        }

        ImGui::SameLine();
        if (ImGui::Button("Go"))
        {
            search_dirty_ = true;
            PerformSearch();
            subpage_index_ = 0;
            SyncRangeFromSelection();
            ClampSelectionToCurrentView();
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            ClearSearch();
            subpage_index_ = 0;
            SyncRangeFromSelection();
            ClampSelectionToCurrentView();
        }
    }
}

void CharacterPicker::RenderGridAndSidePanel()
{
    UpdateConfusablesIfNeeded();

    // Split layout: left grid, right sidebar.
    const float sidebar_w = 360.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float grid_w = std::max(200.0f, avail.x - sidebar_w - ImGui::GetStyle().ItemSpacing.x);

    ImGui::BeginChild("##picker_grid", ImVec2(grid_w, 0.0f), true, ImGuiWindowFlags_None);

    if (search_active_)
    {
        auto cps = FilteredSearchCpsForCurrentBlock();
        RenderGrid(0, 0, &cps);
        HandleGridKeyboardNavigation(0, 0, &cps);
    }
    else
    {
        SyncRangeFromSelection();
        ClampSelectionToCurrentView();
        RenderGrid(range_start_, range_end_, nullptr);
        HandleGridKeyboardNavigation(range_start_, range_end_, nullptr);
    }

    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##picker_sidebar", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_None);

    // Selected info + copy
    {
        const std::string hex = CodePointHex(selected_cp_);
        const std::string glyph = GlyphUtf8(selected_cp_);
        const std::string nm = CharName(selected_cp_);
        const std::string blk = BlockNameFor(selected_cp_);

        ImGui::TextUnformatted("Selected");
        ImGui::Separator();
        ImGui::Text("%s", hex.c_str());
        if (!glyph.empty())
            ImGui::Text("Glyph: %s", glyph.c_str());
        if (!nm.empty())
            ImGui::TextWrapped("Name: %s", nm.c_str());
        ImGui::TextWrapped("Block: %s", blk.c_str());

        if (ImGui::Button("Copy Character") && !glyph.empty())
            ImGui::SetClipboardText(glyph.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Copy U+XXXX"))
            ImGui::SetClipboardText(hex.c_str());
    }

    ImGui::Separator();

    // Confusables list
    ImGui::TextUnformatted("Confusables (ICU spoof)");
    ImGui::SameLine();
    ImGui::TextDisabled("(limit %d)", confusables_limit_);

    const bool conf_visible =
        ImGui::BeginChild("##confusables", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_None);
    if (conf_visible)
    {
        if (confusable_cps_.empty())
        {
            ImGui::TextDisabled("No confusables found (or ICU data unavailable).");
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
                    // Try to keep block selection consistent with the clicked cp.
                    // If currently in a specific block, leave it; otherwise, jump plane.
                    if (block_index_ == 0)
                    {
                        subpage_index_ = static_cast<int>(cp / 0x10000u);
                        SyncRangeFromSelection();
                    }
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::EndChild();
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

    if (ImGui::BeginTable("##unicode_table", total_cols, flags, outer_size))
    {
        ImGui::TableSetupColumn("Row", ImGuiTableColumnFlags_WidthFixed, rowhdr_w);
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
            if (cp < view_start || cp > view_end || !IsScalarValue(cp))
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
                    if (ImGui::Selectable(glyph.c_str(), is_sel, ImGuiSelectableFlags_None,
                                          ImVec2(cell_w, cell_w)))
                    {
                        selected_cp_ = cp;
                        confusables_for_cp_ = 0xFFFFFFFFu;
                    }
                    ImGui::PopStyleVar();

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
}

void CharacterPicker::HandleGridKeyboardNavigation(uint32_t view_start, uint32_t view_end,
                                                   const std::vector<uint32_t>* explicit_cps)
{
    // Only react when the grid child is focused.
    // (We render this inside the grid child.)
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
        return;

    constexpr int kCols = 16;
    auto move_to_index = [&](int idx)
    {
        if (!explicit_cps)
            return;
        if (idx < 0 || idx >= static_cast<int>(explicit_cps->size()))
            return;
        selected_cp_ = (*explicit_cps)[static_cast<size_t>(idx)];
        confusables_for_cp_ = 0xFFFFFFFFu;
    };

    auto find_index = [&]() -> int
    {
        if (!explicit_cps)
            return -1;
        for (int i = 0; i < static_cast<int>(explicit_cps->size()); ++i)
            if ((*explicit_cps)[static_cast<size_t>(i)] == selected_cp_)
                return i;
        return -1;
    };

    const bool left  = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    const bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    const bool up    = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    const bool down  = ImGui::IsKeyPressed(ImGuiKey_DownArrow);

    if (!(left || right || up || down))
        return;

    if (explicit_cps)
    {
        int idx = find_index();
        if (idx < 0)
            idx = 0;

        if (left)  idx -= 1;
        if (right) idx += 1;
        if (up)    idx -= kCols;
        if (down)  idx += kCols;

        idx = std::clamp(idx, 0, std::max(0, static_cast<int>(explicit_cps->size()) - 1));
        move_to_index(idx);
        scroll_to_selected_ = true;
        return;
    }

    // Range view navigation: step within the full range (table will scroll).
    uint32_t cp = selected_cp_;
    if (left && cp > view_start)  cp -= 1;
    if (right && cp < view_end)  cp += 1;
    if (up && cp >= view_start + kCols)   cp -= kCols;
    if (down && cp + kCols <= view_end)   cp += kCols;

    if (cp != selected_cp_)
    {
        selected_cp_ = cp;
        confusables_for_cp_ = 0xFFFFFFFFu;
        scroll_to_selected_ = true;
    }
}


