#pragma once

#include "engine/graph/Graph.h"
#include "engine/nodes/AudioClipNode.h"
#include "engine/nodes/GainNode.h"
#include "engine/nodes/SineNode.h"
#include "engine/nodes/SummingNode.h"
#include "gui/ImGuiLayer.h"
#include "platform/AudioEngine.h"
#include "platform/Window.h"

#include <memory>
#include <string>

namespace dave::application {

// DaveApp is the application root. RB-1 builds a real (if tiny) signal graph:
//   [AudioClipNode]──┐
//   [SineNode]───────┼──►[SummingNode]──►[GainNode(master)]──► out
//
// The UI lets you load a WAV, toggle the sine, play/stop transport, and set
// master gain. This proves real audio playback through our own graph engine.
class DaveApp {
public:
    DaveApp() = default;
    ~DaveApp();

    bool init();
    void run();

private:
    void buildGraph();
    void recompileAndPublish();
    void drawUI();
    void openFileDialog();

    platform::Window window_{960, 600, "Dave"};
    gui::ImGuiLayer imgui_;
    platform::AudioEngine audio_;

    // The mutable graph (UI-thread) and shared node handles for param edits.
    engine::Graph graph_;
    std::shared_ptr<engine::AudioClipNode> clip_;
    std::shared_ptr<engine::SineNode> sine_;
    std::shared_ptr<engine::GainNode> master_;

    // Loaded file info for display.
    std::string loadedFileName_;
    double loadedFileSeconds_ = 0.0;

    bool sineOn_ = false;
};

} // namespace dave::application
