#include "platform/Window.h"

#include <glad.h>

#include <cstdio>

namespace dave::platform {

Window::Window(int width, int height, const std::string& title) {
    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "Dave: glfwInit failed\n");
        return;
    }

    // OpenGL 3.3 Core — the minimum ImGui's GL3 backend needs, and broadly
    // supported on Mac (4.1 is the max on Mac anyway; 3.3 is safe everywhere).
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        std::fprintf(stderr, "Dave: failed to create GLFW window\n");
        glfwTerminate();
        return;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1); // vsync

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
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
}

void Window::run() {
    if (window_ == nullptr) {
        return;
    }
    while (!shouldClose_ && !glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        if (frameCallback_) {
            frameCallback_();
        }
        glfwSwapBuffers(window_);
    }
}

} // namespace dave::platform
