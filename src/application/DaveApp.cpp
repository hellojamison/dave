#include "application/DaveApp.h"
#include "editing/Commands.h"
#include "gui/Theme.h"

#include <imgui.h>
#include <nfd.h>

#include <algorithm>
#include <cstdio>

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

    // TEMPORARY: scan plugins on startup to verify the VST3 host discovery
    // works. Remove once the plugin browser UI is in place.
    pluginHost_.scan();
    for (const auto& d : pluginHost_.descriptors()) {
        std::fprintf(stderr, "Dave[plugins]:   %-20s [%s] by %s  uid=%s\n",
                     d.name.c_str(), d.subCategories.c_str(), d.vendor.c_str(),
                     d.uidString.c_str());
    }

    // Wire the Edit's change signal to graph re-derivation.
    edit_.setChangeListener([this] { onEditChanged(); });

    // Seed an initial track so the timeline isn't empty.
    undo_.execute(std::make_unique<editing::AddTrackCommand>("Track 1"));

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
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
        window_.close();
    }
}

void DaveApp::onEditChanged() {
    // UI thread. Re-derive the engine graph from the Edit, compile, publish.
    auto graph = builder_.build(edit_, audio_.sampleRate());
    auto [compiled, err] = engine::compile(*graph, audio_.sampleRate(), 256);
    if (err.has_value()) {
        std::fprintf(stderr, "Dave: compile failed: %s\n", err->message.c_str());
        return;
    }
    audio_.setCompiledGraph(std::move(compiled));
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

    // --- Main menu bar -----------------------------------------------------
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Load WAV...", "Ctrl+O")) openWavDialog();
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

    // --- Timeline (fills the rest) ----------------------------------------
    ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetMainViewport()->WorkPos.y + 56),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->WorkSize.x,
                                    ImGui::GetMainViewport()->WorkSize.y - 56),
                             ImGuiCond_Always);
    ImGui::Begin("Timeline", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    gui::drawTimeline(edit_, undo_, transport, peaks_, view_, builder_.assetBuffers());
    ImGui::End();
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

void DaveApp::run() {
    window_.run();
}

} // namespace dave::application
