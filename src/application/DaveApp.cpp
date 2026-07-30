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
            if (suffix != ".wav") {
                std::fprintf(stderr, "Dave: ignored dropped non-WAV file: %s\n", path.c_str());
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
    platform::g_menuLoadVideo    = [this](){ openVideoDialog(); };
    platform::g_menuImportMarkers= [this](){ importMarkersDialog(); };
    platform::g_menuExportMarkers= [this](){ exportMarkersDialog(); };
    platform::g_menuUndo         = [this](){ undo_.undo(); };
    platform::g_menuRedo         = [this](){ undo_.redo(); };
    platform::g_menuPlayStop     = [this](){ audio_.transport().toggle(); };
    platform::g_menuReturnToStart= [this](){ audio_.transport().seek(0); };
    platform::g_menuQuit         = [this](){ window_.close(); };
    platform::setupMacMenuBar();
#endif
    if (!audio_.start(48000.0, 2)) {
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
        view_.samplesPerPixel = std::max(4.0, view_.samplesPerPixel * 0.5);
    } else if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        // R = zoom out.
        view_.samplesPerPixel = std::min(50000.0, view_.samplesPerPixel * 2.0);
    } else if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        // M / S toggle the selected track, the standard DAW binding. Guarded
        // on !ctrl so they don't shadow Cmd+M (minimise) or Cmd+S (save).
        toggleSelectedTrackMute();
    } else if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        toggleSelectedTrackSolo();
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

void DaveApp::toggleSelectedTrackMute() {
    if (document::Track* track = selectedTrack()) {
        track->mute = !track->mute;
        edit_.notifyChanged();
    }
}

void DaveApp::toggleSelectedTrackSolo() {
    if (document::Track* track = selectedTrack()) {
        track->solo = !track->solo;
        edit_.notifyChanged();
    }
}

