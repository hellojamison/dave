// SPDX-License-Identifier: GPL-3.0-or-later
#include "application/DaveApp.h"
#include "application/MainEditorLayout.h"
#include "document/MarkerCsv.h"
#include "document/ProjectFile.h"
#include "editing/Commands.h"
#include "gui/Theme.h"
#include "platform/MacMenuBar.h"

#include <glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <nfd.h>

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <string>

namespace dave::application {

namespace {

constexpr const char* kOutputDefaultId = "output:default";
constexpr const char* kInputOffId = gui::IoPanelState::kInputOffId;
constexpr const char* kInputDefaultId = gui::IoPanelState::kInputDefaultId;

std::string outputDeviceId(std::size_t index) {
    return "output:" + std::to_string(index);
}

std::string inputDeviceId(std::size_t index) {
    return "input:" + std::to_string(index);
}

int firstNameIndex(const std::vector<std::string>& names,
                   const std::string& wanted) {
    const auto found = std::find(names.begin(), names.end(), wanted);
    return found == names.end()
        ? -1
        : static_cast<int>(std::distance(names.begin(), found));
}

// Split rather than one stamp: the pattern exposes {date} and {time}
// separately, so a user can group takes by day without the seconds.
void takeDateAndTime(std::string& date, std::string& time) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d", &local);
    date = buffer;
    std::strftime(buffer, sizeof(buffer), "%H%M%S", &local);
    time = buffer;
}

bool drawCenteredEmptyState(const char* message, const char* detail,
                            const char* actionLabel) {
    const auto& pal = gui::theme::palette();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float buttonH = 30.0f;
    const float blockH = ImGui::GetTextLineHeight() * 2.0f + 16.0f + buttonH;
    const float top = std::max(12.0f, (avail.y - blockH) * 0.5f);

    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + top));
    if (gui::theme::fonts().label != nullptr) {
        ImGui::PushFont(gui::theme::fonts().label,
                        static_cast<float>(gui::theme::typeScale().label));
    }
    const float messageW = ImGui::CalcTextSize(message).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, (avail.x - messageW) * 0.5f));
    ImGui::TextColored(pal.text, "%s", message);
    if (gui::theme::fonts().label != nullptr) {
        ImGui::PopFont();
    }

    const float detailW = ImGui::CalcTextSize(detail).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, (avail.x - detailW) * 0.5f));
    ImGui::TextColored(pal.textMuted, "%s", detail);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    const float buttonW = std::max(118.0f, ImGui::CalcTextSize(
        actionLabel, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, (avail.x - buttonW) * 0.5f));
    return gui::theme::gradientButton(
        actionLabel, ImVec2(buttonW, buttonH),
        gui::theme::ButtonVariant::Primary);
}

// A panel header with a right-aligned text action. The button is placed back
// on top of the header bar the theme just drew, so the affordance sits with
// the title instead of spending a row of the panel's content.
bool panelHeaderAction(const char* label, const char* actionLabel,
                       const char* tooltip) {
    constexpr float headerH = 26.0f;  // matches theme::panelHeader
    const ImVec2 headerCursor = ImGui::GetCursorPos();
    const float headerW = ImGui::GetContentRegionAvail().x;
    gui::theme::panelHeader(label);
    const ImVec2 afterHeader = ImGui::GetCursorPos();

    const float buttonW = ImGui::CalcTextSize(actionLabel).x +
                          ImGui::GetStyle().FramePadding.x * 2.0f;
    const float buttonH = ImGui::GetTextLineHeight();
    ImGui::SetCursorPos(
        ImVec2(headerCursor.x + std::max(0.0f, headerW - buttonW - 8.0f),
               headerCursor.y + (headerH - buttonH) * 0.5f));
    const bool clicked = ImGui::SmallButton(actionLabel);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::SetCursorPos(afterHeader);
    return clicked;
}

} // namespace

DaveApp::~DaveApp() {
    // No frame loop left to notice the transport stopping, so close it here.
    if (capturing()) endCapture();
    audio_.stop();
    audio_.setCompiledGraph(nullptr);
}

bool DaveApp::init(bool startAudio) {
    if (!window_.valid() || !imgui_.init(window_)) {
        return false;
    }
    window_.setCloseGuard([this] { return requestClose(); });
    window_.setFileDropCallback([this](const std::vector<std::string>& paths) {
        for (const auto& path : paths) {
            const auto extension = path.find_last_of('.');
            std::string suffix = extension == std::string::npos ? "" : path.substr(extension);
            std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (suffix == ".mid" || suffix == ".midi") {
                importMidiFile(path);
                continue;
            }
            if (suffix != ".wav") {
                std::fprintf(stderr,
                             "Dave: ignored dropped file (not .wav/.mid): %s\n",
                             path.c_str());
                continue;
            }
            loadWavIntoNewTrack(path);
        }
    });

    // ─── macOS native menu bar ───────────────────────────────────────────
    // On macOS the menu bar belongs at the top of the screen, not inside the
    // window. Set up a native NSApplication menu bar and wire the callbacks.
#ifdef __APPLE__
    platform::g_menuNew          = [this](){ newProject(); };
    platform::g_menuOpen         = [this](){ openProjectDialog(); };
    platform::g_menuSave         = [this](){ saveProject(false); };
    platform::g_menuSaveAs       = [this](){ saveProject(true); };
    platform::g_menuLoadWav      = [this](){ openWavDialog(); };
    platform::g_menuImportMidi   = [this](){ openMidiDialog(); };
    platform::g_menuLoadVideo    = [this](){ openVideoDialog(); };
    platform::g_menuImportMarkers= [this](){ importMarkersDialog(); };
    platform::g_menuExportMarkers= [this](){ exportMarkersDialog(); };
    platform::g_menuUndo         = [this](){
        if (!recordingActive()) undo_.undo();
        else showStatus("Stop recording before Undo", true);
    };
    platform::g_menuRedo         = [this](){
        if (!recordingActive()) undo_.redo();
        else showStatus("Stop recording before Redo", true);
    };
    platform::g_menuAddTrack     = [this](){
        undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));
    };
    platform::g_menuToggleTransientNavigation = [this](){
        toggleTransientNavigation();
    };
    platform::g_menuNextLandmark = [this](){
        navigateTimeline(gui::NavigationDirection::Next, false);
    };
    platform::g_menuPreviousLandmark = [this](){
        navigateTimeline(gui::NavigationDirection::Previous, false);
    };
    platform::g_menuExtendNextLandmark = [this](){
        navigateTimeline(gui::NavigationDirection::Next, true);
    };
    platform::g_menuExtendPreviousLandmark = [this](){
        navigateTimeline(gui::NavigationDirection::Previous, true);
    };
    platform::g_menuPlayStop     = [this](){
        // Stopping the transport ends the capture; nothing here has to know
        // whether one is running.
        audio_.transport().toggle();
    };
    platform::g_menuReturnToStart= [this](){ audio_.transport().seek(0); };
    // The native item is always present; with no picture loaded there is
    // nothing for it to move, so it does nothing rather than opening an
    // empty window.
    platform::g_menuToggleVideoWindow = [this](){
        if (!edit_.videoTracks().empty()) setVideoPoppedOut(!videoPoppedOut_);
    };
    // The panel is a Preferences tab now; the menu item opens it there rather
    // than toggling a window that no longer exists.
    platform::g_menuToggleIoPanel = [this]() {
        showPreferences_ = true;
        preferencesJustOpened_ = true;
    };
    platform::g_menuOpenPreferences = [this]() {
        showPreferences_ = true;
        preferencesJustOpened_ = true;
    };
    platform::g_menuQuit         = [this](){ window_.close(); };
    platform::setupMacMenuBar();
#endif
    audioPreferences_ = audioPreferencesStore_.load();
    editorPreferences_ = editorPreferencesStore_.load();
    view_.transientNavigationEnabled =
        editorPreferences_.transientNavigationEnabled;
    view_.showTransientTicks = editorPreferences_.showTransientTicks;
    view_.transientSensitivity = editorPreferences_.transientSensitivity;
    view_.meterOptions.preFader = editorPreferences_.meterPreFader;
    view_.meterOptions.rmsBody = editorPreferences_.meterRmsBody;
    view_.meterOptions.peakHoldSeconds =
        editorPreferences_.meterPeakHoldSeconds;
    if (startAudio) {
        refreshAudioDevices();
        if (!applyAudioPreferences(audioPreferences_, false)) {
            std::fprintf(stderr, "Dave: audio engine failed to start\n");
        }
        if (audio_.playbackChannelCount() == 1) {
            edit_.track(document::kMainBusId)->mainOutput =
                document::RouteTarget::hardwareOutput(0, 1);
        }
    }

    // Wire the Edit's change signal to graph re-derivation.
    edit_.setChangeListener([this] { onEditChanged(); });
    view_.deferRecordArmRequests = true;

    // Seed an initial track so the timeline isn't empty.
    undo_.execute(std::make_unique<editing::AddTrackCommand>("Track 1"));
    view_.selectedTrackIndex = 0;

    // Seed a default marker track so the marker lane is immediately usable —
    // no "Add marker track" hurdle. The user can still add more tracks later
    // (Cues, Scenes, etc.) but the common case (just drop a marker) works
    // out of the box.
    edit_.addMarkerTrack("Markers");
    // addMarkerTrack fires notifyChanged which triggers a graph re-derive;
    // the initial re-derive already happened via AddTrackCommand above, so
    // this is one extra harmless rebuild.

    window_.setFrameCallback([this] {
        imgui_.newFrame();
        handleShortcuts();
        drawUI();
        // Last writer in the frame, so the backend's next NewFrame reads None
        // and takes its hide path instead of putting the arrow back. Without
        // this the two disagree every frame and the arrow flickers through.
        if (view_.wantsHiddenCursor) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        }
        imgui_.render();
        // After render, so it lands after UpdatePlatformWindows — anything
        // ImGui does to the cursor during the frame has already happened.
        window_.setCursorHidden(view_.wantsHiddenCursor);
    });
    return true;
}

void DaveApp::handleShortcuts() {
    auto& transport = audio_.transport();
    ImGuiIO& io = ImGui::GetIO();

    // Ignore shortcuts while typing in a text field.
    if (io.WantTextInput) return;

#ifdef __APPLE__
    const bool primaryModifier = io.KeySuper;
#else
    const bool primaryModifier = io.KeyCtrl;
#endif
    const bool shift = io.KeyShift;
    const bool alt = io.KeyAlt;

    // repeat=false: these are edge-triggered actions — ignore key auto-repeat
    // so a single Space press doesn't toggle play on then off again.
#ifdef __APPLE__
    if (primaryModifier && alt &&
        ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        toggleTransientNavigation();
    } else
#else
    if (primaryModifier && alt &&
        ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        toggleTransientNavigation();
    } else
#endif
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        transport.toggle();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        // Return/Enter: jump playhead to start (standard DAW shortcut).
        transport.seek(0);
    } else if (!recordingActive() && primaryModifier &&
               ImGui::IsKeyPressed(ImGuiKey_Z, false) && !shift &&
               undo_.canUndo()) {
        undo_.undo();
    } else if (!recordingActive() && primaryModifier &&
               ImGui::IsKeyPressed(ImGuiKey_Z, false) && shift &&
               undo_.canRedo()) {
        undo_.redo();
    } else if (primaryModifier && shift && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        openProjectDialog();
    } else if (primaryModifier && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        // Cmd+O: open project (audio import is Cmd+I now).
    } else if (primaryModifier && ImGui::IsKeyPressed(ImGuiKey_I, false)) {
        openWavDialog();
    } else if (primaryModifier && shift && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        saveProject(true);
    } else if (primaryModifier && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        saveProject(false);
    } else if (primaryModifier && shift && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));
    } else if (primaryModifier && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        newProject();
    } else if (primaryModifier &&
               ImGui::IsKeyPressed(ImGuiKey_Comma, false)) {
        // Cmd+, — the macOS convention, and harmless elsewhere.
        showPreferences_ = true;
        preferencesJustOpened_ = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
               ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        // Delete clears automation inside the selection when a lane is open.
        // It returns false when there is nothing to act on, which leaves the
        // key free to mean something else here later.
        gui::deleteAutomationInSelection(edit_, undo_, view_);
    } else if (primaryModifier && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        // Cmd+D duplicates the selected clip, audio or MIDI. The dispatch
        // lives in Timeline.cpp so a test can reach it — this file isn't in
        // the test target.
        gui::duplicateSelectedClip(edit_, undo_, view_);
    } else if (primaryModifier && ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
        window_.close();
    } else if (primaryModifier && !shift && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        toggleRecording();
    } else if (!primaryModifier && shift && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        toggleSelectedTrackArm();
    } else if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        // T = zoom in (Pro Tools convention).
        gui::zoomAroundSample(view_, view_.samplesPerPixel * 0.5,
                              audio_.transport().position());
    } else if (!primaryModifier && !shift && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        // R = zoom out.
        gui::zoomAroundSample(view_, view_.samplesPerPixel * 2.0,
                              audio_.transport().position());
    } else if (!primaryModifier && ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        // L = loop on/off, over whatever range is current.
        const bool anyArmed = std::any_of(
            edit_.tracks().begin(), edit_.tracks().end(),
            [](const document::Track& track) { return track.recordArm; });
        if (!loopEnabled_ && anyArmed) {
            showStatus("Disarm recording tracks before enabling Loop", true);
        } else {
            loopEnabled_ = !loopEnabled_;
            syncTransportLoop();
        }
    } else if (!primaryModifier && shift && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        // Shift+M / Shift+S toggle the selected track. The modifier keeps the
        // bare letters free and stops a stray keypress from silencing a track
        // the user was only looking at. Guarded on !primaryModifier so they
        // don't shadow Cmd/Ctrl+Shift+S (Save As).
        toggleSelectedTrackMute();
    } else if (!primaryModifier && shift && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        toggleSelectedTrackSolo();
    } else if (primaryModifier && shift && ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        undo_.execute(std::make_unique<editing::AddTrackCommand>("Bus", editing::AddTrackCommand::Flavour::Bus));
        view_.selectedTrackIndex = static_cast<int>(
            edit_.tracks().size() - 2);
    } else if (primaryModifier && shift && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        // Pop the picture in or out of its own window. macOS reaches the same
        // action through the native Window menu's Cmd+Shift+V. With no video
        // loaded there is no picture to move, so the binding does nothing.
        if (!edit_.videoTracks().empty()) {
            setVideoPoppedOut(!videoPoppedOut_);
        }
    }
}

void DaveApp::prefetchSelectedTrackTransients() {
    const int selected = view_.selectedTrackIndex;
    if (selected < 0 || selected >= static_cast<int>(edit_.tracks().size())) return;
    const auto& track = edit_.tracks()[static_cast<size_t>(selected)];
    std::unordered_set<std::string> requested;
    for (const auto& clip : track.clips) {
        if (clip.asset.sha256.empty() || !requested.insert(clip.asset.sha256).second) {
            continue;
        }
        transientAnalyses_.request(
            clip.asset.sha256, builder_.decodedAsset(clip.asset.sha256));
    }
}

gui::TransientSnapshotMap DaveApp::selectedTrackTransientSnapshots() {
    gui::TransientSnapshotMap snapshots;
    const int selected = view_.selectedTrackIndex;
    if (selected < 0 || selected >= static_cast<int>(edit_.tracks().size())) {
        return snapshots;
    }
    for (const auto& clip : edit_.tracks()[static_cast<size_t>(selected)].clips) {
        if (!clip.asset.sha256.empty() && !snapshots.contains(clip.asset.sha256)) {
            snapshots.emplace(
                clip.asset.sha256,
                transientAnalyses_.snapshot(clip.asset.sha256));
        }
    }
    return snapshots;
}

