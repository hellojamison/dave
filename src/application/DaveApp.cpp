// SPDX-License-Identifier: GPL-3.0-or-later
#include "application/DaveApp.h"
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
#include <cfloat>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace dave::application {

namespace {

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
    audio_.stop();
    audio_.setCompiledGraph(nullptr);
}

bool DaveApp::init() {
    if (!window_.valid() || !imgui_.init(window_)) {
        return false;
    }
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
    platform::g_menuUndo         = [this](){ undo_.undo(); };
    platform::g_menuRedo         = [this](){ undo_.redo(); };
    platform::g_menuPlayStop     = [this](){ audio_.transport().toggle(); };
    platform::g_menuReturnToStart= [this](){ audio_.transport().seek(0); };
    // The native item is always present; with no picture loaded there is
    // nothing for it to move, so it does nothing rather than opening an
    // empty window.
    platform::g_menuToggleVideoWindow = [this](){
        if (!edit_.videoTracks().empty()) setVideoPoppedOut(!videoPoppedOut_);
    };
    platform::g_menuQuit         = [this](){ window_.close(); };
    platform::setupMacMenuBar();
#endif
    if (!audio_.start(static_cast<double>(edit_.sampleRate()), 2)) {
        std::fprintf(stderr, "Dave: audio engine failed to start\n");
    }

    // Wire the Edit's change signal to graph re-derivation.
    edit_.setChangeListener([this] { onEditChanged(); });

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
        imgui_.render();
    });
    return true;
}

void DaveApp::handleShortcuts() {
    auto& transport = audio_.transport();
    ImGuiIO& io = ImGui::GetIO();

    // Ignore shortcuts while typing in a text field.
    if (io.WantTextInput) return;

    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;

    // repeat=false: these are edge-triggered actions — ignore key auto-repeat
    // so a single Space press doesn't toggle play on then off again.
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        transport.toggle();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        // Return/Enter: jump playhead to start (standard DAW shortcut).
        transport.seek(0);
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && !shift && undo_.canUndo()) {
        undo_.undo();
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && shift && undo_.canRedo()) {
        undo_.redo();
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        // Cmd+O: open project (audio import is Cmd+I now).
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_I, false)) {
        openWavDialog();
    } else if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        openProjectDialog();
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        saveProject(false);
    } else if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        saveProject(true);
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false) && !shift) {
        newProject();
    } else if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
        window_.close();
    } else if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        // T = zoom in (Pro Tools convention).
        gui::zoomAroundSample(view_, view_.samplesPerPixel * 0.5,
                              audio_.transport().position());
    } else if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        // R = zoom out.
        gui::zoomAroundSample(view_, view_.samplesPerPixel * 2.0,
                              audio_.transport().position());
    } else if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        // L = loop on/off, over whatever range is current.
        loopEnabled_ = !loopEnabled_;
        syncTransportLoop();
    } else if (!ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        // Shift+M / Shift+S toggle the selected track. The modifier keeps the
        // bare letters free and stops a stray keypress from silencing a track
        // the user was only looking at. Guarded on !ctrl so they don't shadow
        // Ctrl+Shift+S (Save As).
        toggleSelectedTrackMute();
    } else if (!ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        toggleSelectedTrackSolo();
    } else if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        // Pop the picture in or out of its own window. macOS reaches the same
        // action through the native Window menu's Cmd+Shift+V. With no video
        // loaded there is no picture to move, so the binding does nothing.
        if (!edit_.videoTracks().empty()) {
            setVideoPoppedOut(!videoPoppedOut_);
        }
    }
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
document::MidiTrack* DaveApp::selectedMidiTrack() {
    const int sel = view_.selectedTrackIndex;
    const int audioCount = static_cast<int>(edit_.tracks().size());
    const int midiIndex = sel - audioCount;
    if (midiIndex < 0 || midiIndex >= static_cast<int>(edit_.midiTracks().size())) {
        return nullptr;
    }
    return &edit_.midiTracksMut()[static_cast<size_t>(midiIndex)];
}

