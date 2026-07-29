// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/plugins/PluginInstance.h"

#include <memory>
#include <string>

namespace dave::engine {

// PluginEditor opens a plugin's GUI in its own native OS window (a floating
// NSWindow on macOS, HWND on Windows). This is the "floating FX window" model
// (like Reaper) — simpler and more robust than embedding the plugin view inside
// our GLFW window, which would require child-view coordinate math + careful
// GL/view coexistence.
//
// Threading: open()/close() run on the MAIN thread (VST3 editor lifecycle is
// main-thread-only). The plugin instance must already be loaded.
//
// macOS impl lives in PluginEditor_mac.mm (uses Cocoa via ARC). The .cpp here
// is a factory so the rest of the engine stays Objective-C-free.
class PluginEditor {
public:
    PluginEditor();
    ~PluginEditor();

    PluginEditor(const PluginEditor&) = delete;
    PluginEditor& operator=(const PluginEditor&) = delete;

    // Open the editor for `instance`. The instance must be loaded. The window
    // title is `title`. Returns false if the plugin has no editor or creation
    // fails. Main thread.
    bool open(PluginInstance& instance, const std::string& title);

    // Close the editor window and release the plug view. Main thread. Idempotent.
    void close();

    bool isOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dave::engine
