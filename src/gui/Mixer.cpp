// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/Mixer.h"

#include "editing/Commands.h"
#include "gui/RoutingViewModel.h"
#include "gui/Theme.h"
#include "gui/TrackColorPicker.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dave::gui {

namespace {

ImU32 C(const ImVec4& v) {
    return IM_COL32(int(v.x * 255), int(v.y * 255), int(v.z * 255), int(v.w * 255));
}

// Mixer spacing follows a compact 4-point scale. Controls keep their existing
// optical heights, while the space between related rows (4) and distinct
// signal-path sections (8) creates a consistent vertical rhythm.
constexpr float kSpaceXs = 2.0f;
constexpr float kSpaceSm = 4.0f;
constexpr float kSpaceMd = 8.0f;
constexpr float kControlHeight = 18.0f;
constexpr float kInsertRowHeight = kControlHeight;
constexpr float kPanKnobDiameter = 34.0f;
// Enough of the strip is fixed-height that the fader is what absorbs a resize.
// Below this the fader stops being usable as a fader, so the strip clips
// instead of shrinking it further.
constexpr float kMinFaderHeight = 60.0f;

void verticalSpace(float height) {
    ImGui::Dummy(ImVec2(0.0f, height));
}

void drawSummaryRow(const char* id, const std::string& text, float width,
                    const char* tooltip = nullptr) {
    const auto& pal = theme::palette();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, kControlHeight));
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(pos.x, pos.y + std::max(0.0f, (kControlHeight - size.y) * 0.5f)),
        C(pal.textMuted), text.c_str());
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
}

std::string compactRouteLabel(const document::Edit& edit,
                              const document::RouteTarget& target,
                              int playbackChannels) {
    if (target.kind != document::RouteTarget::Kind::HardwareOutput) {
        return "OUT " + routeTargetLabel(edit, target, playbackChannels);
    }

    const int first = target.hardware.firstChannel;
    const int count = target.hardware.channelCount;
    std::string label = "OUT " + std::to_string(first + 1);
    if (count == 2) label += "-" + std::to_string(first + 2);
    if (first < 0 || count < 1 || first + count > playbackChannels) {
        label += " !";
    }
    return label;
}

// One track's mixer-visible state. Pointers rather than a copy because a strip
// edits in place, and a reference-holding struct is what lets audio and MIDI
// tracks — identical from the mixer's point of view except for the instrument
// row — share one implementation.
struct StripModel {
    std::string trackId;
    std::string* name = nullptr;
    std::string* color = nullptr;
    double* gain = nullptr;
    double* pan = nullptr;
    bool* mute = nullptr;
    bool* solo = nullptr;
    // Audio only. MIDI has no record-arm state in this milestone and passes
    // null so its existing two-control layout remains unchanged.
    bool* recordArm = nullptr;
    document::HardwareChannelSpan* hardwareInput = nullptr;
    bool* inputMonitor = nullptr;
    document::RouteTarget* mainOutput = nullptr;
    std::vector<document::AuxSend>* sends = nullptr;
    std::vector<document::PluginSlot>* inserts = nullptr;
    // Null for an audio track. Non-null (though possibly empty) for MIDI.
    document::PluginSlot* instrument = nullptr;
    bool isMidi = false;
    bool isBus = false;
    bool isMain = false;
    engine::GainNode* meter = nullptr;
};

// What a strip decided to do, applied by the caller once the loop over strips
// has finished. Mutating the track vectors mid-loop would invalidate the very
// references the remaining strips are drawn from.
struct StripAction {
    enum class Kind { None, AddInsert, RemoveInsert, ClearInstrument };
    Kind kind = Kind::None;
    std::string trackId;
    std::string slotId;
    bool isMidi = false;
};

