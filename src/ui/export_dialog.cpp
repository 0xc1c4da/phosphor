#include "ui/export_dialog.h"

#include "imgui.h"
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
    ImGui::TextDisabled("(?)");
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
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "No active canvas to export.");
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
        if (begin_tab(Tab::Ansi, "ANSI"))
        {
            // Preset selector (applies immediately)
            {
                const auto& presets = formats::ansi::Presets();
                std::vector<const char*> items;
                items.reserve(presets.size());
                for (const auto& p : presets)
                    items.push_back(p.name ? p.name : "(unnamed)");

                int idx = std::clamp(ansi_preset_idx_, 0, (int)items.size() - 1);
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::Combo("Preset", &idx, items.data(), (int)items.size()))
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
                const char* items[] = { "Composite (what you see)", "Active Layer" };
                int v = (ansi_opt_.source == formats::ansi::ExportOptions::Source::Composite) ? 0 : 1;
                if (ImGui::Combo("Source", &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.source = (v == 0) ? formats::ansi::ExportOptions::Source::Composite
                                                : formats::ansi::ExportOptions::Source::ActiveLayer;
            }

            // Encoding / newline
            {
                const char* items[] = { "8-bit (codepage bytes)", "UTF-8", "UTF-8 (BOM)" };
                int v = (int)ansi_opt_.text_encoding;
                if (ImGui::Combo("Text Encoding", &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.text_encoding = (formats::ansi::ExportOptions::TextEncoding)v;
            }
            if (ansi_opt_.text_encoding == formats::ansi::ExportOptions::TextEncoding::Cp437)
            {
                // EncodingId selector (applies only to 8-bit byte export).
                struct Item
                {
                    phos::encodings::EncodingId id;
                    const char*                 name;
                };
                static constexpr Item kItems[] = {
                    {phos::encodings::EncodingId::Cp437, "CP437 (IBM PC OEM)"},
                    {phos::encodings::EncodingId::Cp775, "CP775"},
                    {phos::encodings::EncodingId::Cp737, "CP737"},
                    {phos::encodings::EncodingId::Cp850, "CP850"},
                    {phos::encodings::EncodingId::Cp852, "CP852"},
                    {phos::encodings::EncodingId::Cp855, "CP855"},
                    {phos::encodings::EncodingId::Cp857, "CP857"},
                    {phos::encodings::EncodingId::Cp860, "CP860"},
                    {phos::encodings::EncodingId::Cp861, "CP861"},
                    {phos::encodings::EncodingId::Cp862, "CP862"},
                    {phos::encodings::EncodingId::Cp863, "CP863"},
                    {phos::encodings::EncodingId::Cp865, "CP865"},
                    {phos::encodings::EncodingId::Cp866, "CP866"},
                    {phos::encodings::EncodingId::Cp869, "CP869"},
                    {phos::encodings::EncodingId::AmigaLatin1, "Amiga Latin-1 (Topaz 0x7F house)"},
                    {phos::encodings::EncodingId::AmigaIso8859_15, "Amiga ISO-8859-15 (Topaz 0x7F house)"},
                    {phos::encodings::EncodingId::AmigaIso8859_2, "Amiga ISO-8859-2 (Topaz 0x7F house)"},
                    {phos::encodings::EncodingId::Amiga1251, "Amiga-1251"},
                };

                const char* preview = "CP437 (IBM PC OEM)";
                int current_idx = 0;
                for (int ii = 0; ii < (int)IM_ARRAYSIZE(kItems); ++ii)
                {
                    if (kItems[ii].id == ansi_opt_.byte_encoding)
                    {
                        preview = kItems[ii].name;
                        current_idx = ii;
                        break;
                    }
                }

                if (ImGui::BeginCombo("Byte Encoding", preview))
                {
                    for (int ii = 0; ii < (int)IM_ARRAYSIZE(kItems); ++ii)
                    {
                        const bool selected = (ii == current_idx);
                        if (ImGui::Selectable(kItems[ii].name, selected))
                            ansi_opt_.byte_encoding = kItems[ii].id;
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                HelpMarker("Used when exporting an 8-bit ANSI byte stream. BitmapIndex glyph tokens export losslessly as their byte value when possible.");
            }
            {
                const char* items[] = { "CRLF (scene-friendly)", "LF (terminal-friendly)" };
                int v = (int)ansi_opt_.newline;
                if (ImGui::Combo("Newlines", &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.newline = (formats::ansi::ExportOptions::Newline)v;
            }
            {
                const char* items[] = { "None", "Clear Screen (ESC[2J)", "Home (ESC[H)", "Clear + Home" };
                int v = (int)ansi_opt_.screen_prep;
                if (ImGui::Combo("Screen Prep", &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.screen_prep = (formats::ansi::ExportOptions::ScreenPrep)v;
            }

            // Colors
            {
                const char* items[] = { "ANSI16 (classic)", "xterm-256 (38;5/48;5)", "Truecolor SGR (38;2/48;2)", "Pablo/Icy truecolor (...t)" };
                int v = (int)ansi_opt_.color_mode;
                if (ImGui::Combo("Color Mode", &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.color_mode = (formats::ansi::ExportOptions::ColorMode)v;
            }

            if (ansi_opt_.color_mode == formats::ansi::ExportOptions::ColorMode::Ansi16)
            {
                const char* items[] = { "Bold + iCE Blink (scene classic)", "SGR 90-97 / 100-107" };
                int v = (int)ansi_opt_.ansi16_bright;
                if (ImGui::Combo("Bright ANSI16", &v, items, IM_ARRAYSIZE(items)))
                    ansi_opt_.ansi16_bright = (formats::ansi::ExportOptions::Ansi16Bright)v;
                ImGui::Checkbox("iCE colors (blink => bright background)", &ansi_opt_.icecolors);
            }
            else if (ansi_opt_.color_mode == formats::ansi::ExportOptions::ColorMode::Xterm256)
            {
                ImGui::Checkbox("240-color safe (avoid indices 0..15)", &ansi_opt_.xterm_240_safe);
            }
            else if (ansi_opt_.color_mode == formats::ansi::ExportOptions::ColorMode::TrueColorPabloT)
            {
                ImGui::Checkbox("Emit ANSI16 fallback + ...t overlay", &ansi_opt_.pablo_t_with_ansi16_fallback);
            }

            // Default fg/bg override
            {
                ImGui::Checkbox("Override default foreground", &ansi_override_default_fg_);
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
                ImGui::Checkbox("Override default background", &ansi_override_default_bg_);
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

            ImGui::Checkbox("Prefer SGR 39 for unset foreground", &ansi_opt_.use_default_fg_39);
            ImGui::Checkbox("Prefer SGR 49 for unset background", &ansi_opt_.use_default_bg_49);

            // Geometry + compression
            ImGui::SeparatorText("Output policy");
            ImGui::Checkbox("Preserve full line length (fixed width)", &ansi_opt_.preserve_line_length);
            ImGui::Checkbox("Compress output", &ansi_opt_.compress);
            if (!ansi_opt_.compress) ImGui::BeginDisabled();
            ImGui::Checkbox("Use cursor-forward for spaces (CSI Ps C)", &ansi_opt_.use_cursor_forward);
            if (!ansi_opt_.compress) ImGui::EndDisabled();
            ImGui::Checkbox("Final reset (ESC[0m)", &ansi_opt_.final_reset);

            // SAUCE
            ImGui::SeparatorText("SAUCE");
            ImGui::Checkbox("Append SAUCE metadata", &ansi_opt_.write_sauce);
            if (ansi_opt_.write_sauce)
            {
                ImGui::Indent();
                ImGui::Checkbox("Include EOF byte (0x1A)", &ansi_opt_.sauce_write_options.include_eof_byte);
                ImGui::Checkbox("Include COMNT block (if comments exist)", &ansi_opt_.sauce_write_options.include_comments);
                ImGui::Checkbox("Encode SAUCE fixed fields as CP437", &ansi_opt_.sauce_write_options.encode_cp437);
                ImGui::Unindent();
                ImGui::TextDisabled("SAUCE fields are taken from the canvas metadata (Edit via the SAUCE editor).");
            }

            ImGui::Separator();
            if (!has_canvas) ImGui::BeginDisabled();
            if (ImGui::Button("Export ANSI…"))
            {
                io.ClearLastError();
                const std::vector<std::string_view> exts_v = formats::ansi::ExportExtensions();
                std::vector<SdlFileDialogQueue::FilterPair> filters = {
                    {MakeFilterLabel("ANSI", exts_v), JoinExtsForDialog(exts_v)},
                    {"All files", "*"},
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
        if (begin_tab(Tab::Plaintext, "Plaintext"))
        {
            // Preset selector
            {
                const auto& presets = formats::plaintext::Presets();
                std::vector<const char*> items;
                items.reserve(presets.size());
                for (const auto& p : presets)
                    items.push_back(p.name ? p.name : "(unnamed)");

                int idx = std::clamp(text_preset_idx_, 0, (int)items.size() - 1);
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::Combo("Preset", &idx, items.data(), (int)items.size()))
                {
                    text_preset_idx_ = idx;
                    text_opt_ = presets[(size_t)idx].export_;
                }
                if (idx >= 0 && (size_t)idx < presets.size() && presets[(size_t)idx].description)
                    ImGui::TextDisabled("%s", presets[(size_t)idx].description);
            }

            ImGui::Separator();
            {
                const char* items[] = { "Composite (what you see)", "Active Layer" };
                int v = (text_opt_.source == formats::plaintext::ExportOptions::Source::Composite) ? 0 : 1;
                if (ImGui::Combo("Source", &v, items, IM_ARRAYSIZE(items)))
                    text_opt_.source = (v == 0) ? formats::plaintext::ExportOptions::Source::Composite
                                                : formats::plaintext::ExportOptions::Source::ActiveLayer;
            }
            {
                const char* items[] = { "ASCII", "UTF-8", "UTF-8 (BOM)" };
                int v = (int)text_opt_.text_encoding;
                if (ImGui::Combo("Text Encoding", &v, items, IM_ARRAYSIZE(items)))
                    text_opt_.text_encoding = (formats::plaintext::ExportOptions::TextEncoding)v;
            }
            {
                const char* items[] = { "CRLF", "LF" };
                int v = (int)text_opt_.newline;
                if (ImGui::Combo("Newlines", &v, items, IM_ARRAYSIZE(items)))
                    text_opt_.newline = (formats::plaintext::ExportOptions::Newline)v;
            }
            ImGui::Checkbox("Preserve full line length (fixed width)", &text_opt_.preserve_line_length);
            ImGui::Checkbox("Final newline", &text_opt_.final_newline);

            ImGui::Separator();
            if (!has_canvas) ImGui::BeginDisabled();
            if (ImGui::Button("Export Text…"))
            {
                io.ClearLastError();
                const std::vector<std::string_view> txt_exts_v = {"txt"};
                const std::vector<std::string_view> asc_exts_v = {"asc"};
                std::vector<SdlFileDialogQueue::FilterPair> filters = {
                    {MakeFilterLabel("Text", txt_exts_v), JoinExtsForDialog(txt_exts_v)},
                    {MakeFilterLabel("ASCII", asc_exts_v), JoinExtsForDialog(asc_exts_v)},
                    {"All files", "*"},
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
        if (begin_tab(Tab::Image, "Image"))
        {
            // Scale + computed output dimensions.
            ImGui::SetNextItemWidth(200.0f);
            ImGui::SliderInt("Scale", &image_opt_.scale, 1, 8);
            ImGui::SameLine();
            {
                int ow = 0, oh = 0;
                std::string derr;
                if (has_canvas && formats::image::ComputeExportDimensionsPx(*focused_canvas, ow, oh, derr, image_opt_))
                {
                    ImGui::TextDisabled("Output: %dx%d px", ow, oh);
                }
                else
                {
                    ImGui::TextDisabled("Output: (n/a)");
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
            ImGui::Checkbox("Transparent unset background", &image_opt_.transparent_unset_bg);
            ImGui::SameLine();
            HelpMarker("When enabled, cells with bg==0 export as transparent.\nJPEG does not support transparency.\nPNG modes without alpha (RGB/Indexed) will reject this option.");

            {
                const char* items[] = { "RGB 24-bit", "RGBA 32-bit", "Indexed 8-bit (xterm-256)", "Indexed 4-bit (ANSI16)" };
                int v = 0;
                if (image_opt_.png_format == formats::image::ExportOptions::PngFormat::Rgb24) v = 0;
                else if (image_opt_.png_format == formats::image::ExportOptions::PngFormat::Rgba32) v = 1;
                else if (image_opt_.png_format == formats::image::ExportOptions::PngFormat::Indexed8) v = 2;
                else if (image_opt_.png_format == formats::image::ExportOptions::PngFormat::Indexed4) v = 3;
                if (ImGui::Combo("PNG format", &v, items, IM_ARRAYSIZE(items)))
                {
                    if (v == 0) image_opt_.png_format = formats::image::ExportOptions::PngFormat::Rgb24;
                    if (v == 1) image_opt_.png_format = formats::image::ExportOptions::PngFormat::Rgba32;
                    if (v == 2) image_opt_.png_format = formats::image::ExportOptions::PngFormat::Indexed8;
                    if (v == 3) image_opt_.png_format = formats::image::ExportOptions::PngFormat::Indexed4;
                }
            }

            if (image_opt_.png_format == formats::image::ExportOptions::PngFormat::Indexed8)
                ImGui::Checkbox("240-color safe (avoid indices 0..15)", &image_opt_.xterm_240_safe);

            ImGui::SliderInt("PNG compression", &image_opt_.png_compression, 0, 9);
            ImGui::SliderInt("JPEG quality", &image_opt_.jpg_quality, 1, 100);

            ImGui::Separator();
            if (!has_canvas) ImGui::BeginDisabled();
            if (ImGui::Button("Export Image…"))
            {
                io.ClearLastError();
                const std::vector<std::string_view> png_exts_v = {"png"};
                const std::vector<std::string_view> jpg_exts_v = {"jpg", "jpeg"};
                std::vector<SdlFileDialogQueue::FilterPair> filters = {
                    {MakeFilterLabel("PNG", png_exts_v), JoinExtsForDialog(png_exts_v)},
                    {MakeFilterLabel("JPEG", jpg_exts_v), JoinExtsForDialog(jpg_exts_v)},
                    {"All files", "*"},
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
        if (begin_tab(Tab::XBin, "XBin"))
        {
            {
                const char* items[] = { "Composite (what you see)", "Active Layer" };
                int v = (xbin_opt_.source == formats::xbin::ExportOptions::Source::Composite) ? 0 : 1;
                if (ImGui::Combo("Source", &v, items, IM_ARRAYSIZE(items)))
                    xbin_opt_.source = (v == 0) ? formats::xbin::ExportOptions::Source::Composite
                                                : formats::xbin::ExportOptions::Source::ActiveLayer;
            }

            ImGui::Checkbox("Include palette chunk (16 colors)", &xbin_opt_.include_palette);
            ImGui::Checkbox("Include embedded font (if any)", &xbin_opt_.include_font);
            ImGui::Checkbox("Compress (XBin RLE)", &xbin_opt_.compress);
            ImGui::Checkbox("NonBlink (iCE / bright backgrounds)", &xbin_opt_.nonblink);

            // Exposed for completeness; exporter currently doesn't support 512.
            ImGui::BeginDisabled();
            ImGui::Checkbox("512-character mode (not supported)", &xbin_opt_.mode_512);
            ImGui::EndDisabled();

            ImGui::SeparatorText("SAUCE");
            ImGui::Checkbox("Append SAUCE metadata", &xbin_opt_.write_sauce);
            if (xbin_opt_.write_sauce)
            {
                ImGui::Indent();
                ImGui::Checkbox("Include EOF byte (0x1A)", &xbin_opt_.sauce_write_options.include_eof_byte);
                ImGui::Checkbox("Include COMNT block (if comments exist)", &xbin_opt_.sauce_write_options.include_comments);
                ImGui::Checkbox("Encode SAUCE fixed fields as CP437", &xbin_opt_.sauce_write_options.encode_cp437);
                ImGui::Unindent();
            }

            ImGui::Separator();
            if (!has_canvas) ImGui::BeginDisabled();
            if (ImGui::Button("Export XBin…"))
            {
                io.ClearLastError();
                const std::vector<std::string_view> exts_v = formats::xbin::ExportExtensions();
                std::vector<SdlFileDialogQueue::FilterPair> filters = {
                    {MakeFilterLabel("XBin", exts_v), JoinExtsForDialog(exts_v)},
                    {"All files", "*"},
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
        io.SetLastError("No focused canvas to export.");
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
        if (!ok) io.SetLastError(err.empty() ? "ANSI export failed." : err);
        else io.ClearLastError();
        return true;
    }
    if (tag == kDialog_ExportDlg_Plaintext)
    {
        // Default to .txt when extension omitted.
        path = EnsureExtension(path, "txt");
        const bool ok = formats::plaintext::ExportCanvasToFile(path, *focused_canvas, err, text_opt_);
        if (!ok) io.SetLastError(err.empty() ? "Text export failed." : err);
        else io.ClearLastError();
        return true;
    }
    if (tag == kDialog_ExportDlg_Image)
    {
        // Default to .png when extension omitted.
        path = EnsureExtension(path, "png");
        const bool ok = formats::image::ExportCanvasToFile(path, *focused_canvas, err, image_opt_);
        if (!ok) io.SetLastError(err.empty() ? "Image export failed." : err);
        else io.ClearLastError();
        return true;
    }
    if (tag == kDialog_ExportDlg_XBin)
    {
        path = EnsureExtension(path, "xb");
        const bool ok = formats::xbin::ExportCanvasToFile(path, *focused_canvas, err, xbin_opt_);
        if (!ok) io.SetLastError(err.empty() ? "XBin export failed." : err);
        else io.ClearLastError();
        return true;
    }

    return true;
}


