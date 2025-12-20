#pragma once

struct AppState;

namespace app
{
// Run one frame of the main loop: pump events, run UI, render, and update `st.done`.
void RunFrame(AppState& st);
} // namespace app