bool drawPanKnob(const char* id, float& value) {
    const auto& pal = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 knobSize(kPanKnobDiameter, kPanKnobDiameter);
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, knobSize, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    bool changed = false;

    if (active) {
        const ImGuiIO& io = ImGui::GetIO();
        const float sensitivity = io.KeyShift ? 0.0025f : 0.01f;
        const float delta = io.MouseDelta.x - io.MouseDelta.y;
        if (delta != 0.0f) {
            const float next = std::clamp(value + delta * sensitivity,
                                          -1.0f, 1.0f);
            if (next != value) {
                value = next;
                changed = true;
            }
        }
    }
    if ((ImGui::IsItemClicked() && ImGui::GetIO().KeyAlt) ||
        (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
        if (value != 0.0f) {
            value = 0.0f;
            changed = true;
        }
    }
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        const float step = ImGui::GetIO().KeyShift ? 0.01f : 0.05f;
        const float next = std::clamp(
            value + ImGui::GetIO().MouseWheel * step, -1.0f, 1.0f);
        if (next != value) {
            value = next;
            changed = true;
        }
    }

    constexpr float pi = 3.14159265358979323846f;
    constexpr float startAngle = pi * 0.75f;
    constexpr float endAngle = pi * 2.25f;
    constexpr float centerAngle = pi * 1.5f;
    const float angle = startAngle + (value + 1.0f) * 0.5f *
                                         (endAngle - startAngle);
    const ImVec2 center(pos.x + kPanKnobDiameter * 0.5f,
                        pos.y + kPanKnobDiameter * 0.5f);
    const float radius = kPanKnobDiameter * 0.5f - 2.0f;

    dl->AddCircleFilled(center, radius - 2.0f,
                        C(active ? pal.surfaceStrong
                                 : (hovered ? pal.trackControlInactive
                                            : pal.surfaceBase)),
                        32);
    dl->PathArcTo(center, radius, startAngle, endAngle, 28);
    dl->PathStroke(C(active ? pal.textMuted : pal.borderStrong), false, 2.0f);
    if (value < 0.0f) {
        dl->PathArcTo(center, radius, angle, centerAngle, 16);
        dl->PathStroke(C(pal.accent), false, 2.0f);
    } else if (value > 0.0f) {
        dl->PathArcTo(center, radius, centerAngle, angle, 16);
        dl->PathStroke(C(pal.accent), false, 2.0f);
    }
    for (const float tickAngle : {startAngle, centerAngle, endAngle}) {
        const ImVec2 outer(center.x + std::cos(tickAngle) * (radius + 1.0f),
                           center.y + std::sin(tickAngle) * (radius + 1.0f));
        const ImVec2 inner(center.x + std::cos(tickAngle) * (radius - 2.0f),
                           center.y + std::sin(tickAngle) * (radius - 2.0f));
        dl->AddLine(inner, outer, C(pal.textSubtle), 1.0f);
    }
    const ImVec2 needleEnd(
        center.x + std::cos(angle) * (radius - 5.0f),
        center.y + std::sin(angle) * (radius - 5.0f));
    dl->AddLine(center, needleEnd, C(pal.accentStrong), 2.0f);
    dl->AddCircleFilled(center, 2.0f, C(pal.accentStrong), 12);

    if (hovered) {
        ImGui::SetTooltip("Pan %s\nDrag up/right or down/left\nShift for fine adjustment\nOption/Alt-click or double-click to center",
                          theme::formatPan(value).c_str());
    }
    return changed;
}

float amplitudeToMeterY(float amplitude, float top, float bottom) {
    const float db = 20.0f * std::log10(std::max(amplitude, 0.001f));
    const float normalized = std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);
    return bottom - normalized * (bottom - top);
}