void DaveApp::navigateTimeline(gui::NavigationDirection direction,
                               bool extend, bool allowPending) {
    if (recordingActive()) {
        showStatus("Transient navigation is unavailable while recording", true);
        return;
    }
    const int selected = view_.selectedTrackIndex;
    if (selected < 0 || selected >= static_cast<int>(edit_.tracks().size())) {
        showStatus("Select an audio track to navigate", true);
        return;
    }
    const auto& track = edit_.tracks()[static_cast<size_t>(selected)];
    if (track.clips.empty()) {
        showStatus("The selected audio track has no clips", true);
        return;
    }

    const auto mode = view_.transientNavigationEnabled
        ? gui::TimelineLandmarkMode::Transients
        : gui::TimelineLandmarkMode::ClipBoundaries;
    if (mode == gui::TimelineLandmarkMode::Transients) {
        prefetchSelectedTrackTransients();
    }
    const auto snapshots = selectedTrackTransientSnapshots();
    const auto landmarks = gui::collectTrackLandmarks(
        track, mode, snapshots, view_.transientSensitivity);
    if (mode == gui::TimelineLandmarkMode::Transients &&
        landmarks.analysisPending) {
        if (allowPending) {
            pendingTransientNavigation_ =
                std::make_unique<PendingTransientNavigation>(
                    PendingTransientNavigation{
                        direction, extend, selected,
                        audio_.transport().position(), view_.hasSelection,
                        view_.selectionAnchor, view_.selectionFocus});
            showStatus("Analyzing transients…");
        }
        return;
    }

    const int64_t current = extend && view_.hasSelection
        ? view_.selectionFocus : audio_.transport().position();
    int64_t destination = 0;
    if (!gui::findTimelineLandmark(
            landmarks.samples, current, direction, destination)) {
        if (landmarks.analysisFailed && landmarks.samples.empty()) {
            showStatus("Transient analysis failed for the selected track", true);
        } else {
            const char* which = direction == gui::NavigationDirection::Next
                ? "next" : "previous";
            const char* kind = mode == gui::TimelineLandmarkMode::Transients
                ? "transient" : "clip boundary";
            showStatus(std::string("No ") + which + " " + kind);
        }
        pendingTransientNavigation_.reset();
        return;
    }

    gui::NavigationSelection selection{
        view_.hasSelection, view_.selectionAnchor, view_.selectionFocus,
        view_.selectionStart, view_.selectionEnd};
    selection = gui::applyTimelineNavigation(
        selection, current, destination, extend);
    if (!extend) {
        view_.hasSelection = false;
        view_.selectionAnchor = destination;
        view_.selectionFocus = destination;
        view_.selectionStart = destination;
        view_.selectionEnd = destination;
        audio_.transport().seek(destination);
    } else {
        view_.hasSelection = selection.active;
        view_.selectionAnchor = selection.anchor;
        view_.selectionFocus = selection.focus;
        view_.selectionStart = selection.start;
        view_.selectionEnd = selection.end;
        view_.selectionRow = selected;
        audio_.transport().seek(selection.start);
    }
    pendingTransientNavigation_.reset();
}

void DaveApp::servicePendingTransientNavigation() {
    if (!pendingTransientNavigation_) return;
    const auto pending = *pendingTransientNavigation_;
    if (view_.selectedTrackIndex != pending.selectedTrackIndex ||
        audio_.transport().position() != pending.transportSample ||
        view_.hasSelection != pending.hadSelection ||
        view_.selectionAnchor != pending.selectionAnchor ||
        view_.selectionFocus != pending.selectionFocus) {
        pendingTransientNavigation_.reset();
        return;
    }
    navigateTimeline(pending.direction, pending.extend, false);
}

void DaveApp::toggleTransientNavigation() {
    view_.transientNavigationEnabled = !view_.transientNavigationEnabled;
    saveEditorPreferences();
    showStatus(view_.transientNavigationEnabled
                   ? "Transient navigation enabled"
                   : "Clip-boundary navigation enabled");
}

void DaveApp::saveEditorPreferences() {
    editorPreferences_.transientNavigationEnabled =
        view_.transientNavigationEnabled;
    editorPreferences_.showTransientTicks = view_.showTransientTicks;
    editorPreferences_.transientSensitivity =
        std::clamp(view_.transientSensitivity, 0, 100);
    editorPreferences_.meterPreFader = view_.meterOptions.preFader;
    editorPreferences_.meterRmsBody = view_.meterOptions.rmsBody;
    editorPreferences_.meterPeakHoldSeconds =
        view_.meterOptions.peakHoldSeconds;
    if (!editorPreferencesStore_.save(editorPreferences_)) {
        showStatus("Could not save editor preferences", true);
    }
}

// Preferences gathers the per-user settings that were previously reachable
// only by right-clicking the control they affected — discoverable by accident
// at best. Everything here is stored outside the .dave project, in
// editor-preferences.json.
void DaveApp::drawPreferencesWindow() {
    if (!showPreferences_) return;
    const auto& pal = gui::theme::palette();

    if (preferencesJustOpened_) {
        // Centred on first open only. Reopening should return it to wherever
        // the user last dragged it.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowFocus();
        preferencesJustOpened_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);

    bool open = showPreferences_;
    if (ImGui::Begin("Preferences", &open,
                     ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::BeginTabBar("##preferencesTabs");

        // Sample rate and bit depth are set once when a session starts and
        // then left alone, which is what a preference is — they spent toolbar
        // width on a decision nobody revisits mid-edit. They belong to the
        // document rather than to the app, so they lead the tabs rather than
        // sitting among the machine settings.
        if (ImGui::BeginTabItem("Session")) {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::BeginDisabled(recordingActive());

            static constexpr int kRates[] = {44100, 48000, 88200, 96000,
                                             176400, 192000};
            static const char* kRateLabels[] = {
                "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz", "176.4 kHz",
                "192 kHz"};
            int rateIdx = 1;
            for (int i = 0; i < IM_ARRAYSIZE(kRates); ++i) {
                if (kRates[i] == edit_.sampleRate()) rateIdx = i;
            }
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo("Sample rate", &rateIdx, kRateLabels,
                             IM_ARRAYSIZE(kRateLabels))) {
                edit_.setSampleRate(kRates[rateIdx]);
                applySessionSampleRate();
            }
            ImGui::TextDisabled(
                "Reopens the output device. Clip positions are stored as\n"
                "samples and are not converted, so material recorded at\n"
                "another rate plays at the wrong speed. Set this before\n"
                "recording.");

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            static constexpr int kDepths[] = {16, 24, 32};
            static const char* kDepthLabels[] = {"16-bit", "24-bit",
                                                 "32-bit float"};
            int depthIdx = 2;
            for (int i = 0; i < IM_ARRAYSIZE(kDepths); ++i) {
                if (kDepths[i] == edit_.bitDepth()) depthIdx = i;
            }
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo("Bit depth", &depthIdx, kDepthLabels,
                             IM_ARRAYSIZE(kDepthLabels))) {
                edit_.setBitDepth(kDepths[depthIdx]);
            }
            ImGui::TextDisabled(
                "Bit depth for files this session writes. 32-bit float\n"
                "cannot clip and a level set wrong stays recoverable.");

            ImGui::EndDisabled();
            if (recordingActive()) {
                ImGui::TextColored(gui::theme::palette().warning,
                                   "Locked while recording.");
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Recording")) {
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::TextDisabled("Take file names");

        char pattern[128]{};
        std::strncpy(pattern, editorPreferences_.takeNamePattern.c_str(),
                     sizeof(pattern) - 1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##takePattern", pattern, sizeof(pattern))) {
            editorPreferences_.takeNamePattern = pattern;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // An empty or unusable pattern would name every take the same
            // thing, so it snaps back rather than being saved.
            if (expandTakeNamePattern(editorPreferences_.takeNamePattern,
                                      TakeNameContext{}).empty()) {
                editorPreferences_.takeNamePattern = kDefaultTakeNamePattern;
            }
            saveEditorPreferences();
        }

        // A live preview against the selected track, so the tokens do not have
        // to be understood in the abstract.
        TakeNameContext preview;
        takeDateAndTime(preview.date, preview.time);
        preview.projectName = std::filesystem::path(projectPath_).stem().string();
        const auto* selected = selectedTrack();
        preview.trackName = selected != nullptr ? selected->name : "Audio 1";
        preview.takeNumber = 1;
        ImGui::TextDisabled(
            "%s.wav", expandTakeNamePattern(editorPreferences_.takeNamePattern,
                                            preview).c_str());
        if (!takeNamePatternIsUnique(editorPreferences_.takeNamePattern)) {
            ImGui::TextColored(pal.warning,
                               "No {take} — later takes get a numeric suffix.");
        }
        ImGui::TextDisabled("{track} {take} {date} {time} {project}");
        if (ImGui::SmallButton("Restore default##take")) {
            editorPreferences_.takeNamePattern = kDefaultTakeNamePattern;
            saveEditorPreferences();
        }

        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Editing")) {
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        bool navigation = view_.transientNavigationEnabled;
        if (ImGui::Checkbox("Transient navigation", &navigation)) {
            toggleTransientNavigation();
        }
        bool ticks = view_.showTransientTicks;
        if (ImGui::Checkbox("Show transient ticks", &ticks)) {
            view_.showTransientTicks = ticks;
            saveEditorPreferences();
        }
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderInt("Sensitivity", &view_.transientSensitivity, 0, 100,
                         "%d");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            saveEditorPreferences();
        }
        ImGui::TextDisabled("Higher values include softer attacks.");

        ImGui::Separator();
        ImGui::TextUnformatted("Meters");
        // The same choice the meter's own click menu offers — this is where
        // you look for it, that is where you reach for it mid-mix.
        int holdIndex = 0;
        for (int i = 0; i < IM_ARRAYSIZE(gui::kMeterPeakHoldChoices); ++i) {
            if (gui::kMeterPeakHoldChoices[i].seconds ==
                view_.meterOptions.peakHoldSeconds) {
                holdIndex = i;
            }
        }
        const char* holdLabels[IM_ARRAYSIZE(gui::kMeterPeakHoldChoices)];
        for (int i = 0; i < IM_ARRAYSIZE(gui::kMeterPeakHoldChoices); ++i) {
            holdLabels[i] = gui::kMeterPeakHoldChoices[i].label;
        }
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("Peak hold", &holdIndex, holdLabels,
                         IM_ARRAYSIZE(holdLabels))) {
            view_.meterOptions.peakHoldSeconds =
                gui::kMeterPeakHoldChoices[holdIndex].seconds;
            saveEditorPreferences();
        }
        ImGui::TextDisabled(
            "How long the peak marker sits at its maximum before it falls.");

        ImGui::EndTabItem();
        }

        // Device selection lives here rather than in the arrangement window:
        // it is machine configuration, not part of the document, and it is set
        // once and then left alone. The input meters come with it — they are
        // how you confirm a device choice actually works.
        if (ImGui::BeginTabItem("Audio I/O")) {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            syncIoPanelState();
            gui::drawIoPanel(ioPanel_);
            serviceIoPanelRequests();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::Separator();
        ImGui::TextDisabled("%s",
                            editorPreferencesStore_.path().string().c_str());
    }
    ImGui::End();

    if (!open) showPreferences_ = false;
}

// Both toggles operate on the selected track. With no selection there is no
// unambiguous target, so they do nothing rather than guessing at the first
// track — silently muting something the user wasn't looking at is worse than
// a keypress that appears to do nothing.
document::Track* DaveApp::selectedTrack() {
    const int sel = view_.selectedTrackIndex;
    if (sel < 0 || sel >= static_cast<int>(edit_.tracks().size())) {
        return nullptr;
    }
    return &edit_.tracksMut()[static_cast<size_t>(sel)];
}

// MIDI rows sit below the audio rows in one index space, so M and S have to
// reach them too — a shortcut that works on some rows and silently does nothing
// on others is worse than one that doesn't exist.
void DaveApp::toggleSelectedTrackMute() {
    if (document::Track* t = selectedTrack()) {
        t->mute = !t->mute;
        edit_.notifyChanged();
    }
}

void DaveApp::toggleSelectedTrackSolo() {
    if (document::Track* t = selectedTrack()) {
        t->solo = !t->solo;
        edit_.notifyChanged();
    }
}

void DaveApp::showStatus(std::string message, bool error) {
    statusMessage_ = std::move(message);
    statusIsError_ = error;
    statusUntil_ = ImGui::GetTime() + 5.0;
    std::fprintf(stderr, "Dave: %s\n", statusMessage_.c_str());
}

bool DaveApp::requestClose() {
    if (!recordingActive()) return true;
    showStatus("Stop recording before closing Dave", true);
    return false;
}

void DaveApp::toggleSelectedTrackArm() {
    auto* track = selectedTrack();
    if (track == nullptr) {
        showStatus("Select an audio track to arm", true);
        return;
    }
    toggleTrackArm(track->id);
}

void DaveApp::toggleTrackArm(const std::string& trackId) {
    auto* track = edit_.track(trackId);
    if (track == nullptr) return;
    if (recordingActive()) {
        showStatus("Track arming cannot change during a recording", true);
        return;
    }
    // Disarming is always safe. In particular, an unplugged input must not
    // trap a track in the armed state merely because new arming is refused.
    if (track->recordArm) {
        track->recordArm = false;
        edit_.notifyChanged();
        return;
    }
    if (projectPath_.empty()) {
        showStatus("Save the project before arming a track", true);
        return;
    }
    if (loopEnabled_) {
        showStatus("Turn Loop off before arming; loop recording is not available yet",
                   true);
        return;
    }
    if (!audio_.captureAvailable() || audio_.captureChannelCount() == 0) {
        showStatus("Select an available input device before arming", true);
        return;
    }
    const int channels = static_cast<int>(audio_.captureChannelCount());
    if (track->inputChannel < 0 || track->inputChannelCount < 1 ||
        track->inputChannelCount > 2 ||
        track->inputChannel + track->inputChannelCount > channels) {
        showStatus("The saved track input is unavailable on this device", true);
        return;
    }
    track->recordArm = true;
    edit_.notifyChanged();
}

// Open a continuous capture. It does NOT roll the transport and it does NOT
// mark anything to keep — capture runs for as long as the transport does, and
// pressing Record decides which parts of it become regions.
bool DaveApp::beginCapture() {
    if (capturing()) return true;
    if (projectPath_.empty()) {
        showStatus("Save the project before recording", true);
        return false;
    }
    if (loopEnabled_) {
        showStatus("Turn Loop off before recording; loop recording is not available yet",
                   true);
        return false;
    }
    const int captureChannels =
        static_cast<int>(audio_.captureChannelCount());
    if (!audio_.captureAvailable() || captureChannels <= 0) {
        showStatus("Recording refused: no capture input is available", true);
        return false;
    }

    std::vector<document::Track*> armed;
    for (auto& track : edit_.tracksMut()) {
        if (track.recordArm) armed.push_back(&track);
    }
    if (armed.empty()) {
        showStatus("Arm at least one audio track before recording", true);
        return false;
    }

    for (auto* track : armed) {
        if (track->inputChannel < 0 || track->inputChannelCount < 1 ||
            track->inputChannelCount > 2 ||
            track->inputChannel + track->inputChannelCount > captureChannels) {
            showStatus("Recording refused: an armed track input is unavailable",
                       true);
            return false;
        }
    }
    const std::filesystem::path recordings =
        std::filesystem::path(projectPath_) / "recordings";
    std::error_code directoryError;
    std::filesystem::create_directories(recordings, directoryError);
    if (directoryError) {
        showStatus("Recording refused: cannot create the project recordings folder",
                   true);
        return false;
    }

    auto session = std::make_unique<RecordingSession>();
    std::vector<std::unique_ptr<engine::DiskWriter::Track>> writerTracks;
    writerTracks.reserve(armed.size());
    session->armedTrackIds.reserve(armed.size());
    std::unordered_set<std::string> reservedPaths;
    TakeNameContext naming;
    takeDateAndTime(naming.date, naming.time);
    naming.projectName =
        std::filesystem::path(projectPath_).stem().string();
    for (const auto* track : armed) {
        auto destination = std::make_unique<engine::DiskWriter::Track>();
        naming.trackName = track->name;
        naming.takeNumber = 1;
        destination->path = uniqueTakePath(
            recordings, editorPreferences_.takeNamePattern, naming,
            reservedPaths).string();
        destination->trackId = track->id;
        destination->channels = track->inputChannelCount;
        session->armedTrackIds.push_back(track->id);
        writerTracks.push_back(std::move(destination));
    }

    const auto ringFrames = engine::RecordController::ringFramesForSampleRate(
        static_cast<double>(edit_.sampleRate()));
    if (!session->writer.start(
            std::move(writerTracks), edit_.sampleRate(),
            engine::WavWriter::formatForBitDepth(edit_.bitDepth()),
            ringFrames)) {
        showStatus("Recording refused: one or more take files could not be opened",
                   true);
        return false;
    }

    std::vector<engine::RecordController::ArmedTrackConfig> mappings;
    mappings.reserve(armed.size());
    for (std::size_t i = 0; i < armed.size(); ++i) {
        auto* ring = session->writer.ring(i);
        const auto& track = *armed[i];
        if (track.inputChannelCount == 2) {
            mappings.push_back(engine::RecordController::ArmedTrackConfig::stereo(
                *ring, track.inputChannel, track.inputChannel + 1));
        } else {
            mappings.push_back(engine::RecordController::ArmedTrackConfig::mono(
                *ring, track.inputChannel));
        }
    }
    if (!session->controller.prepare(
            std::move(mappings), captureChannels,
            static_cast<std::size_t>(platform::AudioEngine::maxBlockSize())) ||
        !audio_.publishRecordController(&session->controller)) {
        session->writer.stop();
        showStatus("Recording refused: the capture session could not be published",
                   true);
        return false;
    }

    // The capture anchors at wherever the transport already is. It is rolling
    // by the time this runs, so the position is the truth rather than
    // something this function gets to choose.
    session->takeStartSample = audio_.transport().position();
    session->latencyOffsetSamples =
        std::max(0, audioPreferences_.recordLatencyOffsetSamples);
    view_.recordingActive = false;
    recordingSession_ = std::move(session);
    return true;
}

