// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/Theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <initializer_list>
#include <string>

namespace dave::gui::theme {

namespace {

Palette g_palette;
Fonts g_fonts;

ImU32 color(const ImVec4& value) {
    return ImGui::GetColorU32(value);
}

Palette makePalette() {
    Palette p;

    p.backgroundTop     = hex(0x403d3aff);
    p.backgroundMid     = hex(0x322f2dff);
    p.backgroundBottom  = hex(0x242220ff);
    p.surfaceBase       = hex(0x353230ff);
    p.surfaceSoft       = hex(0x3d3a37ff);
    p.surfaceStrong     = hex(0x46423fff);
    p.border            = ImVec4(1.0f, 1.0f, 1.0f, 0.11f);
    p.borderStrong      = ImVec4(1.0f, 1.0f, 1.0f, 0.18f);
    p.insetHighlight    = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);

    p.text              = hex(0xddd7d1ff);
    p.textMuted         = hex(0xb3ada7ff);
    p.textSubtle        = hex(0x8a857fff);
    p.accent            = hex(0x8cab9bff);
    p.accentStrong      = hex(0xaac6b7ff);
    p.accentDeep        = hex(0x668173ff);
    p.danger            = hex(0xe49c9cff);

    // These roles have no direct Overcue token. Muted botanical green and
    // ochre preserve clear status semantics without leaving the warm register.
    p.success           = hex(0x91b394ff);
    p.warning           = hex(0xd2ad7eff);

    p.controlTop            = hex(0x5c5855ff);
    p.controlBottom         = hex(0x474340ff);
    p.controlHoverTop       = hex(0x696460ff);
    p.controlHoverBottom    = hex(0x514c48ff);
    p.controlActiveTop      = hex(0x4d4945ff);
    p.controlActiveBottom   = hex(0x3c3835ff);
    p.primaryTop            = hex(0x7ea18fff);
    p.primaryBottom         = hex(0x627b6eff);
    p.primaryHoverTop       = hex(0x8aab9aff);
    p.primaryHoverBottom    = hex(0x6d8779ff);
    p.primaryActiveTop      = hex(0x657d70ff);
    p.primaryActiveBottom   = hex(0x53685dff);
    p.primaryText           = hex(0xf4efe9ff);

    p.bg                = p.backgroundBottom;
    p.bgAlt             = p.surfaceSoft;
    p.bgElevated        = p.surfaceStrong;
    p.panel             = p.surfaceBase;
    p.accentHover       = p.accentStrong;
    p.trackSelected     = hex(0x4b514dff);

    // Audio colors retain their separate identities, but their saturation and
    // value are pulled toward the taupe/sage shell.
    p.clipAudio         = hex(0x55766cff);
    p.clipAudioBorder   = hex(0x7fa092ff);
    p.waveform          = hex(0xe2ddd7e6);
    p.markerCue         = hex(0xc1a36fff);
    p.markerSection     = hex(0xaa8fa5ff);
    p.markerLoop        = hex(0x83a98eff);
    p.markerPunch       = p.danger;
    p.markerCd          = hex(0x839eafff);
    p.markerCustom      = hex(0x9b9690ff);
    p.clipVideo         = hex(0x466b64ff);
    p.clipVideoBorder   = hex(0x71958cff);

    // Timeline surfaces, taken from PTXExtractor's CueConverterTheme so a user
    // moving between the two apps sees one product. Note lanes are darker than
    // both the header column and the window behind them: content sits in a
    // well, chrome sits above it.
    p.rulerSurface        = hex(0x252422ff);
    p.trackHeaderSurface  = hex(0x302e2bff);
    p.trackLaneSurface    = hex(0x201f1dff);
    p.trackLaneAlt        = hex(0x242220ff);
    p.workspaceBackground = hex(0x242321ff);
    p.toolbarSurface      = hex(0x302e2cff);
    p.inspectorSurface    = hex(0x2c2a28ff);

    p.trackMuteActive      = hex(0xe78a31ff);
    p.trackSoloActive      = hex(0xf0cc3aff);
    p.trackControlInactive = hex(0x242321ff);