void drawStereoMeter(engine::GainNode* node, ImVec2 pos, float height) {
    constexpr float channelWidth = 7.0f;
    constexpr float channelGap = 3.0f;
    constexpr float meterWidth = channelWidth * 2.0f + channelGap;
    const auto& pal = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 afterFader = ImGui::GetCursorScreenPos();

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##trackMeter", ImVec2(meterWidth, height));
    const bool hovered = ImGui::IsItemHovered();
    if (node != nullptr && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        node->clearMeterClips();
    }

    const float top = pos.y;
    const float bottom = pos.y + height;
    const float warningY = amplitudeToMeterY(
        std::pow(10.0f, -12.0f / 20.0f), top, bottom);
    const float dangerY = amplitudeToMeterY(
        std::pow(10.0f, -3.0f / 20.0f), top, bottom);
    std::array<engine::GainNode::MeterSnapshot, 2> snapshots{};
    if (node != nullptr) {
        snapshots[0] = node->meter(0);
        snapshots[1] = node->meter(1);
    }

    for (size_t channel = 0; channel < snapshots.size(); ++channel) {
        const float left = pos.x + static_cast<float>(channel) *
            (channelWidth + channelGap);
        const float right = left + channelWidth;
        dl->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom),
                          C(pal.surfaceBase), 1.5f);
        dl->AddRect(ImVec2(left, top), ImVec2(right, bottom),
                    C(pal.border), 1.5f);

        const float rmsY = amplitudeToMeterY(
            std::clamp(snapshots[channel].rms, 0.0f, 2.0f), top, bottom);
        if (rmsY < bottom) {
            dl->AddRectFilled(ImVec2(left + 1.0f, std::max(rmsY, warningY)),
                              ImVec2(right - 1.0f, bottom - 1.0f),
                              C(pal.success), 1.0f);
            if (rmsY < warningY) {
                dl->AddRectFilled(
                    ImVec2(left + 1.0f, std::max(rmsY, dangerY)),
                    ImVec2(right - 1.0f, warningY), C(pal.warning));
            }
            if (rmsY < dangerY) {
                dl->AddRectFilled(ImVec2(left + 1.0f, rmsY),
                                  ImVec2(right - 1.0f, dangerY), C(pal.danger));
            }
        }

        const float peakY = amplitudeToMeterY(
            std::clamp(snapshots[channel].peak, 0.0f, 2.0f), top, bottom);
        const ImU32 peakColor = snapshots[channel].peak >= 1.0f
            ? C(pal.danger)
            : (snapshots[channel].peak >= 0.25f
                   ? C(pal.warning) : C(pal.accentStrong));
        dl->AddLine(ImVec2(left + 1.0f, peakY),
                    ImVec2(right - 1.0f, peakY), peakColor, 1.0f);
        if (snapshots[channel].clipped) {
            dl->AddRectFilled(ImVec2(left, top),
                              ImVec2(right, top + 3.0f), C(pal.danger), 1.0f);
        }
    }

    if (hovered) {
        const auto db = [](float value) {
            return value > 0.0f
                ? 20.0f * std::log10(value)
                : -60.0f;
        };
        ImGui::SetTooltip("Post-fader meter\nL %+.1f dB  R %+.1f dB\nClick to clear clips",
                          db(snapshots[0].peak), db(snapshots[1].peak));
    }
    ImGui::SetCursorScreenPos(afterFader);
}

// A compact slot button: bypass dot on the left, name filling the rest.
// Left-click opens the plugin's editor, right-click opens a menu. Returns true
// if the row wants to be removed.
bool drawSlotRow(int uid, const char* label, document::PluginSlot& slot,
                 document::Edit& edit, TimelineViewState& view, float width,
                 bool isInstrument) {
    const auto& pal = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(uid);

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, kInsertRowHeight);
    const bool pressed = ImGui::InvisibleButton("##slot", size);
    const bool hovered = ImGui::IsItemHovered();

    if (pressed) view.requestPluginEditorSlotId = slot.id;

    bool remove = false;
    if (ImGui::BeginPopupContextItem("##slotmenu")) {
        if (ImGui::MenuItem("Open Editor")) view.requestPluginEditorSlotId = slot.id;
        if (ImGui::MenuItem("Bypass", nullptr, slot.bypass)) {
            slot.bypass = !slot.bypass;
            edit.notifyChanged();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(isInstrument ? "Clear Instrument" : "Remove Insert")) {
            remove = true;
        }
        ImGui::EndPopup();
    }

    const ImVec2 bMin = pos;
    const ImVec2 bMax(pos.x + size.x, pos.y + size.y);
    const ImVec4 accentColor = isInstrument ? pal.clipMidiBorder : pal.accent;
    dl->AddRectFilled(bMin, bMax,
                      hovered ? C(pal.surfaceStrong) : C(pal.trackControlInactive),
                      2.0f);
    dl->AddRect(bMin, bMax, C(slot.bypass ? pal.border : accentColor), 2.0f);

    // A bypassed insert still occupies its place in the chain — it has to stay
    // visible and in order, or bypassing would look like removing.
    const float dotR = 3.0f;
    const ImVec2 dotC(bMin.x + 7.0f, (bMin.y + bMax.y) * 0.5f);
    dl->AddCircleFilled(dotC, dotR,
                        C(slot.bypass ? pal.textSubtle : pal.success));

    dl->PushClipRect(ImVec2(bMin.x + 13.0f, bMin.y),
                     ImVec2(bMax.x - 3.0f, bMax.y), true);
    dl->AddText(ImVec2(bMin.x + 14.0f, bMin.y + 1.0f),
                C(slot.bypass ? pal.textSubtle : pal.text),
                slot.name.empty() ? label : slot.name.c_str());
    dl->PopClipRect();

    if (hovered) {
        ImGui::SetTooltip("%s%s\nClick to open, right-click for options",
                          slot.name.c_str(), slot.bypass ? " (bypassed)" : "");
    }
    ImGui::PopID();
    return remove;
}

