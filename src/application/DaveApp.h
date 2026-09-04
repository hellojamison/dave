// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "application/AudioPreferences.h"
#include "application/EditorPreferences.h"
#include "application/PunchRecording.h"
#include "audio/TransientAnalysisCache.h"
#include "document/Edit.h"
#include "editing/Command.h"
#include "engine/GraphBuilder.h"
#include "engine/midi/SmfReader.h"
#include "engine/plugins/PluginEditor.h"
#include "engine/plugins/PluginHost.h"
#include "engine/record/DiskWriter.h"
#include "engine/record/RecordController.h"
#include "engine/transport/Transport.h"
#include "engine/video/AsyncVideoDecoder.h"
#include "engine/video/VideoDecoder.h"
#include "gui/ImGuiLayer.h"
#include "gui/IoPanel.h"
#include "gui/ChannelStrip.h"
#include "gui/Mixer.h"
#include "gui/SideRail.h"
#include "gui/Timeline.h"
#include "gui/TrackList.h"
#include "platform/AudioEngine.h"
#include "platform/Window.h"

#include <imgui.h>  // ImDrawList, ImVec2 for drawVideoThumbnails

#include <cstdint>
#include <filesystem>
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

    bool init(bool startAudio = true);
    void run();

    // Developer screenshot fixtures use the same import path as the UI so the
    // captured state exercises real document and graph behavior.
    bool loadWavIntoEdit(const std::string& path);
    bool importMidiIntoEdit(const std::string& path) { return importMidiFile(path); }
    void setTimelineSamplesPerPixel(double samplesPerPixel);
    void configureAutomationScreenshot();
    void configureTransientScreenshot();
    void setVideoPoppedOut(bool poppedOut);

