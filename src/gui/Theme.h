// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <imgui.h>

#include <string>

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
    ImVec4 recordArmed;
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
    ImVec4 clipMidi;
    ImVec4 clipMidiBorder;
    ImVec4 midiNote;             // note blobs inside a MIDI clip

    // Timeline surfaces, matching PTXExtractor's session workspace so the two
    // apps read as one family. The ordering matters more than the exact
    // values: lanes are the DARKEST surface, so clips and waveforms sit on the
    // deepest background and read as content on a canvas. Headers and the
    // ruler step lighter, which puts the chrome visually above the material.
    ImVec4 rulerSurface;
    ImVec4 trackHeaderSurface;
    ImVec4 trackLaneSurface;
    ImVec4 trackLaneAlt;         // subtle banding for adjacent rows
    ImVec4 workspaceBackground;  // behind the tracks, outside the lanes
    ImVec4 toolbarSurface;
    ImVec4 inspectorSurface;

    // Mute and solo are the two per-track states a mix engineer scans for at a
    // glance, so they get dedicated hues rather than the accent — amber for
    // mute, yellow for solo, as in PTXExtractor and Pro Tools itself.
    ImVec4 trackMuteActive;
    ImVec4 trackSoloActive;
    ImVec4 trackControlInactive;
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
    Danger,
};

// Transport glyphs are geometry rather than a font dependency, so the app can
// keep a small, license-transparent control vocabulary across platforms.
enum class TransportIcon {
    Play,
    Stop,
    ReturnToStart,
    Record,
    Loop,
    Transient,
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
// Shared record-arm glyph: neutral at rest and semantic danger red when armed.
// Callers keep their own hit targets so compact timeline and mixer layouts can
// share the same visible control without shifting mute or solo.
void drawRecordArmIndicator(ImDrawList* drawList, ImVec2 center, float radius,
                            bool armed, bool hovered);
// Centre short control labels by their visible glyph bounds rather than the
// font's advance box. Wide glyphs such as M otherwise look shifted even when
// their mathematical text box is centred.
void drawCenteredControlLabel(ImDrawList* drawList, const Rect& rect,
                              ImU32 color, const char* label);

// Pan as a mixer reads it: "C" dead centre, "L50" / "R100" either side. The
// underlying value stays -1..+1 — this is display vocabulary, shared so the
// track gutter and the mixer strip cannot drift into two different dialects
// for the same control.
std::string formatPan(double pan);

ImVec4 hex(uint32_t rgba);

} // namespace dave::gui::theme