// Record while rolling: start a region here. Record again ends it, and the
// capture underneath carries on either way.
// Roll with armed tracks and the capture starts itself. A refusal here is
// silent unless the user has asked for a region: playing back a session with
// something left armed should not nag.
void DaveApp::beginCaptureIfArmed() {
    if (capturing()) return;
    const bool anyArmed = std::any_of(
        edit_.tracks().begin(), edit_.tracks().end(),
        [](const document::Track& track) { return track.recordArm; });
    if (!anyArmed) return;
    if (!audio_.captureAvailable()) return;
    beginCapture();
}

void DaveApp::punchIn() {
    if (!capturing()) {
        // Record from a standing start still means "roll and keep it", so the
        // transport goes first and the capture anchors to it.
        if (!audio_.transport().isPlaying()) {
            audio_.transport().play();
            transportWasRolling_ = true;
        }
        if (!beginCapture()) {
            audio_.transport().stop();
            transportWasRolling_ = false;
            return;
        }
    }
    audio_.transport().setRecording(true);
    const int64_t at = audio_.transport().position();
    recordingSession_->punches.push_back(application::PunchRange{at});
    view_.recordingActive = true;
    view_.recordingStartSample =
        std::max<int64_t>(0, at - recordingSession_->latencyOffsetSamples);
    view_.recordingEndSample = view_.recordingStartSample;
    showStatus("Recording", false);
}

void DaveApp::punchOut() {
    if (!capturing() || recordingSession_->punches.empty()) return;
    auto& punch = recordingSession_->punches.back();
    if (!punch.open()) return;
    punch.out = std::max(punch.in, audio_.transport().position());
    audio_.transport().setRecording(false);
    view_.recordingActive = false;
    showStatus("Punched out; still rolling", false);
}

// The transport has stopped: close the capture, cut every punch out of the
// take file, and commit them.
void DaveApp::endCapture() {
    if (!capturing()) return;

    audio_.transport().setRecording(false);
    audio_.transport().stop();
    view_.recordingActive = false;

    if (!audio_.retireRecordController()) {
        // The controller cannot be destroyed while the callback might still
        // own it. Stopping miniaudio joins that callback; this exceptional
        // path favors memory safety and then reopens the confirmed devices.
        audio_.stop();
        if (!audio_.retireRecordController(std::chrono::milliseconds(1000))) {
            showStatus("Recording stop failed: capture callback did not quiesce",
                       true);
            return;
        }
    }

    std::unique_ptr<RecordingSession> finished =
        std::move(recordingSession_);
    const int64_t firstCapturedSample =
        finished->controller.firstSamplePosition();
    if (firstCapturedSample >= 0) {
        finished->takeStartSample = firstCapturedSample;
    }
    finished->writer.stop();

    // Anything still open ends where the transport stopped, and a punch that
    // captured nothing is dropped rather than committed as an empty region.
    const auto punches = application::closePunches(
        std::move(finished->punches), audio_.transport().position());

    int committed = 0;
    bool hadFailure = false;
    bool hadDrops = false;
    bool badLatencyCalibration = false;
    for (std::size_t i = 0; i < finished->writer.trackCount(); ++i) {
        const auto stats = finished->writer.stats(i);
        const std::string sha = finished->writer.shaOf(i);
        if (stats.failed || sha.empty()) {
            hadFailure = true;
            continue;
        }
        if (stats.droppedFrames > 0) hadDrops = true;
        if (stats.framesWritten == 0) continue;

        document::AudioAsset asset;
        asset.id = document::AssetId{sha};
        asset.path = finished->writer.pathOf(i);
        asset.sampleRate = edit_.sampleRate();
        const auto* track = edit_.track(finished->writer.trackIdOf(i));
        if (track == nullptr) {
            hadFailure = true;
            continue;
        }
        asset.channels = track->inputChannelCount;
        asset.lengthSamples = static_cast<int64_t>(stats.framesWritten);

        // One file, one region per punch. The asset is committed with the
        // first region that uses it; the rest reference it.
        bool assetCommitted = false;
        for (const auto& punch : punches) {
            const auto range = application::punchClipRange(
                finished->takeStartSample, asset.lengthSamples, punch,
                finished->latencyOffsetSamples);
            badLatencyCalibration =
                badLatencyCalibration || range.clampedToCapture;
            if (range.length == 0) continue;

            document::AudioClip clip;
            clip.asset = asset.id;
            clip.timelineStart = range.timelineStart;
            clip.sourceOffset = range.sourceOffset;
            clip.length = range.length;

            if (!assetCommitted) {
                undo_.execute(std::make_unique<editing::CommitTakeCommand>(
                    track->id, asset, std::move(clip)));
                assetCommitted = true;
            } else {
                undo_.execute(std::make_unique<editing::AddClipCommand>(
                    track->id, std::move(clip)));
            }
            ++committed;
        }
    }

    if (!audio_.isRunning()) applyAudioPreferences(audioPreferences_, false);
    if (hadFailure) {
        showStatus("Recording stopped, but one or more take files failed", true);
    } else if (badLatencyCalibration) {
        showStatus("Take committed, but the latency offset exceeded its length",
                   true);
    } else if (hadDrops) {
        showStatus("Recording committed with dropout silence; check disk performance",
                   true);
    } else if (committed == 0) {
        // Rolling with a track armed and never pressing Record is the normal
        // way to rehearse, so it is not an error — only an actual attempt to
        // keep something that produced nothing is.
        if (!punches.empty()) {
            showStatus("Recording stopped before any audio was captured", true);
        }
    } else {
        showStatus(committed == 1 ? "Take committed to the timeline"
                                  : std::to_string(committed) +
                                        " takes committed to the timeline");
    }
}

void DaveApp::toggleRecording() {
    if (recordingActive()) punchOut();
    else punchIn();
}

void DaveApp::setTimelineSamplesPerPixel(double samplesPerPixel) {
    // Screenshot fixtures need deterministic zoom without manufacturing input
    // events, while the same bounds remain in force as interactive zooming.
    gui::zoomAroundSample(view_, samplesPerPixel,
                          audio_.transport().position());
}

void DaveApp::configureAutomationScreenshot() {
    if (edit_.tracks().empty()) {
        edit_.addTrack("Track 1");
    }
    const std::string trackId = edit_.tracks().front().id;
    if (auto* points = edit_.volumeAutomation(trackId); points != nullptr) {
        points->clear();
    }
    edit_.addVolumeAutomationPoint(trackId, 0, -18.0);
    edit_.addVolumeAutomationPoint(trackId, 48000, 0.0);
    edit_.addVolumeAutomationPoint(trackId, 96000, -12.0);
    edit_.addVolumeAutomationPoint(trackId, 144000, 3.0);
    if (auto* points = edit_.panAutomation(trackId); points != nullptr) {
        points->clear();
    }
    edit_.addPanAutomationPoint(trackId, 0, -1.0);
    const std::string editablePointId =
        edit_.addPanAutomationPoint(trackId, 48000, 0.0);
    edit_.addPanAutomationPoint(trackId, 96000, 0.75);
    edit_.addPanAutomationPoint(trackId, 144000, -0.4);
    view_.expandedTracks.insert(trackId);
    view_.automationParameters[trackId] = gui::AutomationParameter::Pan;
    view_.activeAutomationParameter = gui::AutomationParameter::Pan;
    view_.revealAutomationOwnerId = trackId;
    view_.editingAutomationValue = !editablePointId.empty();
    view_.focusAutomationValue = !editablePointId.empty();
    view_.automationEditOwnerId = trackId;
    view_.automationEditPointId = editablePointId;
    view_.automationEditValue = 0.0;
}

void DaveApp::configureTransientScreenshot() {
    view_.transientNavigationEnabled = true;
    view_.showTransientTicks = true;
    view_.transientSensitivity = 70;
    openTransientOptions_ = true;
    prefetchSelectedTrackTransients();
}

void DaveApp::onEditChanged() {
    bool armRefused = false;
    if (recordingSession_) {
        const std::unordered_set<std::string> armedIds(
            recordingSession_->armedTrackIds.begin(),
            recordingSession_->armedTrackIds.end());
        for (auto& track : edit_.tracksMut()) {
            const bool shouldBeArmed = armedIds.contains(track.id);
            if (track.recordArm != shouldBeArmed) {
                track.recordArm = shouldBeArmed;
                armRefused = true;
            }
        }
        if (armRefused) {
            showStatus("Track arming cannot change during a recording", true);
        }
    } else if (projectPath_.empty() || loopEnabled_) {
        for (auto& track : edit_.tracksMut()) {
            if (track.recordArm) {
                track.recordArm = false;
                armRefused = true;
            }
        }
        if (armRefused) {
            showStatus(projectPath_.empty()
                           ? "Save the project before arming a track"
                           : "Turn Loop off before arming; loop recording is not available yet",
                       true);
        }
    }

    // UI thread. Re-derive the engine graph from the Edit, compile, publish.
    dirty_ = true; // any edit marks the project dirty
    const double graphSampleRate = audio_.sampleRate() > 0.0
        ? audio_.sampleRate()
        : static_cast<double>(edit_.sampleRate());
    auto graph = builder_.build(
        edit_, graphSampleRate,
        std::max(1, static_cast<int>(audio_.playbackChannelCount())));
    // Compile for the engine's own maximum, not a literal — the callback
    // sizes its passes from the same constant, so the two cannot drift.
    auto [compiled, err] = engine::compile(*graph, graphSampleRate,
                                           platform::AudioEngine::maxBlockSize());
    if (err.has_value()) {
        std::fprintf(stderr, "Dave: compile failed: %s\n", err->message.c_str());
        return;
    }
    audio_.setCompiledGraph(std::move(compiled));

    syncTransportLoop();
}

// The loop range, and whether the transport should honour it.
//
// A timeline selection wins over a Loop marker. Selecting a range and hitting
// Loop is how the gesture is reached in every DAW, and requiring the user to
// first convert that selection into a marker would make the marker an
// obstacle rather than the persistent alternative it is. The marker remains
// the way to keep a loop across sessions — it is in the document; a selection
// is not.
bool DaveApp::loopRange(int64_t& start, int64_t& end) const {
    if (view_.hasSelection) {
        const int64_t lo = std::min(view_.selectionStart, view_.selectionEnd);
        const int64_t hi = std::max(view_.selectionStart, view_.selectionEnd);
        if (hi > lo) { start = lo; end = hi; return true; }
    }
    if (const auto* loop = edit_.activeLoopMarker()) {
        if (loop->length > 0) {
            start = loop->position;
            end = loop->position + loop->length;
            return true;
        }
    }
    return false;
}

void DaveApp::syncTransportLoop() {
    auto& transport = audio_.transport();
    int64_t start = 0;
    int64_t end = 0;
    if (loopEnabled_ && loopRange(start, end)) {
        transport.setLoop(start, end);
    } else {
        transport.clearLoop();
    }
}

bool DaveApp::loadWavIntoEdit(const std::string& path) {
    auto assetId = edit_.importAsset(path);
    if (!assetId.valid()) {
        std::fprintf(stderr, "Dave: import failed: %s\n", path.c_str());
        return false;
    }
    const auto* asset = edit_.asset(assetId);
    if (asset == nullptr) {
        std::fprintf(stderr, "Dave: asset lookup failed after import\n");
        return false;
    }

    // Ensure at least one track exists.
    if (edit_.tracks().empty()) {
        undo_.execute(std::make_unique<editing::AddTrackCommand>("Track 1"));
    }
    const auto& trackId = edit_.tracks().front().id;

    document::AudioClip clip;
    clip.asset = assetId;
    // Place the clip at position 0 if the track is empty (predictable: a fresh
    // load goes to the start). This avoids the "clip landed at some far-off
    // playhead position and I can't see it" confusion.
    int64_t placeAt = 0;
    const auto* track = edit_.track(trackId);
    if (track) {
        for (const auto& c : track->clips) {
            placeAt = std::max(placeAt, c.timelineStart + c.length);
        }
    }
    clip.timelineStart = placeAt;
    clip.sourceOffset = 0;
    clip.length = asset->lengthSamples;
    clip.fadeIn = 64;
    clip.fadeOut = 64;
    undo_.execute(std::make_unique<editing::AddClipCommand>(trackId, clip));

    // Make the clip immediately playable/visible: seek to its start and reset
    // the timeline scroll so the clip is on screen.
    audio_.transport().seek(placeAt);
    view_.scrollSamples = 0;

    std::fprintf(stderr, "Dave: loaded %s (%.1fs, %dch) at pos %lld\n",
                 path.c_str(),
                 asset->lengthSamples / static_cast<double>(asset->sampleRate),
                 asset->channels,
                 static_cast<long long>(placeAt));
    return true;
}

bool DaveApp::loadWavIntoNewTrack(const std::string& path) {
    auto assetId = edit_.importAsset(path);
    if (!assetId.valid()) {
        std::fprintf(stderr, "Dave: dropped WAV import failed: %s\n", path.c_str());
        return false;
    }
    const auto* asset = edit_.asset(assetId);
    if (asset == nullptr) {
        std::fprintf(stderr, "Dave: dropped WAV lookup failed after import\n");
        return false;
    }

    // A drop is an insertion gesture, not an append request: its own track
    // keeps the imported clip visible and immediately editable.
    undo_.execute(std::make_unique<editing::AddTrackCommand>("Audio"));
    const auto& trackId = edit_.tracks().back().id;
    document::AudioClip clip;
    clip.asset = assetId;
    clip.timelineStart = 0;
    clip.sourceOffset = 0;
    clip.length = asset->lengthSamples;
    clip.fadeIn = 64;
    clip.fadeOut = 64;
    undo_.execute(std::make_unique<editing::AddClipCommand>(trackId, clip));
    view_.selectedTrackIndex = static_cast<int>(edit_.tracks().size()) - 1;
    view_.scrollSamples = 0;
    audio_.transport().seek(0);
    std::fprintf(stderr, "Dave: dropped %s into a new track\n", path.c_str());
    return true;
}