    return p;
}

ImFont* loadFontRole(const char* role, float size,
                     std::initializer_list<const char*> candidates) {
    ImGuiIO& io = ImGui::GetIO();
    for (const char* path : candidates) {
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 1;
        config.Flags |= ImFontFlags_NoLoadError;
        std::snprintf(config.Name, sizeof(config.Name), "Dave %s %.0fpx", role, size);
        if (ImFont* loaded = io.Fonts->AddFontFromFileTTF(path, size, &config)) {
            std::fprintf(stderr, "Dave: loaded font %s: %s @ %.0fpx\n",
                         role, path, size);
            return loaded;
        }
    }

    ImFontConfig fallback;
    fallback.SizePixels = size;
    fallback.OversampleH = 2;
    fallback.OversampleV = 1;
    std::snprintf(fallback.Name, sizeof(fallback.Name),
                  "Dave %s ImGui default %.0fpx", role, size);
    ImFont* loaded = io.Fonts->AddFontDefault(&fallback);
    std::fprintf(stderr, "Dave: loaded font %s: ImGui default @ %.0fpx\n",
                 role, size);
    return loaded;
}

void drawInsetHighlight(ImDrawList* drawList, const Rect& rect, float rounding) {
    const float inset = std::max(1.0f, rounding * 0.5f);
    drawList->AddLine(
        ImVec2(rect.min.x + inset, rect.min.y + 1.0f),
        ImVec2(rect.max.x - inset, rect.min.y + 1.0f),
        color(g_palette.insetHighlight));
}

} // namespace

const Palette& palette() {
    return g_palette;
}

const TypeScale& typeScale() {
    static const TypeScale scale;
    return scale;
}

const Fonts& fonts() {
    return g_fonts;
}

ImVec4 hex(uint32_t rgba) {
    constexpr float scale = 1.0f / 255.0f;
    return ImVec4(
        ((rgba >> 24) & 0xff) * scale,
        ((rgba >> 16) & 0xff) * scale,
        ((rgba >> 8) & 0xff) * scale,
        (rgba & 0xff) * scale);
}

bool loadDefaultFont() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_fonts = {};

#ifdef __APPLE__
    constexpr const char* sansPrimary = "/System/Library/Fonts/SFNS.ttf";
    constexpr const char* monoPrimary = "/System/Library/Fonts/SFNSMono.ttf";
    constexpr const char* sansFallback = "/System/Library/Fonts/Helvetica.ttc";

    const auto& scale = typeScale();
    g_fonts.body = loadFontRole(
        "body", static_cast<float>(scale.body), {sansPrimary, sansFallback});
    g_fonts.small = loadFontRole(
        "small", static_cast<float>(scale.caption), {sansPrimary, sansFallback});
    g_fonts.label = loadFontRole(
        "label", static_cast<float>(scale.label), {sansPrimary, sansFallback});
    g_fonts.heading = loadFontRole(
        "heading", static_cast<float>(scale.title), {sansPrimary, sansFallback});
    g_fonts.monoSmall = loadFontRole(
        "mono-small", 13.0f, {monoPrimary, sansPrimary, sansFallback});
    g_fonts.monoLarge = loadFontRole(
        "mono-large", 20.0f, {monoPrimary, sansPrimary, sansFallback});
#else
    const auto& scale = typeScale();
    g_fonts.body = loadFontRole("body", static_cast<float>(scale.body), {});
    g_fonts.small = loadFontRole("small", static_cast<float>(scale.caption), {});
    g_fonts.label = loadFontRole("label", static_cast<float>(scale.label), {});
    g_fonts.heading = loadFontRole("heading", static_cast<float>(scale.title), {});
    g_fonts.monoSmall = loadFontRole("mono-small", 13.0f, {});
    g_fonts.monoLarge = loadFontRole("mono-large", 20.0f, {});
#endif

    io.FontDefault = g_fonts.body;
    return g_fonts.body != nullptr && g_fonts.small != nullptr &&
           g_fonts.label != nullptr && g_fonts.heading != nullptr &&
           g_fonts.monoSmall != nullptr && g_fonts.monoLarge != nullptr;
}

