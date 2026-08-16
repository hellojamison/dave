// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <imgui.h>

#include <array>
#include <string>

namespace dave::gui {

struct TrackColorChoice {
    const char* name;
    const char* hex;
};

const std::array<TrackColorChoice, 20>& trackColorChoices();
ImVec4 trackColorValue(const std::string& color, const ImVec4& fallback);

// The colour a track shows when nobody has chosen one: a cycle over the marker
// palette, indexed by the track's row. Shared rather than duplicated because
// the timeline band and the channel strip header both answer "what colour is
// this track", and two copies of the cycle would answer differently the first
// time either changed — leaving the strip claiming to belong to a row painted
// something else.
ImVec4 defaultTrackColor(int rowIndex);

// Draw an already-requested popup. Returns true and fills selectedColor when
// the user chooses a swatch; an empty selection restores the type default.
bool drawTrackColorPopup(const char* popupId, const std::string& currentColor,
                         std::string& selectedColor);

} // namespace dave::gui