void DaveApp::toggleSelectedTrackMute() {
    if (document::Track* track = selectedTrack()) {
        track->mute = !track->mute;
        edit_.notifyChanged();
    } else if (document::MidiTrack* midi = selectedMidiTrack()) {
        midi->mute = !midi->mute;
        edit_.notifyChanged();
    }
}

void DaveApp::toggleSelectedTrackSolo() {
    if (document::Track* track = selectedTrack()) {
        track->solo = !track->solo;
        edit_.notifyChanged();
    } else if (document::MidiTrack* midi = selectedMidiTrack()) {
        midi->solo = !midi->solo;
        edit_.notifyChanged();
    }
}

void DaveApp::setTimelineSamplesPerPixel(double samplesPerPixel) {
    // Screenshot fixtures need deterministic zoom without manufacturing input
    // events, while the same bounds remain in force as interactive zooming.
    gui::zoomAroundSample(view_, samplesPerPixel,
                          audio_.transport().position());
}

void DaveApp::onEditChanged() {
    // UI thread. Re-derive the engine graph from the Edit, compile, publish.
    dirty_ = true; // any edit marks the project dirty
    auto graph = builder_.build(edit_, audio_.sampleRate());
    auto [compiled, err] = engine::compile(*graph, audio_.sampleRate(), 256);
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
    // Fixed panel windows remain predictable, while the two gaps between
    // them are live splitters. The ratio-based picture panel keeps its visual
    // priority on taller displays without starving the plugin chain.
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
    const float availablePanelW =
        std::max(2.0f, vp->WorkSize.x - splitterSize);
    const float minSidebarW =
        std::min(320.0f, availablePanelW * 0.5f);
    const float minTimelineW =
        std::min(560.0f, availablePanelW - minSidebarW);
    const float maxSidebarW =
        std::max(minSidebarW, availablePanelW - minTimelineW);
    const float sidebarW = std::clamp(sidebarWidth_, minSidebarW, maxSidebarW);
    sidebarWidth_ = sidebarW;
    const float timelineW = availablePanelW - sidebarW;
    const float sidebarX = baseX + timelineW + splitterSize;

    const float splitContentH = std::max(1.0f, contentH - splitterSize);
    const float minVideoH = std::min(260.0f, splitContentH * 0.55f);
    const float minPluginsH = std::min(180.0f, splitContentH * 0.38f);
    const float maxVideoH = std::max(minVideoH, splitContentH - minPluginsH);
    // Picture is opt-in. Until a video is imported there is nothing to show,
    // so the sidebar is the plugin chain alone rather than a large empty
    // panel advertising a workflow this session isn't using.
    const bool hasVideo = !edit_.videoTracks().empty();
    const bool videoDocked = hasVideo && !videoPoppedOut_;
    // With the picture popped out or absent there is no split to maintain: the
    // plugin chain takes the whole sidebar and videoShare_ is preserved
    // untouched so the old proportions come back when the picture returns.
    const float videoPanelH =
        videoDocked
            ? std::clamp(splitContentH * videoShare_, minVideoH, maxVideoH)
            : 0.0f;
    const float pluginsH =
        videoDocked ? splitContentH - videoPanelH : contentH;
    if (videoDocked) {
        videoShare_ = videoPanelH / splitContentH;
    }

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
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, undo_.canUndo())) undo_.undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, undo_.canRedo())) undo_.redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Add Track"))
                undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Transport")) {
            if (ImGui::MenuItem(transport.isPlaying() ? "Stop" : "Play", "Space"))
                transport.toggle();
            if (ImGui::MenuItem("Return to Start", "Return")) transport.seek(0);
            if (ImGui::MenuItem("Stop & Rewind")) { transport.stop(); transport.seek(0); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Mixer", "Ctrl+=", showMixer_))
                showMixer_ = !showMixer_;
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
                "Space: play/stop | L: loop | Shift+M / Shift+S: mute / solo");
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
        // Every framed control derives its height from FramePadding, so one
        // value here makes buttons, combos and the checkbox agree instead of
        // each settling at whatever its own content implies.
        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(10.0f,
                   std::max(0.0f, (controlH - ImGui::GetFontSize()) * 0.5f)));
        // Two gaps, used consistently: tight between a label and the control
        // it names, wider between separate controls.
        constexpr float kGap = 8.0f;
        constexpr float kLabelGap = 6.0f;
        constexpr float kComboW = 118.0f;
        // Labels sit on the text baseline by default, which floats them to
        // the top of a 30 px row.
        auto label = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(pal.textMuted, "%s", text);
            ImGui::SameLine(0.0f, kLabelGap);
        };
        auto groupSeparator = [&] {
            ImGui::SameLine(0.0f, 12.0f);
            const ImVec2 lineTop = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(1.0f, controlH));
            ImGui::GetWindowDrawList()->AddLine(
                lineTop, ImVec2(lineTop.x, lineTop.y + controlH),
                ImGui::GetColorU32(pal.borderStrong));
            ImGui::SameLine(0.0f, 12.0f);
        };

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
                loopEnabled_ = !loopEnabled_;
                syncTransportLoop();
            }
        }
        groupSeparator();

        // Position readout — double-click to edit (type a timecode to seek).
        int64_t pos = transport.position();
        const char* tcModes[] = {"min:sec", "timecode", "bars|beats", "feet+frames", "samples"};
        int tcIdx = static_cast<int>(view_.tcMode);
        constexpr float counterW = 150.0f;
        if (!view_.editingPosition) {
            std::string tcStr = gui::formatTimecode(pos, view_.tcMode);
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
                        int bars, beats, ticks;
                        if (std::sscanf(view_.positionInput, "%d.%d.%d", &bars, &beats, &ticks) == 3) {
                            double bpm = 120.0;
                            double beatsTotal = (bars - 1) * 4 + (beats - 1) + ticks / 960.0;
                            target = static_cast<int64_t>(beatsTotal * 60.0 / bpm * sr);
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
        ImGui::SameLine(0.0f, kGap);
        ImGui::SetNextItemWidth(kComboW);
        if (ImGui::Combo("##tcmode", &tcIdx, tcModes, 5)) {
            view_.tcMode = static_cast<gui::TimecodeMode>(tcIdx);
        }
        groupSeparator();

        if (ImGui::Button("+Track", ImVec2(76.0f, controlH)))
            undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));

        ImGui::SameLine(0.0f, kGap);
        ImGui::Checkbox("Snap", &view_.snapToMarkers);
        groupSeparator();

        // ─── Session format ──────────────────────────────────────────────
        label("Session");
        static constexpr int kRates[] = {44100, 48000, 88200, 96000,
                                         176400, 192000};
        static const char* kRateLabels[] = {"44.1 kHz", "48 kHz", "88.2 kHz",
                                            "96 kHz", "176.4 kHz", "192 kHz"};
        int rateIdx = 1;
        for (int i = 0; i < IM_ARRAYSIZE(kRates); ++i) {
            if (kRates[i] == edit_.sampleRate()) rateIdx = i;
        }
        ImGui::SetNextItemWidth(kComboW);
        if (ImGui::Combo("##sessionRate", &rateIdx, kRateLabels,
                         IM_ARRAYSIZE(kRateLabels))) {
            edit_.setSampleRate(kRates[rateIdx]);
            applySessionSampleRate();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Session sample rate — reopens the output device.\n"
                "Clip positions are stored as samples and are not converted, "
                "so material recorded at another rate plays at the wrong "
                "speed. Set this before recording.");
        }

        ImGui::SameLine(0.0f, kGap);
        static constexpr int kDepths[] = {16, 24, 32};
        static const char* kDepthLabels[] = {"16-bit", "24-bit",
                                             "32-bit float"};
        int depthIdx = 1;
        for (int i = 0; i < IM_ARRAYSIZE(kDepths); ++i) {
            if (kDepths[i] == edit_.bitDepth()) depthIdx = i;
        }
        ImGui::SetNextItemWidth(kComboW);
        if (ImGui::Combo("##sessionDepth", &depthIdx, kDepthLabels,
                         IM_ARRAYSIZE(kDepthLabels))) {
            edit_.setBitDepth(kDepths[depthIdx]);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Bit depth for files this session writes.\n"
                "Stored with the project; nothing renders yet, so it has no "
                "effect on playback.");
        }
        groupSeparator();

        label("Output");
        static int selectedDevice = -1;
        static std::vector<std::string> devices;
        static bool listed = false;
        if (!listed) {
            devices = audio_.enumerateDevices();
            selectedDevice = audio_.currentDeviceIndex();
            listed = true;
        }
        static std::vector<std::string> labelStore;
        labelStore = devices;
        std::vector<const char*> labels;
        labels.reserve(labelStore.size());
        for (const auto& n : labelStore) labels.push_back(n.c_str());
        const char* preview = (selectedDevice >= 0 && selectedDevice < static_cast<int>(labels.size()))
                              ? labels[selectedDevice] : "Default";
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##output", preview)) {
            for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
                bool sel = (i == selectedDevice);
                if (ImGui::Selectable(labels[i], &sel)) {
                    selectedDevice = i;
                    // Open the new device at the session's rate, not the
                    // default — switching outputs must not silently retune
                    // the session.
                    audio_.selectDevice(
                        i, static_cast<double>(edit_.sampleRate()), 2);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();   // FramePadding
    }
    ImGui::End();

    // ─── Timeline + mixer (left column, split horizontally) ──────────────
    // The mixer takes the bottom of the timeline column rather than a slot in
    // the sidebar: strips sit side by side, so the panel needs width, and the
    // sidebar is a ~360px inspector column.
    const float minMixerH = 190.0f;
    const float maxMixerH = std::max(minMixerH, contentH - 200.0f);
    const float mixerH = showMixer_
        ? std::clamp(mixerHeight_, minMixerH, maxMixerH) : 0.0f;
    if (showMixer_) mixerHeight_ = mixerH;
    const float timelineH =
        showMixer_ ? std::max(1.0f, contentH - mixerH - splitterSize) : contentH;

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
    gui::drawTimeline(
        edit_, undo_, transport, peaks_, view_, builder_.assetBuffers(),
        trackRowHeight);
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
        gui::drawMixer(edit_, undo_, view_);
        ImGui::End();
    }
    // Both the timeline and the mixer can ask for a picker or an editor, so
    // the requests are serviced once, after both have drawn.
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
        videoPanelPos_ = ImVec2(sidebarX, contentY);
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

    ImGui::SetNextWindowPos(
        ImVec2(sidebarX,
               videoDocked ? contentY + videoPanelH + splitterSize
                           : contentY),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sidebarW, pluginsH), ImGuiCond_Always);
    ImGui::Begin("Plugins", nullptr, panelFlags);
    gui::theme::panelHeader("Plugins");
    drawPluginsPanelContent();
    ImGui::End();

    const ImGuiWindowFlags splitterFlags =
        panelFlags | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

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
            ImVec2(sidebarX, contentY + videoPanelH), ImGuiCond_Always);
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
            videoShare_ = std::clamp(
                (videoPanelH + ImGui::GetIO().MouseDelta.y) / splitContentH,
                minVideoH / splitContentH, maxVideoH / splitContentH);
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

    // ─── Auto-stop ───────────────────────────────────────────────────────
    // Only auto-stop if there's actual content (clips) on the timeline. An
    // empty timeline has contentEnd=24000 (just the 0.5s tail), which would
    // instantly stop playback before the user can do anything.
    if (audio_.transport().isPlaying()) {
        int64_t end = edit_.contentEndSamples();
        bool hasClips = false;
        for (const auto& t : edit_.tracks())
            if (!t.clips.empty()) { hasClips = true; break; }
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
        mt.clips.push_back(std::move(clip));
        tracks.push_back(std::move(mt));
    }

    if (tracks.empty()) {
        std::fprintf(stderr, "Dave: %s has no notes to import\n", path.c_str());
        return false;
    }

    const size_t firstNew = edit_.midiTracks().size();
    undo_.execute(std::make_unique<editing::ImportMidiFileCommand>(
        std::move(tracks), fileName));
    // Select the first imported row so the instrument picker in the gutter is
    // the obvious next step rather than something to go hunting for.
    view_.selectedTrackIndex =
        static_cast<int>(edit_.tracks().size() + firstNew);
    std::fprintf(stderr, "Dave: imported %s (%zu track%s)\n", fileName.c_str(),
                 edit_.midiTracks().size() - firstNew,
                 (edit_.midiTracks().size() - firstNew) == 1 ? "" : "s");
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
    edit_.tracksMut().clear();
    edit_.clearMarkerTracks_();
    edit_.clearVideoTracks_();
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

