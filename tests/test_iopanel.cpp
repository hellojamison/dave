// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/IoPanel.h"
#include "gui/Theme.h"

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

using dave::gui::IoDevice;
using dave::gui::IoDeviceSelection;
using dave::gui::IoInputMeter;
using dave::gui::IoMeterSnapshot;
using dave::gui::IoPanelRequest;
using dave::gui::IoPanelState;

namespace {

class HeadlessIoPanel {
public:
    HeadlessIoPanel() {
        context_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(context_);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(400.0f, 300.0f);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
        dave::gui::theme::applyTheme();
    }

    ~HeadlessIoPanel() { ImGui::DestroyContext(context_); }

    float drawHeight(IoPanelState& state) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(300.0f, 220.0f));
        ImGui::Begin("I/O host", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        const float start = ImGui::GetCursorPosY();
        dave::gui::drawIoPanel(state);
        const float height = ImGui::GetCursorPosY() - start;
        ImGui::End();
        ImGui::Render();
        return height;
    }

private:
    ImGuiContext* context_ = nullptr;
};

} // namespace

TEST_CASE("I/O catalogs preserve a selected device that becomes unavailable",
          "[iopanel]") {
    IoPanelState state;
    state.setPlaybackCatalog({
        IoDevice{"built-in", "Mac Speakers", 2, true},
        IoDevice{"studio", "Studio Monitor", 8, false},
    });
    state.setSelectedOutput(IoDeviceSelection{"studio", "Studio Monitor"});
    REQUIRE(state.selectedOutputAvailable());
    CHECK(state.selectedOutputLabel() == "Studio Monitor");

    // Unplugging an interface changes the catalog, not the user's remembered
    // choice. Replacing it with the default would hide the actual failure.
    state.setPlaybackCatalog({
        IoDevice{"built-in", "Mac Speakers", 2, true},
    });
    REQUIRE(state.selectedOutput().has_value());
    CHECK(state.selectedOutput()->id == "studio");
    CHECK(state.selectedOutputLabel() == "Studio Monitor");
    CHECK_FALSE(state.selectedOutputAvailable());
}

TEST_CASE("catalog display names refresh without changing stable selection",
          "[iopanel]") {
    IoPanelState state;
    state.setCaptureCatalog({IoDevice{"mic", "Interface Input", 2, false}});
    state.setSelectedInput(IoDeviceSelection{"mic", "Old Name"});
    REQUIRE(state.selectedInputAvailable());
    CHECK(state.selectedInputLabel() == "Interface Input");

    state.setCaptureCatalog({IoDevice{"mic", "Dialogue Rack", 2, false}});
    CHECK(state.selectedInput()->id == "mic");
    CHECK(state.selectedInputLabel() == "Dialogue Rack");
}

TEST_CASE("capture choices always expose Off and Default", "[iopanel]") {
    IoPanelState state;
    REQUIRE(state.captureCatalog().size() == 2);
    CHECK(state.captureCatalog()[0].id == IoPanelState::kInputOffId);
    CHECK(state.captureCatalog()[1].id == IoPanelState::kInputDefaultId);

    state.setCaptureCatalog({IoDevice{"mic", "Dialogue Rack", 8, false}});

    REQUIRE(state.captureCatalog().size() == 3);
    CHECK(state.captureCatalog()[0].id == IoPanelState::kInputOffId);
    CHECK(state.captureCatalog()[0].name == "Off");
    CHECK(state.captureCatalog()[1].id == IoPanelState::kInputDefaultId);
    CHECK(state.captureCatalog()[1].name == "Default");
    CHECK(state.captureCatalog()[2].id == "mic");

    state.setSelectedInput(
        IoDeviceSelection{IoPanelState::kInputOffId, "Off"});
    CHECK(state.selectedInputAvailable());
}

TEST_CASE("I/O actions queue until application code takes or clears them",
          "[iopanel]") {
    IoPanelState state;
    state.requestOutput("speaker-b");
    state.requestInput("mic-a");
    state.requestRefresh();
    state.requestClearInputClips();

    REQUIRE(state.pendingRequests().size() == 4);
    CHECK(state.pendingRequests()[0].kind ==
          IoPanelRequest::Kind::SelectOutput);
    CHECK(state.pendingRequests()[0].deviceId == "speaker-b");
    CHECK(state.pendingRequests()[1].kind ==
          IoPanelRequest::Kind::SelectInput);
    CHECK(state.pendingRequests()[1].deviceId == "mic-a");
    CHECK(state.pendingRequests()[2].kind ==
          IoPanelRequest::Kind::RefreshDevices);
    CHECK(state.pendingRequests()[3].kind ==
          IoPanelRequest::Kind::ClearInputClips);

    const std::vector<IoPanelRequest> taken = state.takeRequests();
    CHECK(taken.size() == 4);
    CHECK(state.pendingRequests().empty());

    state.requestRefresh();
    REQUIRE_FALSE(state.pendingRequests().empty());
    state.clearRequests();
    CHECK(state.pendingRequests().empty());
}

