#include "ui/sauce_editor_dialog.h"

#include "ui/ImGuiDatePicker.hpp"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string_view>
#include <vector>
#include <ctime>

static inline void TrimTo(std::string& s, size_t n)
{
    if (s.size() > n)
        s.resize(n);
}

// Trim UTF-8 string to at most `max_codepoints` Unicode codepoints (best-effort).
// This is used to enforce SAUCE fixed-width "Character" field limits in the UI.
static inline void TrimUtf8ToCodepoints(std::string& s, size_t max_codepoints)
{
    if (max_codepoints == 0)
    {
        s.clear();
        return;
    }

    size_t i = 0;
    size_t cps = 0;
    const size_t n = s.size();
    while (i < n && cps < max_codepoints)
    {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if (c < 0x80)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        else
            len = 1; // invalid leading byte; treat as single byte

        if (i + len > n)
            break;

        // Validate continuation bytes; if invalid, consume one byte.
        bool ok = true;
        for (size_t k = 1; k < len; ++k)
        {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80)
            {
                ok = false;
                break;
            }
        }
        if (!ok)
            len = 1;

        i += len;
        ++cps;
    }

    if (i < s.size())
        s.resize(i);
}

static inline size_t Utf8CodepointCount(std::string_view s)
{
    size_t i = 0;
    size_t cps = 0;
    while (i < s.size())
    {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else len = 1;
        if (i + len > s.size())
            break;
        bool ok = true;
        for (size_t k = 1; k < len; ++k)
        {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) len = 1;
        i += len;
        ++cps;
    }
    return cps;
}

