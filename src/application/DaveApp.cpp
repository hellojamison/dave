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
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>

namespace dave::application {

DaveApp::~DaveApp() {
    audio_.stop();
    audio_.setCompiledGraph(nullptr);
}

bool DaveApp::init() {
    if (!window_.valid() || !imgui_.init(window_)) {
        return false;
    }

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
        openWavDialog();
    } else if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        openProjectDialog();
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        saveProject(false);
    } else if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        saveProject(true);
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        newProject();
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
        window_.close();
    }
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

void DaveApp::loadWavIntoEdit(const std::string& path) {
    auto assetId = edit_.importAsset(path);
    if (!assetId.valid()) {
        std::fprintf(stderr, "Dave: import failed: %s\n", path.c_str());
        return;
    }
    const auto* asset = edit_.asset(assetId);
    if (asset == nullptr) {
        std::fprintf(stderr, "Dave: asset lookup failed after import\n");
        return;
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
    // The entire window is divided into non-overlapping fixed regions:
    //   [menu bar]                          — auto height (~24px)
    //   [transport bar]                     — 52px, full width
    //   [timeline]     [right sidebar]      — timeline fills, sidebar 300px
    // The sidebar is split: Plugins (top) + Video preview (bottom).
    // NO floating windows, NO dockspace — everything is computed from the
    // viewport and locked with NoMove+NoResize+NoTitleBar.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // menuH = space consumed by the menu bar. On macOS the menu is at the top
    // of the SCREEN (not in-window), so menuH = 0 and the transport bar starts
    // at the very top of the window. On Windows/Linux the in-window ImGui menu
    // bar consumes ~24px.
#ifdef __APPLE__
    const float menuH = 0.0f;
#else
    const float menuH = vp->WorkPos.y;
#endif
    const float toolbarH = 48.0f;
    const float sidebarW = 300.0f;
    const float videoPanelH = 240.0f;
    const float contentY = menuH + toolbarH;
    const float contentH = vp->WorkSize.y - toolbarH;
    const float timelineW = vp->WorkSize.x - sidebarW;
    const float pluginsH = contentH - videoPanelH;

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
    ImGui::SetNextWindowPos(ImVec2(0, menuH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, toolbarH), ImGuiCond_Always);
    ImGui::Begin("Transport", nullptr, panelFlags);
    {
        const ImVec2 btnSize(60, 30);
        // Play/Stop with accent when active. Capture the state BEFORE the
        // button click — if toggle() fires during the click, the push/pop
        // must still balance (push if was playing, pop regardless).
        bool wasPlaying = transport.isPlaying();
        if (wasPlaying) {
            ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal.accentHover);
        }
        if (ImGui::Button(wasPlaying ? "Stop" : "Play", btnSize))
            transport.toggle();
        if (wasPlaying) ImGui::PopStyleColor(2);

        ImGui::SameLine();
        if (ImGui::Button("Rewind", btnSize)) { transport.stop(); transport.seek(0); }
        ImGui::SameLine(0, 20);

        // Position readout.
        int64_t pos = transport.position();
        int totalSec = static_cast<int>(pos / 48000);
        ImGui::TextColored(pal.accent, "%02d:%05.2f",
                           totalSec / 60,
                           static_cast<double>(totalSec % 60) +
                               static_cast<double>(pos % 48000) / 48000.0);

        // Right-aligned buttons.
        ImGui::SameLine(timelineW - btnSize.x * 3 - 20);
        if (ImGui::Button("Undo", btnSize)) undo_.undo();
        ImGui::SameLine();
        if (ImGui::Button("Redo", btnSize)) undo_.redo();
        ImGui::SameLine();
        if (ImGui::Button("+Track", btnSize))
            undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));

        // Snap-to-marker toggle.
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &view_.snapToMarkers);

        // Output device picker (far right, in the sidebar area).
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
        for (const auto& n : labelStore) labels.push_back(n.c_str());
        const char* preview = (selectedDevice >= 0 && selectedDevice < static_cast<int>(labels.size()))
                              ? labels[selectedDevice] : "Default";
        ImGui::SetNextItemWidth(sidebarW - btnSize.x * 3 - 60);
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
    ImGui::SetNextWindowPos(ImVec2(0, contentY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(timelineW, contentH), ImGuiCond_Always);
    ImGui::Begin("Timeline", nullptr, panelFlags);
    gui::drawTimeline(edit_, undo_, transport, peaks_, view_, builder_.assetBuffers());
    ImGui::End();

    // ─── Plugins panel (right sidebar, top portion) ──────────────────────
    ImGui::SetNextWindowPos(ImVec2(timelineW, contentY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sidebarW, pluginsH), ImGuiCond_Always);
    ImGui::Begin("Plugins", nullptr, panelFlags);
    drawPluginsPanelContent();
    ImGui::End();

    // ─── Video preview (right sidebar, bottom portion) ───────────────────
    ImGui::SetNextWindowPos(ImVec2(timelineW, contentY + pluginsH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sidebarW, videoPanelH), ImGuiCond_Always);
    ImGui::Begin("Video", nullptr, panelFlags);
    drawVideoPreviewContent();
    ImGui::End();

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


    // Find the video clip active at the current playhead (RB-6: multi-clip).
    int64_t playhead = audio_.transport().position();
    const auto* clip = edit_.videoClipAt(playhead);

    if (!clip) {
        ImGui::TextDisabled("(no video at playhead)");
        if (ImGui::Button("Load Video...")) openVideoDialog();
        return;
    }

    ImGui::Text("%s", clip->name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Add Video...")) openVideoDialog();
    ImGui::TextDisabled("%dx%d @ %.3ffps  %.1fs", clip->width, clip->height,
                        clip->fps, clip->durationSeconds);

    // Compute the target video frame. relSamples includes sourceOffset so
    // trimmed clips play from the right point in the source.
    const double audioSr = 48000.0;
    int64_t relSamples = (playhead - clip->timelineStart) +
                         (clip->sourceOffset / 1); // both in audio samples
    double videoTimeSec = (relSamples > 0) ? (relSamples / audioSr) : 0.0;
    double clipDurationSec = (clip->length > 0)
        ? (clip->length / audioSr) : clip->durationSeconds;
    bool inRange = (videoTimeSec >= 0.0 && videoTimeSec <= clipDurationSec);
    int64_t frameIndex = (clip->fps > 0.0)
        ? static_cast<int64_t>(videoTimeSec * clip->fps) : 0;

    // If the active clip changed, force a re-decode (close the old process).
    if (lastVideoClipId_ != clip->id) {
        videoDecoder_.close();
        lastDecodedFrameIndex_ = -1;
        lastVideoClipId_ = clip->id;
    }

    bool sequential = videoDecoder_.isOpen() && frameIndex == lastDecodedFrameIndex_ + 1;
    double nowSec = ImGui::GetTime();
    bool seekAllowed = sequential || (nowSec - lastSeekTime_ >= 0.15);
    if (inRange && frameIndex != lastDecodedFrameIndex_ && seekAllowed) {
        lastSeekTime_ = nowSec;
        const int previewMaxW = 480;
        int pw = clip->width, ph = clip->height;
        if (pw > previewMaxW) { ph = ph * previewMaxW / pw; pw = previewMaxW; }
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
        bool got = false;
        if (sequential) {
            got = videoDecoder_.readFrame(videoFrameBuf_);
        }
        if (!got) {
            double seekTo = static_cast<double>(frameIndex) / clip->fps;
            got = videoDecoder_.seekAndRead(clip->path, seekTo, pw, ph, videoFrameBuf_);
        }
        if (got && videoFrameBuf_.size() == static_cast<size_t>(pw * ph * 4)) {
            glBindTexture(GL_TEXTURE_2D, videoTexture_);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, pw);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pw, ph, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, videoFrameBuf_.data());
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            lastDecodedFrameIndex_ = frameIndex;
        }
    }

    if (videoTexture_ != 0 && videoTexW_ > 0 && videoTexH_ > 0) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float drawW = avail.x;
        float drawH = drawW * static_cast<float>(videoTexH_) / static_cast<float>(videoTexW_);
        if (drawH > avail.y) { drawH = avail.y; drawW = drawH * static_cast<float>(videoTexW_) / static_cast<float>(videoTexH_); }
        ImGui::Image(static_cast<ImTextureID>(videoTexture_),
                     ImVec2(drawW, drawH));
    } else {
        ImGui::TextDisabled("(seek to a position to decode a frame)");
    }

    int mm = static_cast<int>(videoTimeSec) / 60;
    int ss = static_cast<int>(videoTimeSec) % 60;
    int ff = (clip->fps > 0.0) ? static_cast<int>((videoTimeSec - static_cast<int>(videoTimeSec)) * clip->fps) : 0;
    char tc[32];
    std::snprintf(tc, sizeof(tc), "%02d:%02d:%02d", mm, ss, ff);
    ImGui::TextDisabled("TC %s  frame %lld  %s", tc,
                        static_cast<long long>(frameIndex),
                        inRange ? "" : "(out of range)");


}

void DaveApp::drawPluginsPanel() { drawPluginsPanelContent(); }

void DaveApp::drawPluginsPanelContent() {
    // NoTitleBar + NoMove: it's a fixed dock column, not a floating window.
    // Operate on the currently-selected track (selectedTrackIndex in the view).
    int sel = view_.selectedTrackIndex;
    if (sel < 0 || sel >= static_cast<int>(edit_.tracks().size())) {
        ImGui::TextDisabled("Select a track to manage its plugins.");
        return;
    }

    const auto& track = edit_.tracks()[sel];
    ImGui::Text("Track: %s", track.name.c_str());
    ImGui::Separator();

    // The plugin chain.
    if (track.plugins.empty()) {
        ImGui::TextDisabled("(no plugins)");
    }
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
            undo_.execute(std::make_unique<editing::RemovePluginCommand>(track.id, slot.id));
            editors_.erase(slot.id); // close its editor if open
        }
        ImGui::PopID();
        ++slotIdx;
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