// An empty row that adds the next insert. Deliberately at the END of the
// chain rather than a fixed grid of empty slots: the chain is a list that
// grows, and a "+" that always sits after the last insert says where the new
// plugin lands in the signal path.
bool drawAddInsertRow(int uid, float width) {
    const auto& pal = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(uid);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed =
        ImGui::InvisibleButton("##addinsert", ImVec2(width, kInsertRowHeight));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 bMin = pos;
    const ImVec2 bMax(pos.x + width, pos.y + kInsertRowHeight);
    if (hovered) dl->AddRectFilled(bMin, bMax, C(pal.surfaceStrong), 2.0f);
    // Dashed, like the timeline's "+ Add track": an affordance, not a control.
    constexpr float dash = 4.0f;
    constexpr float gap = 3.0f;
    const ImU32 col = C(hovered ? pal.accent : pal.borderStrong);
    for (float x = bMin.x; x < bMax.x; x += dash + gap) {
        const float x2 = std::min(x + dash, bMax.x);
        dl->AddLine(ImVec2(x, bMin.y), ImVec2(x2, bMin.y), col);
        dl->AddLine(ImVec2(x, bMax.y), ImVec2(x2, bMax.y), col);
    }
    const ImVec2 ts = ImGui::CalcTextSize("+");
    dl->AddText(ImVec2((bMin.x + bMax.x - ts.x) * 0.5f, bMin.y + 1.0f),
                C(hovered ? pal.accentStrong : pal.textMuted), "+");
    if (hovered) ImGui::SetTooltip("Add an insert to the end of the chain");
    ImGui::PopID();
    return pressed;
}

void drawRouteChoices(const StripModel& model, const document::Edit& edit,
                      TimelineViewState& view, int playbackChannels) {
    const auto options = routingTargetOptions(edit, model.trackId,
                                              playbackChannels, false);
    RoutingTargetOption::Group previous = RoutingTargetOption::Group::HardwareOutputs;
    bool first = true;
    for (const auto& option : options) {
        if (first || option.group != previous) {
            if (!first) ImGui::Separator();
            const char* label = option.group == RoutingTargetOption::Group::MainAndBuses
                ? "Main / Buses"
                : option.group == RoutingTargetOption::Group::AudioTracks
                    ? "Audio Tracks" : "Hardware Outputs";
            ImGui::TextDisabled("%s", label);
            previous = option.group;
            first = false;
        }
        const bool selected = model.mainOutput && *model.mainOutput == option.target;
        if (ImGui::MenuItem(option.label.c_str(), nullptr, selected,
                            option.enabled)) {
            view.routing.request({RoutingRequest::Kind::SetMainOutput,
                                  model.trackId, {}, false, option.target, {}});
        }
        if (!option.enabled &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", option.disabledReason.c_str());
        }
    }
}

