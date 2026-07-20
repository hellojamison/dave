#include "application/DaveApp.h"
#include "document/MarkerCsv.h"
#include "document/ProjectFile.h"
#include "editing/Commands.h"
#include "gui/Theme.h"

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

    // Update the window title to show project name + dirty indicator.
    // ( glfwSetWindowTitle is cheap; calling it every frame is fine. )
    {
        std::string title = "Dave";
        if (!projectPath_.empty()) {
            // Show just the bundle name, not the full path.
            auto slash = projectPath_.find_last_of("/\\");
            title += " — " + ((slash != std::string::npos) ? projectPath_.substr(slash + 1) : projectPath_);
        } else {
            title += " — Untitled";
        }
        if (dirty_) title += " *";
        glfwSetWindowTitle(window_.handle(), title.c_str());
    }

    // --- Main menu bar -----------------------------------------------------
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
            if (ImGui::MenuItem("Add Track")) {
                undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));
            }
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
            ImGui::TextDisabled("Dave — D.A.V.E. (Digital Audio & Video Environment)");
            ImGui::TextDisabled("Scroll: pan | Ctrl+scroll: zoom | drag clip: move");
            ImGui::TextDisabled("click empty: seek | Ctrl+Z: undo");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Dockspace fills everything below the menu bar.
    ImGui::DockSpaceOverViewport();

    // --- Transport toolbar -------------------------------------------------
    ImGuiWindowFlags toolbarFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->WorkSize.x, 56), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetMainViewport()->WorkPos.y), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("Transport", nullptr, toolbarFlags);

    // Big transport buttons with the accent color when active.
    auto transportButton = [](const char* label, bool active, const ImVec4& activeColor,
                              ImVec2 size) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, activeColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeColor);
        }
        bool clicked = ImGui::Button(label, size);
        if (active) ImGui::PopStyleColor(2);
        return clicked;
    };

    const ImVec2 btnSize(64, 34);
    if (transportButton(transport.isPlaying() ? " Stop " : " Play ",
                        transport.isPlaying(), pal.accent, btnSize)) {
        transport.toggle();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rewind", btnSize)) { transport.stop(); transport.seek(0); }
    ImGui::SameLine(0, 16);

    // Position readout (mm:ss:ms) — looks like a real transport.
    int64_t pos = transport.position();
    int totalSec = static_cast<int>(pos / 48000);
    int mm = totalSec / 60;
    int ss = totalSec % 60;
    int ms = static_cast<int>((pos % 48000) * 1000 / 48000);
    char posStr[24];
    std::snprintf(posStr, sizeof(posStr), "%02d:%02d.%03d", mm, ss, ms);
    ImGui::PushFont(nullptr); // keep default; font sizing is a later pass
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);
    ImGui::TextDisabled("POSITION");
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4);
    ImGui::TextColored(pal.accent, "%s", posStr);
    ImGui::PopFont();

    // Right-aligned actions.
    const float rightStart = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine(rightStart - 64*3 - 16*2 - 220);
    if (ImGui::Button("Undo", btnSize)) undo_.undo();
    ImGui::SameLine();
    if (ImGui::Button("Redo", btnSize)) undo_.redo();
    ImGui::SameLine();
    if (ImGui::Button("+ Track", btnSize)) {
        undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));
    }

    // Output device picker — lets the user route audio to the right device
    // (miniaudio's default often picks a virtual device like Jump Desktop).
    ImGui::SameLine();
    static int selectedDevice = -1;          // -1 = whatever start() opened
    static std::vector<std::string> devices;
    static bool listed = false;
    if (!listed) {
        devices = audio_.enumerateDevices();
        selectedDevice = audio_.currentDeviceIndex();
        listed = true;
    }
    ImGui::SetNextItemWidth(200);
    // Build the label list (ImGui wants const char**).
    static std::vector<std::string> labelStore;
    labelStore = devices;
    std::vector<const char*> labels;
    labels.reserve(labelStore.size());
    for (const auto& n : labelStore) labels.push_back(n.c_str());
    int pickerIdx = selectedDevice;          // 0-based for the combo
    const char* preview = (pickerIdx >= 0 && pickerIdx < static_cast<int>(labels.size()))
                          ? labels[pickerIdx] : "Default Device";
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

    ImGui::End();
    ImGui::PopStyleVar();

    // --- Layout: Timeline on the left, Plugins panel docked on the right ----
    // Reserve a fixed-width column on the right for the Plugins panel so it
    // can't be hidden behind the Timeline (which previously filled everything).
    const float pluginsPanelWidth = 280.0f;
    const float workW = ImGui::GetMainViewport()->WorkSize.x;
    const float workH = ImGui::GetMainViewport()->WorkSize.y;
    const float workY = ImGui::GetMainViewport()->WorkPos.y;
    const float timelineWidth = workW - pluginsPanelWidth;

    // --- Timeline (left, fills width minus the plugins column) -------------
    ImGui::SetNextWindowPos(ImVec2(0, workY + 56), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(timelineWidth, workH - 56), ImGuiCond_Always);
    ImGui::Begin("Timeline", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    gui::drawTimeline(edit_, undo_, transport, peaks_, view_, builder_.assetBuffers());
    ImGui::End();

    // --- Plugins panel (right column) -------------------------------------
    ImGui::SetNextWindowPos(ImVec2(timelineWidth, workY + 56), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pluginsPanelWidth, workH - 56), ImGuiCond_Always);
    drawPluginsPanel();

    // Plugin browser modal (only when showPluginBrowser_ is true).
    drawPluginBrowser();

    // Video preview panel (RB-5).
    drawVideoPreview();
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
        // Probe the video for format/duration/fps/resolution.
        engine::VideoInfo info;
        if (!engine::VideoProbe::probe(path, info)) {
            std::fprintf(stderr, "Dave: failed to probe video: %s\n", path.c_str());
            return;
        }
        document::VideoClip clip;
        clip.path = path;
        auto slash = path.find_last_of("/\\");
        clip.name = (slash != std::string::npos) ? path.substr(slash + 1) : path;
        clip.timelineStart = 0;
        clip.codec = info.codec;
        clip.fps = info.fps;
        clip.width = info.width;
        clip.height = info.height;
        clip.durationSeconds = info.durationSeconds;
        edit_.setVideoClip(std::move(clip));
        // Reset preview state so the decoder/texture get recreated for the new clip.
        videoDecoder_.close();
        lastDecodedFrameIndex_ = -1;
        std::fprintf(stderr, "Dave: loaded video %s (%dx%d @ %.3ffps, %.1fs, %s)\n",
                     clip.name.c_str(), clip.width, clip.height, clip.fps,
                     clip.durationSeconds, clip.codec.c_str());
    }
}

