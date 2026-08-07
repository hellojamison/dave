// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ImGuiLayer.h"
#include "gui/Theme.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <glad.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <cstdio>

namespace dave::gui {

ImGuiLayer::~ImGuiLayer() {
    shutdown();
}

bool ImGuiLayer::init(platform::Window& window) {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    if (context_ == nullptr) {
        std::fprintf(stderr, "Dave: ImGui::CreateContext failed\n");
        return false;
    }
    ImGui::SetCurrentContext(context_);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;       // docking branch feature
    // Multi-viewport (detached OS windows) is enabled for one reason: the video
    // preview can be popped out of the sidebar and dragged onto a second
    // display, which is how picture is monitored in post. The layout stays
    // predictable anyway — every docked panel is repositioned with
    // ImGuiCond_Always and carries NoMove | NoDocking, so the popped-out
    // picture window is the only thing that can leave the main window.
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // Keep ImGui's own title bar on detached windows rather than the OS one:
    // this build of ImGui draws both when the platform window is decorated.
    io.ConfigViewportsNoDecoration = true;

    // Dave's visual identity — replaces the ImGui default (which reads as a
    // debug tool) with a cohesive pro-audio dark theme.
    theme::loadDefaultFont(14.0f);
    theme::applyTheme();

    const char* glslVersion = "#version 330";
    if (!ImGui_ImplGlfw_InitForOpenGL(window.handle(), true)) {
        std::fprintf(stderr, "Dave: ImGui_ImplGlfw_InitForOpenGL failed\n");
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
        std::fprintf(stderr, "Dave: ImGui_ImplOpenGL3_Init failed\n");
        return false;
    }

    initialized_ = true;
    return true;
}

void ImGuiLayer::shutdown() {
    if (!initialized_) {
        return;
    }
    ImGui::SetCurrentContext(context_);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(context_);
    context_ = nullptr;
    initialized_ = false;
}

void ImGuiLayer::newFrame() {
    ImGui::SetCurrentContext(context_);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::render() {
    ImGui::SetCurrentContext(context_);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Update and render secondary (detached) windows via viewports.
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup);
    }
}

} // namespace dave::gui
