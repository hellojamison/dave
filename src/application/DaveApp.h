#pragma once

#include "document/Edit.h"
#include "editing/Command.h"
#include "engine/GraphBuilder.h"
#include "engine/plugins/PluginEditor.h"
#include "engine/plugins/PluginHost.h"
#include "engine/transport/Transport.h"
#include "engine/video/AsyncVideoDecoder.h"
#include "engine/video/VideoDecoder.h"
#include "gui/ImGuiLayer.h"
#include "gui/Timeline.h"
#include "platform/AudioEngine.h"
#include "platform/Window.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
    void importMarkersDialog();
    void exportMarkersDialog();
    void openVideoDialog();
    void newProject();
    void openProjectDialog();
    void saveProject(bool saveAs);
    void handleShortcuts();
    void drawUI();
    void drawPluginsPanel();        // legacy wrapper (unused — see drawPluginsPanelContent)
    void drawPluginsPanelContent(); // content only (caller manages Begin/End)
    void drawPluginBrowser();
    void drawVideoPreview();        // legacy wrapper
    void drawVideoPreviewContent(); // content only (caller manages Begin/End)

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

    // ─── Video preview (RB-5) ───────────────────────────────────────────────
    // The preview panel keeps a GL texture + a decoder. Each frame (UI thread),
    // it maps the transport's samplePos to a video time and pulls the right
    // RGBA frame from the decoder (or reuses the last if we're still in the
    // same video frame). Sequential playback keeps the ffmpeg process open;
    // big jumps (seek/scrub) close + respawn.
    engine::VideoDecoder videoDecoder_;
    engine::AsyncVideoDecoder asyncDecoder_;  // background frame decode (no UI stall)
    unsigned int videoTexture_ = 0;     // GL texture id (0 = not created yet)
    int videoTexW_ = 0;                 // texture dimensions (preview-resolution)
    int videoTexH_ = 0;
    std::vector<uint8_t> videoFrameBuf_; // scratch for decoded RGBA
    int64_t lastDecodedFrameIndex_ = -1; // last frame index we uploaded
    double lastSeekTime_ = 0.0;          // ImGui::GetTime() of last random-access seek (debounce)
    std::string lastVideoClipId_;       // which clip the decoder is open for (RB-6 multi-clip)

    // ─── Project persistence ────────────────────────────────────────────────
    std::string projectPath_;            // current .dave bundle path (empty = untitled)
    bool dirty_ = false;                 // unsaved changes?
};

} // namespace dave::application
