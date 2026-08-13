// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/IoPanel.h"

#include "gui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>

namespace dave::gui {

namespace {

ImU32 color(const ImVec4& value) {
    return ImGui::GetColorU32(value);
}

const IoDevice* findDevice(const std::vector<IoDevice>& catalog,
                           const std::string& id) {
    const auto found = std::find_if(
        catalog.begin(), catalog.end(),
        [&](const IoDevice& device) { return device.id == id; });
    return found == catalog.end() ? nullptr : &*found;
}

void drawDevicePicker(const char* id, const char* label,
                      const std::vector<IoDevice>& catalog,
                      const std::optional<IoDeviceSelection>& selected,
                      bool selectedAvailable,
                      const std::string& selectedLabel,
                      const std::function<void(const std::string&)>& request) {
    const auto& palette = theme::palette();
    // Longest label is OUTPUT. Leave enough room at every font/DPI scale so
    // the combo never paints over the final letter.
    constexpr float kLabelWidth = 62.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(-1.0f);

    if (!selectedAvailable && selected.has_value()) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.warning);
    }
    const bool open = ImGui::BeginCombo(id, selectedLabel.c_str());
    if (!selectedAvailable && selected.has_value()) {
        ImGui::PopStyleColor();
    }

    if (open) {
        if (!selectedAvailable && selected.has_value()) {
            const std::string missing = selectedLabel + " (unavailable)";
            ImGui::BeginDisabled();
            ImGui::Selectable(missing.c_str(), true);
            ImGui::EndDisabled();
            if (!catalog.empty()) ImGui::Separator();
        }

        if (catalog.empty()) {
            ImGui::BeginDisabled();
            ImGui::Selectable("No devices found", false);
            ImGui::EndDisabled();
        } else {
            for (const auto& device : catalog) {
                ImGui::PushID(device.id.c_str());
                std::string itemLabel = device.name.empty()
                    ? "Unnamed device"
                    : device.name;
                if (device.isDefault &&
                    device.id != IoPanelState::kInputDefaultId) {
                    itemLabel += " (Default)";
                }
                const bool isSelected =
                    selected.has_value() && selected->id == device.id;
                if (ImGui::Selectable(itemLabel.c_str(), isSelected) &&
                    !isSelected) {
                    request(device.id);
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
}

void drawMeterBank(const char* id, const std::vector<IoInputMeter>& meters) {
    const auto& palette = theme::palette();

    constexpr float kBarHeight = 25.0f;
    constexpr float kLabelHeight = 13.0f;
    constexpr float kBankHeight = kBarHeight + kLabelHeight + 14.0f;
    const float contentWidth = IoPanelState::meterContentWidth(meters.size());
    ImGui::SetNextWindowContentSize(ImVec2(contentWidth, 0.0f));
    ImGui::BeginChild(id, ImVec2(0.0f, kBankHeight), true,
                      ImGuiWindowFlags_HorizontalScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);

    if (meters.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.textSubtle);
        ImGui::TextUnformatted("No channels");
        ImGui::PopStyleColor();
        ImGui::EndChild();
        return;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (size_t channel = 0; channel < meters.size(); ++channel) {
        if (channel > 0) ImGui::SameLine(0.0f, IoPanelState::kMeterChannelGap);
        ImGui::PushID(static_cast<int>(channel));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##meter", ImVec2(IoPanelState::kMeterChannelWidth,
                                                  kBarHeight + kLabelHeight));

        const float peak = std::clamp(meters[channel].peak, 0.0f, 1.0f);
        const float rms = std::clamp(meters[channel].rms, 0.0f, 1.0f);
        const float peakDb = peak > 0.0f ? 20.0f * std::log10(peak) : -60.0f;
        const float rmsDb = rms > 0.0f ? 20.0f * std::log10(rms) : -60.0f;
        const float peakAmount =
            std::clamp((peakDb + 60.0f) / 60.0f, 0.0f, 1.0f);
        const float rmsAmount =
            std::clamp((rmsDb + 60.0f) / 60.0f, 0.0f, 1.0f);
        const ImVec2 barMin(origin.x + 3.0f, origin.y);
        const ImVec2 barMax(origin.x + IoPanelState::kMeterChannelWidth - 3.0f,
                            origin.y + kBarHeight);
        drawList->AddRectFilled(barMin, barMax,
                                color(palette.trackControlInactive), 2.0f);

        const ImVec4& meterColor = meters[channel].clipped
            ? palette.danger
            : (peak >= 0.80f ? palette.warning : palette.success);
        const float fillTop = barMax.y - (barMax.y - barMin.y) * rmsAmount;
        drawList->AddRectFilled(ImVec2(barMin.x, fillTop), barMax,
                                color(meterColor), 2.0f);
        const float peakY = barMax.y - (barMax.y - barMin.y) * peakAmount;
        drawList->AddLine(ImVec2(barMin.x, peakY), ImVec2(barMax.x, peakY),
                          color(meters[channel].clipped
                                    ? palette.danger
                                    : palette.accentStrong),
                          1.5f);
        if (meters[channel].clipped) {
            drawList->AddRectFilled(barMin, ImVec2(barMax.x, barMin.y + 3.0f),
                                    color(palette.danger), 1.0f);
        }
        drawList->AddRect(barMin, barMax, color(palette.borderStrong), 2.0f);

        char channelText[16];
        std::snprintf(channelText, sizeof(channelText), "%zu", channel + 1);
        const ImVec2 textSize = ImGui::CalcTextSize(channelText);
        drawList->AddText(
            ImVec2(origin.x +
                       (IoPanelState::kMeterChannelWidth - textSize.x) * 0.5f,
                   barMax.y + 3.0f),
            color(palette.textMuted), channelText);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

} // namespace

IoPanelState::IoPanelState() {
    setCaptureCatalog({});
}

void IoPanelState::setPlaybackCatalog(std::vector<IoDevice> devices) {
    playback_ = std::move(devices);
}

void IoPanelState::setCaptureCatalog(std::vector<IoDevice> devices) {
    capture_.clear();
    capture_.reserve(devices.size() + 2);
    capture_.push_back(IoDevice{kInputOffId, "Off", 0, false});
    capture_.push_back(IoDevice{kInputDefaultId, "Default", 0, true});
    for (auto& device : devices) {
        if (device.id != kInputOffId && device.id != kInputDefaultId) {
            capture_.push_back(std::move(device));
        }
    }
}

void IoPanelState::setSelectedOutput(
    std::optional<IoDeviceSelection> selection) {
    selectedOutput_ = std::move(selection);
}

void IoPanelState::setSelectedInput(
    std::optional<IoDeviceSelection> selection) {
    selectedInput_ = std::move(selection);
}

bool IoPanelState::catalogContains(
    const std::vector<IoDevice>& catalog,
    const std::optional<IoDeviceSelection>& selected) {
    return selected.has_value() && findDevice(catalog, selected->id) != nullptr;
}

bool IoPanelState::selectedOutputAvailable() const {
    return catalogContains(playback_, selectedOutput_);
}

bool IoPanelState::selectedInputAvailable() const {
    return catalogContains(capture_, selectedInput_);
}

std::string IoPanelState::selectionLabel(
    const std::vector<IoDevice>& catalog,
    const std::optional<IoDeviceSelection>& selected) {
    if (!selected.has_value()) return "No device selected";
    if (const IoDevice* current = findDevice(catalog, selected->id)) {
        return current->name.empty() ? "Unnamed device" : current->name;
    }
    if (!selected->name.empty()) return selected->name;
    return "Unknown device";
}

std::string IoPanelState::selectedOutputLabel() const {
    return selectionLabel(playback_, selectedOutput_);
}

std::string IoPanelState::selectedInputLabel() const {
    return selectionLabel(capture_, selectedInput_);
}

void IoPanelState::setCaptureStatus(std::string statusText,
                                    std::string errorText) {
    captureStatusText_ = std::move(statusText);
    captureErrorText_ = std::move(errorText);
}

void IoPanelState::setMeterSnapshot(IoMeterSnapshot snapshot) {
    meters_ = std::move(snapshot);
}

IoInputMeter IoPanelState::inputMeter(size_t channel) const {
    if (channel >= meters_.inputs.size()) return {};
    IoInputMeter result = meters_.inputs[channel];
    result.peak = std::clamp(result.peak, 0.0f, 1.0f);
    result.rms = std::clamp(result.rms, 0.0f, 1.0f);
    return result;
}

void IoPanelState::requestOutput(const std::string& deviceId) {
    requests_.push_back(
        IoPanelRequest{IoPanelRequest::Kind::SelectOutput, deviceId});
}

void IoPanelState::requestInput(const std::string& deviceId) {
    requests_.push_back(
        IoPanelRequest{IoPanelRequest::Kind::SelectInput, deviceId});
}

void IoPanelState::requestRefresh() {
    requests_.push_back(
        IoPanelRequest{IoPanelRequest::Kind::RefreshDevices, {}});
}

void IoPanelState::requestClearInputClips() {
    requests_.push_back(
        IoPanelRequest{IoPanelRequest::Kind::ClearInputClips, {}});
}

void IoPanelState::requestRecordLatencyOffset(int samples) {
    recordLatencyOffset_ = std::clamp(samples, 0, 1000000);
    requests_.push_back(IoPanelRequest{
        IoPanelRequest::Kind::SetRecordLatencyOffset, {},
        recordLatencyOffset_});
}

std::vector<IoPanelRequest> IoPanelState::takeRequests() {
    std::vector<IoPanelRequest> result = std::move(requests_);
    requests_.clear();
    return result;
}

void IoPanelState::clearRequests() {
    requests_.clear();
}

float IoPanelState::meterContentWidth(size_t channels) {
    if (channels == 0) return 0.0f;
    return static_cast<float>(channels) * kMeterChannelWidth +
           static_cast<float>(channels - 1) * kMeterChannelGap;
}

bool IoPanelState::meterNeedsHorizontalOverflow(size_t channels,
                                                float availableWidth) {
    return meterContentWidth(channels) > std::max(0.0f, availableWidth);
}

void drawIoPanel(IoPanelState& state) {
    const auto& palette = theme::palette();
    theme::panelHeader("I/O");
    ImGui::PushID("io-panel");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5.0f, 3.0f));
    // This panel has a fixed sidebar-height budget. Keep the same control
    // shape and colors as the global theme, but use its compact padding tier.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));

    const float refreshWidth = 68.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Devices");
    ImGui::SameLine();
    ImGui::TextDisabled("Offset");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(64.0f);
    int latencyOffset = state.recordLatencyOffset();
    ImGui::BeginDisabled(state.recordingActive());
    if (ImGui::InputInt("##record-latency", &latencyOffset, 0, 0)) {
        state.requestRecordLatencyOffset(latencyOffset);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Round-trip recording latency in samples. Positive values move "
            "new takes earlier. Record a click through a physical loopback, "
            "measure the delay, and enter that sample count.");
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - refreshWidth);
    if (theme::gradientButton("Refresh", ImVec2(refreshWidth, 0.0f))) {
        state.requestRefresh();
    }

