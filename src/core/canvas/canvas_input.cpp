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

AnsiCanvas::KeyEvents AnsiCanvas::TakeKeyEvents()
{
    KeyEvents out = m_key_events;
    m_key_events = KeyEvents{};
    return out;
}

// ---- end inlined from canvas_input.inc ----


