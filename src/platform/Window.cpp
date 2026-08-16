// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/Window.h"

#include <glad.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dave::platform {

namespace {
std::optional<Window::ScreenshotOptions> g_screenshotOptions;
bool g_screenshotSucceeded = false;
std::unordered_map<GLFWwindow*, Window*> g_windows;

} // namespace

void Window::glfwFileDropCallback(GLFWwindow* window, int count, const char** paths) {
    const auto found = g_windows.find(window);
    if (found == g_windows.end() || !found->second->fileDropCallback_) {
        return;
    }
    std::vector<std::string> droppedPaths;
    droppedPaths.reserve(static_cast<size_t>(std::max(0, count)));
    for (int index = 0; index < count; ++index) {
        if (paths[index] != nullptr) {
            droppedPaths.emplace_back(paths[index]);
        }
    }
    found->second->fileDropCallback_(droppedPaths);
}

void Window::glfwWindowRefreshCallback(GLFWwindow* window) {
    const auto found = g_windows.find(window);
    if (found == g_windows.end()) {
        return;
    }
    found->second->renderFrame();
}

namespace detail {
bool writePng(const std::string& path, int width, int height,
              const unsigned char* pixels, int strideBytes);
} // namespace detail

void Window::configureScreenshot(ScreenshotOptions options) {
    g_screenshotOptions = std::move(options);
    g_screenshotSucceeded = false;
}

bool Window::screenshotSucceeded() {
    return g_screenshotSucceeded;
}

bool Window::screenshotMode() {
    return g_screenshotOptions.has_value();
}

Window::Window(int width, int height, const std::string& title) {
    if (g_screenshotOptions.has_value()) {
        std::fprintf(stderr, "Dave: screenshot window initializing\n");
        std::fflush(stderr);
    }
    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "Dave: glfwInit failed\n");
        return;
    }
    if (g_screenshotOptions.has_value()) {
        std::fprintf(stderr, "Dave: screenshot GLFW initialized\n");
    }

    // OpenGL 3.3 Core — the minimum ImGui's GL3 backend needs, and broadly
    // supported on Mac (4.1 is the max on Mac anyway; 3.3 is safe everywhere).
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    if (g_screenshotOptions.has_value()) {
        width = g_screenshotOptions->width;
        height = g_screenshotOptions->height;
        // Self-capture does not need an onscreen window. Keeping it hidden also
        // makes the harness deterministic on machines without capture access.
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        std::fprintf(stderr, "Dave: failed to create GLFW window\n");
        glfwTerminate();
        return;
    }
    // ImGui owns GLFW's user pointer, so the platform keeps its own lookup for
    // this optional callback rather than competing with the input backend.
    g_windows.emplace(window_, this);
    glfwSetDropCallback(window_, Window::glfwFileDropCallback);
    glfwSetWindowRefreshCallback(window_, Window::glfwWindowRefreshCallback);

    glfwMakeContextCurrent(window_);
    // A hidden screenshot window has nothing to present. Waiting for its
    // display link can stall on macOS, so screenshot frames render directly
    // into the back buffer without vsync.
    glfwSwapInterval(g_screenshotOptions.has_value() ? 0 : 1);

    if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
        std::fprintf(stderr, "Dave: failed to load OpenGL functions\n");
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
        return;
    }

    // Keep the window from shrinking below a usable DAW size.
    glfwSetWindowSizeLimits(window_, 900, 540, GLFW_DONT_CARE, GLFW_DONT_CARE);
}

