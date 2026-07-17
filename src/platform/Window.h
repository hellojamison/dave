#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <functional>
#include <string>

namespace dave::platform {

// Window owns the GLFW window and its OpenGL context. It's the platform
// abstraction over Cocoa (macOS) / Win32 (Windows). All GUI rendering happens
// against the GL context this owns.
//
// Threading: GLFW requires all window calls from the main thread. The render
// loop runs on the main thread.
class Window {
public:
    using FrameCallback = std::function<void()>;

    Window(int width, int height, const std::string& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool valid() const { return window_ != nullptr; }

    // Set the per-frame callback: input is polled, then this is called to
    // build ImGui UI and render.
    void setFrameCallback(FrameCallback cb) { frameCallback_ = std::move(cb); }

    // Run the event loop until the window is closed.
    void run();

    // Request the loop exit (safe from within the frame callback).
    void close() { shouldClose_ = true; }

    GLFWwindow* handle() { return window_; }

private:
    GLFWwindow* window_ = nullptr;
    FrameCallback frameCallback_;
    bool shouldClose_ = false;
};

} // namespace dave::platform
