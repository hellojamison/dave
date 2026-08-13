// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace dave::gui {

// Stable device identity and presentation copied from the platform catalog.
// The GUI deliberately knows nothing about miniaudio device handles: the
// application translates these ids when it services a queued request.
struct IoDevice {
    std::string id;
    std::string name;
    int channels = 0;
    bool isDefault = false;
};

struct IoDeviceSelection {
    std::string id;
    std::string name;
};

struct IoInputMeter {
    float peak = 0.0f;
    float rms = 0.0f;
    bool clipped = false;
};

// Linear 0..1 peak and RMS values, one entry per capture channel. Values
// outside that range are accepted and clamped only for presentation.
struct IoMeterSnapshot {
    std::vector<IoInputMeter> inputs;
};

struct IoPanelRequest {
    enum class Kind {
        SelectOutput,
        SelectInput,
        RefreshDevices,
        ClearInputClips,
        SetRecordLatencyOffset,
    };

    Kind kind = Kind::RefreshDevices;
    std::string deviceId;
    int value = 0;
};

// GUI-owned state for the sidebar I/O panel. Catalogs and confirmed selections
// are snapshots supplied by the application; drawIoPanel() only appends
// requests. That keeps device teardown/opening outside an ImGui frame.
class IoPanelState {
public:
    static constexpr const char* kInputOffId = "dave:input:off";
    static constexpr const char* kInputDefaultId = "dave:input:default";

    IoPanelState();

    void setPlaybackCatalog(std::vector<IoDevice> devices);
    void setCaptureCatalog(std::vector<IoDevice> devices);
    const std::vector<IoDevice>& playbackCatalog() const { return playback_; }
    const std::vector<IoDevice>& captureCatalog() const { return capture_; }

    void setSelectedOutput(std::optional<IoDeviceSelection> selection);
    void setSelectedInput(std::optional<IoDeviceSelection> selection);
    const std::optional<IoDeviceSelection>& selectedOutput() const {
        return selectedOutput_;
    }
    const std::optional<IoDeviceSelection>& selectedInput() const {
        return selectedInput_;
    }

    // A missing device never clears the selection. This matters when an
    // interface is unplugged: the panel must say what went away rather than
    // silently pretending the system default was chosen.
    bool selectedOutputAvailable() const;
    bool selectedInputAvailable() const;
    std::string selectedOutputLabel() const;
    std::string selectedInputLabel() const;

    void setCaptureStatus(std::string statusText, std::string errorText = {});
    const std::string& captureStatusText() const { return captureStatusText_; }
    const std::string& captureErrorText() const { return captureErrorText_; }

    void setMeterSnapshot(IoMeterSnapshot snapshot);
    const IoMeterSnapshot& meterSnapshot() const { return meters_; }
    size_t inputMeterChannels() const { return meters_.inputs.size(); }
    IoInputMeter inputMeter(size_t channel) const;

    void requestOutput(const std::string& deviceId);
    void requestInput(const std::string& deviceId);
    void requestRefresh();
    void requestClearInputClips();
    void setRecordLatencyOffset(int samples) { recordLatencyOffset_ = samples; }
    int recordLatencyOffset() const { return recordLatencyOffset_; }
    void setRecordingActive(bool active) { recordingActive_ = active; }
    bool recordingActive() const { return recordingActive_; }
    void requestRecordLatencyOffset(int samples);
    const std::vector<IoPanelRequest>& pendingRequests() const {
        return requests_;
    }
    std::vector<IoPanelRequest> takeRequests();
    void clearRequests();

    // Shared by layout and tests. The fixed channel pitch guarantees that a
    // large interface becomes horizontally scrollable instead of dropping or
    // compressing its later channels out of existence.
    static constexpr float kMeterChannelWidth = 18.0f;
    static constexpr float kMeterChannelGap = 4.0f;
    static float meterContentWidth(size_t channels);
    static bool meterNeedsHorizontalOverflow(size_t channels,
                                             float availableWidth);

private:
    static bool catalogContains(const std::vector<IoDevice>& catalog,
                                const std::optional<IoDeviceSelection>& selected);
    static std::string selectionLabel(
        const std::vector<IoDevice>& catalog,
        const std::optional<IoDeviceSelection>& selected);

    std::vector<IoDevice> playback_;
    std::vector<IoDevice> capture_;
    std::optional<IoDeviceSelection> selectedOutput_;
    std::optional<IoDeviceSelection> selectedInput_;
    IoMeterSnapshot meters_;
    std::string captureStatusText_;
    std::string captureErrorText_;
    std::vector<IoPanelRequest> requests_;
    int recordLatencyOffset_ = 0;
    bool recordingActive_ = false;
};

// Draws a compact sidebar surface. Device changes and refreshes are queued in
// state and must be serviced by the application after the draw call returns.
void drawIoPanel(IoPanelState& state);

} // namespace dave::gui
