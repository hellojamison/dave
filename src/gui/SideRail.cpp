// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/SideRail.h"

#include "gui/Theme.h"

namespace dave::gui {

float sideRailButtonY(float railTop, int index) {
    return railTop + kSideRailButtonGap +
           static_cast<float>(index) *
               (kSideRailButtonSize + kSideRailButtonGap);
}

bool sideRailButton(const char* id, ImVec2 pos, bool active,
                    const char* tooltip) {
    const auto& pal = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(id);
    ImGui::InvisibleButton("##rail", ImVec2(kSideRailButtonSize,
                                            kSideRailButtonSize));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImGui::PopID();

    const ImVec2 max(pos.x + kSideRailButtonSize, pos.y + kSideRailButtonSize);
    if (active || hovered) {
        dl->AddRectFilled(pos, max,
                          ImGui::GetColorU32(active ? pal.accentDeep
                                                    : pal.surfaceStrong),
                          5.0f);
    }
    if (hovered && tooltip != nullptr) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

void drawStripIcon(ImDrawList* dl, ImVec2 centre, ImU32 color) {
    // A fader: a track with a cap on it, which is what the panel it opens
    // leads with.
    constexpr float halfHeight = 7.0f;
    dl->AddLine(ImVec2(centre.x - 4.0f, centre.y - halfHeight),
                ImVec2(centre.x - 4.0f, centre.y + halfHeight), color, 1.4f);
    dl->AddLine(ImVec2(centre.x + 4.0f, centre.y - halfHeight),
                ImVec2(centre.x + 4.0f, centre.y + halfHeight), color, 1.4f);
    dl->AddRectFilled(ImVec2(centre.x - 7.0f, centre.y - 1.0f),
                      ImVec2(centre.x - 1.0f, centre.y + 2.0f), color, 1.0f);
    dl->AddRectFilled(ImVec2(centre.x + 1.0f, centre.y - 5.0f),
                      ImVec2(centre.x + 7.0f, centre.y - 2.0f), color, 1.0f);
}

} // namespace dave::gui