private:
    void onEditChanged();           // re-derive + recompile + publish
    void applySessionSampleRate();  // reopen the device at the session rate
    void refreshAudioDevices();
    void syncIoPanelState();
    void serviceIoPanelRequests();
    bool applyAudioPreferences(const AudioPreferences& preferences,
                               bool saveOnSuccess);
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
    void prefetchSelectedTrackTransients();
    gui::TransientSnapshotMap selectedTrackTransientSnapshots();
    void navigateTimeline(gui::NavigationDirection direction, bool extend,
                          bool allowPending = true);
    void servicePendingTransientNavigation();
    void toggleTransientNavigation();
    void saveEditorPreferences();
    void drawPreferencesWindow();
    // Targets of the M / S shortcuts; null when nothing is selected.
    document::Track* selectedTrack();
    void toggleSelectedTrackMute();
    void toggleSelectedTrackSolo();
    void toggleSelectedTrackArm();
    void toggleTrackArm(const std::string& trackId);
    // Capture and regions are separate. A session exists for as long as the
    // transport rolls over armed tracks; punches decide what is kept.
    bool beginCapture();
    void beginCaptureIfArmed();
    void endCapture();
    void punchIn();
    void punchOut();
    void toggleRecording();
    // Start/stop playback, rolling in from the pre-roll lead when enabled.
    void togglePlayback();
    // Stop playback once it passes a post-roll stop point (auditioning a
    // selection). -1 means no stop point is armed.
    void servicePostRoll();
    int64_t playStopAt_ = -1;
    // Metronome on/off (session state); applied to the graph's node each frame
    // so it survives rebuilds without a rebuild of its own.
    bool metronomeEnabled_ = false;
    // A capture is running — the engine is writing to disk, so routing
    // topology is frozen even when nothing is being kept.
    bool capturing() const { return recordingSession_ != nullptr; }
    // Actually keeping audio: what the record button is lit for.
    bool recordingActive() const {
        return capturing() && !recordingSession_->punches.empty() &&
               recordingSession_->punches.back().open();
    }
    void showStatus(std::string message, bool error = false);
    bool requestClose();
    bool loadWavIntoNewTrack(const std::string& path);
    // Import a WAV and place it as a clip on an existing track at `sample`.
    bool placeWavOnTrack(const std::string& path, const std::string& trackId,
                         int64_t sample);
    // Frame count of a WAV from its header, cached for the drag-over ghost.
    int64_t wavLengthSamples(const std::string& path);
    std::string fileDragLenPath_;
    int64_t fileDragLenSamples_ = 0;
    void drawUI();
    void drawPluginBrowser();
    void drawVideoPreview();        // legacy wrapper
    void drawVideoPreviewContent(); // content only (caller manages Begin/End)
    void drawVideoPopoutWindow();   // the detached picture window

    platform::Window window_{1280, 800, "Dave"};
    gui::ImGuiLayer imgui_;
    platform::AudioEngine audio_;
    AudioPreferencesStore audioPreferencesStore_;
    AudioPreferences audioPreferences_;
    EditorPreferencesStore editorPreferencesStore_;
    EditorPreferences editorPreferences_;
    platform::DeviceLists audioDevices_;
    gui::IoPanelState ioPanel_;

    struct RecordingSession {
        engine::DiskWriter writer;
        engine::RecordController controller;
        // Where the continuous capture began. Regions are cut out of it.
        int64_t takeStartSample = 0;
        int latencyOffsetSamples = 0;
        std::vector<std::string> armedTrackIds;
        // Which parts of the capture the user asked to keep. Empty means the
        // transport rolled over armed tracks without anyone pressing Record,
        // which writes a file and commits nothing.
        std::vector<application::PunchRange> punches;
    };
    std::unique_ptr<RecordingSession> recordingSession_;
    // Capture follows the transport, so the frame loop watches for the edge
    // rather than every place that can start or stop playback.
    bool transportWasRolling_ = false;
    std::string statusMessage_;
    bool statusIsError_ = false;
    double statusUntil_ = 0.0;

    document::Edit edit_;
    editing::UndoStack undo_{edit_};
    engine::GraphBuilder builder_;
    audio::TransientAnalysisCache transientAnalyses_;
    engine::PluginHost pluginHost_;

    gui::PeakCache peaks_;
    gui::TimelineViewState view_;

    struct PendingTransientNavigation {
        gui::NavigationDirection direction = gui::NavigationDirection::Next;
        bool extend = false;
        int selectedTrackIndex = -1;
        int64_t transportSample = 0;
        bool hadSelection = false;
        int64_t selectionAnchor = 0;
        int64_t selectionFocus = 0;
    };
    std::unique_ptr<PendingTransientNavigation> pendingTransientNavigation_;

    // Docked utility panes keep explicit pixel sizes. Native window resizing
    // changes the arrangement editor; only splitter drags change these.
    // The strip is a single narrow column now, not a plugin list, and it is
    // opt-in: a session that isn't routing anything shouldn't spend width on
    // it. The E button on a track header opens it.
    // What the right column is showing. One panel at a time, the way an
    // activity bar works: two panels sharing a column would each get half the
    // width and neither would be usable.
    enum class RightPanel { None, Strip };
    // The channel strip is the right column's default content: the right
    // Tracks panel was removed (the left list is the only one needed), and an
    // empty right column reads as a missing panel rather than a clean layout.
    RightPanel rightPanel_ = RightPanel::Strip;
    bool showChannelStrip_ = false;
    // The track list on the left. Off by default like the strip: a session
    // with nothing hidden has nothing to look up.
    bool showTrackList_ = false;
    float trackListWidth_ = 190.0f;
    float sidebarWidth_ = 260.0f;
    gui::ChannelStripState channelStrip_;
    float videoHeight_ = 322.0f;


    bool openTransientOptions_ = false;
    // Preferences is a real window rather than a popup: it is reached from the
    // app menu and Cmd+, like any Mac app, and a popup closes the moment the
    // pointer leaves it.
    bool showPreferences_ = false;
    bool preferencesJustOpened_ = false;

    // ─── Mixer ──────────────────────────────────────────────────────────────
    // The mixer sits under the timeline, spanning its full width: strips are
    // laid out side by side, so it wants horizontal room, which the 360px
    // sidebar does not have. Height is user-draggable and persists for the
    // session.
    bool showMixer_ = false;
    float mixerHeight_ = 340.0f;
    // Toolbar snap-increment custom value, in milliseconds (session-only).
    double snapCustomMs_ = 100.0;
    double gridCustomMs_ = 100.0;

    // ─── Loop transport ─────────────────────────────────────────────────────
    // Loop is a transport mode, not a document property: it follows whatever
    // range is current, so toggling it on and then selecting elsewhere loops
    // the new range without any further action.
    bool loopEnabled_ = false;

    // ─── Picture pop-out ────────────────────────────────────────────────────
    // The video preview can leave the sidebar for a real OS window (see
    // drawVideoPopoutWindow) that the user drags wherever picture belongs —
    // a second display, typically. videoHeight_ is left alone while popped out
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
    void drawMeterEditor();
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
