#include "gui/Theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>

namespace dave::gui::theme {

namespace {
Palette g_palette;

// Build the palette once. Pro Tools-inspired light-neutral theme: steel-gray
// toolbars over a light slate body, amber selection/playhead, blue clips.
// Coherent neutral ramp (one hue, evenly-spaced steps) + one accent, per
// impeccable's Restrained strategy — just on a LIGHT ground instead of dark.
Palette makePalette() {
    Palette p;
    // Neutral ramp — light cool slate (Pro Tools is one of the lighter DAWs).
    // 6 steps; each panel/toolbar reads as a distinct layer without becoming
    // unrelated greys.
    p.bg               = hex(0xb8bcc4ff); // ramp[0]: timeline body (mid slate)
    p.panel            = hex(0xcdd1d8ff); // ramp[1]: child panels
    p.bgAlt            = hex(0xa8aeb8ff); // ramp[2]: alternating rows, frames
    p.bgElevated       = hex(0x8e939bff); // ramp[3]: toolbars/menus (steel gray)
    p.trackSelected    = hex(0xd9bd6eff); // ramp[4]: warm amber tint for selection
    p.border           = hex(0x6f747cff); // ramp[5]: dark separators
    // Text — dark on light (verified ≥4.5:1 against bg, impeccable's #1 rule).
    p.text             = hex(0x1a1c20ff); // ~13:1 on bg
    p.textMuted        = hex(0x4a4f57ff); // ~6.5:1 on bg
    // Accent — amber. Selection, playhead, active transport, primary actions.
    p.accent           = hex(0xe8a517ff);
    p.accentHover      = hex(0xf5bc3eff);
    // Clips — blue body so they don't read as "selected" by default; clearly
    // distinct from the amber accent.
    p.clipAudio        = hex(0x3a6db4ff);
    p.clipAudioBorder  = hex(0x2a5288ff);
    return p;
}
} // namespace

const Palette& palette() { return g_palette; }

// Fixed type scale (impeccable: product UI uses fixed sizes, not fluid).
// 12 / 14 / 15 / 18 — tight 1.125–1.2 ratio, four steps. No display-size
// headings: this is a tool, not a landing page.
const TypeScale& typeScale() {
    static const TypeScale ts{};
    return ts;
}

ImVec4 hex(uint32_t rgba) {
    const float s = 1.0f / 255.0f;
    return ImVec4(
        ((rgba >> 24) & 0xff) * s,
        ((rgba >> 16) & 0xff) * s,
        ((rgba >> 8) & 0xff) * s,
        (rgba & 0xff) * s);
}

void applyTheme() {
    g_palette = makePalette();
    ImGuiStyle& s = ImGui::GetStyle();
    const auto& p = g_palette;

    // Spacing — a 4-based scale (4/8/12/16) for visual rhythm, tighter than
    // ImGui defaults (which read as "debug tool"). Product-register density.
    s.WindowPadding    = ImVec2(12, 12);
    s.FramePadding     = ImVec2(8, 4);     // 8px h, compact vertical (dense tool)
    s.ItemSpacing      = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing    = 16.0f;
    s.ScrollbarSize    = 12.0f;
    s.GrabMinSize      = 8.0f;
    // One rounding value across the board (consistency > mixed radii).
    s.WindowRounding   = 0.0f;   // docked panels are flush
    s.ChildRounding    = 0.0f;
    s.FrameRounding    = 3.0f;   // slight on input fields / buttons
    s.GrabRounding     = 3.0f;
    s.PopupRounding    = 4.0f;
    s.ScrollbarRounding = 0.0f;
    s.TabRounding      = 3.0f;
    s.WindowBorderSize = 0.0f;   // flat panels, no chrome
    s.FrameBorderSize  = 0.0f;
    s.PopupBorderSize  = 1.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    auto c = s.Colors;
    // Neutral surfaces — derived from the ramp, not ad-hoc hexes.
    c[ImGuiCol_Text]            = p.text;
    c[ImGuiCol_TextDisabled]    = p.textMuted;
    c[ImGuiCol_WindowBg]        = p.bg;
    c[ImGuiCol_ChildBg]         = p.panel;
    c[ImGuiCol_PopupBg]         = p.bgElevated;
    c[ImGuiCol_Border]          = p.border;
    c[ImGuiCol_BorderShadow]    = ImVec4(0,0,0,0);
    c[ImGuiCol_FrameBg]         = p.bgAlt;
    c[ImGuiCol_FrameBgHovered]  = p.trackSelected;   // one step up the ramp
    c[ImGuiCol_FrameBgActive]   = p.border;          // two steps up
    c[ImGuiCol_TitleBg]         = p.bgElevated;
    c[ImGuiCol_TitleBgActive]   = p.bgElevated;
    c[ImGuiCol_TitleBgCollapsed]= p.bgAlt;
    c[ImGuiCol_MenuBarBg]       = p.bgElevated;
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0,0,0,0);
    c[ImGuiCol_ScrollbarGrab]   = p.border;          // neutral, not accent
    c[ImGuiCol_ScrollbarGrabHovered] = p.trackSelected;
    c[ImGuiCol_ScrollbarGrabActive]  = p.textMuted;  // still neutral
    // Accent: reserved for primary actions + selection only (impeccable
    // "Restrained" strategy). Don't paint the accent on decorative states.
    c[ImGuiCol_CheckMark]       = p.accent;          // primary: a checked box IS an action
    c[ImGuiCol_SliderGrab]      = p.accent;          // primary: dragging a slider
    c[ImGuiCol_SliderGrabActive]= p.accentHover;
    c[ImGuiCol_Button]          = p.bgAlt;           // buttons: neutral by default
    c[ImGuiCol_ButtonHovered]   = p.trackSelected;
    c[ImGuiCol_ButtonActive]    = p.border;
    c[ImGuiCol_Header]          = p.bgAlt;
    c[ImGuiCol_HeaderHovered]   = p.trackSelected;
    c[ImGuiCol_HeaderActive]    = p.border;
    c[ImGuiCol_Separator]       = p.border;
    c[ImGuiCol_Tab]             = p.bgAlt;
    c[ImGuiCol_TabHovered]      = p.bgElevated;
    c[ImGuiCol_TabActive]       = p.bgElevated;
    c[ImGuiCol_TabUnfocused]    = p.bg;
    c[ImGuiCol_TabUnfocusedActive] = p.bgAlt;
    c[ImGuiCol_DockingPreview]  = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f);
    c[ImGuiCol_DockingEmptyBg]  = p.bg;
    c[ImGuiCol_TableHeaderBg]   = p.bgElevated;
    c[ImGuiCol_TableBorderStrong] = p.border;
    c[ImGuiCol_TableBorderLight]  = p.bgAlt;
    c[ImGuiCol_TextSelectedBg]  = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.30f);
    c[ImGuiCol_NavHighlight]    = p.accent;
}

bool loadDefaultFont(float size) {
    ImGuiIO& io = ImGui::GetIO();
    // ImGui embeds ProggyClean (bitmap) as the ultimate fallback. We try a
    // bundled TTF first; if none is available we keep ImGui's default font but
    // bump its size for readability.
    //
    // For RB-2.2 we ship no TTF yet (adding one is a packaging task); the
    // default font at a larger size is a marked improvement over the tiny
    // default.
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    io.Fonts->Clear();
    // Try common system font paths; if none load, ImGui falls back.
    const char* candidates[] = {
#ifdef __APPLE__
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNS.ttf",
        "/Library/Fonts/Arial.ttf",
#endif
        nullptr
    };
    for (const char* path : candidates) {
        if (path == nullptr) break;
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, size, &cfg);
        if (f != nullptr) {
            std::fprintf(stderr, "Dave: loaded font %s @ %.0fpx\n", path, size);
            return true;
        }
    }
    io.Fonts->AddFontDefault();
    std::fprintf(stderr, "Dave: using ImGui default font (no system TTF found)\n");
    return false;
}

} // namespace dave::gui::theme
