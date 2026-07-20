#include "gui/Theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>

namespace dave::gui::theme {

namespace {
Palette g_palette;

// Build the palette once. Pro Tools-inspired light-neutral theme: steel-gray
// Dark neutral theme — the readable choice for long mixing sessions. The light
// "Pro Tools" palette had amber accent text on light slate = unreadable. Dark
// bg with amber accent gives high contrast for both text and UI highlights.
Palette makePalette() {
    Palette p;
    // Neutral ramp — dark cool (6 steps).
    p.bg               = hex(0x1e1f23ff);
    p.panel            = hex(0x25272dff);
    p.bgAlt            = hex(0x2c2e36ff);
    p.bgElevated       = hex(0x34373fff);
    p.trackSelected    = hex(0x3a3d47ff);
    p.border           = hex(0x444852ff);
    // Text — near-white on dark, high contrast.
    p.text             = hex(0xe8e9edff);
    p.textMuted        = hex(0x9da2acff);
    // Accent — amber. Readable as text on dark bg (unlike on light).
    p.accent           = hex(0xf5a623ff);
    p.accentHover      = hex(0xffb84dff);
    p.clipAudio        = hex(0x4a7fc4ff);
    p.clipAudioBorder  = hex(0x6a9fe4ff);
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