void DaveApp::setTimelineSamplesPerPixel(double samplesPerPixel) {
    // Screenshot fixtures need deterministic zoom without manufacturing input
    // events, while the same bounds remain in force as interactive zooming.
    view_.samplesPerPixel = std::clamp(samplesPerPixel, 4.0, 50000.0);
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

    // Sync the transport's loop region from the first active Loop marker (if
    // any). Adding/moving/removing a loop region marker updates transport
    // looping behavior immediately.
    auto& transport = audio_.transport();
    if (const auto* loop = edit_.activeLoopMarker()) {
        transport.setLoop(loop->position, loop->position + loop->length);
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
    const float videoPanelH =
        std::clamp(splitContentH * videoShare_, minVideoH, maxVideoH);
    const float pluginsH = splitContentH - videoPanelH;
    videoShare_ = videoPanelH / splitContentH;

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
        if (ImGui::BeginMenu("Help")) {
            ImGui::TextDisabled("Dave — D.A.V.E.");
            ImGui::TextDisabled("Scroll: pan | Ctrl+scroll: zoom | R-click clip: menu");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
#endif // __APPLE__

    // ─── Transport bar (full width, below menu) ──────────────────────────
    ImGui::SetNextWindowPos(ImVec2(baseX, baseY + menuH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, toolbarH), ImGuiCond_Always);
    ImGui::Begin("Transport", nullptr, panelFlags);
    {
        const float controlH = 30.0f;
        auto groupSeparator = [&] {
            ImGui::SameLine(0.0f, 10.0f);
            const ImVec2 lineTop = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(1.0f, controlH));
            ImGui::GetWindowDrawList()->AddLine(
                lineTop, ImVec2(lineTop.x, lineTop.y + controlH),
                ImGui::GetColorU32(pal.borderStrong));
            ImGui::SameLine(0.0f, 10.0f);
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

        ImGui::SameLine();
        if (gui::theme::iconButton(
                "##transportRewind", gui::theme::TransportIcon::ReturnToStart,
                "Stop and return to start (Return)", ImVec2(controlH, controlH))) {
            transport.stop();
            transport.seek(0);
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
                double sr = 48000.0;
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
        ImGui::SameLine();
        ImGui::SetNextItemWidth(108.0f);
        if (ImGui::Combo("##tcmode", &tcIdx, tcModes, 5)) {
            view_.tcMode = static_cast<gui::TimecodeMode>(tcIdx);
        }
        groupSeparator();

        if (ImGui::Button("+Track", ImVec2(76.0f, controlH)))
            undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));

        ImGui::SameLine();
        ImGui::Checkbox("Snap", &view_.snapToMarkers);
        groupSeparator();

        ImGui::TextColored(pal.textMuted, "Output");
        ImGui::SameLine();
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
                    audio_.selectDevice(i);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::End();

    // ─── Timeline (left, fills remaining width) ──────────────────────────
    ImGui::SetNextWindowPos(ImVec2(baseX, contentY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(timelineW, contentH), ImGuiCond_Always);
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

    // Picture stays above the plugin chain because sync-to-picture is the
    // primary post-production task.
    ImGui::SetNextWindowPos(ImVec2(sidebarX, contentY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sidebarW, videoPanelH), ImGuiCond_Always);
    ImGui::Begin("Video", nullptr, panelFlags);
    gui::theme::panelHeader("Video");
    drawVideoPreviewContent();
    ImGui::End();

    ImGui::SetNextWindowPos(
        ImVec2(sidebarX, contentY + videoPanelH + splitterSize),
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
    ImGui::PopStyleVar(2);

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
        lastDecodedFrameIndex_ = -1;
    }
}

void DaveApp::newProject() {
    edit_.tracksMut().clear();
    edit_.clearMarkerTracks_();
    edit_.clearVideoTracks_();
    builder_ = {};
    videoDecoder_.close();
    lastDecodedFrameIndex_ = -1;
    projectPath_.clear();
    dirty_ = false;
    undo_.clear();
    undo_.execute(std::make_unique<editing::AddTrackCommand>("Track 1"));
    view_.selectedTrackIndex = 0;
    edit_.addMarkerTrack("Markers");
    audio_.transport().stop();
    audio_.transport().seek(0);
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
        lastDecodedFrameIndex_ = -1;
        projectPath_ = path;
        dirty_ = false;
        undo_.clear();
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

    const double audioSr = 48000.0;
    int64_t relSamples = (playhead - clip->timelineStart) + clip->sourceOffset;
    double videoTimeSec = (relSamples > 0) ? (relSamples / audioSr) : 0.0;
    double clipDurationSec = (clip->length > 0)
        ? (clip->length / audioSr) : clip->durationSeconds;
    bool inRange = (videoTimeSec >= 0.0 && videoTimeSec <= clipDurationSec);
    int64_t frameIndex = (clip->fps > 0.0)
        ? static_cast<int64_t>(videoTimeSec * clip->fps) : 0;

    // If the active clip changed, reset state.
    if (lastVideoClipId_ != clip->id) {
        lastDecodedFrameIndex_ = -1;
        lastVideoClipId_ = clip->id;
    }

    // Request a frame from the async decoder if the frame index changed
    // and the decoder isn't already busy with a request.
    if (inRange && frameIndex != lastDecodedFrameIndex_ && !asyncDecoder_.isBusy()) {
        const int previewMaxW = 480;
        int pw = clip->width, ph = clip->height;
        if (pw > previewMaxW) { ph = ph * previewMaxW / pw; pw = previewMaxW; }
        double seekTo = static_cast<double>(frameIndex) / clip->fps;
        asyncDecoder_.requestFrame(clip->path, seekTo, pw, ph, clip->fps);
    }

    // Check if the async decoder has a new frame ready.
    engine::VideoFrame frame;
    if (asyncDecoder_.getLatestFrame(frame) && frame.frameIndex == frameIndex) {
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
        lastDecodedFrameIndex_ = frameIndex;
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

void DaveApp::drawPluginsPanelContent() {
    // Operate on the currently-selected track (selectedTrackIndex in the view).
    int sel = view_.selectedTrackIndex;
    if (sel < 0 || sel >= static_cast<int>(edit_.tracks().size())) {
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
            browserTargetTrackId_ = track.id;
            showPluginBrowser_ = true;
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
            // Open (or focus) the plugin's editor window. The PluginInstance is
            // owned by GraphBuilder, keyed by slot id — we look it up there.
            auto inst = builder_.pluginInstance(slot.id);
            if (inst) {
                auto& ed = editors_[slot.id];
                if (!ed) ed = std::make_unique<engine::PluginEditor>();
                if (!ed->isOpen()) {
                    ed->open(*inst, slot.name);
                }
            }
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
        // Scan if not already done, then open the browser targeting this track.
        if (pluginHost_.descriptors().empty()) {
            pluginHost_.scan();
        }
        browserTargetTrackId_ = track.id;
        showPluginBrowser_ = true;
    }


}

void DaveApp::drawPluginBrowser() {
    if (!showPluginBrowser_) return;

    ImGui::SetNextWindowSize(ImVec2(500, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("Add Plugin", &showPluginBrowser_,
                     ImGuiWindowFlags_NoDocking)) {
        ImGui::Text("Available VST3 plugins");
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu found)", pluginHost_.descriptors().size());
        ImGui::InputText("##filter", browserFilter_, sizeof(browserFilter_));
        ImGui::Separator();

        // List plugins; clicking one adds it to the target track.
        // Use the loop index for PushID to guarantee unique widget IDs (the
        // 'Add' button + Text appear in every row, so they'd collide without
        // a per-row ID scope).
        int pluginIdx = 0;
        for (const auto& d : pluginHost_.descriptors()) {
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
            if (ImGui::SmallButton("Add")) {
                document::PluginSlot slot;
                slot.name = d.name;
                slot.uidString = d.uidString;
                slot.path = d.path;
                slot.bypass = false;
                undo_.execute(std::make_unique<editing::AddPluginCommand>(
                    browserTargetTrackId_, slot));
                showPluginBrowser_ = false;
            }
            ImGui::PopID();
            ++pluginIdx;
        }
    }
    ImGui::End();
}

} // namespace dave::application