    drawDevicePicker(
        "##output-device", "OUTPUT", state.playbackCatalog(),
        state.selectedOutput(), state.selectedOutputAvailable(),
        state.selectedOutputLabel(),
        [&](const std::string& id) { state.requestOutput(id); });
    drawDevicePicker(
        "##input-device", "INPUT", state.captureCatalog(),
        state.selectedInput(), state.selectedInputAvailable(),
        state.selectedInputLabel(),
        [&](const std::string& id) { state.requestInput(id); });

    if (!state.captureErrorText().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.danger);
        ImGui::TextWrapped("%s", state.captureErrorText().c_str());
        ImGui::PopStyleColor();
    } else if (!state.captureStatusText().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.textSubtle);
        ImGui::TextWrapped("%s", state.captureStatusText().c_str());
        ImGui::PopStyleColor();
    } else if (!state.selectedInputAvailable() &&
               state.selectedInput().has_value()) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.warning);
        ImGui::TextUnformatted("Selected input unavailable");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Input meters");
    constexpr float clearWidth = 76.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - clearWidth);
    if (theme::gradientButton("Clear Clips", ImVec2(clearWidth, 0.0f))) {
        state.requestClearInputClips();
    }
    drawMeterBank("##input-meters", state.meterSnapshot().inputs);

    ImGui::PopStyleVar(2);
    ImGui::PopID();
}

} // namespace dave::gui
