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
    const float normalized = std::clamp(
        (db - kMeterFloorDb) / (kMeterCeilingDb - kMeterFloorDb), 0.0f, 1.0f);
    return bottom - normalized * (bottom - top);
}

float meterOverDb(float heldPeak) {
    if (!(heldPeak > 1.0f)) return 0.0f;   // NaN-safe: NaN fails the compare
    return 20.0f * std::log10(heldPeak);
}

float noiseFloorLossFraction(float overDb) {
    if (!(overDb > 0.0f)) return 0.0f;
    constexpr float span = kMeterCeilingDb - kMeterFloorDb;
    return std::clamp(overDb / span, 0.0f, 1.0f);
}

bool drawLevelMeter(engine::GainNode* node, ImVec2 pos, float height,
                    LevelMeterOptions& options, int channels,
                    const LevelMeterStyle& style, bool floatHeadroom) {
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
        if (ImGui::BeginMenu("Peak hold")) {
            for (const auto& choice : kMeterPeakHoldChoices) {
                const bool on = options.peakHoldSeconds == choice.seconds;
                if (ImGui::MenuItem(choice.label, nullptr, on)) {
                    options.peakHoldSeconds = choice.seconds;
                    changed = true;
                }
            }
            ImGui::EndMenu();
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
    // 0 dBFS. In a float session this is not the top of the meter, it is the
    // line above which the level is borrowed rather than free.
    const float unityY = amplitudeToMeterY(1.0f, top, bottom);
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
        // How far this channel has been over, and therefore how much quiet
        // detail a fixed-point render of it would lose.
        const float overDb =
            floatHeadroom ? meterOverDb(snapshot.maxPeak) : 0.0f;

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
                // Above 0 dBFS a float session is into headroom, not into
                // damage: nothing has clipped and the fader recovers it
                // exactly. Blue says "borrowed", red would say "lost".
                const float redFloor = floatHeadroom
                    ? std::max(rmsY, unityY) : rmsY;
                dl->AddRectFilled(ImVec2(left + 1.0f, redFloor),
                                  ImVec2(right - 1.0f, dangerY), C(pal.danger));
                if (floatHeadroom && rmsY < unityY) {
                    dl->AddRectFilled(ImVec2(left + 1.0f, rmsY),
                                      ImVec2(right - 1.0f, unityY),
                                      C(pal.headroom));
                }
            }
        }

        const float peakY = amplitudeToMeterY(
            std::clamp(snapshot.peak, 0.0f, 2.0f), top, bottom);
        const ImU32 peakColor =
            snapshot.peak >= 1.0f
                ? (floatHeadroom ? C(pal.headroom) : C(pal.danger))
                : (snapshot.peak >= 0.25f ? C(pal.warning) : C(pal.accentStrong));
        dl->AddLine(ImVec2(left + 1.0f, peakY), ImVec2(right - 1.0f, peakY),
                    peakColor, 1.0f);

        // The hold marker, drawn only where it has something to say: while it
        // sits above the falling bar. Level with the bar it would just be the
        // peak line drawn twice.
        if (snapshot.holdPeak > snapshot.peak) {
            const float holdY = amplitudeToMeterY(
                std::clamp(snapshot.holdPeak, 0.0f, 2.0f), top, bottom);
            if (holdY < peakY - 0.5f) {
                const ImU32 holdColor =
                    snapshot.holdPeak >= 1.0f
                        ? (floatHeadroom ? C(pal.headroom) : C(pal.danger))
                        : C(pal.text);
                dl->AddLine(ImVec2(left, holdY), ImVec2(right, holdY),
                            holdColor, 1.0f);
            }
        }
        if (overDb > 0.0f) {
            // The bill for the headroom above, paid at the bottom: run 6 dB
            // hot and the quietest 6 dB of the material lands under the LSB of
            // a fixed-point render. That part does not come back when the
            // fader does, which is why this end is red and the other is not.
            const float lost =
                noiseFloorLossFraction(overDb) * (bottom - top);
            if (lost >= 1.0f) {
                dl->AddRectFilled(ImVec2(left + 1.0f, bottom - lost),
                                  ImVec2(right - 1.0f, bottom - 1.0f),
                                  C(pal.danger), 1.0f);
            }
        }
        if (floatHeadroom && height >= 8.0f) {
            // The 0 dBFS reference, so the blue above it reads as a region
            // rather than as a taller bar.
            dl->AddLine(ImVec2(left, unityY), ImVec2(right, unityY),
                        C(pal.borderStrong), 1.0f);
        }
        if (snapshot.clipped && clipDot) {
            dl->AddRectFilled(ImVec2(left, top), ImVec2(right, top + 3.0f),
                              floatHeadroom ? C(pal.headroom) : C(pal.danger),
                              1.0f);
        }
    }

    if (hovered && style.showTooltip) {
        const auto db = [](float value) {
            return value > 0.0f ? 20.0f * std::log10(value) : -60.0f;
        };
        const char* tap = options.preFader ? "Pre-fader" : "Post-fader";
        const char* body = options.rmsBody ? "RMS" : "linear peak";
        float worstOver = 0.0f;
        if (floatHeadroom) {
            for (const auto& snapshot : snapshots) {
                worstOver = std::max(worstOver, meterOverDb(snapshot.maxPeak));
            }
        }
        if (worstOver > 0.0f) {
            ImGui::SetTooltip(
                "%s meter (%s)\n%+.1f dB into 32-bit float headroom\n"
                "The quietest %.1f dB will fall below a fixed-point render\n"
                "Click for options",
                tap, body, worstOver, worstOver);
            ImGui::PopID();
            ImGui::SetCursorScreenPos(restoreCursor);
            return changed;
        }
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