// Reopen the output device at the session rate and re-derive the graph
// against it. Called when the rate changes and after loading a project that
// carries a different one — otherwise the engine keeps running at the old
// rate and everything plays back detuned.
void DaveApp::applySessionSampleRate() {
    const double rate = static_cast<double>(edit_.sampleRate());
    if (audio_.sampleRate() == rate) return;
    if (!audio_.selectDevice(audio_.currentDeviceIndex(), rate, 2)) {
        std::fprintf(stderr,
                     "Dave: could not open the output device at %.0f Hz; "
                     "the engine is still running at %.0f Hz\n",
                     rate, audio_.sampleRate());
        return;
    }
    onEditChanged();   // recompile the graph against the new rate
}

void DaveApp::openProjectDialog() {
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
        builder_ = {};  // force re-derive with fresh plugin instances
        videoDecoder_.close();
        lastRequestedFrameIndex_ = -1;
        lastUploadedFrameIndex_ = -1;
        projectPath_ = path;
        dirty_ = false;
        undo_.clear();
        applySessionSampleRate();   // the project may carry a different rate
        onEditChanged();  // rebuild graph for the loaded Edit
        view_.selectedTrackIndex = edit_.tracks().empty() ? -1 : 0;
        std::fprintf(stderr, "Dave: opened %s\n", path.c_str());
    }
}

