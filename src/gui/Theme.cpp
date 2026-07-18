#include "gui/Theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>

namespace dave::gui::theme {

namespace {
Palette g_palette;

// Build the palette once. Colors are RGBA hex; chosen for a cohesive pro-audio
// dark look. Tweak here and the whole UI follows.
Palette makePalette() {
    Palette p;
    p.bg               = hex(0x1a1b1eff); // deep neutral
    p.bgAlt            = hex(0x222328ff); // alternating rows
    p.bgElevated       = hex(0x2a2c33ff); // toolbars, headers
    p.panel            = hex(0x202127ff);
    p.border           = hex(0x3a3d46ff);
    p.text             = hex(0xe6e7ebff);
    p.textMuted        = hex(0x9a9da6ff);
    p.accent           = hex(0xf5a623ff); // amber
    p.accentHover      = hex(0xffb84dff);
    p.clipAudio        = hex(0x3d6fb0ff); // blue body
    p.clipAudioBorder  = hex(0x5b8fd0ff);
    p.trackSelected    = hex(0x34373fff);
    return p;
}
} // namespace

const Palette& palette() { return g_palette; }

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

    // Spacing & rounding — tighter, more app-like than defaults.
    s.WindowPadding    = ImVec2(10, 10);
    s.FramePadding     = ImVec2(8, 5);
    s.ItemSpacing      = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.WindowRounding   = 4.0f;
    s.ChildRounding    = 4.0f;
    s.FrameRounding    = 3.0f;
    s.GrabRounding     = 3.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize = 0.0f;   // flat panels
    s.FrameBorderSize  = 0.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    auto c = s.Colors;
    c[ImGuiCol_Text]            = p.text;
    c[ImGuiCol_TextDisabled]    = p.textMuted;
    c[ImGuiCol_WindowBg]        = p.bg;
    c[ImGuiCol_ChildBg]         = p.panel;
    c[ImGuiCol_PopupBg]         = p.bgElevated;
    c[ImGuiCol_Border]          = p.border;
    c[ImGuiCol_BorderShadow]    = ImVec4(0,0,0,0);
    c[ImGuiCol_FrameBg]         = p.bgAlt;
    c[ImGuiCol_FrameBgHovered]  = hex(0x353841ff);
    c[ImGuiCol_FrameBgActive]   = hex(0x3d404aff);
    c[ImGuiCol_TitleBg]         = p.bgElevated;
    c[ImGuiCol_TitleBgActive]   = p.bgElevated;
    c[ImGuiCol_TitleBgCollapsed]= p.bgAlt;
    c[ImGuiCol_MenuBarBg]       = p.bgElevated;
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0,0,0,0);
    c[ImGuiCol_ScrollbarGrab]   = hex(0x4a4d57ff);
    c[ImGuiCol_ScrollbarGrabHovered] = hex(0x5a5d67ff);
    c[ImGuiCol_ScrollbarGrabActive]  = p.accent;
    c[ImGuiCol_CheckMark]       = p.accent;
    c[ImGuiCol_SliderGrab]      = p.accent;
    c[ImGuiCol_SliderGrabActive]= p.accentHover;
    c[ImGuiCol_Button]          = p.bgAlt;
    c[ImGuiCol_ButtonHovered]   = hex(0x353841ff);
    c[ImGuiCol_ButtonActive]    = p.accent;
    c[ImGuiCol_Header]          = p.bgAlt;
    c[ImGuiCol_HeaderHovered]   = hex(0x353841ff);
    c[ImGuiCol_HeaderActive]    = p.trackSelected;
    c[ImGuiCol_Separator]       = p.border;
    c[ImGuiCol_Tab]             = p.bgAlt;
    c[ImGuiCol_TabHovered]      = p.bgElevated;
    c[ImGuiCol_TabActive]       = p.bgElevated;
    c[ImGuiCol_TabUnfocused]    = p.bg;
    c[ImGuiCol_TabUnfocusedActive] = p.bgAlt;
    c[ImGuiCol_DockingPreview]  = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.4f);
    c[ImGuiCol_DockingEmptyBg]  = p.bg;
    c[ImGuiCol_TableHeaderBg]   = p.bgElevated;
    c[ImGuiCol_TableBorderStrong] = p.border;
    c[ImGuiCol_TableBorderLight]  = hex(0x2a2c33ff);
    c[ImGuiCol_TextSelectedBg]  = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.3f);
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
