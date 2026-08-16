// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/LevelMeter.h"

#include "gui/Theme.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dave::gui {

namespace {

ImU32 C(const ImVec4& value) { return ImGui::GetColorU32(value); }

} // namespace

float levelMeterWidth(const LevelMeterStyle& style, int channels) {
    if (channels <= 0) return 0.0f;
    return static_cast<float>(channels) * style.channelWidth +
           static_cast<float>(channels - 1) * style.channelGap;
}

float amplitudeToMeterY(float amplitude, float top, float bottom) {
    const float db = 20.0f * std::log10(std::max(amplitude, 0.001f));
    const float normalized = std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);
    return bottom - normalized * (bottom - top);
}

bool drawLevelMeter(engine::GainNode* node, ImVec2 pos, float height,
                    LevelMeterOptions& options, int channels,
                    const LevelMeterStyle& style) {
    if (channels <= 0 || height <= 1.0f) return false;
    bool changed = false;

    const auto& pal = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 restoreCursor = ImGui::GetCursorScreenPos();
    const float width = levelMeterWidth(style, channels);

    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(node);
    ImGui::InvisibleButton("##levelMeter", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        ImGui::OpenPopup("##meterMenu");
    }
    if (ImGui::BeginPopup("##meterMenu")) {
        ImGui::TextDisabled("Meter");
        ImGui::Separator();
        if (ImGui::MenuItem("Post-fader", nullptr, !options.preFader)) {
            options.preFader = false;
            changed = true;
        }
        if (ImGui::MenuItem("Pre-fader", nullptr, options.preFader)) {
            options.preFader = true;
            changed = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("RMS", nullptr, options.rmsBody)) {
            options.rmsBody = true;
            changed = true;
        }
        if (ImGui::MenuItem("Linear (peak)", nullptr, !options.rmsBody)) {
            options.rmsBody = false;
            changed = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear clip", nullptr, false, node != nullptr)) {
            if (node != nullptr) node->clearMeterClips();
        }
        ImGui::EndPopup();
    }

    const float top = pos.y;
    const float bottom = pos.y + height;
    const float warningY =
        amplitudeToMeterY(std::pow(10.0f, -12.0f / 20.0f), top, bottom);
    const float dangerY =
        amplitudeToMeterY(std::pow(10.0f, -3.0f / 20.0f), top, bottom);
    const bool clipDot = height >= style.minHeightForClipDot;

    std::vector<engine::GainNode::MeterSnapshot> snapshots(
        static_cast<size_t>(channels));
    if (node != nullptr) {
        for (int channel = 0; channel < channels; ++channel) {
            snapshots[static_cast<size_t>(channel)] =
                node->meter(channel, options.preFader);
        }
    }

    for (int channel = 0; channel < channels; ++channel) {
        const auto& snapshot = snapshots[static_cast<size_t>(channel)];
        const float left = pos.x + static_cast<float>(channel) *
                                       (style.channelWidth + style.channelGap);
        const float right = left + style.channelWidth;
        dl->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom),
                          C(pal.surfaceBase), 1.5f);
        dl->AddRect(ImVec2(left, top), ImVec2(right, bottom), C(pal.border),
                    1.5f);

        // The body is RMS — what the level sounds like — banded green/amber/red
        // by zone. Peak rides on top as a line, because a transient that never
        // shows on RMS is exactly the one that clips.
        const float bodyLevel = options.rmsBody ? snapshot.rms : snapshot.peak;
        const float rmsY =
            amplitudeToMeterY(std::clamp(bodyLevel, 0.0f, 2.0f), top, bottom);
        if (rmsY < bottom) {
            dl->AddRectFilled(ImVec2(left + 1.0f, std::max(rmsY, warningY)),
                              ImVec2(right - 1.0f, bottom - 1.0f),
                              C(pal.success), 1.0f);
            if (rmsY < warningY) {
                dl->AddRectFilled(ImVec2(left + 1.0f, std::max(rmsY, dangerY)),
                                  ImVec2(right - 1.0f, warningY),
                                  C(pal.warning));
            }
            if (rmsY < dangerY) {
                dl->AddRectFilled(ImVec2(left + 1.0f, rmsY),
                                  ImVec2(right - 1.0f, dangerY), C(pal.danger));
            }
        }

        const float peakY = amplitudeToMeterY(
            std::clamp(snapshot.peak, 0.0f, 2.0f), top, bottom);
        const ImU32 peakColor =
            snapshot.peak >= 1.0f
                ? C(pal.danger)
                : (snapshot.peak >= 0.25f ? C(pal.warning) : C(pal.accentStrong));
        dl->AddLine(ImVec2(left + 1.0f, peakY), ImVec2(right - 1.0f, peakY),
                    peakColor, 1.0f);
        if (snapshot.clipped && clipDot) {
            dl->AddRectFilled(ImVec2(left, top), ImVec2(right, top + 3.0f),
                              C(pal.danger), 1.0f);
        }
    }

    if (hovered && style.showTooltip) {
        const auto db = [](float value) {
            return value > 0.0f ? 20.0f * std::log10(value) : -60.0f;
        };
        const char* tap = options.preFader ? "Pre-fader" : "Post-fader";
        const char* body = options.rmsBody ? "RMS" : "linear peak";
        if (channels >= 2) {
            ImGui::SetTooltip("%s meter (%s)\nL %+.1f dB  R %+.1f dB\nClick for options",
                              tap, body, db(snapshots[0].peak),
                              db(snapshots[1].peak));
        } else {
            ImGui::SetTooltip("%s meter (%s)\n%+.1f dB\nClick for options", tap,
                              body, db(snapshots[0].peak));
        }
    }

    ImGui::PopID();
    ImGui::SetCursorScreenPos(restoreCursor);
    return changed;
}

} // namespace dave::gui
