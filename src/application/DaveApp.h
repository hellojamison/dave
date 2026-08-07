// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Edit.h"
#include "editing/Command.h"
#include "engine/GraphBuilder.h"
#include "engine/midi/SmfReader.h"
#include "engine/plugins/PluginEditor.h"
#include "engine/plugins/PluginHost.h"
#include "engine/transport/Transport.h"
#include "engine/video/AsyncVideoDecoder.h"
#include "engine/video/VideoDecoder.h"
#include "gui/ImGuiLayer.h"
#include "gui/Mixer.h"
#include "gui/Timeline.h"
#include "platform/AudioEngine.h"
#include "platform/Window.h"

#include <imgui.h>  // ImDrawList, ImVec2 for drawVideoThumbnails

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

    // Developer screenshot fixtures use the same import path as the UI so the
    // captured state exercises real document and graph behavior.
    bool loadWavIntoEdit(const std::string& path);
    bool importMidiIntoEdit(const std::string& path) { return importMidiFile(path); }
    void setTimelineSamplesPerPixel(double samplesPerPixel);
    void setVideoPoppedOut(bool poppedOut);

private:
    void onEditChanged();           // re-derive + recompile + publish
    void applySessionSampleRate();  // reopen the device at the session rate
    // Loop transport. The range comes from the timeline selection if there is
    // one, otherwise from the first active Loop marker.
    bool loopRange(int64_t& start, int64_t& end) const;
    void syncTransportLoop();
    void openWavDialog();
    void openMidiDialog();
    // Parse a .mid and push one ImportMidiFileCommand for the whole file.
    // Returns false (and logs) if the file isn't readable as an SMF.
    bool importMidiFile(const std::string& path);
    void importMarkersDialog();
    void exportMarkersDialog();
    void openVideoDialog();
    void newProject();
    void openProjectDialog();
    void saveProject(bool saveAs);
    void handleShortcuts();
    // Targets of the M / S shortcuts; null when nothing is selected.
    document::Track* selectedTrack();
    document::MidiTrack* selectedMidiTrack();
    void toggleSelectedTrackMute();
    void toggleSelectedTrackSolo();
    bool loadWavIntoNewTrack(const std::string& path);
    void drawUI();
    void drawPluginsPanel();        // legacy wrapper (unused — see drawPluginsPanelContent)
    void drawPluginsPanelContent(); // content only (caller manages Begin/End)
    void drawMidiTrackPanel(const document::MidiTrack& track);
    void drawPluginBrowser();
    void drawVideoPreview();        // legacy wrapper
    void drawVideoPreviewContent(); // content only (caller manages Begin/End)
    void drawVideoPopoutWindow();   // the detached picture window

    platform::Window window_{1280, 800, "Dave"};
    gui::ImGuiLayer imgui_;
    platform::AudioEngine audio_;

    document::Edit edit_;
    editing::UndoStack undo_{edit_};
    engine::GraphBuilder builder_;
    engine::PluginHost pluginHost_;

    gui::PeakCache peaks_;
    gui::TimelineViewState view_;

    // Ratios keep the picture-first sidebar useful as the window grows, while
    // pixel clamps in drawUI protect the timeline and plugin controls.
    float sidebarWidth_ = 360.0f;
    float videoShare_ = 0.62f;

    // ─── Mixer ──────────────────────────────────────────────────────────────
    // The mixer sits under the timeline, spanning its full width: strips are
    // laid out side by side, so it wants horizontal room, which the 360px
    // sidebar does not have. Height is user-draggable and persists for the
    // session.
    bool showMixer_ = true;
    float mixerHeight_ = 340.0f;

    // ─── Loop transport ─────────────────────────────────────────────────────
    // Loop is a transport mode, not a document property: it follows whatever
    // range is current, so toggling it on and then selecting elsewhere loops
    // the new range without any further action.
    bool loopEnabled_ = false;

    // ─── Picture pop-out ────────────────────────────────────────────────────
    // The video preview can leave the sidebar for a real OS window (see
    // drawVideoPopoutWindow) that the user drags wherever picture belongs —
    // a second display, typically. videoShare_ is left alone while popped out
    // so the sidebar split comes back exactly as the user left it.
    bool videoPoppedOut_ = false;
    // Toggles are deferred to the next frame boundary: the request can arrive
    // from the native menu bar (outside any ImGui frame) or from a button that
    // lives inside the very panel the toggle removes.
    bool videoPopoutRequest_ = false;
    bool videoPopoutRequestValue_ = false;
    // Where the docked panel sat, used to seed the popped-out window the first
    // time it appears so the picture lifts out of the layout in place.
    ImVec2 videoPanelPos_{0.0f, 0.0f};
    ImVec2 videoPanelSize_{0.0f, 0.0f};

    // Plugin browser modal state.
    //
    // The browser serves three jobs that differ in what they list and what they
    // do with the choice. Picking an EQ where an instrument belongs is the kind
    // of mistake a plugin list should make impossible, so the mode filters the
    // list rather than only changing what happens on click.
    enum class BrowserMode {
        AudioFx,         // effect chain of an audio track
        MidiInstrument,  // the instrument slot of a MIDI track (instruments only)
        MidiFx,          // post-instrument effect chain of a MIDI track
    };
    void openPluginBrowser(BrowserMode mode, std::string trackId);
    void openPluginEditor(const document::PluginSlot& slot);
    // Find a slot by id anywhere it can live: an audio track's chain, a MIDI
    // track's instrument, or a MIDI track's chain. Both the timeline and the
    // mixer identify plugins by slot id alone, so the lookup has to cover all
    // three or "open the editor" silently does nothing for some of them.
    const document::PluginSlot* findSlot(const std::string& slotId) const;
    // Apply whatever the timeline/mixer asked for this frame, then clear it.
    void serviceViewRequests();

    bool showPluginBrowser_ = false;
    BrowserMode browserMode_ = BrowserMode::AudioFx;
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
    int64_t lastRequestedFrameIndex_ = -1; // last frame index sent to the decoder
    int64_t lastUploadedFrameIndex_ = -1;  // last frame index uploaded to the texture
    double lastSeekTime_ = 0.0;          // ImGui::GetTime() of last random-access seek (debounce)
    std::string lastVideoClipId_;       // which clip the decoder is open for (RB-6 multi-clip)

    // Video thumbnails for the timeline lane. One AsyncVideoDecoder per clip,
    // caching a few representative frames as GL textures.
    struct ThumbTexture {
        unsigned int texId = 0;
        int w = 0, h = 0;
        double timeSeconds = 0.0;
    };
    struct ClipThumbnails {
        std::string clipId;
        std::string path;
        std::vector<ThumbTexture> textures;  // one per interval
        double fps = 24.0;
        bool requested = false;  // true while async decode is in progress
    };
    std::vector<ClipThumbnails> thumbCache_;
    engine::AsyncVideoDecoder thumbDecoder_;  // separate decoder for thumbs
    int thumbRequestIndex_ = 0;  // which thumb to request next
    std::string thumbRequestClipId_;  // which clip we're filling

    void updateThumbnails(const document::Edit& edit);
    void drawVideoThumbnails(ImDrawList* dl, ImVec2 origin, float laneHeight,
                             float totalWidth, float gutterWidth,
                             double scroll, double spp,
                             const document::Edit& edit);

    // ─── Project persistence ────────────────────────────────────────────────
    std::string projectPath_;            // current .dave bundle path (empty = untitled)
    bool dirty_ = false;                 // unsaved changes?
};

} // namespace dave::application
