#pragma once

#include "engine/graph/CompiledGraph.h"
#include "engine/nodes/SineNode.h"
#include "gui/ImGuiLayer.h"
#include "platform/AudioEngine.h"
#include "platform/Window.h"

#include <memory>

namespace dave::application {

// DaveApp is the application root. It owns the window, the ImGui layer, the
// audio engine, and the live node graph. The main thread runs the render loop;
// the audio engine's miniaudio callback drives the graph on the RT thread.
//
// RB-0 UI is intentionally minimal: a window with a header and a Play/Stop
// button that toggles the sine node's gain. The point is to prove the
// permissive-stack RT path end to end:
//   ImGui button -> UI-thread atomic store -> SineNode -> AudioEngine
//   -> miniaudio callback -> audio device.
class DaveApp {
public:
    DaveApp() = default;
    ~DaveApp();

    // Create the window, init ImGui, start audio, build the graph. Returns
    // false on any fatal init failure (caller should exit).
    bool init();
    void run();

private:
    void buildGraph();
    void drawUI();

    platform::Window window_{960, 600, "Dave"};
    gui::ImGuiLayer imgui_;
    platform::AudioEngine audio_;

    // The live graph's sine node, shared with the graph so the UI can edit its
    // gain (RT-safe atomic store).
    std::shared_ptr<engine::SineNode> sine_;
    bool playing_ = false;
};

} // namespace dave::application
