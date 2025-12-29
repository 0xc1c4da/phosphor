#include "core/canvas/canvas_internal.h"

#include "core/key_bindings.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// ---- inlined from canvas_input.inc ----
void AnsiCanvas::TakeTypedCodepoints(std::vector<char32_t>& out)
{
    out.clear();
    out.swap(m_typed_queue);
}

void AnsiCanvas::QueueTypedUtf8(std::string_view utf8)
{
    if (utf8.empty())
        return;

    // Reuse the canvas' internal UTF-8 decoding helper (best effort).
    std::string bytes(utf8);
    std::vector<char32_t> cps;
    DecodeUtf8(bytes, cps);
    if (cps.empty())
        return;

    m_typed_queue.insert(m_typed_queue.end(), cps.begin(), cps.end());
}

void AnsiCanvas::SetImeCompositionUtf8(std::string_view utf8, int cursor, int selection_len)
{
    // Empty string clears composition state.
    if (utf8.empty())
    {
        ClearImeComposition();
        return;
    }

    m_ime.active = true;
    m_ime.utf8.assign(utf8.begin(), utf8.end());

    // Decode into codepoints for stable cursor/selection semantics.
    std::vector<char32_t> cps;
    DecodeUtf8(m_ime.utf8, cps);
    m_ime.cps = std::move(cps);

    const int n = (int)m_ime.cps.size();
    if (cursor < 0) cursor = 0;
    if (cursor > n) cursor = n;
    if (selection_len < 0) selection_len = 0;
    if (cursor + selection_len > n)
        selection_len = std::max(0, n - cursor);

    m_ime.cursor = cursor;
    m_ime.selection_len = selection_len;
}

void AnsiCanvas::ClearImeComposition()
{
    m_ime = ImeCompositionState{};
}

bool AnsiCanvas::GetImeTextInputAreaPx(int& out_x, int& out_y, int& out_w, int& out_h, int& out_cursor_x) const
{
    if (!m_last_view.valid)
        return false;
    if (!(m_last_view.cell_w > 0.0f) || !(m_last_view.cell_h > 0.0f))
        return false;

    ImVec2 caret{};
    if (!GetCaretScreenPos(caret))
        return false;

    const int len_cells = m_ime.active ? std::max(1, (int)m_ime.cps.size()) : 1;
    const float fw = m_last_view.cell_w * (float)len_cells;
    const float fh = m_last_view.cell_h;

    out_x = (int)std::floor(caret.x + 0.5f);
    out_y = (int)std::floor(caret.y + 0.5f);
    out_w = (int)std::ceil(std::max(1.0f, fw));
    out_h = (int)std::ceil(std::max(1.0f, fh));

    const int cur_cells = m_ime.active ? std::clamp(m_ime.cursor, 0, len_cells) : 0;
    out_cursor_x = (int)std::floor((float)cur_cells * m_last_view.cell_w + 0.5f);
    return true;
}

AnsiCanvas::KeyEvents AnsiCanvas::TakeKeyEvents()
{
    KeyEvents out = m_key_events;
    m_key_events = KeyEvents{};
    return out;
}

// ---- end inlined from canvas_input.inc ----


