#include "ui/export_dialog.h"

#include "imgui.h"
#include "core/i18n.h"
#include "io/file_dialog_tags.h"
#include "io/io_manager.h"
#include "io/sdl_file_dialog_queue.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"

#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

static std::string JoinExtsForDialog(const std::vector<std::string_view>& exts)
{
    std::string out;
    for (size_t i = 0; i < exts.size(); ++i)
    {
        if (i) out.push_back(';');
        out.append(exts[i].begin(), exts[i].end());
    }
    return out;
}

static std::string JoinExtsForLabel(const std::vector<std::string_view>& exts)
{
    std::string out;
    for (size_t i = 0; i < exts.size(); ++i)
    {
        if (i) out.push_back(';');
        out.append("*.");
        out.append(exts[i].begin(), exts[i].end());
    }
    return out;
}

static std::string MakeFilterLabel(std::string_view base, const std::vector<std::string_view>& exts)
{
    if (exts.empty())
        return std::string(base);
    return std::string(base) + " (" + JoinExtsForLabel(exts) + ")";
}

static bool IsUri(const std::string& s)
{
    return s.find("://") != std::string::npos;
}

static std::string EnsureExtension(const std::string& chosen, std::string_view ext_no_dot)
{
    if (IsUri(chosen))
        return chosen;
    try
    {
        fs::path p(chosen);
        if (!p.extension().empty())
            return chosen;
    }
    catch (...) {}
    if (ext_no_dot.empty() || ext_no_dot == "*")
        return chosen;
    return chosen + "." + std::string(ext_no_dot);
}

static std::string SuggestedPath(const IoManager& io, const AnsiCanvas* canvas, std::string_view ext_no_dot)
{
    fs::path base;
    try
    {
        base = io.GetLastDir().empty() ? fs::current_path() : fs::path(io.GetLastDir());
    }
    catch (...)
    {
        base = io.GetLastDir().empty() ? fs::path(".") : fs::path(io.GetLastDir());
    }

    std::string stem = "export";
    if (canvas)
    {
        const std::string src = canvas->GetFilePath();
        if (!src.empty() && !IsUri(src))
        {
            try
            {
                fs::path p(src);
                if (!p.stem().empty())
                    stem = p.stem().string();
            }
            catch (...) {}
        }
    }

    std::string filename = stem;
    if (!ext_no_dot.empty() && ext_no_dot != "*")
        filename += "." + std::string(ext_no_dot);
    return (base / filename).string();
}

