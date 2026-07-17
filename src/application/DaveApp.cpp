#include "application/DaveApp.h"

#include <imgui.h>

#include <cstdio>

namespace dave::application {

DaveApp::~DaveApp() {
    // Stop audio before tearing down the graph it references.
    audio_.stop();
    audio_.setGraph(nullptr);
}

bool DaveApp::init() {
    if (!window_.valid()) {
        return false;
    }
    if (!imgui_.init(window_)) {
        return false;
    }

    // Start audio first so the graph is prepared at the right sample rate.
    if (!audio_.start(48000.0, 2)) {
        std::fprintf(stderr, "Dave: audio engine failed to start; continuing without sound\n");
    }

    buildGraph();

    window_.setFrameCallback([this] {
        imgui_.newFrame();
        drawUI();
        imgui_.render();
    });
    return true;
}

void DaveApp::buildGraph() {
    // UI thread. Build the RB-0 graph: a single SineNode. Publish to the audio
    // engine, which prepares it and swaps it in at the next block boundary.
    sine_ = std::make_shared<engine::SineNode>();
    sine_->setFrequency(440.0);
    sine_->setGain(0.0); // silent until the user hits Play

    auto graph = std::make_unique<engine::CompiledGraph>();
    graph->addNode(sine_);
    audio_.setGraph(std::move(graph));
}

void DaveApp::drawUI() {
    // A full-viewport docking host. RB-0 just proves the pipeline; real DAW UI
    // (timeline, mixer) lands in RB-2.
    ImGui::DockSpaceOverViewport();

    ImGui::Begin("Dave");
    ImGui::TextUnformatted("Dave — D.A.V.E. (Digital Audio & Video Environment)");
    ImGui::TextDisabled("RB-0: permissive-stack skeleton");
    ImGui::Separator();

    if (audio_.isRunning()) {
        ImGui::Text("Audio: running @ %.0f Hz", audio_.sampleRate());
    } else {
        ImGui::TextDisabled("Audio: not running");
    }

    ImGui::Spacing();
    const char* label = playing_ ? "Stop" : "Play 440 Hz";
    if (ImGui::Button(label, ImVec2(180, 0))) {
        playing_ = !playing_;
        if (sine_) {
            // UI-thread parameter edit -> RT-safe atomic store in SineNode.
            sine_->setGain(playing_ ? 0.2 : 0.0);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Stack: miniaudio + Dear ImGui + OpenGL (GLFW). Our own RT graph.");

    ImGui::End();
}

void DaveApp::run() {
    window_.run();
}

} // namespace dave::application
