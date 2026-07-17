#include "application/DaveApp.h"
#include "editing/Commands.h"

#include <imgui.h>
#include <nfd.h>

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

    // Wire the Edit's change signal to graph re-derivation.
    edit_.setChangeListener([this] { onEditChanged(); });

    // Seed an initial track so the timeline isn't empty.
    undo_.execute(std::make_unique<editing::AddTrackCommand>("Track 1"));

    window_.setFrameCallback([this] {
        imgui_.newFrame();
        drawUI();
        imgui_.render();
    });
    return true;
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
    if (asset == nullptr) return;

    // Ensure at least one track exists; place the clip at the playhead.
    if (edit_.tracks().empty()) {
        undo_.execute(std::make_unique<editing::AddTrackCommand>("Track 1"));
    }
    const auto& trackId = edit_.tracks().front().id;

    document::AudioClip clip;
    clip.asset = assetId;
    clip.timelineStart = audio_.transport().position();
    clip.sourceOffset = 0;
    clip.length = asset->lengthSamples;
    clip.fadeIn = 64;
    clip.fadeOut = 64;
    undo_.execute(std::make_unique<editing::AddClipCommand>(trackId, clip));
}

void DaveApp::drawUI() {
    ImGui::DockSpaceOverViewport();

    // --- Transport + actions bar -------------------------------------------
    ImGui::Begin("Transport");
    auto& transport = audio_.transport();
    if (ImGui::Button(transport.isPlaying() ? "Stop" : "Play")) {
        transport.toggle();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop & Rewind")) {
        transport.stop();
        transport.seek(0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Undo")) undo_.undo();
    ImGui::SameLine();
    if (ImGui::Button("Redo")) undo_.redo();
    ImGui::SameLine();
    if (ImGui::Button("Add Track")) {
        undo_.execute(std::make_unique<editing::AddTrackCommand>("Track"));
    }
    ImGui::SameLine();
    if (ImGui::Button("Load WAV...")) {
        nfdnchar_t* outPath = nullptr;
        nfdnfilteritem_t filter{"WAV audio", "wav"};
        if (NFD_OpenDialog(&outPath, &filter, 1, nullptr) == NFD_OKAY && outPath) {
            std::string p = outPath;
            NFD_FreePath(outPath);
            loadWavIntoEdit(p);
        }
    }
    ImGui::Text("Pos: %lld  |  Undo depth: %zu  |  Tracks: %zu  |  Zoom: %.0f smp/px",
                static_cast<long long>(transport.position()), undo_.undoDepth(),
                edit_.tracks().size(), view_.samplesPerPixel);
    ImGui::TextDisabled("Scroll: wheel. Zoom: Ctrl+wheel. Drag clip to move. Click empty area to seek.");
    ImGui::End();

    // --- Timeline ----------------------------------------------------------
    ImGui::Begin("Timeline");
    gui::drawTimeline(edit_, undo_, transport, peaks_, view_, builder_.assetBuffers());
    ImGui::End();
}

void DaveApp::run() {
    window_.run();
}

} // namespace dave::application