static void HelpMarker(const char* text)
{
    ImGui::TextDisabled("%s", PHOS_TR("common.help_marker").c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace

void ExportDialog::Render(const char* title,
                          SDL_Window* window,
                          SdlFileDialogQueue& dialogs,
                          IoManager& io,
                          AnsiCanvas* focused_canvas,
                          SessionState* session,
                          bool apply_placement_this_frame)
{
    if (!open_)
        return;

    // Initialize defaults once.
    if (!initialized_)
    {
        initialized_ = true;

        // Load reasonable defaults from presets.
        if (const auto* p = formats::ansi::FindPreset(formats::ansi::PresetId::ModernUtf8_256))
            ansi_opt_ = p->export_;
        if (const auto* p = formats::plaintext::FindPreset(formats::plaintext::PresetId::PlainUtf8))
            text_opt_ = p->export_;

        ansi_override_default_fg_ = (ansi_opt_.default_fg != 0);
        ansi_override_default_bg_ = (ansi_opt_.default_bg != 0);
    }

    // If the dialog was opened with a specific tab requested, we will select it
    // exactly once (this frame) and then let ImGui manage tab selection normally.
    const Tab select_tab_this_frame = requested_tab_;
    const bool should_force_select = apply_requested_tab_;
    apply_requested_tab_ = false;

    // Default window size, but prefer persisted placement.
    if (session && apply_placement_this_frame)
    {
        auto it = session->imgui_windows.find(title);
        const bool has = (it != session->imgui_windows.end() && it->second.valid);
        if (!has)
            ImGui::SetNextWindowSize(ImVec2(860, 620), ImGuiCond_Always);
    }
    else if (!session)
    {
        ImGui::SetNextWindowSize(ImVec2(860, 620), ImGuiCond_FirstUseEver);
    }

    if (session)
        ApplyImGuiWindowPlacement(*session, title, apply_placement_this_frame);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_None |
        (session ? GetImGuiWindowChromeExtraFlags(*session, title) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, title);
    if (!ImGui::Begin(title, &open_, flags))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, title);
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return;
    }
    if (session)
        CaptureImGuiWindowPlacement(*session, title);
    if (session)
    {
        ApplyImGuiWindowChromeZOrder(session, title);
        RenderImGuiWindowChromeMenu(session, title);
    }

    const bool has_canvas = (focused_canvas != nullptr);
    if (!has_canvas)
    {
        const std::string s = PHOS_TR("export_dialog.no_active_canvas");
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", s.c_str());
        ImGui::Separator();
    }

    if (ImGui::BeginTabBar("##export_tabs"))
    {
        const auto begin_tab = [&](Tab t, const char* tab_label) -> bool
        {
            ImGuiTabItemFlags tab_flags = ImGuiTabItemFlags_None;
            if (should_force_select && t == select_tab_this_frame)
                tab_flags |= ImGuiTabItemFlags_SetSelected;
            if (!ImGui::BeginTabItem(tab_label, nullptr, tab_flags))
                return false;
            active_tab_ = t;
            return true;
        };

        // ---------------------------------------------------------------------
        // ANSI
        // ---------------------------------------------------------------------
        if (begin_tab(Tab::Ansi, PHOS_TR("export_dialog.tabs.ansi").c_str()))
        {
            // Preset selector (applies immediately)
            {
                const auto& presets = formats::ansi::Presets();
                const std::string unnamed = PHOS_TR("export_dialog.unnamed_preset");
                std::vector<const char*> items;
                items.reserve(presets.size());
                for (const auto& p : presets)
                    items.push_back(p.name ? p.name : unnamed.c_str());

                int idx = std::clamp(ansi_preset_idx_, 0, (int)items.size() - 1);
                ImGui::SetNextItemWidth(420.0f);
                const std::string preset_lbl = PHOS_TR("export_dialog.preset");
                if (ImGui::Combo(preset_lbl.c_str(), &idx, items.data(), (int)items.size()))
                {
                    ansi_preset_idx_ = idx;
                    ansi_opt_ = presets[(size_t)idx].export_;
                    ansi_override_default_fg_ = (ansi_opt_.default_fg != 0);
                    ansi_override_default_bg_ = (ansi_opt_.default_bg != 0);
                }
                if (idx >= 0 && (size_t)idx < presets.size() && presets[(size_t)idx].description)
                {
                    ImGui::TextDisabled("%s", presets[(size_t)idx].description);
                }
            }

            ImGui::Separator();

            // Source
            {
                const std::string src0 = PHOS_TR("export_dialog.source_items.composite");
                const std::string src1 = PHOS_TR("export_dialog.source_items.active_layer");
                const char* items[] = { src0.c_str(), src1.c_str() };
                int v = (ansi_opt_.source == formats::ansi::ExportOptions::Source::Composite) ? 0 : 1;
                const std::string source_lbl = PHOS_TR("export_dialog.source");
                if (ImGui::Combo(source_lbl.c_str(), &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.source = (v == 0) ? formats::ansi::ExportOptions::Source::Composite
                                                : formats::ansi::ExportOptions::Source::ActiveLayer;
            }

            // Encoding / newline
            {
                const std::string e0 = PHOS_TR("export_dialog.ansi_tab.text_encoding_items.eight_bit");
                const std::string e1 = PHOS_TR("export_dialog.ansi_tab.text_encoding_items.utf8");
                const std::string e2 = PHOS_TR("export_dialog.ansi_tab.text_encoding_items.utf8_bom");
                const char* items[] = { e0.c_str(), e1.c_str(), e2.c_str() };
                int v = (int)ansi_opt_.text_encoding;
                const std::string te_lbl = PHOS_TR("export_dialog.ansi_tab.text_encoding");
                if (ImGui::Combo(te_lbl.c_str(), &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.text_encoding = (formats::ansi::ExportOptions::TextEncoding)v;
            }
            if (ansi_opt_.text_encoding == formats::ansi::ExportOptions::TextEncoding::Cp437)
            {
                // EncodingId selector (applies only to 8-bit byte export).
                struct Item
                {
                    phos::encodings::EncodingId id;
                    std::string                 name;
                };
                const std::vector<Item> kItems = {
                    {phos::encodings::EncodingId::Cp437, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp437")},
                    {phos::encodings::EncodingId::Cp775, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp775")},
                    {phos::encodings::EncodingId::Cp737, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp737")},
                    {phos::encodings::EncodingId::Cp850, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp850")},
                    {phos::encodings::EncodingId::Cp852, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp852")},
                    {phos::encodings::EncodingId::Cp855, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp855")},
                    {phos::encodings::EncodingId::Cp857, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp857")},
                    {phos::encodings::EncodingId::Cp860, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp860")},
                    {phos::encodings::EncodingId::Cp861, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp861")},
                    {phos::encodings::EncodingId::Cp862, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp862")},
                    {phos::encodings::EncodingId::Cp863, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp863")},
                    {phos::encodings::EncodingId::Cp865, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp865")},
                    {phos::encodings::EncodingId::Cp866, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp866")},
                    {phos::encodings::EncodingId::Cp869, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.cp869")},
                    {phos::encodings::EncodingId::AmigaLatin1, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.amiga_latin1")},
                    {phos::encodings::EncodingId::AmigaIso8859_15, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.amiga_iso8859_15")},
                    {phos::encodings::EncodingId::AmigaIso8859_2, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.amiga_iso8859_2")},
                    {phos::encodings::EncodingId::Amiga1251, PHOS_TR("export_dialog.ansi_tab.byte_encoding_items.amiga_1251")},
                };

                const std::string byte_default_preview = PHOS_TR("export_dialog.ansi_tab.byte_encoding_default_preview");
                const char* preview = byte_default_preview.c_str();
                int current_idx = 0;
                for (int ii = 0; ii < (int)kItems.size(); ++ii)
                {
                    if (kItems[ii].id == ansi_opt_.byte_encoding)
                    {
                        preview = kItems[ii].name.c_str();
                        current_idx = ii;
                        break;
                    }
                }

                const std::string be_lbl = PHOS_TR("export_dialog.ansi_tab.byte_encoding");
                if (ImGui::BeginCombo(be_lbl.c_str(), preview))
                {
                    for (int ii = 0; ii < (int)kItems.size(); ++ii)
                    {
                        const bool selected = (ii == current_idx);
                        if (ImGui::Selectable(kItems[ii].name.c_str(), selected))
                            ansi_opt_.byte_encoding = kItems[ii].id;
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                const std::string help = PHOS_TR("export_dialog.ansi_tab.byte_encoding_help");
                HelpMarker(help.c_str());
            }
            {
                const std::string n0 = PHOS_TR("export_dialog.ansi_tab.newlines_items.crlf_scene_friendly");
                const std::string n1 = PHOS_TR("export_dialog.ansi_tab.newlines_items.lf_terminal_friendly");
                const char* items[] = { n0.c_str(), n1.c_str() };
                int v = (int)ansi_opt_.newline;
                const std::string nl_lbl = PHOS_TR("export_dialog.ansi_tab.newlines");
                if (ImGui::Combo(nl_lbl.c_str(), &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.newline = (formats::ansi::ExportOptions::Newline)v;
            }
            {
                const std::string sp0 = PHOS_TR("export_dialog.ansi_tab.screen_prep_items.none");
                const std::string sp1 = PHOS_TR("export_dialog.ansi_tab.screen_prep_items.clear_screen");
                const std::string sp2 = PHOS_TR("export_dialog.ansi_tab.screen_prep_items.home");
                const std::string sp3 = PHOS_TR("export_dialog.ansi_tab.screen_prep_items.clear_plus_home");
                const char* items[] = { sp0.c_str(), sp1.c_str(), sp2.c_str(), sp3.c_str() };
                int v = (int)ansi_opt_.screen_prep;
                const std::string sp_lbl = PHOS_TR("export_dialog.ansi_tab.screen_prep");
                if (ImGui::Combo(sp_lbl.c_str(), &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.screen_prep = (formats::ansi::ExportOptions::ScreenPrep)v;
            }

            // Colors
            {
                const std::string cm0 = PHOS_TR("export_dialog.ansi_tab.color_mode_items.ansi16_classic");
                const std::string cm1 = PHOS_TR("export_dialog.ansi_tab.color_mode_items.xterm256");
                const std::string cm2 = PHOS_TR("export_dialog.ansi_tab.color_mode_items.truecolor_sgr");
                const std::string cm3 = PHOS_TR("export_dialog.ansi_tab.color_mode_items.pablo_t");
                const char* items[] = { cm0.c_str(), cm1.c_str(), cm2.c_str(), cm3.c_str() };
                int v = (int)ansi_opt_.color_mode;
                const std::string cm_lbl = PHOS_TR("export_dialog.ansi_tab.color_mode");
                if (ImGui::Combo(cm_lbl.c_str(), &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.color_mode = (formats::ansi::ExportOptions::ColorMode)v;
            }

            if (ansi_opt_.color_mode == formats::ansi::ExportOptions::ColorMode::Ansi16)
            {
                const std::string ba0 = PHOS_TR("export_dialog.ansi_tab.bright_ansi16_items.bold_ice");
                const std::string ba1 = PHOS_TR("export_dialog.ansi_tab.bright_ansi16_items.sgr_90_97");
                const char* items[] = { ba0.c_str(), ba1.c_str() };
                int v = (int)ansi_opt_.ansi16_bright;
                const std::string ba_lbl = PHOS_TR("export_dialog.ansi_tab.bright_ansi16");
                if (ImGui::Combo(ba_lbl.c_str(), &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.ansi16_bright = (formats::ansi::ExportOptions::Ansi16Bright)v;
                const std::string ice = PHOS_TR("export_dialog.ansi_tab.ice_colors");
                ImGui::Checkbox(ice.c_str(), &ansi_opt_.icecolors);
            }
            else if (ansi_opt_.color_mode == formats::ansi::ExportOptions::ColorMode::Xterm256)
            {
                const std::string s = PHOS_TR("export_dialog.ansi_tab.xterm_240_safe");
                ImGui::Checkbox(s.c_str(), &ansi_opt_.xterm_240_safe);
            }
            else if (ansi_opt_.color_mode == formats::ansi::ExportOptions::ColorMode::TrueColorPabloT)
            {
                const std::string s = PHOS_TR("export_dialog.ansi_tab.pablo_t_overlay");
                ImGui::Checkbox(s.c_str(), &ansi_opt_.pablo_t_with_ansi16_fallback);
            }

            // Default fg/bg override
            {
                const std::string s = PHOS_TR("export_dialog.ansi_tab.override_default_foreground");
                ImGui::Checkbox(s.c_str(), &ansi_override_default_fg_);
                if (ansi_override_default_fg_)
                {
                    ImGui::SameLine();
                    ImVec4 col = ImGui::ColorConvertU32ToFloat4((ImU32)ansi_opt_.default_fg);
                    if (ImGui::ColorEdit4("##ansi_def_fg", (float*)&col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
                        ansi_opt_.default_fg = (AnsiCanvas::Color32)ImGui::ColorConvertFloat4ToU32(col);
                }
                else
                {
                    ansi_opt_.default_fg = 0;
                }
            }
            {
                const std::string s = PHOS_TR("export_dialog.ansi_tab.override_default_background");
                ImGui::Checkbox(s.c_str(), &ansi_override_default_bg_);
                if (ansi_override_default_bg_)
                {
                    ImGui::SameLine();
                    ImVec4 col = ImGui::ColorConvertU32ToFloat4((ImU32)ansi_opt_.default_bg);
                    if (ImGui::ColorEdit4("##ansi_def_bg", (float*)&col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
                        ansi_opt_.default_bg = (AnsiCanvas::Color32)ImGui::ColorConvertFloat4ToU32(col);
                }
                else
                {
                    ansi_opt_.default_bg = 0;
                }
            }

            {
                const std::string s = PHOS_TR("export_dialog.ansi_tab.prefer_sgr39");
                ImGui::Checkbox(s.c_str(), &ansi_opt_.use_default_fg_39);
            }
            {
                const std::string s = PHOS_TR("export_dialog.ansi_tab.prefer_sgr49");
                ImGui::Checkbox(s.c_str(), &ansi_opt_.use_default_bg_49);
            }

            // Geometry + compression
            {
                const std::string s = PHOS_TR("export_dialog.ansi_tab.output_policy");
                ImGui::SeparatorText(s.c_str());
            }
            {
                const std::string s = PHOS_TR("export_dialog.ansi_tab.preserve_full_line_length");
                ImGui::Checkbox(s.c_str(), &ansi_opt_.preserve_line_length);
            }
            {
                const std::string s = PHOS_TR("export_dialog.ansi_tab.compress_output");
                ImGui::Checkbox(s.c_str(), &ansi_opt_.compress);
            }
            if (!ansi_opt_.compress) ImGui::BeginDisabled();
            {
                const std::string s = PHOS_TR("export_dialog.ansi_tab.use_cursor_forward");
                ImGui::Checkbox(s.c_str(), &ansi_opt_.use_cursor_forward);
            }
            if (!ansi_opt_.compress) ImGui::EndDisabled();
            {
                const std::string s = PHOS_TR("export_dialog.ansi_tab.final_reset");
                ImGui::Checkbox(s.c_str(), &ansi_opt_.final_reset);
            }

            // SAUCE
            ImGui::SeparatorText(PHOS_TR("export_dialog.ansi_tab.sauce").c_str());
            ImGui::Checkbox(PHOS_TR("export_dialog.ansi_tab.append_sauce").c_str(), &ansi_opt_.write_sauce);
            if (ansi_opt_.write_sauce)
            {
                ImGui::Indent();
                ImGui::Checkbox(PHOS_TR("export_dialog.ansi_tab.sauce_include_eof").c_str(), &ansi_opt_.sauce_write_options.include_eof_byte);
                ImGui::Checkbox(PHOS_TR("export_dialog.ansi_tab.sauce_include_comnt").c_str(), &ansi_opt_.sauce_write_options.include_comments);
                ImGui::Checkbox(PHOS_TR("export_dialog.ansi_tab.sauce_encode_cp437").c_str(), &ansi_opt_.sauce_write_options.encode_cp437);
                ImGui::Unindent();
                ImGui::TextDisabled("%s", PHOS_TR("export_dialog.ansi_tab.sauce_fields_hint").c_str());
            }

            ImGui::Separator();
            if (!has_canvas) ImGui::BeginDisabled();
            const std::string export_btn = PHOS_TR("export_dialog.ansi_tab.export_ansi_ellipsis");
            if (ImGui::Button(export_btn.c_str()))
            {
                io.ClearLastError();
                const std::vector<std::string_view> exts_v = formats::ansi::ExportExtensions();
                std::vector<SdlFileDialogQueue::FilterPair> filters = {
                    {PHOS_TR("export_dialog.ansi_tab.export_filters.ansi_ans"), JoinExtsForDialog(exts_v)},
                    {PHOS_TR("io.file_dialog_filters.all_files"), "*"},
                };
                dialogs.ShowSaveFileDialog(kDialog_ExportDlg_Ansi, window, filters,
                                           SuggestedPath(io, focused_canvas, "ans"));
            }
            if (!has_canvas) ImGui::EndDisabled();

            ImGui::EndTabItem();
        }

        // ---------------------------------------------------------------------
        // Plaintext
        // ---------------------------------------------------------------------
        if (begin_tab(Tab::Plaintext, PHOS_TR("export_dialog.tabs.plaintext").c_str()))
        {
            // Preset selector
            {
                const auto& presets = formats::plaintext::Presets();
                const std::string unnamed = PHOS_TR("export_dialog.unnamed_preset");
                std::vector<const char*> items;
                items.reserve(presets.size());
                for (const auto& p : presets)
                    items.push_back(p.name ? p.name : unnamed.c_str());

                int idx = std::clamp(text_preset_idx_, 0, (int)items.size() - 1);
                ImGui::SetNextItemWidth(420.0f);
                const std::string preset_lbl = PHOS_TR("export_dialog.preset");
                if (ImGui::Combo(preset_lbl.c_str(), &idx, items.data(), (int)items.size()))
                {
                    text_preset_idx_ = idx;
                    text_opt_ = presets[(size_t)idx].export_;
                }
                if (idx >= 0 && (size_t)idx < presets.size() && presets[(size_t)idx].description)
                    ImGui::TextDisabled("%s", presets[(size_t)idx].description);
            }

            ImGui::Separator();
            {
                const std::string src0 = PHOS_TR("export_dialog.source_items.composite");
                const std::string src1 = PHOS_TR("export_dialog.source_items.active_layer");
                const char* items[] = { src0.c_str(), src1.c_str() };
                int v = (text_opt_.source == formats::plaintext::ExportOptions::Source::Composite) ? 0 : 1;
                const std::string source_lbl = PHOS_TR("export_dialog.source");
                if (ImGui::Combo(source_lbl.c_str(), &v, items, IM_ARRAYSIZE(items)))
                    text_opt_.source = (v == 0) ? formats::plaintext::ExportOptions::Source::Composite
                                                : formats::plaintext::ExportOptions::Source::ActiveLayer;
            }
            {
                const std::string e0 = PHOS_TR("export_dialog.plaintext_tab.text_encoding_items.ascii");
                const std::string e1 = PHOS_TR("export_dialog.plaintext_tab.text_encoding_items.utf8");
                const std::string e2 = PHOS_TR("export_dialog.plaintext_tab.text_encoding_items.utf8_bom");
                const char* items[] = { e0.c_str(), e1.c_str(), e2.c_str() };
                int v = (int)text_opt_.text_encoding;
                const std::string te_lbl = PHOS_TR("export_dialog.plaintext_tab.text_encoding");
                if (ImGui::Combo(te_lbl.c_str(), &v, items, IM_ARRAYSIZE(items)))
                    text_opt_.text_encoding = (formats::plaintext::ExportOptions::TextEncoding)v;
            }
            {
                const std::string n0 = PHOS_TR("export_dialog.plaintext_tab.newlines_items.crlf");
                const std::string n1 = PHOS_TR("export_dialog.plaintext_tab.newlines_items.lf");
                const char* items[] = { n0.c_str(), n1.c_str() };
                int v = (int)text_opt_.newline;
                const std::string nl_lbl = PHOS_TR("export_dialog.plaintext_tab.newlines");
                if (ImGui::Combo(nl_lbl.c_str(), &v, items, IM_ARRAYSIZE(items)))
                    text_opt_.newline = (formats::plaintext::ExportOptions::Newline)v;
            }
            ImGui::Checkbox(PHOS_TR("export_dialog.plaintext_tab.preserve_full_line_length").c_str(), &text_opt_.preserve_line_length);
            ImGui::Checkbox(PHOS_TR("export_dialog.plaintext_tab.final_newline").c_str(), &text_opt_.final_newline);

            ImGui::Separator();
            if (!has_canvas) ImGui::BeginDisabled();
            const std::string export_btn = PHOS_TR("export_dialog.plaintext_tab.export_text_ellipsis");
            if (ImGui::Button(export_btn.c_str()))
            {
                io.ClearLastError();
                const std::vector<std::string_view> txt_exts_v = {"txt"};
                const std::vector<std::string_view> asc_exts_v = {"asc"};
                std::vector<SdlFileDialogQueue::FilterPair> filters = {
                    {PHOS_TR("export_dialog.plaintext_tab.export_filters.text_txt"), JoinExtsForDialog(txt_exts_v)},
                    {PHOS_TR("export_dialog.plaintext_tab.export_filters.ascii_asc"), JoinExtsForDialog(asc_exts_v)},
                    {PHOS_TR("io.file_dialog_filters.all_files"), "*"},
                };
                dialogs.ShowSaveFileDialog(kDialog_ExportDlg_Plaintext, window, filters,
                                           SuggestedPath(io, focused_canvas, "txt"));
            }
            if (!has_canvas) ImGui::EndDisabled();

            ImGui::EndTabItem();
        }

        // ---------------------------------------------------------------------
        // Image
        // ---------------------------------------------------------------------
        if (begin_tab(Tab::Image, PHOS_TR("export_dialog.tabs.image").c_str()))
        {
            // Scale + computed output dimensions.
            ImGui::SetNextItemWidth(200.0f);
            const std::string scale_lbl = PHOS_TR("export_dialog.image_tab.scale");
            ImGui::SliderInt(scale_lbl.c_str(), &image_opt_.scale, 1, 8);
            ImGui::SameLine();
            {
                int ow = 0, oh = 0;
                std::string derr;
                if (has_canvas && formats::image::ComputeExportDimensionsPx(*focused_canvas, ow, oh, derr, image_opt_))
                {
                    const std::string s = PHOS_TRF("export_dialog.image_tab.output_px",
                                                  phos::i18n::Arg::I64(ow),
                                                  phos::i18n::Arg::I64(oh));
                    ImGui::TextDisabled("%s", s.c_str());
                }
                else
                {
                    ImGui::TextDisabled("%s", PHOS_TR("export_dialog.image_tab.output_na").c_str());
                    if (!derr.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
                        ImGui::TextUnformatted(derr.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                }
            }
            ImGui::Checkbox(PHOS_TR("export_dialog.image_tab.transparent_unset_bg").c_str(), &image_opt_.transparent_unset_bg);
            ImGui::SameLine();
            HelpMarker(PHOS_TR("export_dialog.image_tab.transparent_unset_bg_help").c_str());

            {
                const std::string p0 = PHOS_TR("export_dialog.image_tab.png_format_items.rgb24");
                const std::string p1 = PHOS_TR("export_dialog.image_tab.png_format_items.rgba32");
                const std::string p2 = PHOS_TR("export_dialog.image_tab.png_format_items.indexed8");
                const std::string p3 = PHOS_TR("export_dialog.image_tab.png_format_items.indexed4");
                const char* items[] = { p0.c_str(), p1.c_str(), p2.c_str(), p3.c_str() };
                int v = 0;
                if (image_opt_.png_format == formats::image::ExportOptions::PngFormat::Rgb24) v = 0;
                else if (image_opt_.png_format == formats::image::ExportOptions::PngFormat::Rgba32) v = 1;
                else if (image_opt_.png_format == formats::image::ExportOptions::PngFormat::Indexed8) v = 2;
                else if (image_opt_.png_format == formats::image::ExportOptions::PngFormat::Indexed4) v = 3;
                const std::string fmt_lbl = PHOS_TR("export_dialog.image_tab.png_format");
                if (ImGui::Combo(fmt_lbl.c_str(), &v, items, IM_ARRAYSIZE(items)))
                {
                    if (v == 0) image_opt_.png_format = formats::image::ExportOptions::PngFormat::Rgb24;
                    if (v == 1) image_opt_.png_format = formats::image::ExportOptions::PngFormat::Rgba32;
                    if (v == 2) image_opt_.png_format = formats::image::ExportOptions::PngFormat::Indexed8;
                    if (v == 3) image_opt_.png_format = formats::image::ExportOptions::PngFormat::Indexed4;
                }
            }

            if (image_opt_.png_format == formats::image::ExportOptions::PngFormat::Indexed8)
                ImGui::Checkbox(PHOS_TR("export_dialog.ansi_tab.xterm_240_safe").c_str(), &image_opt_.xterm_240_safe);

            {
                const std::string s = PHOS_TR("export_dialog.image_tab.png_compression");
                ImGui::SliderInt(s.c_str(), &image_opt_.png_compression, 0, 9);
            }
            {
                const std::string s = PHOS_TR("export_dialog.image_tab.jpeg_quality");
                ImGui::SliderInt(s.c_str(), &image_opt_.jpg_quality, 1, 100);
            }

            ImGui::Separator();
            if (!has_canvas) ImGui::BeginDisabled();
            const std::string export_btn = PHOS_TR("export_dialog.image_tab.export_image_ellipsis");
            if (ImGui::Button(export_btn.c_str()))
            {
                io.ClearLastError();
                const std::vector<std::string_view> png_exts_v = {"png"};
                const std::vector<std::string_view> jpg_exts_v = {"jpg", "jpeg"};
                std::vector<SdlFileDialogQueue::FilterPair> filters = {
                    {PHOS_TR("export_dialog.image_tab.export_filters.png"), JoinExtsForDialog(png_exts_v)},
                    {PHOS_TR("export_dialog.image_tab.export_filters.jpeg"), JoinExtsForDialog(jpg_exts_v)},
                    {PHOS_TR("io.file_dialog_filters.all_files"), "*"},
                };
                dialogs.ShowSaveFileDialog(kDialog_ExportDlg_Image, window, filters,
                                           SuggestedPath(io, focused_canvas, "png"));
            }
            if (!has_canvas) ImGui::EndDisabled();

            ImGui::EndTabItem();
        }

        // ---------------------------------------------------------------------
        // XBin
        // ---------------------------------------------------------------------
        if (begin_tab(Tab::XBin, PHOS_TR("export_dialog.tabs.xbin").c_str()))
        {
            {
                const std::string src0 = PHOS_TR("export_dialog.source_items.composite");
                const std::string src1 = PHOS_TR("export_dialog.source_items.active_layer");
                const char* items[] = { src0.c_str(), src1.c_str() };
                int v = (xbin_opt_.source == formats::xbin::ExportOptions::Source::Composite) ? 0 : 1;
                const std::string source_lbl = PHOS_TR("export_dialog.source");
                if (ImGui::Combo(source_lbl.c_str(), &v, items, IM_ARRAYSIZE(items)))
                    xbin_opt_.source = (v == 0) ? formats::xbin::ExportOptions::Source::Composite
                                                : formats::xbin::ExportOptions::Source::ActiveLayer;
            }

            ImGui::Checkbox(PHOS_TR("export_dialog.xbin_tab.include_palette_chunk").c_str(), &xbin_opt_.include_palette);
            ImGui::Checkbox(PHOS_TR("export_dialog.xbin_tab.include_embedded_font").c_str(), &xbin_opt_.include_font);
            ImGui::Checkbox(PHOS_TR("export_dialog.xbin_tab.compress_rle").c_str(), &xbin_opt_.compress);
            ImGui::Checkbox(PHOS_TR("export_dialog.xbin_tab.nonblink").c_str(), &xbin_opt_.nonblink);

            // Exposed for completeness; exporter currently doesn't support 512.
            ImGui::BeginDisabled();
            ImGui::Checkbox(PHOS_TR("export_dialog.xbin_tab.mode_512_not_supported").c_str(), &xbin_opt_.mode_512);
            ImGui::EndDisabled();

            ImGui::SeparatorText(PHOS_TR("export_dialog.xbin_tab.sauce").c_str());
            ImGui::Checkbox(PHOS_TR("export_dialog.xbin_tab.append_sauce").c_str(), &xbin_opt_.write_sauce);
            if (xbin_opt_.write_sauce)
            {
                ImGui::Indent();
                ImGui::Checkbox(PHOS_TR("export_dialog.ansi_tab.sauce_include_eof").c_str(), &xbin_opt_.sauce_write_options.include_eof_byte);
                ImGui::Checkbox(PHOS_TR("export_dialog.ansi_tab.sauce_include_comnt").c_str(), &xbin_opt_.sauce_write_options.include_comments);
                ImGui::Checkbox(PHOS_TR("export_dialog.ansi_tab.sauce_encode_cp437").c_str(), &xbin_opt_.sauce_write_options.encode_cp437);
                ImGui::Unindent();
            }

            ImGui::Separator();
            if (!has_canvas) ImGui::BeginDisabled();
            const std::string export_btn = PHOS_TR("export_dialog.xbin_tab.export_xbin_ellipsis");
            if (ImGui::Button(export_btn.c_str()))
            {
                io.ClearLastError();
                const std::vector<std::string_view> exts_v = formats::xbin::ExportExtensions();
                std::vector<SdlFileDialogQueue::FilterPair> filters = {
                    {PHOS_TR("export_dialog.xbin_tab.export_filters.xbin_xb"), JoinExtsForDialog(exts_v)},
                    {PHOS_TR("io.file_dialog_filters.all_files"), "*"},
                };
                dialogs.ShowSaveFileDialog(kDialog_ExportDlg_XBin, window, filters,
                                           SuggestedPath(io, focused_canvas, "xb"));
            }
            if (!has_canvas) ImGui::EndDisabled();

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
}

bool ExportDialog::HandleDialogResult(const SdlFileDialogResult& r, IoManager& io, AnsiCanvas* focused_canvas)
{
    const int tag = r.tag;
    if (tag != kDialog_ExportDlg_Ansi &&
        tag != kDialog_ExportDlg_Plaintext &&
        tag != kDialog_ExportDlg_Image &&
        tag != kDialog_ExportDlg_XBin)
        return false;

    if (!r.error.empty())
    {
        io.SetLastError(r.error);
        return true;
    }
    if (r.canceled || r.paths.empty())
        return true;
    if (!focused_canvas)
    {
        io.SetLastError(PHOS_TR("io.errors.no_focused_canvas_to_export"));
        return true;
    }

    std::string chosen = r.paths[0];
    if (!IsUri(chosen))
    {
        try
        {
            fs::path p(chosen);
            if (p.has_parent_path())
                io.SetLastDir(p.parent_path().string());
        }
        catch (...) {}
    }

    std::string err;

    // Ensure extension if the user didn't type one.
    std::string path = chosen;
    if (tag == kDialog_ExportDlg_Ansi)
    {
        path = EnsureExtension(path, "ans");
        const bool ok = formats::ansi::ExportCanvasToFile(path, *focused_canvas, err, ansi_opt_);
        if (!ok) io.SetLastError(err.empty() ? PHOS_TR("io.errors.export_failed") : err);
        else io.ClearLastError();
        return true;
    }
    if (tag == kDialog_ExportDlg_Plaintext)
    {
        // Default to .txt when extension omitted.
        path = EnsureExtension(path, "txt");
        const bool ok = formats::plaintext::ExportCanvasToFile(path, *focused_canvas, err, text_opt_);
        if (!ok) io.SetLastError(err.empty() ? PHOS_TR("io.errors.export_failed") : err);
        else io.ClearLastError();
        return true;
    }
    if (tag == kDialog_ExportDlg_Image)
    {
        // Default to .png when extension omitted.
        path = EnsureExtension(path, "png");
        const bool ok = formats::image::ExportCanvasToFile(path, *focused_canvas, err, image_opt_);
        if (!ok) io.SetLastError(err.empty() ? PHOS_TR("io.errors.export_failed") : err);
        else io.ClearLastError();
        return true;
    }
    if (tag == kDialog_ExportDlg_XBin)
    {
        path = EnsureExtension(path, "xb");
        const bool ok = formats::xbin::ExportCanvasToFile(path, *focused_canvas, err, xbin_opt_);
        if (!ok) io.SetLastError(err.empty() ? PHOS_TR("io.errors.export_failed") : err);
        else io.ClearLastError();
        return true;
    }

    return true;
}


