// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/TrackColorPicker.h"

#include "document/Types.h"
#include "gui/Theme.h"

#include <algorithm>

namespace dave::gui {

const std::array<TrackColorChoice, 20>& trackColorChoices() {
    // Deliberately broad enough for a large post session, but pulled toward
    // Dave's muted value range so a track color identifies rather than glows.
    static constexpr std::array<TrackColorChoice, 20> choices{{
        {"Red", "#c96f72"},       {"Coral", "#cf8068"},
        {"Orange", "#ce915f"},    {"Gold", "#c2a45f"},
        {"Olive", "#9fa363"},     {"Green", "#79a076"},
        {"Sage", "#7fa092"},      {"Teal", "#65a096"},
        {"Cyan", "#68a0aa"},      {"Blue", "#6f92b0"},
        {"Indigo", "#7783ad"},    {"Violet", "#8d79a8"},
        {"Purple", "#9c78a1"},    {"Magenta", "#ae7796"},
        {"Rose", "#b97984"},      {"Sand", "#aa9172"},
        {"Brown", "#957762"},     {"Slate", "#7f8d91"},
        {"Silver", "#9b9994"},    {"Graphite", "#74716e"},
    }};
    return choices;
}

ImVec4 trackColorValue(const std::string& color, const ImVec4& fallback) {
    if (!document::validTrackColor(color) || color.empty()) return fallback;
    auto byte = [&](size_t offset) {
        return static_cast<float>(std::stoi(color.substr(offset, 2), nullptr, 16)) /
               255.0f;
    };
    return ImVec4(byte(1), byte(3), byte(5), 1.0f);
}

bool drawTrackColorPopup(const char* popupId, const std::string& currentColor,
                         std::string& selectedColor) {
    if (!ImGui::BeginPopup(popupId)) return false;

    bool changed = false;
    ImGui::TextUnformatted("Track Color");
    ImGui::Separator();
    constexpr int columns = 5;
    constexpr float swatchSize = 24.0f;
    const auto& choices = trackColorChoices();
    for (size_t i = 0; i < choices.size(); ++i) {
        const auto& choice = choices[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const ImVec4 color = trackColorValue(choice.hex, ImVec4(1, 1, 1, 1));
        if (ImGui::ColorButton("##swatch", color,
                               ImGuiColorEditFlags_NoTooltip |
                                   ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(swatchSize, swatchSize))) {
            selectedColor = choice.hex;
            changed = selectedColor != currentColor;
            ImGui::CloseCurrentPopup();
        }
        if (currentColor == choice.hex) {
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(position.x - 2.0f, position.y - 2.0f),
                ImVec2(position.x + swatchSize + 2.0f,
                       position.y + swatchSize + 2.0f),
                ImGui::GetColorU32(theme::palette().text), 2.0f, 0, 2.0f);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", choice.name);
        ImGui::PopID();
        if ((i + 1) % columns != 0) ImGui::SameLine();
    }

    ImGui::Separator();
    if (ImGui::Button("Use Default")) {
        selectedColor.clear();
        changed = !currentColor.empty();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return changed;
}

} // namespace dave::gui
