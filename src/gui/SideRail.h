// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <imgui.h>

namespace dave::gui {

// The thin strips down the left and right edges.
//
// Panels that can be closed need somewhere permanent to be reopened from. A
// toolbar button works until there are several, at which point the toolbar is
// carrying controls for things that are not on screen — the rail is the one
// place that is always the same width whatever is open, so what it holds is
// "which panels exist" rather than "what can I do right now".

inline constexpr float kSideRailWidth = 30.0f;
inline constexpr float kSideRailButtonSize = 26.0f;
inline constexpr float kSideRailButtonGap = 4.0f;

// The top of the nth button in a rail starting at `railTop`.
float sideRailButtonY(float railTop, int index);

// One rail button. Returns true when clicked. `active` lights it, so the rail
// also reports which panels are open — the same mark doing both jobs.
bool sideRailButton(const char* id, ImVec2 pos, bool active,
                    const char* tooltip);

// The marks. Drawn rather than glyphs so they match the ones inside the panels
// they open.
void drawStripIcon(ImDrawList* dl, ImVec2 centre, ImU32 color);

} // namespace dave::gui
