// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <imgui.h>

namespace dave::gui::theme {

// Brand and audio-domain colors live together so later layout phases can
// compose new surfaces without introducing component-local literals.
struct Palette {
    ImVec4 backgroundTop;
    ImVec4 backgroundMid;
    ImVec4 backgroundBottom;

    ImVec4 surfaceBase;
    ImVec4 surfaceSoft;
    ImVec4 surfaceStrong;
    ImVec4 border;
    ImVec4 borderStrong;
    ImVec4 insetHighlight;

    ImVec4 text;
    ImVec4 textMuted;
    ImVec4 textSubtle;

    ImVec4 accent;
    ImVec4 accentStrong;
    ImVec4 accentDeep;
    ImVec4 danger;
    ImVec4 success;
    ImVec4 warning;

    ImVec4 controlTop;
    ImVec4 controlBottom;
    ImVec4 controlHoverTop;
    ImVec4 controlHoverBottom;
    ImVec4 controlActiveTop;
    ImVec4 controlActiveBottom;
    ImVec4 primaryTop;
    ImVec4 primaryBottom;
    ImVec4 primaryHoverTop;
    ImVec4 primaryHoverBottom;
    ImVec4 primaryActiveTop;
    ImVec4 primaryActiveBottom;
    ImVec4 primaryText;

    // Existing semantic names remain stable while the layout is rebuilt.
    ImVec4 bg;
    ImVec4 bgAlt;
    ImVec4 bgElevated;
    ImVec4 panel;
    ImVec4 accentHover;
    ImVec4 trackSelected;

    ImVec4 clipAudio;
    ImVec4 clipAudioBorder;
    ImVec4 waveform;
    ImVec4 markerCue;
    ImVec4 markerSection;
    ImVec4 markerLoop;
    ImVec4 markerPunch;
    ImVec4 markerCd;
    ImVec4 markerCustom;
    ImVec4 clipVideo;
    ImVec4 clipVideoBorder;
};

const Palette& palette();

struct TypeScale {
    int caption = 12;
    int body = 13;
    int label = 15;
    int title = 18;
};

const TypeScale& typeScale();

struct Fonts {
    ImFont* body = nullptr;
    ImFont* small = nullptr;
    ImFont* label = nullptr;
    ImFont* heading = nullptr;
    ImFont* monoSmall = nullptr;
    ImFont* monoLarge = nullptr;
};

const Fonts& fonts();

// Loads every role into one atlas, preferring SF Pro/SF Mono, then Helvetica,
// then ImGui's embedded font. Each selected face is logged to stderr.
bool loadDefaultFont();

// Kept while ImGuiLayer still calls the former single-size API. The hierarchy
// is deliberately fixed; callers should use fonts() instead of ad-hoc sizes.
inline bool loadDefaultFont(float) { return loadDefaultFont(); }

// Apply the token layer to stock ImGui widgets. Custom gradients are available
// through the drawing helpers below.
void applyTheme();

struct Rect {
    ImVec2 min;
    ImVec2 max;
};

enum class PanelElevation {
    Base,
    Soft,
    Strong,
};

enum class ButtonVariant {
    Normal,
    Primary,
};

// Transport glyphs are geometry rather than a font dependency, so the app can
// keep a small, license-transparent control vocabulary across platforms.
enum class TransportIcon {
    Play,
    Stop,
    ReturnToStart,
    Record,
};

void drawVerticalGradient(ImDrawList* drawList, const Rect& rect,
                          ImU32 topColor, ImU32 bottomColor, float rounding = 0.0f);
void drawPanelSurface(ImDrawList* drawList, const Rect& rect,
                      PanelElevation elevation = PanelElevation::Base);
void drawShellBackground(ImDrawList* drawList, const Rect& rect);
bool gradientButton(const char* label, ImVec2 size = ImVec2(0, 0),
                    ButtonVariant variant = ButtonVariant::Normal);
bool iconButton(const char* id, TransportIcon icon, const char* tooltip,
                ImVec2 size, ButtonVariant variant = ButtonVariant::Normal);
void panelHeader(const char* label);

ImVec4 hex(uint32_t rgba);

} // namespace dave::gui::theme