TEST_CASE("record latency offset is bounded and queued for application code",
          "[iopanel][recording]") {
    IoPanelState state;
    state.requestRecordLatencyOffset(384);
    REQUIRE(state.recordLatencyOffset() == 384);
    REQUIRE(state.pendingRequests().size() == 1);
    CHECK(state.pendingRequests().front().kind ==
          IoPanelRequest::Kind::SetRecordLatencyOffset);
    CHECK(state.pendingRequests().front().value == 384);

    state.requestRecordLatencyOffset(-4);
    CHECK(state.recordLatencyOffset() == 0);
    CHECK(state.pendingRequests().back().value == 0);
}

TEST_CASE("meter snapshots retain every hardware channel", "[iopanel]") {
    IoPanelState state;
    IoMeterSnapshot snapshot;
    for (int channel = 0; channel < 32; ++channel) {
        const float value = static_cast<float>(channel) / 31.0f;
        snapshot.inputs.push_back(IoInputMeter{value, value * 0.5f,
                                                channel == 31});
    }
    state.setMeterSnapshot(std::move(snapshot));

    CHECK(state.inputMeterChannels() == 32);
    CHECK(state.inputMeter(0).peak == 0.0f);
    CHECK(state.inputMeter(31).peak == 1.0f);
    CHECK(state.inputMeter(31).rms == 0.5f);
    CHECK(state.inputMeter(31).clipped);
    CHECK(state.inputMeter(32).peak == 0.0f);
}

TEST_CASE("meter presentation clamps peaks but not channel count",
          "[iopanel]") {
    IoPanelState state;
    state.setMeterSnapshot(IoMeterSnapshot{{
        IoInputMeter{-0.5f, -1.0f, false},
        IoInputMeter{0.25f, 0.125f, false},
        IoInputMeter{2.0f, 3.0f, true},
    }});
    REQUIRE(state.inputMeterChannels() == 3);
    CHECK(state.inputMeter(0).peak == 0.0f);
    CHECK(state.inputMeter(0).rms == 0.0f);
    CHECK(state.inputMeter(1).peak == 0.25f);
    CHECK(state.inputMeter(2).peak == 1.0f);
    CHECK(state.inputMeter(2).rms == 1.0f);
    CHECK(state.inputMeter(2).clipped);
}

TEST_CASE("capture fallback status and error remain visible to the view",
          "[iopanel]") {
    IoPanelState state;
    state.setCaptureStatus("Output-only mode",
                           "Selected interface cannot open for duplex capture");
    CHECK(state.captureStatusText() == "Output-only mode");
    CHECK(state.captureErrorText() ==
          "Selected interface cannot open for duplex capture");

    state.setCaptureStatus("Input ready");
    CHECK(state.captureStatusText() == "Input ready");
    CHECK(state.captureErrorText().empty());
}

TEST_CASE("meter width exposes horizontal overflow deterministically",
          "[iopanel]") {
    CHECK(IoPanelState::meterContentWidth(0) == 0.0f);
    CHECK(IoPanelState::meterContentWidth(1) ==
          IoPanelState::kMeterChannelWidth);
    CHECK(IoPanelState::meterContentWidth(4) ==
          4.0f * IoPanelState::kMeterChannelWidth +
              3.0f * IoPanelState::kMeterChannelGap);

    CHECK_FALSE(IoPanelState::meterNeedsHorizontalOverflow(4, 100.0f));
    CHECK(IoPanelState::meterNeedsHorizontalOverflow(16, 240.0f));
    CHECK(IoPanelState::meterNeedsHorizontalOverflow(1, -1.0f));
}

TEST_CASE("compact I/O panel draws inside the sidebar height budget",
          "[iopanel]") {
    HeadlessIoPanel rig;
    IoPanelState state;
    state.setPlaybackCatalog({IoDevice{"speakers", "Main Output", 2, true}});
    state.setSelectedOutput(IoDeviceSelection{"speakers", "Main Output"});
    state.setSelectedInput(
        IoDeviceSelection{IoPanelState::kInputDefaultId, "Default Input"});

    IoMeterSnapshot snapshot;
    for (int channel = 0; channel < 24; ++channel) {
        snapshot.inputs.push_back(
            IoInputMeter{0.5f, 0.25f, channel == 23});
    }
    state.setMeterSnapshot(std::move(snapshot));

    CHECK(rig.drawHeight(state) <= 190.0f);
    CHECK(state.inputMeterChannels() == 24);
    CHECK(state.pendingRequests().empty());
}