void DaveApp::drawUI() {
    auto& transport = audio_.transport();
    const auto& pal = gui::theme::palette();
    // Cleared before drawing and set by whichever tool draws its own cursor.
    view_.wantsHiddenCursor = false;

    // Capture follows the transport. Watching the edge here rather than
    // hooking every place that can start or stop playback means Space, the
    // menu, the toolbar and a script all behave the same — and that a punch
    // is possible for as long as the transport is rolling, because the audio
    // has been going to disk since it started.
    {
        const bool rolling = transport.isPlaying();
        if (rolling && !transportWasRolling_) {
            beginCaptureIfArmed();
        } else if (!rolling && transportWasRolling_) {
            endCapture();
        }
        transportWasRolling_ = rolling;
    }

    if (builder_.latencyChangePending() && !recordingActive()) {
        // VST3 requires deactivation before its new latency is queried. Stop
        // the device first so the UI thread never mutates a processor while
        // the callback is inside process().
        const double deviceRate = audio_.sampleRate();
        const int deviceOutputs = std::max(
            1, static_cast<int>(audio_.playbackChannelCount()));
        audio_.stop();
        const bool latencyChanged = builder_.consumeLatencyChange();
        auto graph = builder_.build(edit_, deviceRate, deviceOutputs);
        auto [compiled, error] = engine::compile(
            *graph, deviceRate, platform::AudioEngine::maxBlockSize());
        if (error) {
            std::fprintf(stderr, "Dave: latency rebuild refused: %s\n",
                         error->message.c_str());
        } else {
            audio_.setCompiledGraph(std::move(compiled));
        }
        if (latencyChanged) applyAudioPreferences(audioPreferences_, false);
    }
    // The growing region follows the open punch, not the capture. Capture
    // has been running since the transport rolled; drawing from there would
    // claim to be keeping audio the user never asked for.
    if (recordingActive()) {
        const auto& punch = recordingSession_->punches.back();
        const int64_t offset = recordingSession_->latencyOffsetSamples;
        view_.recordingStartSample = std::max<int64_t>(0, punch.in - offset);
        view_.recordingEndSample = std::max(
            view_.recordingStartSample,
            std::max<int64_t>(0, transport.position() - offset));
    }

    // Pop-out toggles land here, at the frame boundary, so the sidebar split
    // and the picture window agree for the whole frame.
    if (videoPopoutRequest_) {
        videoPoppedOut_ = videoPopoutRequestValue_;
        videoPopoutRequest_ = false;
    }

    // ─── Window title ────────────────────────────────────────────────────
    {
        std::string title = "Dave";
        if (!projectPath_.empty()) {
            auto slash = projectPath_.find_last_of("/\\");
            title += " — " + ((slash != std::string::npos) ? projectPath_.substr(slash + 1) : projectPath_);
        } else {
            title += " — Untitled";
        }
        if (dirty_) title += " *";
        glfwSetWindowTitle(window_.handle(), title.c_str());
    }

    // ─── Layout constants ────────────────────────────────────────────────
    // Utility panes retain their explicit splitter sizes. The arrangement is
    // the flexible editor surface and absorbs native window resize deltas.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // WorkPos already excludes an in-window menu bar on platforms that use
    // one; adding that offset again would push the entire panel stack down.
    const float menuH = 0.0f;
    const float baseX = vp->WorkPos.x;
    const float baseY = vp->WorkPos.y;
    const float toolbarH = 48.0f;
    const float splitterSize = 6.0f;
    const float contentY = baseY + menuH + toolbarH;
    const float contentH = vp->WorkSize.y - toolbarH;
    constexpr float minEditorW = 320.0f;
    constexpr float minEditorH = 120.0f;
    // Picture is opt-in and so is the strip; with neither showing there is no
    // sidebar to size, and the timeline takes the whole window.
    const bool sidebarVisible =
        showChannelStrip_ || (!edit_.videoTracks().empty() && !videoPoppedOut_);
    const auto mainLayout = application::calculateMainEditorLayout(
        vp->WorkSize.x, contentH, splitterSize, sidebarWidth_, mixerHeight_,
        showMixer_, minEditorW, minEditorH, sidebarVisible);
    const float sidebarW = mainLayout.sidebarWidth;
    const float timelineW = mainLayout.editorWidth;
    const float sidebarX = baseX + timelineW + splitterSize;
    const float availablePanelW =
        std::max(0.0f, vp->WorkSize.x - splitterSize);
    const float maxSidebarW =
        std::max(0.0f, availablePanelW - minEditorW);
    const float minSidebarW = std::min(220.0f, maxSidebarW);

    // Hardware I/O is global and always starts the sidebar stack. Its height
    // is responsive only within a narrow useful range; the document panels
    // divide whatever remains below it.
    // I/O moved into Preferences, so the sidebar stack starts at the top.
    constexpr float ioPanelH = 0.0f;
    constexpr float ioGap = 0.0f;
    const float sidebarContentY = contentY + ioPanelH + ioGap;
    const float sidebarContentH =
        std::max(1.0f, contentH - ioPanelH - ioGap);
    const float splitContentH =
        std::max(1.0f, sidebarContentH - splitterSize);
    const float minVideoH = std::min(260.0f, splitContentH * 0.55f);
    const float minPluginsH = std::min(180.0f, splitContentH * 0.38f);
    const float maxVideoH = std::max(minVideoH, splitContentH - minPluginsH);
    // Picture is opt-in. Until a video is imported there is nothing to show,
    // so the sidebar is the plugin chain alone rather than a large empty
    // panel advertising a workflow this session isn't using.
    const bool hasVideo = !edit_.videoTracks().empty();
    const bool videoDocked = hasVideo && !videoPoppedOut_;
    // With the picture popped out or absent there is no split to maintain: the
    // plugin chain takes the whole sidebar and videoHeight_ is preserved
    // untouched so the old proportions come back when the picture returns.
    const float videoPanelH =
        videoDocked
            ? std::clamp(videoHeight_, minVideoH, maxVideoH)
            : 0.0f;
    const float pluginsH =
        videoDocked ? splitContentH - videoPanelH : sidebarContentH;

    // Common flags for all docked panels — nothing can move or overlap.
    const ImGuiWindowFlags panelFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking;

    // ─── Main menu bar (Windows/Linux only — macOS uses native screen-top) ─
#ifndef __APPLE__
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New", "Ctrl+N")) newProject();
            if (ImGui::MenuItem("Open...", "Ctrl+Shift+O")) openProjectDialog();
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S", false, !projectPath_.empty())) saveProject(false);
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) saveProject(true);
            ImGui::Separator();
            if (ImGui::MenuItem("Load WAV...", "Ctrl+O")) openWavDialog();
            if (ImGui::MenuItem("Import MIDI...")) openMidiDialog();
            if (ImGui::MenuItem("Load Video...")) openVideoDialog();
            ImGui::Separator();
            if (ImGui::MenuItem("Import Markers (Reaper CSV)...")) importMarkersDialog();
            if (ImGui::MenuItem("Export Markers (Reaper CSV)...")) exportMarkersDialog();
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Q")) window_.close();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false,
                                undo_.canUndo() && !recordingActive()))
                undo_.undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false,
                                undo_.canRedo() && !recordingActive()))
                undo_.redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Add Track", "Ctrl+Shift+N"))
                undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));
            if (ImGui::MenuItem("Preferences...", "Ctrl+,")) {
                showPreferences_ = true;
                preferencesJustOpened_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Add Bus", "Ctrl+Shift+B"))
                undo_.execute(std::make_unique<editing::AddTrackCommand>("Bus", editing::AddTrackCommand::Flavour::Bus));
            ImGui::Separator();
            if (ImGui::MenuItem("Transient Navigation", "Ctrl+Alt+T",
                                view_.transientNavigationEnabled)) {
                toggleTransientNavigation();
            }
            if (ImGui::MenuItem("Next Transient or Boundary", "Tab")) {
                navigateTimeline(gui::NavigationDirection::Next, false);
            }
            if (ImGui::MenuItem("Previous Transient or Boundary", "Ctrl+Tab")) {
                navigateTimeline(gui::NavigationDirection::Previous, false);
            }
            if (ImGui::MenuItem("Extend Selection to Next", "Shift+Tab")) {
                navigateTimeline(gui::NavigationDirection::Next, true);
            }
            if (ImGui::MenuItem("Extend Selection to Previous",
                                "Ctrl+Shift+Tab")) {
                navigateTimeline(gui::NavigationDirection::Previous, true);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Transport")) {
            if (ImGui::MenuItem(transport.isPlaying() ? "Stop" : "Play", "Space"))
                recordingActive() ? stopRecording() : transport.toggle();
            if (ImGui::MenuItem(recordingActive() ? "Stop Recording" : "Record",
                                "Ctrl+R"))
                toggleRecording();
            if (ImGui::MenuItem("Return to Start", "Return")) transport.seek(0);
            if (ImGui::MenuItem("Stop & Rewind")) { transport.stop(); transport.seek(0); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Mixer", "Ctrl+=", showMixer_))
                showMixer_ = !showMixer_;
            if (ImGui::MenuItem("Channel Strip", nullptr, showChannelStrip_)) {
                showChannelStrip_ = !showChannelStrip_;
                if (showChannelStrip_ && pluginHost_.descriptors().empty()) {
                    pluginHost_.scan();
                }
            }
            if (ImGui::MenuItem("Audio I/O...")) {
                showPreferences_ = true;
                preferencesJustOpened_ = true;
            }
            if (ImGui::MenuItem("Video Window", "Ctrl+Shift+V",
                                videoPoppedOut_ && hasVideo, hasVideo))
                setVideoPoppedOut(!videoPoppedOut_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::TextDisabled("Dave — D.A.V.E.");
            ImGui::TextDisabled(
                "Scroll: pan | Cmd/Ctrl+scroll: zoom | R-click clip: menu");
            ImGui::TextDisabled(
                "Cmd/Ctrl+R: record | Shift+R/M/S: arm / mute / solo");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
#endif // __APPLE__

    // ─── Transport bar (full width, below menu) ──────────────────────────
    ImGui::SetNextWindowPos(ImVec2(baseX, baseY + menuH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, toolbarH), ImGuiCond_Always);
    // One row of 30 px controls inside a 48 px bar: the 14 px window padding
    // the panels use would need 58 and ImGui answers that with a scrollbar.
    // The vertical padding is derived from the two so the row is centred, and
    // scrollbars are off outright — a fixed-height toolbar has nowhere to
    // scroll to, so one appearing is always a layout bug rather than a
    // feature.
    const float kToolbarControlH = 30.0f;
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(14.0f, std::max(0.0f, (toolbarH - kToolbarControlH) * 0.5f)));
    ImGui::Begin("Transport", nullptr,
                 panelFlags | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    {
        const float controlH = kToolbarControlH;
        const bool compactToolbar = vp->WorkSize.x < 1050.0f;
        // Every framed control derives its height from FramePadding, so one
        // value here makes buttons, combos and the checkbox agree instead of
        // each settling at whatever its own content implies.
        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(10.0f,
                   std::max(0.0f, (controlH - ImGui::GetFontSize()) * 0.5f)));
        // Two gaps, used consistently: tight between a label and the control
        // it names, wider between separate controls.
        const float kGap = compactToolbar ? 5.0f : 8.0f;
        constexpr float kLabelGap = 6.0f;
        const float groupGap = compactToolbar ? 7.0f : 12.0f;
        // Labels sit on the text baseline by default, which floats them to
        // the top of a 30 px row.
        auto label = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(pal.textMuted, "%s", text);
            ImGui::SameLine(0.0f, kLabelGap);
        };
        auto groupSeparator = [&] {
            ImGui::SameLine(0.0f, groupGap);
            const ImVec2 lineTop = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(1.0f, controlH));
            ImGui::GetWindowDrawList()->AddLine(
                lineTop, ImVec2(lineTop.x, lineTop.y + controlH),
                ImGui::GetColorU32(pal.borderStrong));
            ImGui::SameLine(0.0f, groupGap);
        };

        const bool wasRecording = recordingActive();
        if (gui::theme::iconButton(
                "##transportRecord", gui::theme::TransportIcon::Record,
                wasRecording ? "Stop recording (Cmd/Ctrl+R)"
                             : "Record and roll (Cmd/Ctrl+R)",
                ImVec2(controlH, controlH),
                wasRecording ? gui::theme::ButtonVariant::Danger
                             : gui::theme::ButtonVariant::Normal)) {
            toggleRecording();
        }

        // Right-click the record button for its settings. A separate toolbar
        // button would cost width the row does not have, and the naming
        // pattern is the only thing here to configure.
        // Right-click still reaches the take-naming setting, but Preferences
        // owns it now — two editors for one value is how they drift apart.
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            showPreferences_ = true;
            preferencesJustOpened_ = true;
        }
        if (ImGui::IsItemHovered() && !recordingActive()) {
            ImGui::SetTooltip("Record and roll (Cmd/Ctrl+R)\n"
                              "Right-click for take file names");
        }

        ImGui::SameLine(0.0f, kGap);
        const bool wasPlaying = transport.isPlaying();
        if (gui::theme::iconButton(
                "##transportPlay",
                wasPlaying ? gui::theme::TransportIcon::Stop
                           : gui::theme::TransportIcon::Play,
                wasPlaying ? "Stop (Space)" : "Play (Space)",
                ImVec2(controlH, controlH),
                wasPlaying ? gui::theme::ButtonVariant::Primary
                           : gui::theme::ButtonVariant::Normal)) {
            transport.toggle();
        }

        ImGui::SameLine(0.0f, kGap);
        if (gui::theme::iconButton(
                "##transportRewind", gui::theme::TransportIcon::ReturnToStart,
                "Stop and return to start (Return)", ImVec2(controlH, controlH))) {
            transport.stop();
            transport.seek(0);
        }

        ImGui::SameLine(0.0f, kGap);
        {
            int64_t loopLo = 0, loopHi = 0;
            const bool haveRange = loopRange(loopLo, loopHi);
            const char* loopTip = haveRange
                ? "Loop playback over the selection (L)"
                : "Loop playback (L) — select a range, or add a loop marker, "
                  "to give it something to loop";
            if (gui::theme::iconButton(
                    "##transportLoop", gui::theme::TransportIcon::Loop,
                    loopTip, ImVec2(controlH, controlH),
                    loopEnabled_ ? gui::theme::ButtonVariant::Primary
                                 : gui::theme::ButtonVariant::Normal)) {
                const bool anyArmed = std::any_of(
                    edit_.tracks().begin(), edit_.tracks().end(),
                    [](const document::Track& track) {
                        return track.recordArm;
                    });
                if (!loopEnabled_ && anyArmed) {
                    showStatus("Disarm recording tracks before enabling Loop",
                               true);
                } else {
                    loopEnabled_ = !loopEnabled_;
                    syncTransportLoop();
                }
            }
        }
        groupSeparator();

        // Position readout — double-click to edit (type a timecode to seek).
        int64_t pos = transport.position();
        const char* tcModes[] = {"min:sec", "timecode", "bars|beats", "feet+frames", "samples"};
        int tcIdx = static_cast<int>(view_.tcMode);
        const float counterW = compactToolbar ? 132.0f : 150.0f;
        if (!view_.editingPosition) {
            std::string tcStr = gui::formatTimecode(
                pos, view_.tcMode, static_cast<double>(edit_.sampleRate()),
                24.0, edit_.tempoBpm(), &edit_.meterMap(),
                &edit_.tempoMap());
            const ImVec2 counterMin = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##positionCounter",
                                   ImVec2(counterW, controlH));
            ImFont* counterFont = gui::theme::fonts().monoLarge != nullptr
                ? gui::theme::fonts().monoLarge : ImGui::GetFont();
            constexpr float counterFontSize = 20.0f;
            const ImVec2 textSize =
                counterFont->CalcTextSizeA(counterFontSize, FLT_MAX, 0.0f,
                                           tcStr.c_str());
            ImGui::GetWindowDrawList()->PushClipRect(
                counterMin,
                ImVec2(counterMin.x + counterW,
                       counterMin.y + controlH),
                true);
            ImGui::GetWindowDrawList()->AddText(
                counterFont, counterFontSize,
                ImVec2(counterMin.x + (counterW - textSize.x) * 0.5f,
                       counterMin.y + (controlH - textSize.y) * 0.5f),
                ImGui::GetColorU32(pal.accent), tcStr.c_str());
            ImGui::GetWindowDrawList()->PopClipRect();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                view_.editingPosition = true;
                // Pre-fill the input with the current timecode.
                std::strncpy(view_.positionInput, tcStr.c_str(), sizeof(view_.positionInput) - 1);
                view_.positionInput[sizeof(view_.positionInput) - 1] = '\0';
            }
        } else {
            // Editable input. Enter = seek, Escape = cancel.
            ImGui::SetNextItemWidth(counterW);
            ImGui::SetKeyboardFocusHere();
            if (gui::theme::fonts().monoSmall != nullptr) {
                ImGui::PushFont(gui::theme::fonts().monoSmall, 13.0f);
            }
            if (ImGui::InputText("##posedit", view_.positionInput,
                                 sizeof(view_.positionInput),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                // Parse the timecode back to samples based on the current mode.
                int64_t target = -1;
                double sr = static_cast<double>(edit_.sampleRate());
                switch (view_.tcMode) {
                    case gui::TimecodeMode::MinSec: {
                        int mm; double ss;
                        if (std::sscanf(view_.positionInput, "%d:%lf", &mm, &ss) == 2)
                            target = static_cast<int64_t>((mm * 60 + ss) * sr);
                        break;
                    }
                    case gui::TimecodeMode::Smpte: {
                        int hh, mm, ss2, ff;
                        if (std::sscanf(view_.positionInput, "%d:%d:%d:%d", &hh, &mm, &ss2, &ff) == 4)
                            target = static_cast<int64_t>((hh * 3600 + mm * 60 + ss2 + ff / 24.0) * sr);
                        break;
                    }
                    case gui::TimecodeMode::BarsBeats: {
                        int bars = 1;
                        int beats = 1;
                        int ticks = 0;
                        // Both separators. The display uses pipes, but a dot
                        // is what a numeric keypad can type without a
                        // modifier, and refusing it would be pedantry.
                        const bool parsed =
                            std::sscanf(view_.positionInput, "%d|%d|%d",
                                        &bars, &beats, &ticks) == 3 ||
                            std::sscanf(view_.positionInput, "%d.%d.%d",
                                        &bars, &beats, &ticks) == 3;
                        if (parsed) {
                            // Through the maps, so typing a bar goes where
                            // the ruler says that bar is — the old arithmetic
                            // assumed 120 bpm in 4/4 and would disagree with
                            // the counter it was typed into.
                            target = document::sampleAtBarsBeats(
                                document::BarsBeats{bars, beats, ticks}, sr,
                                edit_.tempoMap(), edit_.meterMap());
                        }
                        break;
                    }
                    case gui::TimecodeMode::FeetFrames: {
                        int feet, ff2;
                        if (std::sscanf(view_.positionInput, "%d+%d", &feet, &ff2) == 2)
                            target = static_cast<int64_t>((feet * 16 + ff2) / 24.0 * sr);
                        break;
                    }
                    case gui::TimecodeMode::Samples: {
                        long long s;
                        if (std::sscanf(view_.positionInput, "%lld", &s) == 1)
                            target = s;
                        break;
                    }
                }
                if (target >= 0) transport.seek(target);
                view_.editingPosition = false;
            }
            if (gui::theme::fonts().monoSmall != nullptr) {
                ImGui::PopFont();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                view_.editingPosition = false;
        }
        // The format is a property of the counter beside it, so it hangs off
        // the counter as a disclosure arrow rather than restating the current
        // format in a combo as wide as the readout itself. The counter
        // already shows which format is in use — spelling it out twice was
        // the widest thing in the toolbar.
        // A caret, not a button. The counter beside it is a 20 px readout with
        // no frame of its own; putting a boxed control against it made the
        // format look like a second, equally important thing rather than a
        // property of the number.
        ImGui::SameLine(0.0f, 1.0f);
        const float arrowW = 14.0f;
        const ImVec2 caretMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tcmode", ImVec2(arrowW, controlH));
        const bool caretHovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            ImGui::OpenPopup("##tcmodeMenu");
        }
        {
            const float cx = caretMin.x + arrowW * 0.5f;
            // Sits on the counter's baseline rather than the control's centre,
            // so it reads as attached to the digits.
            const float cy = caretMin.y + controlH * 0.5f + 3.0f;
            constexpr float half = 3.5f;
            ImGui::GetWindowDrawList()->AddTriangleFilled(
                ImVec2(cx - half, cy - half * 0.6f),
                ImVec2(cx + half, cy - half * 0.6f),
                ImVec2(cx, cy + half * 0.8f),
                ImGui::GetColorU32(caretHovered ? pal.text : pal.textMuted));
        }
        if (caretHovered) {
            ImGui::SetTooltip("Counter format (%s)", tcModes[tcIdx]);
        }
        // Tempo and meter live on the counter too, because they are what
        // bars|beats MEANS — putting them in Preferences would separate the
        // reading from the thing that decides it.
        if (view_.tcMode == gui::TimecodeMode::BarsBeats) {
            ImGui::SameLine(0.0f, kGap);
            const auto& map = edit_.meterMap();
            const auto atStart = document::signatureAtBar(map, 1);
            char meterLabel[48];
            std::snprintf(meterLabel, sizeof(meterLabel), "%.0f  %d/%d",
                          edit_.tempoBpm(), atStart.numerator,
                          atStart.denominator);
            if (ImGui::Button(meterLabel, ImVec2(0.0f, controlH))) {
                ImGui::OpenPopup("##meterMenu");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Tempo and time signature");
            }
            if (ImGui::BeginPopup("##meterMenu")) {
                drawMeterEditor();
                ImGui::EndPopup();
            }
        }
        if (ImGui::BeginPopup("##tcmodeMenu")) {
            for (int i = 0; i < 5; ++i) {
                if (ImGui::MenuItem(tcModes[i], nullptr, tcIdx == i)) {
                    view_.tcMode = static_cast<gui::TimecodeMode>(i);
                }
            }
            ImGui::EndPopup();
        }
        groupSeparator();

        // No +Track here: the timeline carries a + above its topmost row,
        // which is where a track gets added and where the new one appears.
        ImGui::Checkbox("Snap", &view_.snapEnabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Snap timeline edits to the current %s grid",
                              tcModes[tcIdx]);
        }

        ImGui::SameLine(0.0f, kGap);
        if (gui::theme::iconButton(
                "##transientNavigation",
                gui::theme::TransportIcon::Transient,
#ifdef __APPLE__
                "Transient navigation (Cmd+Option+Tab)\n"
                "Tab: next  Option+Tab: previous\n"
                "Add Shift to extend the selection",
#else
                "Transient navigation (Ctrl+Alt+T)\n"
                "Tab: next  Ctrl+Tab: previous\n"
                "Add Shift to extend the selection",
#endif
                ImVec2(controlH, controlH),
                view_.transientNavigationEnabled
                    ? gui::theme::ButtonVariant::Primary
                    : gui::theme::ButtonVariant::Normal)) {
            toggleTransientNavigation();
        }
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("...##transientOptions",
                          ImVec2(controlH, controlH))) {
            ImGui::OpenPopup("Transient options");
        }
        if (openTransientOptions_) {
            ImGui::OpenPopup("Transient options");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Transient display and sensitivity");
        }
        if (ImGui::BeginPopup("Transient options")) {
            openTransientOptions_ = false;
            ImGui::TextUnformatted("Transient Navigation");
            ImGui::Separator();
            bool showTicks = view_.showTransientTicks;
            if (ImGui::Checkbox("Show transient ticks", &showTicks)) {
                view_.showTransientTicks = showTicks;
                saveEditorPreferences();
            }
            ImGui::SetNextItemWidth(190.0f);
            ImGui::SliderInt("Sensitivity", &view_.transientSensitivity,
                             0, 100, "%d");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                saveEditorPreferences();
            }
            ImGui::TextDisabled("Higher values include softer attacks.");
            const size_t pendingAnalyses = transientAnalyses_.pendingCount();
            if (pendingAnalyses > 0) {
                ImGui::TextColored(pal.warning, "Analyzing transients...");
            } else {
                ImGui::TextDisabled("Analysis ready when audio is selected.");
            }
            ImGui::EndPopup();
        }

        ImGui::PopStyleVar();   // FramePadding
    }
    ImGui::End();

    // ─── Timeline + mixer (left column, split horizontally) ──────────────
    // The mixer takes the bottom of the timeline column rather than a slot in
    // the sidebar: strips sit side by side, so the panel needs width, and the
    // sidebar is a ~360px inspector column.
    const float minMixerH = std::min(
        190.0f, std::max(0.0f, contentH - splitterSize - minEditorH));
    const float maxMixerH =
        std::max(minMixerH, contentH - splitterSize - minEditorH);
    const float mixerH = mainLayout.mixerHeight;
    const float timelineH = mainLayout.timelineHeight;

    ImGui::SetNextWindowPos(ImVec2(baseX, contentY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(timelineW, timelineH), ImGuiCond_Always);
    ImGui::Begin("Timeline", nullptr, panelFlags);
    gui::theme::panelHeader("Timeline");
    // Taller windows can afford more waveform detail; the gutter itself
    // computes a content minimum so both values remain overlap-safe.
    // PTXExtractor's playlist lane height. drawTimeline raises this if the
    // gutter's gain and pan sliders need more room — PTXExtractor gets away
    // with 58 because its track controls are read-only indicators, where
    // Dave's are draggable — so the result is PTX's density wherever the
    // controls allow it, rather than a fixed height picked per window size.
    const float trackRowHeight = 58.0f;
    if (view_.transientNavigationEnabled || view_.showTransientTicks ||
        pendingTransientNavigation_) {
        prefetchSelectedTrackTransients();
    }
    servicePendingTransientNavigation();
    const auto transientSnapshots = selectedTrackTransientSnapshots();
    gui::drawTimeline(
        edit_, undo_, transport, peaks_, view_, builder_.assetBuffers(),
        trackRowHeight, 30.0f, transientSnapshots, &builder_.trackGains());
    if (view_.requestTransientNavigation) {
        const auto direction = view_.transientNavigationDirection;
        const bool extend = view_.requestTransientSelectionExtension;
        view_.requestTransientNavigation = false;
        view_.requestTransientSelectionExtension = false;
        navigateTimeline(direction, extend);
    }
    ImGui::End();

    // ─── Mixer (bottom of the timeline column) ───────────────────────────
    if (showMixer_) {
        const float mixerY = contentY + timelineH + splitterSize;
        ImGui::SetNextWindowPos(ImVec2(baseX, mixerY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(timelineW, mixerH), ImGuiCond_Always);
        ImGui::Begin("Mixer", nullptr, panelFlags);
        if (panelHeaderAction("Mixer", "Hide",
                              "Hide the mixer (View > Mixer to bring it back)")) {
            showMixer_ = false;
        }
        gui::drawMixer(edit_, undo_, view_, 124.0f,
                       static_cast<int>(audio_.captureChannelCount()),
                       static_cast<int>(audio_.playbackChannelCount()),
                       &builder_.trackGains());
        ImGui::End();
    }
    // Both the timeline and the mixer can ask for a picker or an editor, so
    // the requests are serviced once, after both have drawn.
    if (view_.meterOptionsChanged) {
        view_.meterOptionsChanged = false;
        saveEditorPreferences();
    }
    // Pushed every frame rather than on change: the graph is rebuilt from the
    // document on every edit, and a fresh node would otherwise come back with
    // the default hold instead of the one the user chose.
    const float hold = view_.meterOptions.peakHoldSeconds;
    for (const auto& [id, node] : builder_.trackGains()) {
        if (node) node->setPeakHoldSeconds(hold);
    }
    for (const auto& [id, node] : builder_.meterTaps()) {
        if (node) node->setPeakHoldSeconds(hold);
    }
    serviceViewRequests();
    // The loop follows the live selection, which the timeline has just
    // finished updating — so this runs every frame rather than only on
    // document changes. Dragging a new range while looping re-points the loop
    // at it without a second gesture.
    syncTransportLoop();

    // Picture stays above the plugin chain because sync-to-picture is the
    // primary post-production task — unless it has been popped out or never
    // imported, in which case the sidebar slot disappears entirely rather
    // than leaving a hole.
    if (videoDocked) {
        videoPanelPos_ = ImVec2(sidebarX, sidebarContentY);
        videoPanelSize_ = ImVec2(sidebarW, videoPanelH);
        ImGui::SetNextWindowPos(videoPanelPos_, ImGuiCond_Always);
        ImGui::SetNextWindowSize(videoPanelSize_, ImGuiCond_Always);
        ImGui::Begin("Video", nullptr, panelFlags);
        if (panelHeaderAction("Video", "Pop Out",
                              "Detach the picture into its own window "
                              "(drag it to another display)")) {
            setVideoPoppedOut(true);
        }
        drawVideoPreviewContent();
        ImGui::End();
    }

    if (showChannelStrip_) {
        ImGui::SetNextWindowPos(
            ImVec2(sidebarX,
                   videoDocked ? sidebarContentY + videoPanelH + splitterSize
                               : sidebarContentY),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(sidebarW, pluginsH), ImGuiCond_Always);
        ImGui::Begin("Channel Strip", nullptr, panelFlags);
        // No panel header: the strip's own coloured header already names the
        // track, and a second title above it said only what the panel is —
        // which the E button that opened it had just answered. Closing is the
        // same E button, or View > Channel Strip.
        gui::drawChannelStrip(edit_, undo_, view_, channelStrip_,
                              static_cast<int>(audio_.captureChannelCount()),
                              static_cast<int>(audio_.playbackChannelCount()),
                              recordingActive(), pluginHost_.descriptors(),
                              &builder_.meterTaps(), &view_.meterOptions);
        ImGui::End();
    }

    const ImGuiWindowFlags splitterFlags =
        panelFlags | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (sidebarVisible) {
    ImGui::SetNextWindowPos(
        ImVec2(baseX + timelineW, contentY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(splitterSize, contentH), ImGuiCond_Always);
    ImGui::Begin("##timelineSidebarSplitter", nullptr, splitterFlags);
    const ImVec2 sidebarSplitterOrigin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(
        "##dragTimelineSidebar", ImVec2(splitterSize, contentH));
    const bool sidebarSplitterHovered =
        ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (sidebarSplitterHovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActive()) {
        sidebarWidth_ = std::clamp(
            sidebarWidth_ - ImGui::GetIO().MouseDelta.x,
            minSidebarW, maxSidebarW);
    }
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(sidebarSplitterOrigin.x + splitterSize * 0.5f,
               sidebarSplitterOrigin.y),
        ImVec2(sidebarSplitterOrigin.x + splitterSize * 0.5f,
               sidebarSplitterOrigin.y + contentH),
        ImGui::GetColorU32(sidebarSplitterHovered
            ? pal.accent : pal.borderStrong), 1.0f);
    ImGui::End();
    }

    // Drag the timeline/mixer split. Absent when the mixer is hidden — there
    // is nothing to size.
    if (showMixer_) {
        ImGui::SetNextWindowPos(
            ImVec2(baseX, contentY + timelineH), ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(timelineW, splitterSize), ImGuiCond_Always);
        ImGui::Begin("##timelineMixerSplitter", nullptr, splitterFlags);
        const ImVec2 mixerSplitterOrigin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(
            "##dragTimelineMixer", ImVec2(timelineW, splitterSize));
        const bool mixerSplitterHovered =
            ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (mixerSplitterHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        if (ImGui::IsItemActive()) {
            mixerHeight_ = std::clamp(
                mixerHeight_ - ImGui::GetIO().MouseDelta.y,
                minMixerH, maxMixerH);
        }
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(mixerSplitterOrigin.x,
                   mixerSplitterOrigin.y + splitterSize * 0.5f),
            ImVec2(mixerSplitterOrigin.x + timelineW,
                   mixerSplitterOrigin.y + splitterSize * 0.5f),
            ImGui::GetColorU32(mixerSplitterHovered
                ? pal.accent : pal.borderStrong), 1.0f);
        ImGui::End();
    }

    // No picture in the sidebar means no split to drag.
    if (videoDocked) {
        ImGui::SetNextWindowPos(
            ImVec2(sidebarX, sidebarContentY + videoPanelH), ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(sidebarW, splitterSize), ImGuiCond_Always);
        ImGui::Begin("##videoPluginsSplitter", nullptr, splitterFlags);
        const ImVec2 panelSplitterOrigin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(
            "##dragVideoPlugins", ImVec2(sidebarW, splitterSize));
        const bool panelSplitterHovered =
            ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (panelSplitterHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        if (ImGui::IsItemActive()) {
            videoHeight_ = std::clamp(
                videoPanelH + ImGui::GetIO().MouseDelta.y,
                minVideoH, maxVideoH);
        }
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(panelSplitterOrigin.x,
                   panelSplitterOrigin.y + splitterSize * 0.5f),
            ImVec2(panelSplitterOrigin.x + sidebarW,
                   panelSplitterOrigin.y + splitterSize * 0.5f),
            ImGui::GetColorU32(panelSplitterHovered
                ? pal.accent : pal.borderStrong), 1.0f);
        ImGui::End();
    }
    ImGui::PopStyleVar(2);

    // The detached picture, drawn after the panels so it stacks above them
    // while it is still merged into the main window.
    drawVideoPopoutWindow();

    // Modals (browser) still float — they're temporary popups, not panels.
    drawPluginBrowser();
    drawPreferencesWindow();

    if (!statusMessage_.empty() && ImGui::GetTime() < statusUntil_) {
        ImDrawList* foreground = ImGui::GetForegroundDrawList();
        const ImVec2 textSize = ImGui::CalcTextSize(statusMessage_.c_str());
        const ImVec2 padding(12.0f, 8.0f);
        const ImVec2 boxSize(textSize.x + padding.x * 2.0f,
                             textSize.y + padding.y * 2.0f);
        const ImVec2 boxMin(
            vp->WorkPos.x + (vp->WorkSize.x - boxSize.x) * 0.5f,
            vp->WorkPos.y + vp->WorkSize.y - boxSize.y - 18.0f);
        const ImVec2 boxMax(boxMin.x + boxSize.x, boxMin.y + boxSize.y);
        foreground->AddRectFilled(
            boxMin, boxMax,
            ImGui::GetColorU32(statusIsError_ ? pal.danger
                                              : pal.surfaceStrong),
            5.0f);
        foreground->AddRect(boxMin, boxMax,
                            ImGui::GetColorU32(statusIsError_ ? pal.danger
                                                             : pal.borderStrong),
                            5.0f);
        foreground->AddText(ImVec2(boxMin.x + padding.x,
                                   boxMin.y + padding.y),
                            ImGui::GetColorU32(statusIsError_
                                                  ? ImVec4(1, 1, 1, 1)
                                                  : pal.text),
                            statusMessage_.c_str());
    }

    // ─── Auto-stop ───────────────────────────────────────────────────────
    // Only auto-stop if there's actual content (clips) on the timeline. An
    // empty timeline has contentEnd=24000 (just the 0.5s tail), which would
    // instantly stop playback before the user can do anything.
    if (audio_.transport().isPlaying() && !recordingActive()) {
        int64_t end = edit_.contentEndSamples();
        bool hasClips = false;
        for (const auto& t : edit_.tracks())
            if (!t.clips.empty()) { hasClips = true; break; }
        if (!hasClips) {
            for (const auto& track : edit_.tracks()) {
                if (!track.midiClips.empty()) { hasClips = true; break; }
            }
        }
        if (!edit_.videoTracks().empty())
            for (const auto& vt : edit_.videoTracks())
                if (!vt.clips.empty()) { hasClips = true; break; }
        if (hasClips && audio_.transport().position() > end)
            audio_.transport().stop();
    }
}

void DaveApp::openWavDialog() {
    nfdnchar_t* outPath = nullptr;
    nfdnfilteritem_t filter{"WAV audio", "wav"};
    if (NFD_OpenDialog(&outPath, &filter, 1, nullptr) == NFD_OKAY && outPath) {
        std::string p = outPath;
        NFD_FreePath(outPath);
        loadWavIntoEdit(p);
    }
}

void DaveApp::openMidiDialog() {
    nfdnchar_t* outPath = nullptr;
    nfdnfilteritem_t filter{"MIDI file", "mid,midi"};
    if (NFD_OpenDialog(&outPath, &filter, 1, nullptr) == NFD_OKAY && outPath) {
        std::string p = outPath;
        NFD_FreePath(outPath);
        importMidiFile(p);
    }
}

bool DaveApp::importMidiFile(const std::string& path) {
    // Notes are baked to samples against the session rate and stay that way;
    // the file's tick-domain provenance rides along on each clip for a future
    // tempo map.
    const auto smf = engine::midi::readSmf(path, edit_.sampleRate());
    if (!smf.ok) {
        std::fprintf(stderr, "Dave: MIDI import failed (%s): %s\n",
                     path.c_str(), smf.error.c_str());
        return false;
    }

    const size_t slash = path.find_last_of("/\\");
    const std::string fileName =
        slash == std::string::npos ? path : path.substr(slash + 1);

    // One Dave track per non-empty SMF track. Empty ones are almost always the
    // conductor track (tempo and time signature only) — importing them would
    // add a row that can never make a sound.
    std::vector<document::MidiTrack> tracks;
    int index = 0;
    for (const auto& st : smf.tracks) {
        ++index;
        if (st.notes.empty()) continue;

        document::MidiTrack mt;
        mt.name = !st.name.empty() ? st.name
                                   : (fileName + " " + std::to_string(index));
        document::MidiClip clip;
        clip.name = mt.name;
        clip.timelineStart = 0;
        clip.sourceOffset = 0;
        clip.length = st.lengthSamples;
        clip.notes = st.notes;
        clip.sourcePath = path;
        clip.sourcePpq = smf.ppq;
        clip.sourceTempi = smf.tempi;
        mt.midiClips.push_back(std::move(clip));
        tracks.push_back(std::move(mt));
    }

    if (tracks.empty()) {
        std::fprintf(stderr, "Dave: %s has no notes to import\n", path.c_str());
        return false;
    }

    const size_t firstNew = edit_.tracks().size();
    undo_.execute(std::make_unique<editing::ImportMidiFileCommand>(
        std::move(tracks), fileName));
    // Select the first imported row so the instrument picker in the gutter is
    // the obvious next step rather than something to go hunting for.
    view_.selectedTrackIndex =
        static_cast<int>(edit_.tracks().size() + firstNew);
    std::fprintf(stderr, "Dave: imported %s (%zu track%s)\n", fileName.c_str(),
                 edit_.tracks().size() - firstNew,
                 (edit_.tracks().size() - firstNew) == 1 ? "" : "s");
    return true;
}

void DaveApp::importMarkersDialog() {
    nfdnchar_t* outPath = nullptr;
    nfdnfilteritem_t filter{"Reaper marker CSV", "csv"};
    if (NFD_OpenDialog(&outPath, &filter, 1, nullptr) == NFD_OKAY && outPath) {
        std::string path = outPath;
        NFD_FreePath(outPath);
        std::ifstream f(path);
        if (!f) { std::fprintf(stderr, "Dave: could not open %s\n", path.c_str()); return; }
        std::string csv((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        // Import into the first existing marker track (the default "Markers"),
        // or create an "Imported" track if none.
        std::string targetId;
        if (!edit_.markerTracks().empty()) targetId = edit_.markerTracks().front().id;
        std::string usedId = document::importMarkersReaperCsv(
            edit_, audio_.sampleRate(), csv, targetId);
        if (!usedId.empty()) {
            std::fprintf(stderr, "Dave: imported markers into track '%s'\n", usedId.c_str());
        } else {
            std::fprintf(stderr, "Dave: marker CSV had no parseable rows\n");
        }
    }
}

void DaveApp::exportMarkersDialog() {
    nfdnchar_t* outPath = nullptr;
    nfdnfilteritem_t filter{"Reaper marker CSV", "csv"};
    if (NFD_SaveDialog(&outPath, &filter, 1, nullptr, "markers.csv") == NFD_OKAY && outPath) {
        std::string path = outPath;
        NFD_FreePath(outPath);
        std::string csv = document::exportMarkersReaperCsv(edit_, audio_.sampleRate());
        std::ofstream f(path);
        if (!f) { std::fprintf(stderr, "Dave: could not write %s\n", path.c_str()); return; }
        f << csv;
        std::fprintf(stderr, "Dave: exported markers to %s (%zu bytes)\n",
                     path.c_str(), csv.size());
    }
}

void DaveApp::run() {
    window_.run();
}

void DaveApp::updateThumbnails(const document::Edit& edit) {
    // For each video clip, ensure we have thumbnail textures. We request them
    // one at a time via the background decoder (not all at once — avoids
    // spawning many ffmpeg processes). Each clip gets ~1 thumb per 2 seconds.
    for (const auto& vt : edit.videoTracks()) {
        for (const auto& clip : vt.clips) {
            // Find or create the thumbnail entry.
            ClipThumbnails* ct = nullptr;
            for (auto& t : thumbCache_) {
                if (t.clipId == clip.id) { ct = &t; break; }
            }
            if (!ct) {
                thumbCache_.push_back({clip.id, clip.path, {}, clip.fps, false});
                ct = &thumbCache_.back();
            }
            // Count how many thumbs we need.
            int64_t len = (clip.length > 0) ? clip.length
                : static_cast<int64_t>(clip.durationSeconds * 48000.0);
            double durSec = len / 48000.0;
            int needed = std::max(1, static_cast<int>(durSec / 2.0));
            // Request thumbs one at a time.
            if (static_cast<int>(ct->textures.size()) < needed &&
                !ct->requested && !thumbDecoder_.isBusy()) {
                double t = ct->textures.size() * 2.0;  // every 2 seconds
                if (t > durSec) t = durSec - 0.1;
                int tw = 80, th = static_cast<int>(80.0 * clip.height / clip.width);
                thumbDecoder_.requestFrame(clip.path, t, tw, th, clip.fps);
                ct->requested = true;
                thumbRequestClipId_ = clip.id;
            }
        }
    }
    // Check for a completed thumbnail.
    if (!thumbRequestClipId_.empty() && !thumbDecoder_.isBusy()) {
        engine::VideoFrame frame;
        if (thumbDecoder_.getLatestFrame(frame)) {
            for (auto& ct : thumbCache_) {
                if (ct.clipId == thumbRequestClipId_) {
                    unsigned int tex = 0;
                    glGenTextures(1, &tex);
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.width);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.width, frame.height,
                                 0, GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    ct.textures.push_back({tex, frame.width, frame.height, frame.timeSeconds});
                    ct.requested = false;
                    break;
                }
            }
        }
        thumbRequestClipId_.clear();
    }
}

void DaveApp::drawVideoThumbnails(ImDrawList* dl, ImVec2 origin, float laneHeight,
                                   float totalWidth, float gutterWidth,
                                   double scroll, double spp,
                                   const document::Edit& edit) {
    // Draw thumbnail textures inside video clip blocks in the video lane.
    for (const auto& vt : edit.videoTracks()) {
        for (const auto& clip : vt.clips) {
            // Find the thumbnail cache entry.
            const ClipThumbnails* ct = nullptr;
            for (const auto& t : thumbCache_) {
                if (t.clipId == clip.id) { ct = &t; break; }
            }
            if (!ct || ct->textures.empty()) continue;

            // Clip screen rect.
            double clipX = origin.x + gutterWidth +
                (clip.timelineStart - scroll) / spp;
            int64_t len = (clip.length > 0) ? clip.length
                : static_cast<int64_t>(clip.durationSeconds * 48000.0);
            double clipW = static_cast<double>(len) / spp;
            if (clipX + clipW < origin.x + gutterWidth) continue;
            if (clipX > origin.x + totalWidth) continue;

            // Draw each thumbnail at its position within the clip.
            for (const auto& thumb : ct->textures) {
                double thumbX = clipX + (thumb.timeSeconds * 48000.0) / spp;
                if (thumbX < clipX || thumbX > clipX + clipW) continue;
                if (thumb.texId == 0) continue;
                float thumbW = static_cast<float>(thumb.w);
                float thumbH = static_cast<float>(thumb.h);
                // Scale to fit the lane height.
                float scale = (laneHeight - 10) / thumbH;
                ImGui::SetCursorScreenPos(ImVec2(thumbX, origin.y + 5));
                ImGui::Image(static_cast<ImTextureID>(thumb.texId),
                             ImVec2(thumbW * scale, thumbH * scale));
            }
        }
    }
}

void DaveApp::openVideoDialog() {
    nfdnchar_t* outPath = nullptr;
    nfdnfilteritem_t filter{"Video", "mp4,mov,mkv,m4v,mxf"};
    if (NFD_OpenDialog(&outPath, &filter, 1, nullptr) == NFD_OKAY && outPath) {
        std::string path = outPath;
        NFD_FreePath(outPath);
        engine::VideoInfo info;
        if (!engine::VideoProbe::probe(path, info)) {
            std::fprintf(stderr, "Dave: failed to probe video: %s\n", path.c_str());
            return;
        }
        document::VideoClip clip;
        clip.path = path;
        auto slash = path.find_last_of("/\\");
        clip.name = (slash != std::string::npos) ? path.substr(slash + 1) : path;
        clip.timelineStart = audio_.transport().position();
        clip.sourceOffset = 0;
        clip.length = 0; // auto = full duration
        clip.codec = info.codec;
        clip.fps = info.fps;
        clip.width = info.width;
        clip.height = info.height;
        clip.durationSeconds = info.durationSeconds;
        if (edit_.videoTracks().empty()) {
            edit_.addVideoTrack("Video 1");
        }
        edit_.addVideoClip(edit_.videoTracks().front().id, std::move(clip));
        videoDecoder_.close();
        lastRequestedFrameIndex_ = -1;
        lastUploadedFrameIndex_ = -1;
    }
}

void DaveApp::newProject() {
    if (recordingActive()) {
        showStatus("Stop recording before creating a new project", true);
        return;
    }
    edit_.tracksMut().clear();
    edit_.clearTracks_();
    edit_.clearTracks_();
    edit_.ensureMainBus_();
    if (audio_.playbackChannelCount() == 1) {
        edit_.track(document::kMainBusId)->mainOutput =
            document::RouteTarget::hardwareOutput(0, 1);
    }
    edit_.clearMarkerTracks_();
    edit_.clearVideoTracks_();
    transientAnalyses_.cancelAll();
    pendingTransientNavigation_.reset();
    builder_ = {};
    videoDecoder_.close();
    lastRequestedFrameIndex_ = -1;
    lastUploadedFrameIndex_ = -1;
    projectPath_.clear();
    dirty_ = false;
    undo_.clear();
    undo_.execute(std::make_unique<editing::AddTrackCommand>("Track 1"));
    view_.selectedTrackIndex = 0;
    edit_.addMarkerTrack("Markers");
    audio_.transport().stop();
    audio_.transport().seek(0);
}

void DaveApp::refreshAudioDevices() {
    audioDevices_ = audio_.enumerateDevices();

    std::vector<gui::IoDevice> playback;
    playback.reserve(audioDevices_.playbackNames.size() + 1);
    playback.push_back({kOutputDefaultId, "Default", 0, true});
    for (std::size_t index = 0; index < audioDevices_.playbackNames.size();
         ++index) {
        playback.push_back({outputDeviceId(index),
                            audioDevices_.playbackNames[index], 0, false});
    }
    ioPanel_.setPlaybackCatalog(std::move(playback));

    std::vector<gui::IoDevice> capture;
    capture.reserve(audioDevices_.captureNames.size());
    for (std::size_t index = 0; index < audioDevices_.captureNames.size();
         ++index) {
        capture.push_back({inputDeviceId(index),
                           audioDevices_.captureNames[index], 0, false});
    }
    ioPanel_.setCaptureCatalog(std::move(capture));

    if (audioPreferences_.outputDeviceName.empty()) {
        ioPanel_.setSelectedOutput(
            gui::IoDeviceSelection{kOutputDefaultId, "Default"});
    } else {
        const int index = firstNameIndex(audioDevices_.playbackNames,
                                         audioPreferences_.outputDeviceName);
        ioPanel_.setSelectedOutput(gui::IoDeviceSelection{
            index >= 0 ? outputDeviceId(static_cast<std::size_t>(index))
                       : "output:missing",
            audioPreferences_.outputDeviceName});
    }

    if (audioPreferences_.inputMode == InputMode::Off) {
        ioPanel_.setSelectedInput(
            gui::IoDeviceSelection{kInputOffId, "Off"});
    } else if (audioPreferences_.inputMode == InputMode::Default) {
        ioPanel_.setSelectedInput(
            gui::IoDeviceSelection{kInputDefaultId, "Default"});
    } else {
        const int index = firstNameIndex(audioDevices_.captureNames,
                                         audioPreferences_.inputDeviceName);
        ioPanel_.setSelectedInput(gui::IoDeviceSelection{
            index >= 0 ? inputDeviceId(static_cast<std::size_t>(index))
                       : "input:missing",
            audioPreferences_.inputDeviceName});
    }
}

bool DaveApp::applyAudioPreferences(const AudioPreferences& preferences,
                                    bool saveOnSuccess) {
    if (recordingActive()) {
        showStatus("Stop recording before changing audio devices", true);
        return false;
    }
    int outputIndex = -1;
    if (!preferences.outputDeviceName.empty()) {
        outputIndex = firstNameIndex(audioDevices_.playbackNames,
                                     preferences.outputDeviceName);
        // Keep a missing remembered output visible in the panel while using
        // the system output as a safe startup fallback.
        if (outputIndex < 0) outputIndex = -1;
    }

    platform::InputDeviceSelection input =
        platform::InputDeviceSelection::off();
    if (preferences.inputMode == InputMode::Default) {
        input = platform::InputDeviceSelection::defaultDevice();
    } else if (preferences.inputMode == InputMode::Device) {
        input = platform::InputDeviceSelection::device(
            firstNameIndex(audioDevices_.captureNames,
                           preferences.inputDeviceName));
    }

    const bool opened = audio_.selectDevices(
        outputIndex, input, static_cast<double>(edit_.sampleRate()), 2);
    if (!opened) return false;

    audioPreferences_ = preferences;
    if (saveOnSuccess && !audioPreferencesStore_.save(audioPreferences_)) {
        std::fprintf(stderr, "Dave: could not save audio preferences: %s\n",
                     audioPreferencesStore_.path().string().c_str());
    }
    refreshAudioDevices();
    return true;
}

void DaveApp::syncIoPanelState() {
    ioPanel_.setRecordingActive(recordingActive());
    ioPanel_.setRecordLatencyOffset(
        audioPreferences_.recordLatencyOffsetSamples);
    gui::IoMeterSnapshot meters;
    const auto channels = audio_.captureChannelCount();
    meters.inputs.reserve(channels);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        const auto meter = audio_.inputMeter(channel);
        meters.inputs.push_back(
            gui::IoInputMeter{meter.peak, meter.rms, meter.clipped});
    }
    ioPanel_.setMeterSnapshot(std::move(meters));

    if (audioPreferences_.inputMode == InputMode::Off) {
        ioPanel_.setCaptureStatus("Input Off");
    } else if (audio_.captureAvailable()) {
        ioPanel_.setCaptureStatus(
            std::to_string(audio_.captureChannelCount()) +
            "-channel input ready");
    } else {
        ioPanel_.setCaptureStatus("Playback only", audio_.captureError());
    }
}

void DaveApp::serviceIoPanelRequests() {
    for (const auto& request : ioPanel_.takeRequests()) {
        if (request.kind == gui::IoPanelRequest::Kind::ClearInputClips) {
            audio_.clearInputClips();
            continue;
        }
        if (request.kind ==
            gui::IoPanelRequest::Kind::SetRecordLatencyOffset) {
            if (recordingActive()) {
                showStatus("Stop recording before changing latency calibration",
                           true);
                continue;
            }
            audioPreferences_.recordLatencyOffsetSamples =
                std::clamp(request.value, 0, 1000000);
            if (!audioPreferencesStore_.save(audioPreferences_)) {
                showStatus("Could not save the recording latency preference",
                           true);
            }
            continue;
        }
        if (recordingActive()) {
            showStatus("Stop recording before refreshing or changing audio devices",
                       true);
            continue;
        }
        if (request.kind == gui::IoPanelRequest::Kind::RefreshDevices) {
            refreshAudioDevices();
            applyAudioPreferences(audioPreferences_, false);
            continue;
        }

        AudioPreferences requested = audioPreferences_;
        if (request.kind == gui::IoPanelRequest::Kind::SelectOutput) {
            if (request.deviceId == kOutputDefaultId) {
                requested.outputDeviceName.clear();
            } else {
                const auto& catalog = ioPanel_.playbackCatalog();
                const auto found = std::find_if(
                    catalog.begin(), catalog.end(), [&](const gui::IoDevice& device) {
                        return device.id == request.deviceId;
                    });
                if (found == catalog.end()) continue;
                requested.outputDeviceName = found->name;
            }
        } else if (request.kind == gui::IoPanelRequest::Kind::SelectInput) {
            if (request.deviceId == kInputOffId) {
                requested.inputMode = InputMode::Off;
                requested.inputDeviceName.clear();
            } else if (request.deviceId == kInputDefaultId) {
                requested.inputMode = InputMode::Default;
                requested.inputDeviceName.clear();
            } else {
                const auto& catalog = ioPanel_.captureCatalog();
                const auto found = std::find_if(
                    catalog.begin(), catalog.end(), [&](const gui::IoDevice& device) {
                        return device.id == request.deviceId;
                    });
                if (found == catalog.end()) continue;
                requested.inputMode = InputMode::Device;
                requested.inputDeviceName = found->name;
            }
        }
        if (!applyAudioPreferences(requested, true)) {
            // A failed output switch may have stopped the old device. Reopen
            // the last confirmed preference so one bad choice does not leave
            // the rest of the session silent.
            applyAudioPreferences(audioPreferences_, false);
        }
    }
}

// Reopen the output/capture device at the session rate and re-derive the graph
// against it. Called when the rate changes and after loading a project that
// carries a different one — otherwise the engine keeps running at the old
// rate and everything plays back detuned.
void DaveApp::applySessionSampleRate() {
    if (recordingActive()) {
        showStatus("Stop recording before changing the session sample rate",
                   true);
        return;
    }
    const double rate = static_cast<double>(edit_.sampleRate());
    if (audio_.sampleRate() == rate) return;
    // Resolve exact remembered names against the freshly enumerated catalog;
    // numeric device indices are only valid for one enumeration snapshot.
    if (!applyAudioPreferences(audioPreferences_, false)) {
        std::fprintf(stderr,
                     "Dave: could not open the output device at %.0f Hz; "
                     "the engine is still running at %.0f Hz\n",
                     rate, audio_.sampleRate());
        return;
    }
    onEditChanged();   // recompile the graph against the new rate
}

void DaveApp::openProjectDialog() {
    if (recordingActive()) {
        showStatus("Stop recording before opening another project", true);
        return;
    }
    nfdnchar_t* outPath = nullptr;
    nfdnfilteritem_t filter{"Dave project", "dave"};
    // NFD's folder picker is more correct for a bundle (it's a directory), but
    // most users expect to pick a "file". We allow .dave which on disk is a
    // dir; NFD will return the path either way.
    if (NFD_PickFolder(&outPath, nullptr) == NFD_OKAY && outPath) {
        std::string path = outPath;
        NFD_FreePath(outPath);
        auto r = document::loadBundle(path, edit_);
        if (!r.ok) {
            std::fprintf(stderr, "Dave: open failed: %s\n", r.message.c_str());
            return;
        }
        transientAnalyses_.cancelAll();
        pendingTransientNavigation_.reset();
        builder_ = {};  // force re-derive with fresh plugin instances
        videoDecoder_.close();
        lastRequestedFrameIndex_ = -1;
        lastUploadedFrameIndex_ = -1;
        projectPath_ = path;
        undo_.clear();
        applySessionSampleRate();
        onEditChanged();  // rebuild graph for the loaded Edit
        // Hardware channel numbers are project intent. Missing channels stay
        // unavailable until the interface returns or the user changes them.
        dirty_ = false;
        view_.selectedTrackIndex = edit_.tracks().empty() ? -1 : 0;
        std::fprintf(stderr, "Dave: opened %s\n", path.c_str());
    }
}

void DaveApp::saveProject(bool saveAs) {
    // Capture plugin states before serializing — get each loaded plugin's
    // current parameter/internal state and stash it in the slot.
    const auto captureSlotState = [&](document::PluginSlot& slot) {
        auto instance = builder_.pluginInstance(slot.id);
        if (instance && instance->isLoaded()) {
            slot.stateBase64 = instance->getStateBase64();
        }
    };
    for (auto& track : edit_.tracksMut()) {
        captureSlotState(track.instrument);
        for (auto& slot : track.plugins) captureSlotState(slot);
    }

    std::string path = projectPath_;
    if (saveAs || path.empty()) {
        // Pick a folder to save the bundle into.
        nfdnchar_t* outPath = nullptr;
        if (NFD_PickFolder(&outPath, nullptr) != NFD_OKAY || !outPath) return;
        std::string dir = outPath;
        NFD_FreePath(outPath);
        // Append a default name.
        path = dir + "/Untitled.dave";
    }
    auto r = document::saveBundle(path, edit_);
    if (!r.ok) {
        std::fprintf(stderr, "Dave: save failed: %s\n", r.message.c_str());
        return;
    }
    // Rebase in-memory media paths to exactly what project.json now names.
    // This is essential on first save and Save As: continuing to reference
    // the old bundle makes the next save depend on media the user may move or
    // delete. Loading into a temporary Edit gives us the serializer's actual
    // collision-resolved paths without disturbing ids, undo, or plugin state.
    document::Edit rebased;
    const auto rebasedResult = document::loadBundle(path, rebased);
    if (!rebasedResult.ok) {
        projectPath_ = path;
        dirty_ = true;
        showStatus("Project saved, but media paths could not be rebased: " +
                       rebasedResult.message,
                   true);
        return;
    }
    for (auto& [id, asset] : edit_.assets()) {
        if (const auto* savedAsset = rebased.asset(id)) {
            asset.path = savedAsset->path;
        }
    }
    for (auto& videoTrack : edit_.videoTracksMut()) {
        for (auto& clip : videoTrack.clips) {
            for (const auto& savedTrack : rebased.videoTracks()) {
                const auto found = std::find_if(
                    savedTrack.clips.begin(), savedTrack.clips.end(),
                    [&](const document::VideoClip& savedClip) {
                        return savedClip.id == clip.id;
                    });
                if (found != savedTrack.clips.end()) {
                    clip.path = found->path;
                    break;
                }
            }
        }
    }
    projectPath_ = path;
    dirty_ = false;
    std::fprintf(stderr, "Dave: saved %s\n", path.c_str());
}

void DaveApp::drawVideoPreview() { drawVideoPreviewContent(); }

// Public/menu entry point: requests the toggle, which drawUI applies on the
// next frame boundary. Safe to call from the native menu bar, which fires
// during event polling rather than inside an ImGui frame.
void DaveApp::setVideoPoppedOut(bool poppedOut) {
    videoPopoutRequest_ = true;
    videoPopoutRequestValue_ = poppedOut;
}

void DaveApp::drawVideoPopoutWindow() {
    // No picture means no picture window — including when a project that had
    // one is closed while it is popped out. videoPoppedOut_ survives so the
    // window comes back where the user put it on the next import.
    if (!videoPoppedOut_ || edit_.videoTracks().empty()) {
        return;
    }

    // FirstUseEver, not Always: a window the user has already placed (this
    // session or a previous one, via imgui.ini) reopens where they left it —
    // the whole point being that the picture monitor stays put. Only a window
    // that has never been positioned gets seeded over the panel it left, so
    // the first pop-out reads as the picture lifting out of the layout.
    const ImGuiViewport* mainVp = ImGui::GetMainViewport();
    const ImVec2 seedSize(std::max(videoPanelSize_.x, 560.0f),
                          std::max(videoPanelSize_.y, 360.0f));
    ImVec2 seedPos = (videoPanelSize_.x > 0.0f)
        ? videoPanelPos_
        : ImVec2(mainVp->Pos.x + 120.0f, mainVp->Pos.y + 120.0f);
    // The picture panel hugs the right edge, so a window seeded at its corner
    // would hang off the app — and possibly off the display. Keep the whole
    // thing over the main window; the user drags it out from there.
    seedPos.x = std::clamp(
        seedPos.x, mainVp->Pos.x + 16.0f,
        std::max(mainVp->Pos.x + 16.0f,
                 mainVp->Pos.x + mainVp->Size.x - seedSize.x - 16.0f));
    seedPos.y = std::clamp(
        seedPos.y, mainVp->Pos.y + 16.0f,
        std::max(mainVp->Pos.y + 16.0f,
                 mainVp->Pos.y + mainVp->Size.y - seedSize.y - 16.0f));
    ImGui::SetNextWindowPos(seedPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(seedSize, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 180.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));

    // NoAutoMerge is what makes "pop out" mean what it says: the picture gets
    // a real OS window immediately and keeps it, instead of ImGui folding it
    // back into the main window whenever it happens to be dragged over it.
    // That window can then go anywhere the window manager allows, second
    // display included.
    ImGuiWindowClass popoutClass;
    popoutClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&popoutClass);

    // The platform window is opaque, so rounded corners would leave unpainted
    // pixels at its edges.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    bool open = true;
    if (ImGui::Begin("Video###videoPopout", &open,
                     ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoCollapse)) {
        // Right-aligned to mirror the docked panel's header action.
        const float buttonW = ImGui::CalcTextSize("Pop In").x +
                              ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(0.0f, ImGui::GetContentRegionAvail().x - buttonW));
        if (ImGui::SmallButton("Pop In")) {
            setVideoPoppedOut(false);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Return the picture to the sidebar");
        }

        drawVideoPreviewContent();
    }
    ImGui::End();
    ImGui::PopStyleVar();

    // The title-bar close button means "put it back", not "hide the picture" —
    // there is nowhere else for it to live.
    if (!open) {
        setVideoPoppedOut(false);
    }
}