StripAction drawStrip(const StripModel& m, document::Edit& edit,
                      TimelineViewState& view, int uid, bool selected,
                      bool anySoloed, float stripWidth, float stripHeight,
                      int captureChannels, int playbackChannels) {
    const auto& pal = theme::palette();
    StripAction action;
    ImGui::PushID(uid);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
    ImGui::BeginChild("##strip", ImVec2(stripWidth, stripHeight), true,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    // A strip stacks a dozen items, and the theme's default vertical item
    // spacing adds up to more than the fader's whole height budget — which is
    // what pushed the fader off the bottom of the strip. Sections are spaced
    // deliberately below instead.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kSpaceSm, 0.0f));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 stripMin = ImGui::GetWindowPos();
    const ImVec2 stripMax(stripMin.x + ImGui::GetWindowWidth(),
                          stripMin.y + ImGui::GetWindowHeight());
    if (selected) {
        dl->AddRectFilled(stripMin, stripMax,
                          IM_COL32(int(pal.accent.x * 255), int(pal.accent.y * 255),
                                   int(pal.accent.z * 255), 20));
    }

    const float inner = ImGui::GetContentRegionAvail().x;

    // ─── Name ───────────────────────────────────────────────────────────────
    // The colour bar reuses the timeline's identity colours so a strip and its
    // row are recognisably the same track.
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec4 defaultColor = m.isMain
            ? pal.accentStrong
            : (m.isBus ? pal.success
                       : (m.isMidi ? pal.clipMidiBorder : pal.clipAudioBorder));
        const ImVec4 identityColor =
            trackColorValue(m.color ? *m.color : std::string{}, defaultColor);
        if (ImGui::InvisibleButton("##color", ImVec2(inner, 7.0f))) {
            ImGui::OpenPopup("##trackColor");
        }
        const bool hovered = ImGui::IsItemHovered();
        dl->AddRectFilled(ImVec2(pos.x, pos.y + 2.0f),
                          ImVec2(pos.x + inner, pos.y + 5.0f),
                          C(identityColor));
        if (hovered) ImGui::SetTooltip("Choose track color");
        std::string selectedColor;
        if (drawTrackColorPopup("##trackColor",
                                m.color ? *m.color : std::string{},
                                selectedColor)) {
            view.requestTrackColorId = m.trackId;
            view.requestTrackColor = std::move(selectedColor);
        }
    }
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##name", ImVec2(inner, 16.0f))) {
            view.selectedTrackIndex = uid;
        }
        dl->PushClipRect(pos, ImVec2(pos.x + inner, pos.y + 16.0f), true);
        dl->AddText(pos, C(selected ? pal.accentStrong : pal.text),
                    m.name->c_str());
        dl->PopClipRect();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.name->c_str());
    }
    verticalSpace(kSpaceSm);

    // ─── Compact routing ───────────────────────────────────────────────────
    // Every strip owns the same three-row routing zone. Audio uses row one for
    // hardware input, MIDI for its instrument, and buses label their mix input.
    // That keeps output, sends, inserts, pan and faders on shared baselines.
    if (m.hardwareInput != nullptr) {
        const std::string inputLabel = m.hardwareInput->channelCount == 2
            ? "IN " + std::to_string(m.hardwareInput->firstChannel + 1) + "-" +
                  std::to_string(m.hardwareInput->firstChannel + 2)
            : "IN " + std::to_string(m.hardwareInput->firstChannel + 1);
        const float monitorWidth = 22.0f;
        if (ImGui::Button(inputLabel.c_str(),
                          ImVec2(inner - monitorWidth - kSpaceSm,
                                 kControlHeight))) {
            ImGui::OpenPopup("##inputRoute");
        }
        ImGui::SameLine(0.0f, kSpaceSm);
        const bool monitoring = m.inputMonitor && *m.inputMonitor;
        if (monitoring) ImGui::PushStyleColor(ImGuiCol_Button, pal.success);
        if (ImGui::Button("I", ImVec2(monitorWidth, kControlHeight)) &&
            m.inputMonitor) {
            RoutingRequest request;
            request.kind = RoutingRequest::Kind::SetInputMonitor;
            request.ownerId = m.trackId;
            request.enabled = !*m.inputMonitor;
            view.routing.request(std::move(request));
        }
        if (monitoring) ImGui::PopStyleColor();
        if (ImGui::BeginPopup("##inputRoute")) {
            for (int channel = 0; channel < captureChannels; ++channel) {
                const std::string label = "Input " + std::to_string(channel + 1);
                if (ImGui::MenuItem(label.c_str())) {
                    RoutingRequest request;
                    request.kind = RoutingRequest::Kind::SetHardwareInput;
                    request.ownerId = m.trackId;
                    request.hardware = {channel, 1};
                    view.routing.request(std::move(request));
                }
            }
            for (int channel = 0; channel + 1 < captureChannels; channel += 2) {
                const std::string label = "Input " + std::to_string(channel + 1) +
                    "-" + std::to_string(channel + 2);
                if (ImGui::MenuItem(label.c_str())) {
                    RoutingRequest request;
                    request.kind = RoutingRequest::Kind::SetHardwareInput;
                    request.ownerId = m.trackId;
                    request.hardware = {channel, 2};
                    view.routing.request(std::move(request));
                }
            }
            if (captureChannels == 0) ImGui::TextDisabled("No capture channels");
            ImGui::EndPopup();
        }
    } else if (m.instrument != nullptr) {
        if (m.instrument->uidString.empty()) {
            if (drawAddInsertRow(1, inner)) {
                view.requestPicker =
                    TimelineViewState::PluginPicker::MidiInstrument;
                view.requestPickerTrackId = m.trackId;
            }
        } else if (drawSlotRow(2, "Instrument", *m.instrument, edit, view,
                               inner, true)) {
            action = StripAction{StripAction::Kind::ClearInstrument, m.trackId,
                                 m.instrument->id, true};
        }
    } else {
        drawSummaryRow("##mixInput", "IN MIX", inner,
                       "Incoming track and bus routes");
    }
    verticalSpace(kSpaceXs);

    if (m.mainOutput != nullptr) {
        const std::string route = routeTargetLabel(edit, *m.mainOutput,
                                                   playbackChannels);
        const std::string compact = compactRouteLabel(
            edit, *m.mainOutput, playbackChannels);
        if (ImGui::Button(compact.c_str(), ImVec2(inner, kControlHeight))) {
            ImGui::OpenPopup("##mainRoute");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output: %s", route.c_str());
        if (ImGui::BeginPopup("##mainRoute")) {
            drawRouteChoices(m, edit, view, playbackChannels);
            ImGui::EndPopup();
        }
    } else {
        ImGui::Dummy(ImVec2(inner, kControlHeight));
    }
    verticalSpace(kSpaceXs);

    if (m.sends != nullptr) {
        int active = 0;
        for (const auto& send : *m.sends) if (!send.muted && send.gain > 0.0) ++active;
        const std::string summary = "SENDS " + std::to_string(active) + "/" +
            std::to_string(m.sends->size());
        drawSummaryRow("##sendSummary", summary, inner,
                       "Edit sends in the selected channel's Routing inspector");
    } else {
        ImGui::Dummy(ImVec2(inner, kControlHeight));
    }
    verticalSpace(kSpaceMd);

    // ─── Inserts ────────────────────────────────────────────────────────────
    ImGui::TextDisabled("INSERTS");
    verticalSpace(kSpaceXs);
    int insertIdx = 0;
    for (auto& slot : *m.inserts) {
        char label[16];
        std::snprintf(label, sizeof(label), "Insert %d", insertIdx + 1);
        if (drawSlotRow(100 + insertIdx, label, slot, edit, view, inner, false)) {
            action = StripAction{StripAction::Kind::RemoveInsert, m.trackId,
                                 slot.id, m.isMidi};
        }
        verticalSpace(kSpaceXs);
        ++insertIdx;
    }
    if (drawAddInsertRow(3, inner)) {
        action = StripAction{StripAction::Kind::AddInsert, m.trackId, "",
                             m.isMidi};
    }

    // ─── Record arm / mute / solo ───────────────────────────────────────────
    verticalSpace(kSpaceMd);
    {
        const int toggleCount = m.recordArm != nullptr ? 3 : 2;
        const float buttonWidth =
            (inner - kSpaceSm * static_cast<float>(toggleCount - 1)) /
            static_cast<float>(toggleCount);
        struct Toggle {
            const char* label;
            bool* flag;
            ImVec4 active;
            const char* tooltip;
        };
        Toggle toggles[3];
        int nextToggle = 0;
        if (m.recordArm != nullptr) {
            toggles[nextToggle++] =
                Toggle{"R", m.recordArm, pal.danger, "Record arm"};
        }
        toggles[nextToggle++] =
            Toggle{"M", m.mute, pal.trackMuteActive, "Mute"};
        toggles[nextToggle++] =
            Toggle{"S", m.solo, pal.trackSoloActive, "Solo"};
        for (int b = 0; b < toggleCount; ++b) {
            ImGui::PushID(b);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton(
                    "##rms", ImVec2(buttonWidth, kControlHeight))) {
                if (b == 0 && m.recordArm != nullptr &&
                    view.deferRecordArmRequests) {
                    view.requestRecordArmTrackId = m.trackId;
                } else {
                    *toggles[b].flag = !*toggles[b].flag;
                    edit.notifyChanged();
                }
            }
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 bMin = pos;
            const ImVec2 bMax(pos.x + buttonWidth,
                              pos.y + kControlHeight);
            const bool on = *toggles[b].flag;
            const bool isRecordArm = b == 0 && m.recordArm != nullptr;
            if (isRecordArm) {
                theme::drawRecordArmIndicator(
                    dl, ImVec2((bMin.x + bMax.x) * 0.5f,
                               (bMin.y + bMax.y) * 0.5f),
                    6.5f, on, hovered);
            } else {
                dl->AddRectFilled(
                    bMin, bMax,
                    on ? C(toggles[b].active)
                       : (hovered ? C(pal.surfaceStrong)
                                  : C(pal.trackControlInactive)),
                    3.0f);
                dl->AddRect(bMin, bMax,
                            on ? C(toggles[b].active) : C(pal.border), 3.0f);
                theme::drawCenteredControlLabel(
                    dl, theme::Rect{bMin, bMax},
                    on ? IM_COL32(32, 30, 28, 255) : C(pal.textMuted),
                    toggles[b].label);
            }
            if (hovered) ImGui::SetTooltip("%s", toggles[b].tooltip);
            ImGui::PopID();
            if (b + 1 < toggleCount) ImGui::SameLine(0.0f, kSpaceSm);
        }
    }

    // Someone else's solo is silencing this strip. Same cue as the timeline
    // gutter, for the same reason: an unexplained silent track is the hardest
    // kind of problem to hear your way out of.
    if (!*m.mute && !*m.solo && anySoloed) {
        dl->AddRectFilled(stripMin, stripMax, IM_COL32(20, 19, 18, 110));
    }

    // ─── Pan ────────────────────────────────────────────────────────────────
    verticalSpace(kSpaceSm);
    {
        float panVal = static_cast<float>(*m.pan);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (inner - kPanKnobDiameter) * 0.5f);
        if (drawPanKnob("##pan", panVal)) {
            *m.pan = panVal;
            edit.notifyChanged();
        }
        const std::string panText = theme::formatPan(panVal);
        const ImVec2 textSize = ImGui::CalcTextSize(panText.c_str());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, (inner - textSize.x) * 0.5f));
        ImGui::TextUnformatted(panText.c_str());
    }
    verticalSpace(kSpaceSm);

    // ─── Fader ──────────────────────────────────────────────────────────────
    // Vertical, because that is what a fader is, and it is what absorbs the
    // panel's spare height. The dB readout underneath is reserved for FIRST:
    // taking the leftovers meant a short panel pushed the readout past the
    // strip's bottom edge and clipped it away.
    {
        const float readoutH = ImGui::GetTextLineHeightWithSpacing();
        const float remaining = ImGui::GetContentRegionAvail().y - readoutH;
        // When the panel really is too short, the fader shrinks rather than
        // overflowing — a stubby fader is usable, a clipped one is not.
        const float faderH = std::max(kMinFaderHeight * 0.4f,
                                      std::min(remaining, kMinFaderHeight * 4.0f));
        float gainDb =
            20.0f * std::log10(std::max(0.0001f, static_cast<float>(*m.gain)));
        constexpr float faderW = 24.0f;
        constexpr float meterW = 17.0f;
        constexpr float faderMeterGap = 8.0f;
        const float groupW = faderW + faderMeterGap + meterW;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, (inner - groupW) * 0.5f));
        const ImVec2 faderPos = ImGui::GetCursorScreenPos();
        const bool faderDragged = ImGui::VSliderFloat(
            "##fader", ImVec2(faderW, faderH), &gainDb,
            -60.0f, 6.0f, "");
        const bool faderReset =
            ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
            ImGui::GetIO().KeyAlt;
        if (faderReset) gainDb = 0.0f;
        const double nextGain = std::pow(10.0f, gainDb / 20.0f);
        if ((faderDragged || faderReset) && *m.gain != nextGain) {
            *m.gain = nextGain;
            edit.notifyChanged();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Fader (Option/Alt-click for 0 dB)");
        }
        drawStereoMeter(m.meter,
                        ImVec2(faderPos.x + faderW + faderMeterGap,
                               faderPos.y),
                        faderH);
        char dbText[16];
        std::snprintf(dbText, sizeof(dbText), "%+.1f", gainDb);
        const ImVec2 ts = ImGui::CalcTextSize(dbText);
        ImGui::SetCursorPosX(std::max(0.0f, (inner - ts.x) * 0.5f));
        ImGui::TextUnformatted(dbText);
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopID();
    return action;
}

} // namespace

