#pragma once

#include "document/Edit.h"
#include "editing/Command.h"
#include "engine/GraphBuilder.h"
#include "engine/plugins/PluginEditor.h"
#include "engine/plugins/PluginHost.h"
#include "engine/transport/Transport.h"
#include "gui/ImGuiLayer.h"
#include "gui/Timeline.h"
#include "platform/AudioEngine.h"
#include "platform/Window.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace dave::application {

// DaveApp is the application root for RB-2.
//
// Architecture (single source of truth):
//   document::Edit  ── the source of truth (tracks/clips/assets)
//        │ setChangeListener → rebuild + recompile + publish
//        ▼
//   GraphBuilder.build(edit) → engine::Graph → compile() → CompiledGraph
//        │ atomic publish
//        ▼
//   AudioEngine (RT)  ←── Transport advances each block
//
// The UI edits the Edit via Commands (UndoStack). Every Edit mutation fires
// notifyChanged(), which re-derives the graph. The Timeline widget reads the
// Edit directly (pure view).
class DaveApp {
public:
    DaveApp() = default;
    ~DaveApp();

    bool init();
    void run();

private:
    void onEditChanged();           // re-derive + recompile + publish
    void loadWavIntoEdit(const std::string& path);
    void openWavDialog();
    void handleShortcuts();
    void drawUI();
    void drawPluginsPanel();
    void drawPluginBrowser();

    platform::Window window_{1280, 800, "Dave"};
    gui::ImGuiLayer imgui_;
    platform::AudioEngine audio_;

    document::Edit edit_;
    editing::UndoStack undo_{edit_};
    engine::GraphBuilder builder_;
    engine::PluginHost pluginHost_;

    gui::PeakCache peaks_;
    gui::TimelineViewState view_;

    // Plugin browser modal state.
    bool showPluginBrowser_ = false;
    std::string browserTargetTrackId_;  // track to add the plugin to
    char browserFilter_[128] = "";

    // Open plugin editors, keyed by slot id. One floating NSWindow per slot.
    std::unordered_map<std::string, std::unique_ptr<engine::PluginEditor>> editors_;
};

} // namespace dave::application