void DaveApp::saveProject(bool saveAs) {
    // Capture plugin states before serializing — get each loaded plugin's
    // current parameter/internal state and stash it in the slot.
    for (auto& track : edit_.tracksMut()) {
        for (auto& slot : track.plugins) {
            auto inst = builder_.pluginInstance(slot.id);
            if (inst && inst->isLoaded()) {
                slot.stateBase64 = inst->getStateBase64();
            }
        }
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
        gui::formatTimecode(playhead, view_.tcMode).c_str(),
        static_cast<long long>(frameIndex),
        inRange ? "" : "(out of range)");
}

void DaveApp::drawPluginsPanel() { drawPluginsPanelContent(); }

void DaveApp::drawMidiTrackPanel(const document::MidiTrack& track) {
    ImGui::Text("MIDI Track: %s", track.name.c_str());
    ImGui::Separator();

    // Instrument first: the effect chain below it is meaningless until
    // something is generating audio to run through it.
    ImGui::TextUnformatted("Instrument");
    if (track.instrument.uidString.empty()) {
        if (ImGui::Button("Choose Instrument...")) {
            openPluginBrowser(BrowserMode::MidiInstrument, track.id);
        }
    } else {
        ImGui::Text("%s", track.instrument.name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit##instrument")) {
            openPluginEditor(track.instrument);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Replace##instrument")) {
            openPluginBrowser(BrowserMode::MidiInstrument, track.id);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##instrument")) {
            editors_.erase(track.instrument.id);
            undo_.execute(std::make_unique<editing::SetMidiInstrumentCommand>(
                track.id, document::PluginSlot{}));
            return;   // `track` may be reallocated by the command
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Effects");
    std::string removeSlotId;
    int slotIdx = 0;
    for (const auto& slot : track.plugins) {
        ImGui::PushID(slotIdx);
        bool bypass = slot.bypass;
        if (ImGui::Checkbox("##bypass", &bypass)) {
            const_cast<document::PluginSlot&>(slot).bypass = bypass;
            edit_.notifyChanged();
        }
        ImGui::SameLine();
        if (slot.bypass) ImGui::TextDisabled("%s", slot.name.c_str());
        else             ImGui::Text("%s", slot.name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit")) openPluginEditor(slot);
        ImGui::SameLine();
        // Defer the mutation until the range-for completes; removing in place
        // invalidates the current slot reference.
        if (ImGui::SmallButton("Remove")) removeSlotId = slot.id;
        ImGui::PopID();
        ++slotIdx;
    }
    if (!removeSlotId.empty()) {
        undo_.execute(std::make_unique<editing::RemoveMidiPluginCommand>(
            track.id, removeSlotId));
        editors_.erase(removeSlotId);
        return;
    }
    if (ImGui::Button("Add Plugin")) {
        openPluginBrowser(BrowserMode::MidiFx, track.id);
    }
}

void DaveApp::drawPluginsPanelContent() {
    // Operate on the currently-selected track (selectedTrackIndex in the view).
    // The index spans both bands: audio rows first, then MIDI rows, which is
    // the order they're drawn in on the timeline.
    int sel = view_.selectedTrackIndex;
    const int audioCount = static_cast<int>(edit_.tracks().size());
    const int midiCount = static_cast<int>(edit_.midiTracks().size());
    if (sel >= audioCount && sel < audioCount + midiCount) {
        drawMidiTrackPanel(edit_.midiTracks()[sel - audioCount]);
        return;
    }
    if (sel < 0 || sel >= audioCount) {
        if (edit_.tracks().empty()) {
            if (drawCenteredEmptyState(
                    "No track selected",
                    "Create a track before building an effect chain.",
                    "+ Add Track###pluginsEmptyTrack")) {
                undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));
                view_.selectedTrackIndex =
                    static_cast<int>(edit_.tracks().size()) - 1;
            }
        } else if (drawCenteredEmptyState(
                       "Select a track to manage plugins",
                       "The plugin chain follows the selected timeline row.",
                       "Select First Track###pluginsSelectTrack")) {
            view_.selectedTrackIndex = 0;
        }
        return;
    }

    const auto& track = edit_.tracks()[sel];
    ImGui::Text("Track: %s", track.name.c_str());
    ImGui::Separator();

    if (track.plugins.empty()) {
        std::string message = "No plugins on " + track.name;
        if (drawCenteredEmptyState(
                message.c_str(),
                "Add an effect to begin shaping this track.",
                "Add Plugin###pluginsEmptyChain")) {
            if (pluginHost_.descriptors().empty()) {
                pluginHost_.scan();
            }
            openPluginBrowser(BrowserMode::AudioFx, track.id);
        }
        return;
    }

    std::string removeSlotId;
    int slotIdx = 0;
    for (const auto& slot : track.plugins) {
        ImGui::PushID(slotIdx);
        // Bypass checkbox + name + Edit + Remove.
        bool bypass = slot.bypass;
        if (ImGui::Checkbox("##bypass", &bypass)) {
            const_cast<document::PluginSlot&>(slot).bypass = bypass;
            edit_.notifyChanged();
        }
        ImGui::SameLine();
        if (slot.bypass) ImGui::TextDisabled("%s", slot.name.c_str());
        else             ImGui::Text("%s", slot.name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit")) {
            openPluginEditor(slot);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            // Defer mutation until the range-for completes; removing in place
            // invalidates the current slot reference.
            removeSlotId = slot.id;
        }
        ImGui::PopID();
        ++slotIdx;
    }
    if (!removeSlotId.empty()) {
        undo_.execute(std::make_unique<editing::RemovePluginCommand>(
            track.id, removeSlotId));
        editors_.erase(removeSlotId);
    }

    ImGui::Separator();
    if (ImGui::Button("Add Plugin")) {
        openPluginBrowser(BrowserMode::AudioFx, track.id);
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
        for (const auto& s : t.plugins) {
            if (s.id == slotId) return &s;
        }
    }
    for (const auto& mt : edit_.midiTracks()) {
        if (mt.instrument.id == slotId) return &mt.instrument;
        for (const auto& s : mt.plugins) {
            if (s.id == slotId) return &s;
        }
    }
    return nullptr;
}

void DaveApp::serviceViewRequests() {
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
                        undo_.execute(std::make_unique<editing::AddMidiPluginCommand>(
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
