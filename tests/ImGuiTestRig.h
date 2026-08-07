// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A headless ImGui session for driving Dave's widgets in tests.
//
// ImGui needs a context but not a graphics backend: give it a DisplaySize and
// a built font atlas and it runs a whole frame into a draw list nobody
// rasterizes. That is enough to press buttons, drag things, and then ask the
// document what happened — which is the only way to catch wiring bugs, where
// every command is correct and none of them ever gets issued.

#include "document/Edit.h"
#include "editing/Command.h"
#include "engine/transport/Transport.h"
#include "gui/Theme.h"
#include "gui/Timeline.h"

#include <imgui.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dave::testing {

class ImGuiRig {
public:
    static constexpr float kDisplayW = 1600.0f;
    static constexpr float kDisplayH = 1000.0f;

    ImGuiRig() {
        ctx_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx_);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(kDisplayW, kDisplayH);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
        // These tests only ever mean single clicks. Without this, probe loops
        // land two presses inside ImGui's 0.3 s double-click window and trip
        // handlers nobody asked for.
        io.MouseDoubleClickTime = 0.0f;
        gui::theme::applyTheme();
    }
    ~ImGuiRig() { ImGui::DestroyContext(ctx_); }
    ImGuiRig(const ImGuiRig&) = delete;
    ImGuiRig& operator=(const ImGuiRig&) = delete;

    // Run one frame with the mouse at (x, y) and the left button in `down`,
    // calling `body` inside a full-viewport window.
    void frame(float x, float y, bool down, const std::function<void()>& body) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(kDisplayW, kDisplayH));
        ImGui::Begin("Host", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    // Press and release at one point, three frames, no movement.
    void clickAt(float x, float y, const std::function<void()>& body) {
        frame(x, y, false, body);
        frame(x, y, true, body);
        frame(x, y, false, body);
    }

    document::Edit edit;
    editing::UndoStack undo{edit};
    engine::Transport transport;
    gui::TimelineViewState view;

private:
    ImGuiContext* ctx_ = nullptr;
};

} // namespace dave::testing