void DaveApp::drawVideoPreviewContent() {
    int64_t playhead = audio_.transport().position();
    const auto* clip = edit_.videoClipAt(playhead);

    if (!clip) {
        if (drawCenteredEmptyState(
                "No video at the playhead",
                "Load picture to start working in sync.",
                "Load Video...###emptyVideo")) {
            openVideoDialog();
        }
        return;
    }

    ImGui::Text("%s", clip->name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Add Video...")) openVideoDialog();
    ImGui::TextDisabled("%dx%d @ %.3ffps", clip->width, clip->height, clip->fps);

    const double audioSr = static_cast<double>(edit_.sampleRate());
    int64_t relSamples = (playhead - clip->timelineStart) + clip->sourceOffset;
    double videoTimeSec = (relSamples > 0) ? (relSamples / audioSr) : 0.0;
    double clipDurationSec = (clip->length > 0)
        ? (clip->length / audioSr) : clip->durationSeconds;
    bool inRange = (videoTimeSec >= 0.0 && videoTimeSec <= clipDurationSec);
    int64_t frameIndex = (clip->fps > 0.0)
        ? static_cast<int64_t>(videoTimeSec * clip->fps) : 0;

    // If the active clip changed, reset state.
    if (lastVideoClipId_ != clip->id) {
        lastRequestedFrameIndex_ = -1;
        lastUploadedFrameIndex_ = -1;
        lastVideoClipId_ = clip->id;
    }

    // Request a frame from the async decoder if the playhead's frame differs
    // from what we last asked for and the decoder is free. During playback the
    // playhead outruns the decoder, so each finished decode immediately
    // triggers a request for the current frame.
    if (inRange && frameIndex != lastRequestedFrameIndex_ && !asyncDecoder_.isBusy()) {
        const int previewMaxW = 480;
        int pw = clip->width, ph = clip->height;
        if (pw > previewMaxW) { ph = ph * previewMaxW / pw; pw = previewMaxW; }
        double seekTo = static_cast<double>(frameIndex) / clip->fps;
        asyncDecoder_.requestFrame(clip->path, seekTo, pw, ph, clip->fps);
        lastRequestedFrameIndex_ = frameIndex;
    }

    // Upload whatever the decoder finished most recently, even if the playhead
    // has since moved on — audio is master and video chases it. Requiring an
    // exact frame match here made playback freeze: every decode arrived a few
    // frames late and was discarded, so the picture only updated on stop.
    engine::VideoFrame frame;
    if (asyncDecoder_.getLatestFrame(frame) &&
        frame.frameIndex != lastUploadedFrameIndex_) {
        int pw = frame.width;
        int ph = frame.height;
        if (videoTexture_ == 0 || videoTexW_ != pw || videoTexH_ != ph) {
            if (videoTexture_ == 0) glGenTextures(1, &videoTexture_);
            glBindTexture(GL_TEXTURE_2D, videoTexture_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            videoTexW_ = pw;
            videoTexH_ = ph;
        }
        glBindTexture(GL_TEXTURE_2D, videoTexture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, pw);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pw, ph, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        lastUploadedFrameIndex_ = frame.frameIndex;
    }

    if (videoTexture_ != 0 && videoTexW_ > 0 && videoTexH_ > 0) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float drawW = avail.x;
        float drawH = drawW * static_cast<float>(videoTexH_) / static_cast<float>(videoTexW_);
        if (drawH > avail.y) { drawH = avail.y; drawW = drawH * static_cast<float>(videoTexW_) / static_cast<float>(videoTexH_); }
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - drawW) * 0.5f));
        ImGui::Image(static_cast<ImTextureID>(videoTexture_),
                     ImVec2(drawW, drawH));
    } else {
        if (drawCenteredEmptyState(
                "Preparing the video frame",
                "Picture will appear as soon as decoding finishes.",
                "Choose Video...###loadingVideo")) {
            openVideoDialog();
        }
        return;
    }

    ImGui::TextDisabled("%s  frame %lld  %s",
        gui::formatTimecode(playhead, view_.tcMode,
                            static_cast<double>(edit_.sampleRate()), 24.0,
                            edit_.tempoBpm(), &edit_.meterMap(),
                            &edit_.tempoMap()).c_str(),
        static_cast<long long>(frameIndex),
        inRange ? "" : "(out of range)");
}

