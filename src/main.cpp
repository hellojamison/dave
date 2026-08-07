// SPDX-License-Identifier: GPL-3.0-or-later
#include "application/DaveApp.h"

#ifndef NDEBUG
#include <charconv>
#include <cmath>
#include <cstdlib>
#endif
#include <cstdio>
#include <string>
#ifndef NDEBUG
#include <string_view>
#endif

#ifndef NDEBUG
namespace {

void printUsage(const char* executable) {
    std::fprintf(
        stderr,
        "Usage: %s [--video-popped-out] [--screenshot <out.png> [--frames N]"
        " [--width W] [--height H] [--demo-audio <file.wav>]"
        " [--demo-midi <file.mid>] [--samples-per-pixel N]]\n",
        executable);
}

bool parsePositiveInt(std::string_view text, int& value) {
    int parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed <= 0) {
        return false;
    }
    value = parsed;
    return true;
}

bool parseSamplesPerPixel(std::string_view text, double& value) {
    char* end = nullptr;
    const double parsed = std::strtod(text.data(), &end);
    if (end != text.data() + text.size() || !std::isfinite(parsed) ||
        parsed < 4.0 || parsed > 50000.0) {
        return false;
    }
    value = parsed;
    return true;
}

bool isOptionToken(const char* value) {
    return std::string_view(value).starts_with("--");
}

} // namespace
#endif

int main(int argc, char** argv) {
    dave::platform::Window::ScreenshotOptions screenshot;
    bool screenshotRequested = false;
    bool screenshotOnlyOptionProvided = false;
    std::string demoAudioPath;
    std::string demoMidiPath;
    double screenshotSamplesPerPixel = 0.0;
    bool startVideoPoppedOut = false;

#ifdef NDEBUG
    if (argc > 1) {
        std::fprintf(stderr, "Dave: developer CLI flags are unavailable in release builds\n");
        return 2;
    }
#else
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--screenshot") {
            if (++i >= argc || isOptionToken(argv[i])) {
                std::fprintf(stderr, "Dave: --screenshot requires an output path\n");
                printUsage(argv[0]);
                return 2;
            }
            screenshot.outputPath = argv[i];
            screenshotRequested = true;
        } else if (arg == "--frames" || arg == "--width" || arg == "--height") {
            screenshotOnlyOptionProvided = true;
            if (++i >= argc) {
                std::fprintf(stderr, "Dave: %.*s requires a positive integer\n",
                             static_cast<int>(arg.size()), arg.data());
                printUsage(argv[0]);
                return 2;
            }
            int* target = arg == "--frames" ? &screenshot.frames
                        : arg == "--width"  ? &screenshot.width
                                             : &screenshot.height;
            if (!parsePositiveInt(argv[i], *target) ||
                (arg == "--width" && *target < 900) ||
                (arg == "--height" && *target < 540)) {
                std::fprintf(stderr, "Dave: invalid value for %.*s: %s\n",
                             static_cast<int>(arg.size()), arg.data(), argv[i]);
                if (arg == "--width" || arg == "--height") {
                    std::fprintf(stderr,
                                 "Dave: screenshot dimensions must be at least 900x540\n");
                }
                return 2;
            }
        } else if (arg == "--demo-audio") {
            screenshotOnlyOptionProvided = true;
            if (++i >= argc || isOptionToken(argv[i])) {
                std::fprintf(stderr, "Dave: --demo-audio requires a WAV path\n");
                printUsage(argv[0]);
                return 2;
            }
            demoAudioPath = argv[i];
        } else if (arg == "--demo-midi") {
            screenshotOnlyOptionProvided = true;
            if (++i >= argc) {
                printUsage(argv[0]);
                return 2;
            }
            demoMidiPath = argv[i];
        } else if (arg == "--video-popped-out") {
            // Start with the picture in its own window: a screenshot fixture
            // for the detached-picture sidebar, and a way to exercise the
            // popped-out path without clicking through the UI.
            startVideoPoppedOut = true;
        } else if (arg == "--samples-per-pixel") {
            screenshotOnlyOptionProvided = true;
            if (++i >= argc || !parseSamplesPerPixel(argv[i], screenshotSamplesPerPixel)) {
                std::fprintf(stderr,
                             "Dave: --samples-per-pixel must be between 4 and 50000\n");
                printUsage(argv[0]);
                return 2;
            }
        } else {
            std::fprintf(stderr, "Dave: unknown option: %s\n", argv[i]);
            printUsage(argv[0]);
            return 2;
        }
    }

    if (screenshotOnlyOptionProvided && !screenshotRequested) {
        std::fprintf(stderr,
                     "Dave: screenshot options require --screenshot\n");
        printUsage(argv[0]);
        return 2;
    }
#endif

    if (screenshotRequested) {
        dave::platform::Window::configureScreenshot(screenshot);
    }

    dave::application::DaveApp app;
    if (!app.init()) {
        std::fprintf(stderr, "Dave: initialization failed\n");
        return 1;
    }
    if (screenshotSamplesPerPixel > 0.0) {
        app.setTimelineSamplesPerPixel(screenshotSamplesPerPixel);
    }
    if (startVideoPoppedOut) {
        app.setVideoPoppedOut(true);
    }
    if (!demoMidiPath.empty()) {
        if (!app.importMidiIntoEdit(demoMidiPath)) {
            return 1;
        }
    }
    if (!demoAudioPath.empty()) {
        if (!app.loadWavIntoEdit(demoAudioPath)) {
            return 1;
        }
    }
    app.run();
    if (screenshotRequested && !dave::platform::Window::screenshotSucceeded()) {
        return 1;
    }
    return 0;
}