void applyTheme() {
    g_palette = makePalette();
    ImGuiStyle& style = ImGui::GetStyle();
    const auto& p = g_palette;

    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 8.0f;

    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = p.text;
    colors[ImGuiCol_TextDisabled] = p.textMuted;
    colors[ImGuiCol_WindowBg] = p.surfaceBase;
    colors[ImGuiCol_ChildBg] = p.surfaceBase;
    colors[ImGuiCol_PopupBg] = p.surfaceStrong;
    colors[ImGuiCol_Border] = p.border;
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_FrameBg] = p.controlBottom;
    colors[ImGuiCol_FrameBgHovered] = p.controlHoverBottom;
    colors[ImGuiCol_FrameBgActive] = p.controlActiveBottom;
    colors[ImGuiCol_TitleBg] = p.surfaceStrong;
    colors[ImGuiCol_TitleBgActive] = p.surfaceStrong;
    colors[ImGuiCol_TitleBgCollapsed] = p.surfaceSoft;
    colors[ImGuiCol_MenuBarBg] = p.surfaceStrong;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab] = p.surfaceStrong;
    colors[ImGuiCol_ScrollbarGrabHovered] = p.accentDeep;
    colors[ImGuiCol_ScrollbarGrabActive] = p.accent;
    colors[ImGuiCol_CheckMark] = p.accentStrong;
    colors[ImGuiCol_SliderGrab] = p.accent;
    colors[ImGuiCol_SliderGrabActive] = p.accentStrong;
    colors[ImGuiCol_Button] = p.controlBottom;
    colors[ImGuiCol_ButtonHovered] = p.controlHoverBottom;
    colors[ImGuiCol_ButtonActive] = p.controlActiveBottom;
    colors[ImGuiCol_Header] = p.surfaceSoft;
    colors[ImGuiCol_HeaderHovered] = p.trackSelected;
    colors[ImGuiCol_HeaderActive] = p.surfaceStrong;
    colors[ImGuiCol_Separator] = p.border;
    colors[ImGuiCol_Tab] = p.surfaceSoft;
    colors[ImGuiCol_TabHovered] = p.surfaceStrong;
    colors[ImGuiCol_TabActive] = p.surfaceStrong;
    colors[ImGuiCol_TabUnfocused] = p.backgroundBottom;
    colors[ImGuiCol_TabUnfocusedActive] = p.surfaceSoft;
    colors[ImGuiCol_DockingPreview] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f);
    colors[ImGuiCol_DockingEmptyBg] = p.backgroundBottom;
    colors[ImGuiCol_TableHeaderBg] = p.surfaceStrong;
    colors[ImGuiCol_TableBorderStrong] = p.borderStrong;
    colors[ImGuiCol_TableBorderLight] = p.border;
    colors[ImGuiCol_TextSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.30f);
    colors[ImGuiCol_NavHighlight] = p.accentStrong;
}

void drawVerticalGradient(ImDrawList* drawList, const Rect& rect,
                          ImU32 topColor, ImU32 bottomColor, float rounding) {
    if (drawList == nullptr || rect.max.x <= rect.min.x || rect.max.y <= rect.min.y) {
        return;
    }

    const int vertexStart = drawList->VtxBuffer.Size;
    drawList->AddRectFilled(rect.min, rect.max, topColor, rounding);
    const int vertexEnd = drawList->VtxBuffer.Size;
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(
        drawList, vertexStart, vertexEnd,
        rect.min, ImVec2(rect.min.x, rect.max.y),
        topColor, bottomColor);

    const int topAlpha = static_cast<int>(
        (topColor >> IM_COL32_A_SHIFT) & 0xff);
    const int bottomAlpha = static_cast<int>(
        (bottomColor >> IM_COL32_A_SHIFT) & 0xff);
    const int alphaDelta = bottomAlpha - topAlpha;
    const float inverseHeight = 1.0f / (rect.max.y - rect.min.y);
    for (int index = vertexStart; index < vertexEnd; ++index) {
        ImDrawVert& vertex = drawList->VtxBuffer[index];
        const float amount = ImClamp(
            (vertex.pos.y - rect.min.y) * inverseHeight, 0.0f, 1.0f);
        const ImU32 alpha = static_cast<ImU32>(
            topAlpha + static_cast<int>(alphaDelta * amount));
        vertex.col = (vertex.col & ~IM_COL32_A_MASK) |
                     (alpha << IM_COL32_A_SHIFT);
    }
}