Window::~Window() {
    if (window_ != nullptr) {
        g_windows.erase(window_);
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
}

void Window::setFileDropCallback(FileDropCallback cb) {
    fileDropCallback_ = std::move(cb);
}

void Window::close() {
    if (!closeGuard_ || closeGuard_()) shouldClose_ = true;
}

void Window::renderFrame() {
    // Refresh callbacks can be delivered from GLFW calls made while rendering
    // ImGui platform windows. Ignore those nested requests; the active frame
    // will present the same or newer geometry when it completes.
    if (renderingFrame_ || !frameCallback_) {
        return;
    }

    renderingFrame_ = true;
    frameCallback_();
    if (!g_screenshotOptions.has_value()) {
        glfwSwapBuffers(window_);
    }
    renderingFrame_ = false;
}

void Window::run() {
    if (window_ == nullptr) {
        return;
    }
    if (g_screenshotOptions.has_value()) {
        std::fprintf(stderr,
                     "Dave: screenshot loop starting (%d frames -> %s)\n",
                     g_screenshotOptions->frames,
                     g_screenshotOptions->outputPath.c_str());
    }
    int completedFrames = 0;
    while (!shouldClose_ && !glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window_) && closeGuard_ && !closeGuard_()) {
            glfwSetWindowShouldClose(window_, GLFW_FALSE);
        }
        renderFrame();

        ++completedFrames;
        if (g_screenshotOptions.has_value() &&
            completedFrames >= g_screenshotOptions->frames) {
            int framebufferWidth = 0;
            int framebufferHeight = 0;
            glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);

            const bool dimensionsInvalid =
                framebufferWidth <= 0 || framebufferHeight <= 0 ||
                framebufferWidth > std::numeric_limits<int>::max() / 4 ||
                static_cast<size_t>(framebufferHeight) >
                    std::numeric_limits<size_t>::max() /
                        (static_cast<size_t>(framebufferWidth) * 4);
            if (dimensionsInvalid) {
                std::fprintf(stderr,
                             "Dave: screenshot failed: invalid framebuffer size %dx%d\n",
                             framebufferWidth, framebufferHeight);
            } else {
                const size_t rowBytes = static_cast<size_t>(framebufferWidth) * 4;
                std::vector<unsigned char> pixels(
                    rowBytes * static_cast<size_t>(framebufferHeight));

                const GLenum previousError = glGetError();
                if (previousError != GL_NO_ERROR) {
                    std::fprintf(
                        stderr,
                        "Dave: screenshot warning: pre-existing OpenGL error 0x%x\n",
                        previousError);
                }
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadBuffer(GL_BACK);
                glReadPixels(0, 0, framebufferWidth, framebufferHeight,
                             GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

                const GLenum readError = glGetError();
                if (readError != GL_NO_ERROR) {
                    std::fprintf(stderr,
                                 "Dave: screenshot glReadPixels failed (OpenGL error 0x%x)\n",
                                 readError);
                } else {
                    for (int top = 0, bottom = framebufferHeight - 1;
                         top < bottom; ++top, --bottom) {
                        auto topRow = pixels.begin() + static_cast<ptrdiff_t>(
                            static_cast<size_t>(top) * rowBytes);
                        auto bottomRow = pixels.begin() + static_cast<ptrdiff_t>(
                            static_cast<size_t>(bottom) * rowBytes);
                        std::swap_ranges(
                            topRow, topRow + static_cast<ptrdiff_t>(rowBytes),
                            bottomRow);
                    }

                    g_screenshotSucceeded = detail::writePng(
                        g_screenshotOptions->outputPath,
                        framebufferWidth,
                        framebufferHeight,
                        pixels.data(),
                        static_cast<int>(rowBytes));
                    if (g_screenshotSucceeded) {
                        int windowWidth = 0;
                        int windowHeight = 0;
                        glfwGetWindowSize(window_, &windowWidth, &windowHeight);
                        std::fprintf(
                            stderr,
                            "Dave: screenshot wrote %s (%dx%d window, %dx%d framebuffer, %d frames)\n",
                            g_screenshotOptions->outputPath.c_str(),
                            windowWidth, windowHeight,
                            framebufferWidth, framebufferHeight,
                            completedFrames);
                    } else {
                        std::fprintf(stderr, "Dave: screenshot failed to write %s\n",
                                     g_screenshotOptions->outputPath.c_str());
                    }
                }
            }
            shouldClose_ = true;
            continue;
        }
    }
}

void Window::setCursorHidden(bool hidden) {
    if (window_ == nullptr) return;
    // Left alone if something has taken the cursor outright, which is not a
    // state this should be flipping in and out of.
    const int mode = glfwGetInputMode(window_, GLFW_CURSOR);
    if (mode == GLFW_CURSOR_DISABLED) return;

    // Reasserted every frame rather than only on change. ImGui's GLFW backend
    // rewrites this mode from its own NewFrame, and it is not the only thing
    // that can: a focus change, a platform window appearing for a viewport, or
    // any glfwSetCursor call can put the arrow back. Caching "already hidden"
    // made the fix one-shot — it worked until something else wrote the mode,
    // and then never corrected itself.
    const int want = hidden ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL;
    if (mode != want) glfwSetInputMode(window_, GLFW_CURSOR, want);
    cursorHidden_ = hidden;
}

} // namespace dave::platform