void DaveApp::newProject() {
    // Clear the Edit and reset everything to a fresh state.
    edit_.tracksMut().clear();
    edit_.clearMarkerTracks_();
    edit_.clearVideoClip();
    builder_ = {};  // drop cached plugin instances / asset buffers
    videoDecoder_.close();
    lastDecodedFrameIndex_ = -1;
    projectPath_.clear();
    dirty_ = false;
    undo_.clear();
    // Re-seed a default track + marker track (matches init()).
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

void DaveApp::drawVideoPreview() {
    ImGui::Begin("Video");

    const auto* clip = edit_.videoClip();
    if (!clip) {
        ImGui::TextDisabled("(no video loaded)");
        if (ImGui::Button("Load Video...")) openVideoDialog();
        ImGui::End();
        return;
    }

    // Header: name + reload button.
    ImGui::Text("%s", clip->name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Replace...")) openVideoDialog();
    ImGui::TextDisabled("%dx%d @ %.3ffps  %.1fs", clip->width, clip->height,
                        clip->fps, clip->durationSeconds);

    // Compute the target video frame from the transport position (audio master).
    // timelineStart is in audio samples; sampleRate is 48000.
    const double audioSr = 48000.0;
    int64_t playhead = audio_.transport().position();
    int64_t relSamples = playhead - clip->timelineStart;
    double videoTimeSec = (relSamples > 0) ? (relSamples / audioSr) : 0.0;
    bool inRange = (videoTimeSec >= 0.0 && videoTimeSec <= clip->durationSeconds);
    int64_t frameIndex = (clip->fps > 0.0)
        ? static_cast<int64_t>(videoTimeSec * clip->fps) : 0;

    // If the frame index changed, decode + upload a new RGBA frame.
    // Debounce random-access seeks: spawning ffmpeg per scrub-tick floods the
    // pipe + tanks responsiveness. Allow at most one seek per ~150ms; during
    // fast scrub we just hold the last decoded frame. Sequential playback
    // (frameIndex advancing by 1) is NOT throttled — it reuses the process.
    bool sequential = videoDecoder_.isOpen() && frameIndex == lastDecodedFrameIndex_ + 1;
    double nowSec = ImGui::GetTime();
    bool seekAllowed = sequential || (nowSec - lastSeekTime_ >= 0.15);
    if (inRange && frameIndex != lastDecodedFrameIndex_ && seekAllowed) {
        lastSeekTime_ = nowSec;
        // Preview resolution — cap to 480 wide. 1920x1080 RGBA is 8MB/frame and
        // swamps the pipe + GL upload; 480 wide is ~1MB and plenty for a preview.
        const int previewMaxW = 480;
        int pw = clip->width, ph = clip->height;
        if (pw > previewMaxW) { ph = ph * previewMaxW / pw; pw = previewMaxW; }
        // Create or resize the GL texture if needed.
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
        // Decode the frame. Sequential playback keeps the process open; a jump
        // of >1 frame closes + respawns (seekAndRead).
        bool got = false;
        if (sequential) {
            got = videoDecoder_.readFrame(videoFrameBuf_);
        }
        if (!got) {
            // Random access: close + reopen at the requested time.
            double seekTo = static_cast<double>(frameIndex) / clip->fps;
            got = videoDecoder_.seekAndRead(clip->path, seekTo, pw, ph, videoFrameBuf_);
        }
        if (got && videoFrameBuf_.size() == static_cast<size_t>(pw * ph * 4)) {
            glBindTexture(GL_TEXTURE_2D, videoTexture_);
            // Be explicit about pixel unpack alignment — default (4) is fine for
            // RGBA but setting it removes a whole class of striping bugs when
            // row widths change.
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, pw);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pw, ph, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, videoFrameBuf_.data());
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            lastDecodedFrameIndex_ = frameIndex;
        }
    }

    // Draw the latest frame. Preserve aspect ratio; fit to panel width.
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

    // Timecode readout.
    int mm = static_cast<int>(videoTimeSec) / 60;
    int ss = static_cast<int>(videoTimeSec) % 60;
    int ff = (clip->fps > 0.0) ? static_cast<int>((videoTimeSec - static_cast<int>(videoTimeSec)) * clip->fps) : 0;
    char tc[32];
    std::snprintf(tc, sizeof(tc), "%02d:%02d:%02d", mm, ss, ff);
    ImGui::TextDisabled("TC %s  frame %lld  %s", tc,
                        static_cast<long long>(frameIndex),
                        inRange ? "" : "(out of range)");

    ImGui::End();
}

void DaveApp::drawPluginsPanel() {
    // NoTitleBar + NoMove: it's a fixed dock column, not a floating window.
    ImGui::Begin("Plugins", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse);

    // Operate on the currently-selected track (selectedTrackIndex in the view).
    int sel = view_.selectedTrackIndex;
    if (sel < 0 || sel >= static_cast<int>(edit_.tracks().size())) {
        ImGui::TextDisabled("Select a track to manage its plugins.");
        ImGui::End();
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

    ImGui::End();
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
