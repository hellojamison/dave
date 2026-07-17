#include "application/DaveApp.h"

#include <imgui.h>
#include <nfd.h>

#include <cstdio>

namespace dave::application {

DaveApp::~DaveApp() {
    audio_.stop();
    audio_.setCompiledGraph(nullptr);
}

bool DaveApp::init() {
    if (!window_.valid()) {
        return false;
    }
    if (!imgui_.init(window_)) {
        return false;
    }
    if (!audio_.start(48000.0, 2)) {
        std::fprintf(stderr, "Dave: audio engine failed to start\n");
    }

    buildGraph();
    recompileAndPublish();

    window_.setFrameCallback([this] {
        imgui_.newFrame();
        drawUI();
        imgui_.render();
    });
    return true;
}

void DaveApp::buildGraph() {
    // Build the RB-1 graph topology. Node handles are kept for UI edits.
    clip_ = std::make_shared<engine::AudioClipNode>();
    sine_ = std::make_shared<engine::SineNode>();
    sine_->setGain(0.0); // off until toggled
    master_ = std::make_shared<engine::GainNode>();
    master_->setGain(0.8);

    auto sum = std::make_shared<engine::SummingNode>(2);

    // Register nodes; remember IDs for wiring.
    auto clipId = graph_.addNode(clip_);
    auto sineId = graph_.addNode(sine_);
    auto sumId = graph_.addNode(sum);
    auto masterId = graph_.addNode(master_);

    // Wire: clip/sine → sum(0,1) → master(0) → out (master is the root).
    graph_.connect(clipId, 0, sumId, 0);
    graph_.connect(sineId, 0, sumId, 1);
    graph_.connect(sumId, 0, masterId, 0);
}

void DaveApp::recompileAndPublish() {
    auto [compiled, err] = engine::compile(graph_, audio_.sampleRate(), 256);
    if (err.has_value()) {
        std::fprintf(stderr, "Dave: graph compile failed: %s\n", err->message.c_str());
        return;
    }
    audio_.setCompiledGraph(std::move(compiled));
}

void DaveApp::drawUI() {
    ImGui::DockSpaceOverViewport();
    ImGui::Begin("Dave");

    ImGui::TextUnformatted("Dave — D.A.V.E.");
    ImGui::TextDisabled("RB-1: graph engine + WAV playback");
    ImGui::Separator();

    // --- Transport bar -----------------------------------------------------
    auto& transport = audio_.transport();
    if (ImGui::Button(transport.isPlaying() ? "Stop" : "Play")) {
        transport.toggle();
    }
    ImGui::SameLine();
    ImGui::Text("Pos: %lld", static_cast<long long>(transport.position()));
    ImGui::Separator();

    // --- File loading ------------------------------------------------------
    if (ImGui::Button("Load WAV...")) {
        openFileDialog();
    }
    if (!loadedFileName_.empty()) {
        ImGui::Text("Loaded: %s (%.1fs, %d ch)",
                    loadedFileName_.c_str(), loadedFileSeconds_,
                    clip_ ? clip_->numChannels() : 0);
    } else {
        ImGui::TextDisabled("(no file loaded)");
    }
    ImGui::Separator();

    // --- Sine toggle -------------------------------------------------------
    if (ImGui::Checkbox("440 Hz tone", &sineOn_)) {
        if (sine_) {
            sine_->setGain(sineOn_ ? 0.15 : 0.0);
        }
    }
    ImGui::Separator();

    // --- Master gain -------------------------------------------------------
    // ImGui dropped SliderDouble; use SliderFloat and widen to double for the node.
    float g = static_cast<float>(master_ ? master_->gain() : 0.8);
    if (ImGui::SliderFloat("Master", &g, 0.0f, 1.5f)) {
        if (master_) {
            master_->setGain(static_cast<double>(g));
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Stack: miniaudio + ImGui + our RT graph (DAG, topo-sorted)");

    ImGui::End();
}

void DaveApp::openFileDialog() {
    // Native file dialog via nativefiledialog-extended (MIT). Single-select.
    nfdnchar_t* outPath = nullptr;
    nfdnfilteritem_t filter{"WAV audio", "wav"};
    nfdresult_t result = NFD_OpenDialog(&outPath, &filter, 1, nullptr);
    if (result != NFD_OKAY || outPath == nullptr) {
        return;
    }
    std::string path = outPath;
    NFD_FreePath(outPath);

    if (clip_ && clip_->loadFromFile(path)) {
        auto slash = path.find_last_of("/\\");
        loadedFileName_ = (slash != std::string::npos) ? path.substr(slash + 1) : path;
        loadedFileSeconds_ = clip_->lengthSamples() / clip_->sampleRate();
        clip_->setStart(0);
        audio_.transport().seek(0);
        std::fprintf(stderr, "Dave: loaded %s (%.1fs)\n",
                     loadedFileName_.c_str(), loadedFileSeconds_);
    }
}

void DaveApp::run() {
    window_.run();
}

} // namespace dave::application