// Tempo and the time signature map. Small enough to live in the popup that
// opens it: a session has one tempo and usually one meter, and the list only
// grows for the sessions that genuinely change.
void DaveApp::drawMeterEditor() {
    ImGui::TextDisabled("TEMPO");
    const auto tempo = edit_.tempoMap();
    int removeTempoBar = 0;
    int removeTempoBeat = 0;
    for (const auto& change : tempo) {
        ImGui::PushID(change.bar * 1000 + change.beat);
        ImGui::AlignTextToFramePadding();
        if (change.bar == 1 && change.beat == 1) {
            ImGui::TextUnformatted("Start");
        } else {
            ImGui::Text("%d|%d", change.bar, change.beat);
        }
        ImGui::SameLine(70.0f);
        float bpm = static_cast<float>(change.bpm);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::DragFloat("##bpm", &bpm, 0.1f, 20.0f, 300.0f, "%.2f bpm")) {
            // Live while dragging so the ruler moves under the pointer; the
            // undo entry lands once, on release.
            edit_.setTempoChange(change.bar, change.beat,
                                 static_cast<double>(bpm));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            undo_.execute(std::make_unique<editing::SetTempoCommand>(
                change.bar, change.beat, static_cast<double>(bpm)));
        }
        if (change.bar != 1 || change.beat != 1) {
            ImGui::SameLine(0.0f, 6.0f);
            if (ImGui::SmallButton("\xc3\x97")) {
                removeTempoBar = change.bar;
                removeTempoBeat = change.beat;
            }
        }
        ImGui::PopID();
    }
    if (removeTempoBar > 0) {
        undo_.execute(std::make_unique<editing::RemoveTempoCommand>(
            removeTempoBar, removeTempoBeat));
    }

    {
        // A new change defaults to the playhead's beat, which is where
        // someone editing tempo is looking.
        const auto at = document::barsBeatsAtSample(
            audio_.transport().position(),
            static_cast<double>(edit_.sampleRate()), edit_.tempoMap(),
            edit_.meterMap());
        char label[64];
        std::snprintf(label, sizeof(label), "+ Tempo at %d|%d", at.bar,
                      at.beat);
        const bool atStart = at.bar == 1 && at.beat == 1;
        ImGui::BeginDisabled(atStart);
        if (ImGui::Button(label)) {
            undo_.execute(std::make_unique<editing::SetTempoCommand>(
                at.bar, at.beat,
                document::bpmAt(edit_.tempoMap(), edit_.meterMap(), at.bar,
                                at.beat)));
        }
        ImGui::EndDisabled();
        if (atStart && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("The playhead is at the start, which already "
                              "has a tempo.");
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextDisabled("TIME SIGNATURE");

    const auto map = edit_.meterMap();
    int removeBar = 0;
    for (const auto& signature : map) {
        ImGui::PushID(signature.bar);
        ImGui::AlignTextToFramePadding();
        // Bar 1 is the session's meter rather than a change, so it is not
        // editable as a position — moving it would leave earlier bars with
        // no meter, and there are no earlier bars.
        if (signature.bar == 1) {
            ImGui::TextUnformatted("Bar 1");
        } else {
            ImGui::Text("Bar %d", signature.bar);
        }
        ImGui::SameLine(70.0f);

        int numerator = signature.numerator;
        ImGui::SetNextItemWidth(46.0f);
        const bool numeratorChanged =
            ImGui::InputInt("##num", &numerator, 0, 0);
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("/");
        ImGui::SameLine(0.0f, 2.0f);

        static constexpr int kDenominators[] = {1, 2, 4, 8, 16, 32};
        static const char* kDenominatorLabels[] = {"1", "2", "4", "8", "16",
                                                   "32"};
        int denominatorIdx = 2;
        for (int i = 0; i < IM_ARRAYSIZE(kDenominators); ++i) {
            if (kDenominators[i] == signature.denominator) denominatorIdx = i;
        }
        ImGui::SetNextItemWidth(58.0f);
        const bool denominatorChanged = ImGui::Combo(
            "##den", &denominatorIdx, kDenominatorLabels,
            IM_ARRAYSIZE(kDenominatorLabels));

        if (numeratorChanged || denominatorChanged) {
            undo_.execute(std::make_unique<editing::SetTimeSignatureCommand>(
                signature.bar, std::max(1, numerator),
                kDenominators[denominatorIdx]));
        }
        if (signature.bar != 1) {
            ImGui::SameLine(0.0f, 6.0f);
            if (ImGui::SmallButton("\xc3\x97")) removeBar = signature.bar;
        }
        ImGui::PopID();
    }
    if (removeBar > 0) {
        undo_.execute(
            std::make_unique<editing::RemoveTimeSignatureCommand>(removeBar));
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    // A new change defaults to the bar the playhead is on, which is where
    // someone editing meter is almost always looking.
    const auto here = document::barsBeatsAtSample(
        audio_.transport().position(),
        static_cast<double>(edit_.sampleRate()), edit_.tempoMap(),
        edit_.meterMap());
    const int newBar = std::max(2, here.bar);
    char addLabel[48];
    std::snprintf(addLabel, sizeof(addLabel), "+ Change at bar %d", newBar);
    if (ImGui::Button(addLabel)) {
        const auto current = document::signatureAtBar(edit_.meterMap(), newBar);
        undo_.execute(std::make_unique<editing::SetTimeSignatureCommand>(
            newBar, current.numerator, current.denominator));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Every bar line after this one moves.");
    }
}

void DaveApp::openPluginBrowser(BrowserMode mode, std::string trackId) {
    if (pluginHost_.descriptors().empty()) {
        pluginHost_.scan();
    }
    browserMode_ = mode;
    browserTargetTrackId_ = std::move(trackId);
    showPluginBrowser_ = true;
}

const document::PluginSlot* DaveApp::findSlot(const std::string& slotId) const {
    if (slotId.empty()) return nullptr;
    for (const auto& t : edit_.tracks()) {
        if (t.instrument.id == slotId) return &t.instrument;
        for (const auto& slot : t.plugins) {
            if (slot.id == slotId) return &slot;
        }
    }
    return nullptr;
}

void DaveApp::serviceViewRequests() {
    for (auto& request : view_.routing.takeRequests()) {
        using Kind = gui::RoutingRequest::Kind;
        const bool topologyChange = request.kind == Kind::SetHardwareInput ||
            request.kind == Kind::SetMainOutput || request.kind == Kind::AddSend ||
            request.kind == Kind::UpdateSend || request.kind == Kind::RemoveSend ||
            request.kind == Kind::AddBus || request.kind == Kind::RemoveBus;
        if (recordingActive() && topologyChange) {
            showStatus("Routing topology cannot change during recording", true);
            continue;
        }
        switch (request.kind) {
            case Kind::SetHardwareInput:
                undo_.execute(std::make_unique<editing::SetHardwareInputCommand>(
                    request.ownerId, request.hardware)); break;
            case Kind::SetInputMonitor:
                undo_.execute(std::make_unique<editing::SetInputMonitorCommand>(
                    request.ownerId, request.enabled)); break;
            case Kind::SetMainOutput:
                undo_.execute(std::make_unique<editing::SetMainRouteCommand>(
                    request.ownerId, request.route)); break;
            case Kind::AddSend:
                undo_.execute(std::make_unique<editing::AddSendCommand>(
                    request.ownerId, request.send)); break;
            case Kind::UpdateSend:
                undo_.execute(std::make_unique<editing::UpdateSendCommand>(
                    request.ownerId, request.send)); break;
            case Kind::RemoveSend:
                undo_.execute(std::make_unique<editing::RemoveSendCommand>(
                    request.ownerId, request.send.id)); break;
            case Kind::AddBus:
                undo_.execute(std::make_unique<editing::AddTrackCommand>("Bus", editing::AddTrackCommand::Flavour::Bus));
                break;
            case Kind::RemoveBus:
                undo_.execute(std::make_unique<editing::RemoveTrackCommand>(
                    request.ownerId)); break;
        }
    }
    if (!view_.requestRecordArmTrackId.empty()) {
        const std::string trackId =
            std::move(view_.requestRecordArmTrackId);
        view_.requestRecordArmTrackId.clear();
        toggleTrackArm(trackId);
    }
    if (view_.requestPicker != gui::TimelineViewState::PluginPicker::None) {
        BrowserMode mode = BrowserMode::AudioFx;
        switch (view_.requestPicker) {
            case gui::TimelineViewState::PluginPicker::MidiInstrument:
                mode = BrowserMode::MidiInstrument; break;
            case gui::TimelineViewState::PluginPicker::MidiFx:
                mode = BrowserMode::MidiFx; break;
            case gui::TimelineViewState::PluginPicker::AudioFx:
            case gui::TimelineViewState::PluginPicker::None:
                mode = BrowserMode::AudioFx; break;
        }
        openPluginBrowser(mode, view_.requestPickerTrackId);
        view_.requestPicker = gui::TimelineViewState::PluginPicker::None;
        view_.requestPickerTrackId.clear();
    }
    if (!view_.requestPluginEditorSlotId.empty()) {
        if (const auto* slot = findSlot(view_.requestPluginEditorSlotId)) {
            openPluginEditor(*slot);
        }
        view_.requestPluginEditorSlotId.clear();
    }
    if (!view_.requestChannelStripTrackId.empty()) {
        const std::string trackId =
            std::move(view_.requestChannelStripTrackId);
        view_.requestChannelStripTrackId.clear();
        const auto& rows = edit_.tracks();
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].id != trackId) continue;
            // The strip follows the selection, so pressing E on a row both
            // selects it and opens the panel — pressing E on the row that is
            // already showing closes it, which is the only way the button can
            // read as a toggle without holding state of its own.
            const int row = static_cast<int>(i);
            showChannelStrip_ =
                !(showChannelStrip_ && view_.selectedTrackIndex == row);
            // The strip's pickers list what has been scanned, and the scan is
            // cached — so this runs once, the first time a strip is opened,
            // instead of costing every session that never opens one.
            if (showChannelStrip_ && pluginHost_.descriptors().empty()) {
                pluginHost_.scan();
            }
            view_.selectedTrackIndex = row;
            break;
        }
    }
}

