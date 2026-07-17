#pragma once

#include "platform/Window.h"

struct ImGuiContext;

namespace dave::gui {

// ImGuiLayer wraps ImGui init/shutdown/new-frame/render against a Window's GL
// context. One instance per window.
class ImGuiLayer {
public:
    ImGuiLayer() = default;
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Initialise ImGui for the given (already-current) GL context. The
    // ImGui sources come from third_party/imgui; the GL3 + GLFW backends are
    // compiled into Dave directly.
    bool init(platform::Window& window);
    void shutdown();

    // Call at the start of each frame, after glfwPollEvents.
    void newFrame();

    // Call after all ImGui:: widgets are issued, to build the draw data and
    // render via the GL3 backend.
    void render();

private:
    ImGuiContext* context_ = nullptr;
    bool initialized_ = false;
};

} // namespace dave::gui