static int Utf8ClampEditCallback(ImGuiInputTextCallbackData* data)
{
    if (!data || data->EventFlag != ImGuiInputTextFlags_CallbackEdit)
        return 0;
    if (!data->UserData)
        return 0;

    const size_t max_cps = *(const size_t*)data->UserData;
    if (max_cps == 0)
    {
        if (data->BufTextLen > 0)
            data->DeleteChars(0, data->BufTextLen);
        return 0;
    }

    // Find byte index after N codepoints.
    int i = 0;
    size_t cps = 0;
    while (i < data->BufTextLen && cps < max_cps)
    {
        unsigned char c = (unsigned char)data->Buf[i];
        int len = 1;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else len = 1;
        if (i + len > data->BufTextLen)
            break;
        bool ok = true;
        for (int k = 1; k < len; ++k)
        {
            unsigned char cc = (unsigned char)data->Buf[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) len = 1;
        i += len;
        ++cps;
    }

    if (i < data->BufTextLen)
        data->DeleteChars(i, data->BufTextLen - i);
    return 0;
}

static bool InputTextUtf8Clamped(const char* label, std::string& s, size_t max_cps)
{
    // Clamp live while editing (no flicker / no “type then truncate next frame”).
    // Still run our ASCII/control filtering on the stored std::string after.
    return ImGui::InputText(label, &s, ImGuiInputTextFlags_CallbackEdit, Utf8ClampEditCallback, &max_cps);
}

static void TooltipLastItem(const char* text)
{
    if (!text || !*text)
        return;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static inline void FilterAscii(std::string& s)
{
    // SAUCE spec is CP437; for now we keep UI permissive but drop control chars.
    // (Export will eventually encode to CP437 and replace unsupported chars.)
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
    {
        if (c == '\n' || c == '\t')
            continue;
        if (c >= 0x20)
            out.push_back((char)c);
    }
    s.swap(out);
}

static void GetTodayYmd(int& y, int& m, int& d)
{
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    y = tmv.tm_year + 1900;
    m = tmv.tm_mon + 1;
    d = tmv.tm_mday;
}

static bool ParseSauceDateYYYYMMDD(const std::string& s, int& y, int& m, int& d)
{
    if (s.size() != 8)
        return false;
    for (unsigned char c : s)
        if (!std::isdigit(c))
            return false;
    y = std::atoi(s.substr(0, 4).c_str());
    m = std::atoi(s.substr(4, 2).c_str());
    d = std::atoi(s.substr(6, 2).c_str());
    if (!(y >= 1900 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 && d <= 31))
        return false;
    auto is_leap = [](int yy) {
        return (yy % 400 == 0) || ((yy % 4 == 0) && (yy % 100 != 0));
    };
    auto days_in_month = [&](int mm, int yy) -> int {
        static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int days = mdays[mm - 1];
        if (mm == 2 && is_leap(yy))
            days = 29;
        return days;
    };
    return d <= days_in_month(m, y);
}

static void FormatSauceDateYYYYMMDD(int y, int m, int d, std::string& out)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", y, m, d);
    out = buf;
}

static std::tm MakeLocalDateTm(int y, int m, int d)
{
    std::tm t{};
    t.tm_isdst = -1;
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    return t;
}

static void ClampSauceDateTm(std::tm& t)
{
    int y = std::clamp(t.tm_year + 1900, 1900, 9999);
    int m = std::clamp(t.tm_mon + 1, 1, 12);
    int d = std::clamp(t.tm_mday, 1, 31);
    // Keep it simple: if d is invalid for month/year, clamp down.
    auto is_leap = [](int yy) {
        return (yy % 400 == 0) || ((yy % 4 == 0) && (yy % 100 != 0));
    };
    auto days_in_month = [&](int mm, int yy) -> int {
        static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int days = mdays[mm - 1];
        if (mm == 2 && is_leap(yy))
            days = 29;
        return days;
    };
    d = std::min(d, days_in_month(m, y));
    t = MakeLocalDateTm(y, m, d);
}

void SauceEditorDialog::OpenFromCanvas(const AnsiCanvas& canvas)
{
    m_meta = canvas.GetSauceMeta();
    m_open = true;
    m_open_queued = true;

    m_title = m_meta.title;
    m_author = m_meta.author;
    m_group = m_meta.group;
    m_tinfos = m_meta.tinfos;

    // Date picker: if date is missing/unparseable, prefill with "creation date" = today.
    {
        int y = 0, m = 0, d = 0;
        if (!ParseSauceDateYYYYMMDD(m_meta.date, y, m, d))
            GetTodayYmd(y, m, d);
        m_date = MakeLocalDateTm(y, m, d);
        ClampSauceDateTm(m_date);
    }

    // Join comments into editable multiline form.
    {
        std::string txt;
        for (size_t i = 0; i < m_meta.comments.size(); ++i)
        {
            txt += m_meta.comments[i];
            if (i + 1 < m_meta.comments.size())
                txt.push_back('\n');
        }
        m_comments_text = std::move(txt);
    }

    m_data_type = (int)m_meta.data_type;
    m_file_type = (int)m_meta.file_type;
    m_tinfo1 = (int)m_meta.tinfo1;
    m_tinfo2 = (int)m_meta.tinfo2;
    m_tinfo3 = (int)m_meta.tinfo3;
    m_tinfo4 = (int)m_meta.tinfo4;
    m_tflags = (int)m_meta.tflags;
}

void SauceEditorDialog::ClampAndSanitizeForSauce(AnsiCanvas::ProjectState::SauceMeta& meta)
{
    FilterAscii(meta.title);
    FilterAscii(meta.author);
    FilterAscii(meta.group);
    FilterAscii(meta.tinfos);
    FilterAscii(meta.date);

    // SAUCE fixed widths.
    TrimUtf8ToCodepoints(meta.title, 35);
    TrimUtf8ToCodepoints(meta.author, 20);
    TrimUtf8ToCodepoints(meta.group, 20);
    TrimUtf8ToCodepoints(meta.tinfos, 22);
    TrimTo(meta.date, 8);

    // Date: keep digits only (best-effort).
    {
        std::string d;
        d.reserve(meta.date.size());
        for (unsigned char c : meta.date)
            if (std::isdigit(c))
                d.push_back((char)c);
        meta.date = d;
        TrimTo(meta.date, 8);
    }

    // Comments: <= 255 lines, each <= 64 chars.
    if (meta.comments.size() > 255)
        meta.comments.resize(255);
    for (std::string& line : meta.comments)
    {
        FilterAscii(line);
        TrimUtf8ToCodepoints(line, 64);
    }
}

void SauceEditorDialog::Render(AnsiCanvas& canvas, const char* popup_id)
{
    if (!m_open || !popup_id || !*popup_id)
        return;

    if (m_open_queued)
    {
        ImGui::OpenPopup(popup_id);
        m_open_queued = false;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize;
    bool open = true;
    if (!ImGui::BeginPopupModal(popup_id, &open, flags))
        return;

    if (!open)
    {
        ImGui::CloseCurrentPopup();
        m_open = false;
        ImGui::EndPopup();
        return;
    }

    // Fixed text fields (with spec lengths)
    InputTextUtf8Clamped("Title", m_title, 35);
    TooltipLastItem("SAUCE Title (35 chars): the title of the file/artwork. Fixed-width field in the 128-byte SAUCE record (space-padded when written).");
    FilterAscii(m_title);
    TrimUtf8ToCodepoints(m_title, 35);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu/35", Utf8CodepointCount(m_title));

    InputTextUtf8Clamped("Author", m_author, 20);
    TooltipLastItem("SAUCE Author (20 chars): the (nick)name or handle of the creator of the file/artwork. Fixed-width field in the SAUCE record.");
    FilterAscii(m_author);
    TrimUtf8ToCodepoints(m_author, 20);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu/20", Utf8CodepointCount(m_author));

    InputTextUtf8Clamped("Group", m_group, 20);
    TooltipLastItem("SAUCE Group (20 chars): the name of the group or company the creator is affiliated with. Fixed-width field in the SAUCE record.");
    FilterAscii(m_group);
    TrimUtf8ToCodepoints(m_group, 20);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu/20", Utf8CodepointCount(m_group));

    // Date picker: calendar-style dropdown (stores SAUCE as CCYYMMDD).
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Date:");
        ImGui::SameLine();
        ClampSauceDateTm(m_date);
        ImGui::SetNextItemWidth(220.0f);
        ImGui::DatePicker("##sauce_date", m_date, false, 0.0f);
        TooltipLastItem("SAUCE Date (8 chars): creation date in the format CCYYMMDD. Example: May 4, 2013 is stored as \"20130504\".");
        ClampSauceDateTm(m_date);

        // Show exact SAUCE-encoded value for clarity/debugging.
        std::string ymd;
        FormatSauceDateYYYYMMDD(m_date.tm_year + 1900, m_date.tm_mon + 1, m_date.tm_mday, ymd);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", ymd.c_str());
    }

    InputTextUtf8Clamped("TInfoS", m_tinfos, 22);
    TooltipLastItem("SAUCE TInfoS (22 bytes, ZString): type-dependent string information field. For Character data, this is commonly used as the font name (FontName). Unused bytes are NUL-padded when written.");
    FilterAscii(m_tinfos);
    TrimUtf8ToCodepoints(m_tinfos, 22);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu/22", Utf8CodepointCount(m_tinfos));

    ImGui::Separator();

    ImGui::TextUnformatted("Comments:");
    {
        // Fill available width so the right edge aligns with the window content region.
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::InputTextMultiline("##sauce_comments", &m_comments_text, ImVec2(w, 180.0f));
        TooltipLastItem("SAUCE Comment Block: optional extra comment lines. Up to 255 lines, each 64 characters wide. When present in a file, it is stored as \"COMNT\" + (lines*64) right before the 128-byte SAUCE record.");
    }

    // Advanced/raw fields (hide low-value internals like FileSize by default).
    if (ImGui::CollapsingHeader("Advanced", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::InputInt("DataType (u8)", &m_data_type);
        TooltipLastItem(
            "SAUCE DataType: what kind of content this file is.\n"
            "\n"
            "Common values:\n"
            "- 1: Character (ANSI/ASCII streams)\n"
            "- 2: Bitmap (images/animations)\n"
            "- 5: BinaryText (.BIN screen memory dump)\n"
            "- 6: XBin (extended .BIN)\n"
            "- 7: Archive, 8: Executable (metadata is usually not meaningful for rendering)\n"
            "\n"
            "This selection changes how viewers interpret FileType/TInfo/TFlags/TInfoS.");
        ImGui::InputInt("FileType (u8)", &m_file_type);
        TooltipLastItem(
            "SAUCE FileType: subtype for the chosen DataType.\n"
            "\n"
            "For DataType=Character, common values are:\n"
            "- 0: ASCII (plain text)\n"
            "- 1: ANSi (ANSI escape codes)\n"
            "- 2: ANSiMation\n"
            "- 8: TundraDraw\n"
            "\n"
            "For DataType=Bitmap, FileType selects the bitmap format (GIF/PNG/JPG/etc.).\n"
            "For DataType=BinaryText, FileType is special: it encodes the character width (see tooltip on TInfo1/TInfo2).\n"
            "This helps viewers/editors choose sensible defaults when rendering.");
        ImGui::InputInt("TInfo1 (u16)", &m_tinfo1);
        TooltipLastItem(
            "SAUCE TInfo1: type-dependent numeric info.\n"
            "\n"
            "Common meanings:\n"
            "- DataType=Character: character width (columns), e.g. 80\n"
            "- DataType=Bitmap: pixel width\n"
            "- DataType=XBin: character width (columns)\n"
            "\n"
            "Special case:\n"
            "- DataType=BinaryText: TInfo1 is not used; instead FileType stores half the character width.\n"
            "  (So FileType=40 implies 80 columns.)\n"
            "\n"
            "If you don't know, leaving 0 is usually safe (many files in the wild are inconsistent).");
        ImGui::InputInt("TInfo2 (u16)", &m_tinfo2);
        TooltipLastItem(
            "SAUCE TInfo2: type-dependent numeric info.\n"
            "\n"
            "Common meanings:\n"
            "- DataType=Character: number of screen lines (rows), e.g. 25 or 50\n"
            "- DataType=Bitmap: pixel height\n"
            "- DataType=XBin: number of lines (rows)\n"
            "\n"
            "Special case:\n"
            "- DataType=BinaryText: height is typically inferred from file size and FileType (width/2).\n"
            "\n"
            "If you don't know, leaving 0 is usually safe.");
        ImGui::InputInt("TInfo3 (u16)", &m_tinfo3);
        TooltipLastItem(
            "SAUCE TInfo3: extra type-dependent numeric info.\n"
            "\n"
            "Common meanings:\n"
            "- DataType=Bitmap: pixel depth (bits per pixel)\n"
            "\n"
            "For most Character/XBin/BinaryText files this is unused and typically 0.");
        ImGui::InputInt("TInfo4 (u16)", &m_tinfo4);
        TooltipLastItem(
            "SAUCE TInfo4: extra type-dependent numeric info.\n"
            "\n"
            "Most common art formats leave this as 0.\n"
            "Some DataTypes reserve it for additional subtype details, but it is rarely used in practice.");
        ImGui::InputInt("TFlags (u8)", &m_tflags);
        TooltipLastItem(
            "SAUCE TFlags: type-dependent flags.\n"
            "\n"
            "Common meanings:\n"
            "- DataType=Character: ANSiFlags (rendering hints)\n"
            "  - iCE Color / non-blink background mode (enables 16 background colors)\n"
            "  - (newer SAUCE) font width/aspect-ratio hints\n"
            "- DataType=BinaryText: also commonly uses ANSiFlags\n"
            "\n"
            "If you don't use flags, 0 is fine.");
    }

    ImGui::Separator();

    // Buttons
    if (ImGui::Button("Cancel"))
    {
        ImGui::CloseCurrentPopup();
        m_open = false;
        ImGui::EndPopup();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        m_meta = AnsiCanvas::ProjectState::SauceMeta{};
        m_title.clear();
        m_author.clear();
        m_group.clear();
        m_tinfos.clear();
        m_comments_text.clear();
        m_data_type = 1;
        m_file_type = 1;
        m_tinfo1 = m_tinfo2 = m_tinfo3 = m_tinfo4 = 0;
        m_tflags = 0;
        {
            int y = 0, m = 0, d = 0;
            GetTodayYmd(y, m, d);
            m_date = MakeLocalDateTm(y, m, d);
            ClampSauceDateTm(m_date);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        // Rebuild meta from working buffers.
        AnsiCanvas::ProjectState::SauceMeta meta = m_meta;
        meta.title = m_title;
        meta.author = m_author;
        meta.group = m_group;
        ClampSauceDateTm(m_date);
        FormatSauceDateYYYYMMDD(m_date.tm_year + 1900, m_date.tm_mon + 1, m_date.tm_mday, meta.date);
        meta.tinfos = m_tinfos;

        meta.data_type = (std::uint8_t)std::clamp(m_data_type, 0, 255);
        meta.file_type = (std::uint8_t)std::clamp(m_file_type, 0, 255);
        meta.tinfo1 = (std::uint16_t)std::clamp(m_tinfo1, 0, 65535);
        meta.tinfo2 = (std::uint16_t)std::clamp(m_tinfo2, 0, 65535);
        meta.tinfo3 = (std::uint16_t)std::clamp(m_tinfo3, 0, 65535);
        meta.tinfo4 = (std::uint16_t)std::clamp(m_tinfo4, 0, 65535);
        meta.tflags = (std::uint8_t)std::clamp(m_tflags, 0, 255);

        // Split comments by newline.
        meta.comments.clear();
        {
            std::istringstream iss(m_comments_text);
            std::string line;
            while (std::getline(iss, line))
                meta.comments.push_back(line);
        }

        ClampAndSanitizeForSauce(meta);

        // Auto-manage "present": if the user saved any meaningful SAUCE content, mark present.
        //
        // Important: non-text fields (DataType/FileType/TInfo/TFlags) are meaningful too.
        // Otherwise a user can set (or we can auto-fill) cols/rows but still end up with
        // `present=false`, which prevents writing SAUCE on export.
        const bool any_text =
            !(meta.title.empty() &&
              meta.author.empty() &&
              meta.group.empty() &&
              meta.date.empty() &&
              meta.tinfos.empty() &&
              meta.comments.empty());
        const bool any_numeric =
            meta.file_size != 0 ||
            meta.data_type != 0 ||
            meta.file_type != 0 ||
            meta.tinfo1 != 0 ||
            meta.tinfo2 != 0 ||
            meta.tinfo3 != 0 ||
            meta.tinfo4 != 0 ||
            meta.tflags != 0;
        meta.present = any_text || any_numeric;
        canvas.SetSauceMeta(meta);

        ImGui::CloseCurrentPopup();
        m_open = false;
        ImGui::EndPopup();
        return;
    }

    ImGui::EndPopup();
}