void drawPanelSurface(ImDrawList* drawList, const Rect& rect,
                      PanelElevation elevation) {
    if (drawList == nullptr) {
        return;
    }

    const ImVec4* fill = &g_palette.surfaceBase;
    if (elevation == PanelElevation::Soft) {
        fill = &g_palette.surfaceSoft;
    } else if (elevation == PanelElevation::Strong) {
        fill = &g_palette.surfaceStrong;
    }

    constexpr float rounding = 8.0f;
    drawList->AddRectFilled(rect.min, rect.max, color(*fill), rounding);
    drawList->AddRect(rect.min, rect.max, color(g_palette.border), rounding);
    drawInsetHighlight(drawList, rect, rounding);
}

void drawShellBackground(ImDrawList* drawList, const Rect& rect) {
    if (drawList == nullptr || rect.max.y <= rect.min.y) {
        return;
    }

    const float middleY = rect.min.y + (rect.max.y - rect.min.y) * 0.52f;
    drawVerticalGradient(
        drawList, Rect{rect.min, ImVec2(rect.max.x, middleY)},
        color(g_palette.backgroundTop), color(g_palette.backgroundMid));
    drawVerticalGradient(
        drawList, Rect{ImVec2(rect.min.x, middleY), rect.max},
        color(g_palette.backgroundMid), color(g_palette.backgroundBottom));
}

bool gradientButton(const char* label, ImVec2 size, ButtonVariant variant) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
    if (size.x <= 0.0f) {
        size.x = labelSize.x + style.FramePadding.x * 2.0f;
    }
    if (size.y <= 0.0f) {
        size.y = labelSize.y + style.FramePadding.y * 2.0f;
    }

    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(
        label, size, ImGuiButtonFlags_EnableNav);
    const Rect rect{min, ImVec2(min.x + size.x, min.y + size.y)};
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImVec4 top;
    ImVec4 bottom;
    ImVec4 textColor;
    if (variant == ButtonVariant::Primary) {
        top = active ? g_palette.primaryActiveTop
                     : hovered ? g_palette.primaryHoverTop : g_palette.primaryTop;
        bottom = active ? g_palette.primaryActiveBottom
                        : hovered ? g_palette.primaryHoverBottom : g_palette.primaryBottom;
        textColor = g_palette.primaryText;
    } else {
        top = active ? g_palette.controlActiveTop
                     : hovered ? g_palette.controlHoverTop : g_palette.controlTop;
        bottom = active ? g_palette.controlActiveBottom
                        : hovered ? g_palette.controlHoverBottom : g_palette.controlBottom;
        textColor = g_palette.text;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    constexpr float rounding = 6.0f;
    drawVerticalGradient(drawList, rect, color(top), color(bottom), rounding);
    drawList->AddRect(
        rect.min, rect.max,
        color(variant == ButtonVariant::Primary
                  ? ImVec4(1.0f, 1.0f, 1.0f, 0.16f)
                  : ImVec4(1.0f, 1.0f, 1.0f, 0.12f)),
        rounding);
    drawInsetHighlight(drawList, rect, rounding);

    if (ImGui::IsItemFocused()) {
        drawList->AddRect(rect.min, rect.max, color(g_palette.accentStrong),
                          rounding, 0, 1.5f);
    }

    const char* textEnd = ImGui::FindRenderedTextEnd(label);
    drawList->PushClipRect(rect.min, rect.max, true);
    drawList->AddText(
        ImVec2(rect.min.x + (size.x - labelSize.x) * 0.5f,
               rect.min.y + (size.y - labelSize.y) * 0.5f),
        color(textColor), label, textEnd);
    drawList->PopClipRect();
    return pressed;
}