void drawMixer(document::Edit& edit, editing::UndoStack& undo,
               TimelineViewState& view, float stripWidth,
               int captureChannels, int playbackChannels,
               const TrackGainNodes* gainNodes) {
    const bool anySoloed = edit.anySoloed();

    if (edit.tracks().empty() && edit.midiTracks().empty() &&
        edit.buses().empty()) {
        const auto& pal = theme::palette();
        const char* msg = "No tracks yet";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - ts.x) * 0.5f,
                                   ImGui::GetCursorPosY() + (avail.y - ts.y) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, pal.textSubtle);
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
        return;
    }

    ImGui::BeginChild("##mixerScroll", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    // The horizontal scrollbar sits inside this child and eats from the bottom.
    // Sizing strips to the full available height instead pushed each strip's
    // last widget — the fader — under the scrollbar and out of view.
    const float stripHeight = std::max(
        120.0f, ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ScrollbarSize);

    StripAction action;
    int uid = 0;
    const auto meterFor = [&](const std::string& id) -> engine::GainNode* {
        if (gainNodes == nullptr) return nullptr;
        const auto found = gainNodes->find(id);
        return found == gainNodes->end() ? nullptr : found->second.get();
    };

    // Audio strips, then MIDI, matching the timeline's row order — the mixer
    // and the timeline have to agree on what "the third track" means, since
    // selectedTrackIndex is shared between them.
    for (auto& t : edit.tracksMut()) {
        StripModel m;
        m.trackId = t.id;
        m.name = &t.name;
        m.color = &t.color;
        m.gain = &t.gain;
        m.pan = &t.pan;
        m.mute = &t.mute;
        m.solo = &t.solo;
        m.recordArm = &t.recordArm;
        m.hardwareInput = &t.hardwareInput;
        m.inputMonitor = &t.inputMonitor;
        m.mainOutput = &t.mainOutput;
        m.sends = &t.sends;
        m.inserts = &t.plugins;
        m.meter = meterFor(t.id);
        const StripAction a = drawStrip(m, edit, view, uid,
                                        view.selectedTrackIndex == uid,
                                        anySoloed, stripWidth, stripHeight,
                                        captureChannels, playbackChannels);
        if (a.kind != StripAction::Kind::None) action = a;
        ImGui::SameLine(0.0f, kSpaceSm);
        ++uid;
    }
    for (auto& t : edit.midiTracksMut()) {
        StripModel m;
        m.trackId = t.id;
        m.name = &t.name;
        m.color = &t.color;
        m.gain = &t.gain;
        m.pan = &t.pan;
        m.mute = &t.mute;
        m.solo = &t.solo;
        m.mainOutput = &t.mainOutput;
        m.sends = &t.sends;
        m.inserts = &t.plugins;
        m.instrument = &t.instrument;
        m.isMidi = true;
        m.meter = meterFor(t.id);
        const StripAction a = drawStrip(m, edit, view, uid,
                                        view.selectedTrackIndex == uid,
                                        anySoloed, stripWidth, stripHeight,
                                        captureChannels, playbackChannels);
        if (a.kind != StripAction::Kind::None) action = a;
        ImGui::SameLine(0.0f, kSpaceSm);
        ++uid;
    }
    for (auto& bus : edit.busesMut()) {
        StripModel m;
        m.trackId = bus.id;
        m.name = &bus.name;
        m.color = &bus.color;
        m.gain = &bus.gain;
        m.pan = &bus.pan;
        m.mute = &bus.mute;
        m.solo = &bus.solo;
        m.mainOutput = &bus.mainOutput;
        m.sends = &bus.sends;
        m.inserts = &bus.plugins;
        m.isBus = true;
        m.isMain = bus.isMain;
        m.meter = meterFor(bus.id);
        const StripAction a = drawStrip(m, edit, view, uid,
                                        view.selectedTrackIndex == uid,
                                        anySoloed, stripWidth, stripHeight,
                                        captureChannels, playbackChannels);
        if (a.kind != StripAction::Kind::None) action = a;
        ImGui::SameLine(0.0f, kSpaceSm);
        ++uid;
    }
    ImGui::EndChild();
    if (ImGui::BeginPopupContextWindow("##mixerChannelMenu",
                                       ImGuiPopupFlags_MouseButtonRight |
                                       ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Add Audio Track", "Shift+Cmd+N")) {
            undo.execute(std::make_unique<editing::AddTrackCommand>("Track"));
        }
        if (ImGui::MenuItem("Add Bus", "Shift+Cmd+B")) {
            undo.execute(std::make_unique<editing::AddBusCommand>("Bus"));
        }
        ImGui::EndPopup();
    }

    // Applied after the loop: every one of these reallocates a track vector or
    // a plugin vector that the strips above were drawn from.
    switch (action.kind) {
        case StripAction::Kind::None:
            break;
        case StripAction::Kind::AddInsert:
            view.requestPicker = action.isMidi
                ? TimelineViewState::PluginPicker::MidiFx
                : TimelineViewState::PluginPicker::AudioFx;
            view.requestPickerTrackId = action.trackId;
            break;
        case StripAction::Kind::RemoveInsert:
            if (action.isMidi) {
                undo.execute(std::make_unique<editing::RemoveMidiPluginCommand>(
                    action.trackId, action.slotId));
            } else {
                undo.execute(std::make_unique<editing::RemovePluginCommand>(
                    action.trackId, action.slotId));
            }
            break;
        case StripAction::Kind::ClearInstrument:
            undo.execute(std::make_unique<editing::SetMidiInstrumentCommand>(
                action.trackId, document::PluginSlot{}));
            break;
    }
    if (!view.requestTrackColorId.empty()) {
        undo.execute(std::make_unique<editing::SetTrackColorCommand>(
            view.requestTrackColorId, view.requestTrackColor));
        view.requestTrackColorId.clear();
        view.requestTrackColor.clear();
    }
}

} // namespace dave::gui
