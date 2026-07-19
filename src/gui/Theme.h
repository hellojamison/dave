#pragma once

#include <cstdint>
#include <imgui.h>

namespace dave::gui {

// Dave's visual identity. A modern, low-glare dark theme inspired by pro
// audio tools (Ableton/Logic/Reaper dark) — not the ImGui default, which
// reads as a debug tool.
//
// Palette goals:
//   - Deep neutral background (not pure black — easier on the eyes for long
//     sessions, and clips/controls pop against it).
//   - A single accent hue (warm amber) for the playhead, selection, and primary
//     actions — ties the UI together and signals "this is Dave".
//   - Cool blue clip bodies (audio) so waveforms read clearly.
//   - High but not maximum contrast for text; muted for secondary text.
//
// applyTheme() configures colors, spacing, rounding, and the default font.
// Call once after ImGui::CreateContext().
namespace theme {

// Brand palette (referenced by name so changes are centralized).
struct Palette {
    ImVec4 bg;            // main window background
    ImVec4 bgAlt;         // alternating rows, panels
    ImVec4 bgElevated;    // headers, toolbars (slightly raised)
    ImVec4 panel;         // child panels
    ImVec4 border;
    ImVec4 text;
    ImVec4 textMuted;
    ImVec4 accent;        // amber — playhead, selection, primary action
    ImVec4 accentHover;
    ImVec4 clipAudio;     // blue clip body
    ImVec4 clipAudioBorder;
    ImVec4 trackSelected;
};

const Palette& palette();

// Apply the theme to the current ImGui context.
void applyTheme();

// Load the default UI font at a sensible size. Returns true on success.
// Falls back to the ImGui default if the embedded font can't load.
bool loadDefaultFont(float size = 14.0f);

// Type hierarchy (impeccable: fixed rem-like scale, ratio ~1.125–1.2).
// Push/pop these around headings, captions, etc. for consistent hierarchy
// without re-deriving sizes at each call site.
struct TypeScale {
    int caption = 12;   // muted secondary labels, meta info
    int body    = 14;   // default body / UI text
    int label   = 15;   // emphasized labels, section heads
    int title   = 18;   // panel titles, the rare large heading
};
const TypeScale& typeScale();

// Convenience: a hex color -> ImVec4.
ImVec4 hex(uint32_t rgba);

} // namespace theme

} // namespace dave::gui