bool iconButton(const char* id, TransportIcon icon, const char* tooltip,
                ImVec2 size, ButtonVariant variant) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    const Rect rect{min, ImVec2(min.x + size.x, min.y + size.y)};
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImVec4 top;
    ImVec4 bottom;
    ImVec4 glyphColor;
    if (variant == ButtonVariant::Primary) {
        top = active ? g_palette.primaryActiveTop
                     : hovered ? g_palette.primaryHoverTop : g_palette.primaryTop;
        bottom = active ? g_palette.primaryActiveBottom
                        : hovered ? g_palette.primaryHoverBottom : g_palette.primaryBottom;
        glyphColor = g_palette.primaryText;
    } else {
        top = active ? g_palette.controlActiveTop
                     : hovered ? g_palette.controlHoverTop : g_palette.controlTop;
        bottom = active ? g_palette.controlActiveBottom
                        : hovered ? g_palette.controlHoverBottom : g_palette.controlBottom;
        glyphColor = g_palette.text;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    constexpr float rounding = 6.0f;
    drawVerticalGradient(drawList, rect, color(top), color(bottom), rounding);
    drawList->AddRect(
        rect.min, rect.max,
        color(variant == ButtonVariant::Primary
                  ? ImVec4(1.0f, 1.0f, 1.0f, 0.16f)
                  : ImVec4(1.0f, 1.0f, 1.0f, 0.12f)),
        rounding);
    drawInsetHighlight(drawList, rect, rounding);
    if (ImGui::IsItemFocused()) {
        drawList->AddRect(rect.min, rect.max, color(g_palette.accentStrong),
                          rounding, 0, 1.5f);
    }

    const ImVec2 center((rect.min.x + rect.max.x) * 0.5f,
                        (rect.min.y + rect.max.y) * 0.5f);
    const ImU32 glyph = color(glyphColor);
    switch (icon) {
        case TransportIcon::Play:
            drawList->AddTriangleFilled(
                ImVec2(center.x - 4.0f, center.y - 7.0f),
                ImVec2(center.x - 4.0f, center.y + 7.0f),
                ImVec2(center.x + 7.0f, center.y), glyph);
            break;
        case TransportIcon::Stop:
            drawList->AddRectFilled(
                ImVec2(center.x - 5.5f, center.y - 5.5f),
                ImVec2(center.x + 5.5f, center.y + 5.5f), glyph, 1.0f);
            break;
        case TransportIcon::ReturnToStart:
            drawList->AddLine(ImVec2(center.x - 7.0f, center.y - 7.0f),
                              ImVec2(center.x - 7.0f, center.y + 7.0f), glyph, 2.0f);
            drawList->AddTriangleFilled(
                ImVec2(center.x - 5.0f, center.y),
                ImVec2(center.x + 6.0f, center.y - 7.0f),
                ImVec2(center.x + 6.0f, center.y + 7.0f), glyph);
            break;
        case TransportIcon::Record:
            drawList->AddCircleFilled(center, 6.0f, glyph, 16);
            break;
    }

    if (tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return pressed;
}

void panelHeader(const char* label) {
    constexpr float height = 26.0f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
    const Rect rect{min, ImVec2(min.x + size.x, min.y + size.y)};
    drawPanelSurface(ImGui::GetWindowDrawList(), rect, PanelElevation::Soft);

    std::string uppercase(label);
    std::transform(
        uppercase.begin(), uppercase.end(), uppercase.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (g_fonts.small != nullptr) {
        ImGui::PushFont(
            g_fonts.small, static_cast<float>(typeScale().caption));
    }
    const ImVec2 textSize = ImGui::CalcTextSize(uppercase.c_str());
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(rect.min.x + 8.0f,
               rect.min.y + (height - textSize.y) * 0.5f),
        color(g_palette.textMuted), uppercase.c_str());
    if (g_fonts.small != nullptr) {
        ImGui::PopFont();
    }
    ImGui::Dummy(size);
}

} // namespace dave::gui::theme