void DaveApp::openPluginEditor(const document::PluginSlot& slot) {
    // The PluginInstance is owned by GraphBuilder, keyed by slot id — the
    // document only stores what to load, not the loaded thing.
    auto inst = builder_.pluginInstance(slot.id);
    if (!inst) return;
    auto& ed = editors_[slot.id];
    if (!ed) ed = std::make_unique<engine::PluginEditor>();
    if (!ed->isOpen()) ed->open(*inst, slot.name);
}

void DaveApp::drawPluginBrowser() {
    if (!showPluginBrowser_) return;

    const bool instrumentsOnly = browserMode_ == BrowserMode::MidiInstrument;
    const char* title = instrumentsOnly ? "Choose Instrument###pluginBrowser"
                                        : "Add Plugin###pluginBrowser";

    ImGui::SetNextWindowSize(ImVec2(500, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::Begin(title, &showPluginBrowser_,
                     ImGuiWindowFlags_NoDocking)) {
        // Count what the mode will actually offer, not what was scanned: "312
        // found" above a list of four synths is a worse answer than "4".
        size_t shown = 0;
        for (const auto& d : pluginHost_.descriptors()) {
            if (!instrumentsOnly || d.isInstrument) ++shown;
        }
        ImGui::Text("%s", instrumentsOnly ? "Available VST3 instruments"
                                          : "Available VST3 plugins");
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu found)", shown);
        ImGui::InputText("##filter", browserFilter_, sizeof(browserFilter_));
        ImGui::Separator();

        if (shown == 0 && instrumentsOnly) {
            ImGui::TextWrapped(
                "No VST3 instruments found. Dave scanned %zu plugins, all of "
                "them effects. Install a synth in "
                "/Library/Audio/Plug-Ins/VST3 and reopen this window.",
                pluginHost_.descriptors().size());
        }

        // List plugins; clicking one adds it to the target track.
        // Use the loop index for PushID to guarantee unique widget IDs (the
        // 'Add' button + Text appear in every row, so they'd collide without
        // a per-row ID scope).
        int pluginIdx = 0;
        for (const auto& d : pluginHost_.descriptors()) {
            if (instrumentsOnly && !d.isInstrument) { ++pluginIdx; continue; }
            if (browserFilter_[0] != '\0') {
                std::string name = d.name;
                std::string filt = browserFilter_;
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                std::transform(filt.begin(), filt.end(), filt.begin(), ::tolower);
                if (name.find(filt) == std::string::npos) { ++pluginIdx; continue; }
            }
            ImGui::PushID(pluginIdx);
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::Text("%s", d.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("[%s] %s", d.subCategories.c_str(), d.vendor.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(instrumentsOnly ? "Use" : "Add")) {
                document::PluginSlot slot;
                slot.name = d.name;
                slot.uidString = d.uidString;
                slot.path = d.path;
                slot.bypass = false;
                switch (browserMode_) {
                    case BrowserMode::AudioFx:
                        undo_.execute(std::make_unique<editing::AddPluginCommand>(
                            browserTargetTrackId_, slot));
                        break;
                    case BrowserMode::MidiInstrument:
                        undo_.execute(
                            std::make_unique<editing::SetMidiInstrumentCommand>(
                                browserTargetTrackId_, slot));
                        break;
                    case BrowserMode::MidiFx:
                        undo_.execute(std::make_unique<editing::AddPluginCommand>(
                            browserTargetTrackId_, slot));
                        break;
                }
                showPluginBrowser_ = false;
            }
            ImGui::PopID();
            ++pluginIdx;
        }
    }
    ImGui::End();
}

} // namespace dave::application
