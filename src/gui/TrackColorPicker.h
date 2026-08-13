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

// Draw an already-requested popup. Returns true and fills selectedColor when
// the user chooses a swatch; an empty selection restores the type default.
bool drawTrackColorPopup(const char* popupId, const std::string& currentColor,
                         std::string& selectedColor);

} // namespace dave::gui
