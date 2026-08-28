// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <functional>
#include <string>
#include <vector>

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
    using FileDropCallback = std::function<void(const std::vector<std::string>&)>;
    using CloseGuard = std::function<bool()>;

    struct ScreenshotOptions {
        std::string outputPath;
        int frames = 15;
        int width = 1280;
        int height = 800;
    };

    // Screenshot configuration must be set before DaveApp is constructed:
    // its Window member creates the native window during construction.
    static void configureScreenshot(ScreenshotOptions options);
    static bool screenshotSucceeded();
    static bool screenshotMode();

    Window(int width, int height, const std::string& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool valid() const { return window_ != nullptr; }

    // Set the per-frame callback: input is polled, then this is called to
    // build ImGui UI and render.
    void setFrameCallback(FrameCallback cb) { frameCallback_ = std::move(cb); }
    void setFileDropCallback(FileDropCallback cb);
    void setCloseGuard(CloseGuard guard) { closeGuard_ = std::move(guard); }

    // Run the event loop until the window is closed.
    void run();

    // Request the loop exit (safe from within the frame callback).
    void close();

    GLFWwindow* handle() { return window_; }
    // Current cursor position in window content coordinates (points), which
    // match ImGui screen coordinates for the single main viewport. Valid to
    // call from the file-drop callback, where ImGui's own mouse position may
    // be a frame stale.
    void cursorPos(double& x, double& y) const;

    // Hide or show the system cursor over this window's content.
    //
    // Deliberately not routed through ImGui's per-frame cursor. That value is
    // arbitrated between every widget in the frame and then applied by a
    // backend that shares one "last cursor" across all viewports, so a request
    // to hide reached the screen only some of the time. The code that knows
    // the pointer is over a drawing tool says so directly.
    void setCursorHidden(bool hidden);

private:
    static void glfwFileDropCallback(GLFWwindow* window, int count, const char** paths);
    static void glfwWindowRefreshCallback(GLFWwindow* window);

    // Draw and present one frame. GLFW can keep glfwPollEvents() inside the
    // native macOS live-resize loop, so the refresh callback uses this same
    // path to avoid stretching the last completed frame until resizing ends.
    void renderFrame();

    GLFWwindow* window_ = nullptr;
    bool cursorHidden_ = false;
    FrameCallback frameCallback_;
    FileDropCallback fileDropCallback_;
    CloseGuard closeGuard_;
    bool shouldClose_ = false;
    bool renderingFrame_ = false;
};

} // namespace dave::platform
