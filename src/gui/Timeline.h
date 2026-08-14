// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Edit.h"
#include "editing/Command.h"
#include "engine/transport/Transport.h"
#include "gui/RoutingViewModel.h"
#include "gui/TransientNavigation.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace dave::gui {

// Peak cache: min/max per "bucket" of samples, used to draw waveforms cheaply
// at a given zoom level. Power-of-two levels form a bounded mip chain so zoom
// changes reuse nearby work instead of filling memory with arbitrary resolutions.
struct PeakBucket { float min; float max; };

struct PeakLevel {
    int bucketSize = 1;
    std::vector<PeakBucket> buckets;
    float displayScale = 1.0f;
};

class PeakCache {
public:
    // Get the nearest power-of-two level for `samplesPerPixel`, building it on
    // the UI thread only when that level has not already been retained.
    const PeakLevel& get(const std::string& assetId,
                         const std::vector<std::vector<float>>& buffer,
                         int samplesPerPixel);

private:
    struct CachedLevel {
        PeakLevel peaks;
        uint64_t lastUsed = 0;
    };
    std::unordered_map<std::string, std::vector<CachedLevel>> cache_;
    size_t cacheBytes_ = 0;
    uint64_t useClock_ = 0;

    static int bucketSizeFor(int samplesPerPixel, size_t sampleCount);
    void trim(size_t incomingBytes);
};

// Timecode display modes for the ruler + transport readout.
enum class TimecodeMode {
    MinSec,      // mm:ss.ms
    Smpte,       // HH:MM:SS:FF (needs fps)
    BarsBeats,   // bars.beats.ticks (needs tempo/bpm)
    FeetFrames,  // feet+frames (film convention, 16 frames/foot)
    Samples      // raw sample count
};

enum class AutomationParameter { Volume, Pan };
enum class AutomationTool { Pencil, Line, Curve };

// TimelineViewState holds the user's view: horizontal scroll + zoom level.
// Timeline itself is a pure function of (Edit, viewState) and reports any
// interaction (drag, seek) back via the UndoStack + Transport.
// Zoom limits, in samples per pixel. The floor is sub-sample-accurate editing;
// the ceiling is how far out a whole session has to fit. At 48 kHz the old
// 50,000 ceiling showed about 17 minutes across a 1000 px timeline, which is
// less than one reel — a feature-length session could not be seen end to end.
constexpr double kMinSamplesPerPixel = 4.0;
constexpr double kMaxSamplesPerPixel = 1'000'000.0;   // ~5.8 hours / 1000 px

struct TimelineViewState {
    double samplesPerPixel = 200.0;  // zoom; lower = zoomed in
    double scrollSamples = 0.0;      // leftmost visible sample
    int selectedTrackIndex = -1;
    std::string selectedClipId;
    // UI-only recording preview. The document does not receive a clip until
    // the WAV is closed and hashed; armed lanes still show the growing take.
    bool recordingActive = false;
    int64_t recordingStartSample = 0;
    int64_t recordingEndSample = 0;
    // Transient drag state (held only while a drag is active).
    int64_t dragClipOriginalStart = 0;
    // Where a dragged clip is currently being previewed. The document keeps
    // the pre-drag position until mouse-up, so undo has a real "before" and
    // the audio engine never rebuilds against an uncommitted position.
    int64_t dragPreviewStart = 0;
    std::string dragOriginalTrackId;  // track the clip came from
    // What is being dragged.
    //
    // This must be explicit, not a bare bool. The marker lane, the track rows
    // and the video lane all run inside one frame and all reach for the same
    // selectedClipId/dragOriginalTrackId fields; with only a "something is
    // being dragged" flag, each one's mouse-up handler fired for every drag
    // and then cleared the shared state. The marker lane draws FIRST, so it
    // reliably cancelled clip drags before the clip commit could run — the
    // clip followed the mouse and then snapped back on release.
    //
    // Every handler now checks its own kind, so a drag belongs to exactly one
    // of them.
    enum class DragKind { None, AudioClip, MidiClip, Marker, VideoClip };
    DragKind dragKind = DragKind::None;

    bool isDragging() const { return dragKind != DragKind::None; }
    bool isDragging(DragKind kind) const { return dragKind == kind; }
    // The MIDI track whose clip opened the context menu (selectedTrackIndex is
    // a row index across both bands, which the popup can't resolve on its own
    // once the track list changes underneath it).
    std::string contextMidiTrackId;

    // ─── Requests back to the application ──────────────────────────────────
    // Widgets draw into an existing window and can't open a modal or a
    // plugin's own OS window. They record the request here and DaveApp
    // services and clears it after the frame. Shared by the timeline and the
    // mixer so both reach the picker by one path.
    enum class PluginPicker { None, AudioFx, MidiInstrument, MidiFx };
    PluginPicker requestPicker = PluginPicker::None;
    std::string requestPickerTrackId;
    std::string requestPluginEditorSlotId;       // open a plugin's editor
    std::string requestTrackColorId;
    std::string requestTrackColor;               // empty restores default
    bool deferRecordArmRequests = false;
    std::string requestRecordArmTrackId;
    RoutingViewModel routing;

    // Marker drag: the screen-X where the drag started, so we can show the
    // marker moving live (offset from dragClipOriginalStart) before commit.
    float markerDragStartX = 0.0f;
    // Where a clip drag was picked up, in screen space. This lives with the
    // rest of the view state rather than in a function-local static inside
    // drawTimeline: a static is shared by every caller in the process, which
    // makes the widget non-reentrant and leaks one drag's origin into the
    // next one's delta.
    float dragStartMouseX = 0.0f;
    float dragStartMouseY = 0.0f;
    // When enabled, timeline edits snap to divisions expressed by the active
    // ruler format: frames for SMPTE/feet+frames, musical subdivisions for
    // bars|beats, round time values for min:sec, or round sample counts.
    bool snapEnabled = false;
    // Timeline keyboard ownership is remembered from the previous frame so
    // the application can route Tab before the immediate-mode widget draws.
    bool timelineKeyboardFocus = true;
    bool transientNavigationEnabled = false;
    bool showTransientTicks = false;
    int transientSensitivity = 50;
    bool requestTransientNavigation = false;
    NavigationDirection transientNavigationDirection =
        NavigationDirection::Next;
    bool requestTransientSelectionExtension = false;
    // Selection region (click-drag on empty timeline to create).
    bool hasSelection = false;
    int64_t selectionStart = 0;
    int64_t selectionEnd = 0;
    // Normalized start/end alone cannot express which edge Shift+Tab should
    // move after the focus crosses the anchor.
    int64_t selectionAnchor = 0;
    int64_t selectionFocus = 0;
    bool isSelecting = false;
    // Which lane the selection belongs to, as a row index across both bands
    // (audio 0..n-1, then MIDI). -1 means every track: that is what a drag on
    // the ruler makes, and it is the only way to get one — a drag inside a
    // lane stays in that lane, so selecting a range on one track cannot
    // silently arm an edit on its neighbours.
    int selectionRow = -1;
    // Where the press actually landed, before the format snap. A click that
    // turns out not to be a drag seeks here: the cursor goes where you
    // clicked, while a selection edge goes to the nearest division.
    int64_t selectionPressSample = 0;
    // Track ids whose disclosure arrow points down. Every audio, MIDI and bus
    // channel reveals one parameter-selectable automation lane below its row.
    std::unordered_set<std::string> expandedTracks;
    std::unordered_map<std::string, AutomationParameter> automationParameters;
    // Automation editing tools are global like the pointer tools in a DAW,
    // even though their compact toggle is repeated inside each open lane.
    AutomationTool automationTool = AutomationTool::Pencil;
    std::string revealAutomationOwnerId;
    bool draggingAutomation = false;
    AutomationParameter activeAutomationParameter =
        AutomationParameter::Volume;
    std::string automationOwnerId;
    std::string automationPointId;
    // The `db` member is a generic lane value here: dB for Volume and the
    // normalized -1..+1 position for Pan. It never enters the document until
    // the parameter-specific undo command converts it on commit.
    document::VolumeAutomationPoint automationOriginal;
    document::VolumeAutomationPoint automationPreview;
    // Pencil, Line and Curve gestures are preview-only until mouse-up. The
    // generic `db` value is dB for Volume and normalized -1..+1 for Pan,
    // matching the point-drag preview above. One bulk command commits the
    // completed stroke. Curve captures the Command modifier at mouse-down so
    // the entire gesture is consistently parabolic or logarithmic.
    bool drawingAutomation = false;
    AutomationParameter drawingAutomationParameter =
        AutomationParameter::Volume;
    AutomationTool drawingAutomationTool = AutomationTool::Pencil;
    std::string drawingAutomationOwnerId;
    document::VolumeAutomationPoint automationDrawAnchor;
    std::vector<document::VolumeAutomationPoint> automationDrawOriginal;
    std::vector<document::VolumeAutomationPoint> automationDrawStroke;
    float automationDrawLastX = 0.0f;
    float automationDrawLastY = 0.0f;
    bool automationDrawLogarithmic = false;
    // Double-clicking an envelope point opens a compact numeric editor beside
    // it. This stays view-only; committing still goes through the undo stack.
    bool editingAutomationValue = false;
    bool focusAutomationValue = false;
    std::string automationEditOwnerId;
    std::string automationEditPointId;
    double automationEditValue = 0.0;
    // Inline rename state.
    bool isRenaming = false;
    int renameTrackIndex = -1;
    // Width of the clip lane in pixels — everything right of the gutter.
    // drawTimeline writes it each frame so zoom can be re-centred from
    // outside the widget (a keyboard shortcut has no other way to know how
    // wide the visible span is).
    float laneWidthPixels = 0.0f;
    // Timecode display mode for the ruler + position readout.
    TimecodeMode tcMode = TimecodeMode::MinSec;
    // Editable position display state.
    bool editingPosition = false;
    char positionInput[64] = {};
};

// Change zoom while preserving `anchorSample` at its current screen X when it
// is visible. An off-screen anchor is centred as a recovery affordance. The
// zoom is clamped to the supported range and scroll never passes sample zero,
// so the left boundary is the one case where the anchor may have to move.
void zoomAroundSample(TimelineViewState& view, double newSamplesPerPixel,
                      int64_t anchorSample);

// Format a sample position into a timecode string for the selected mode.
std::string formatTimecode(int64_t samples, TimecodeMode mode,
                           double sr = 48000.0, double fps = 24.0,
                           double bpm = 120.0);

// The grid divisions for a timing format, in samples. `major` is the labelled
// one; `minor` is its subdivision.
struct GridStep {
    int64_t major = 1;
    int64_t minor = 1;
};

// Pick the grid for a format at a given zoom. Every format lands on divisions
// that are whole in its own units — frames for SMPTE and feet+frames, beats
// and bars for bars|beats, round decades for samples — so the ruler labels
// read as exact values and the lane lines fall where the format says they
// should. A seconds grid under a bars|beats ruler is the failure this exists
// to prevent: the labels tick over at positions that are not beats.
GridStep gridStepFor(TimecodeMode mode, double samplesPerPixel,
                     double sr = 48000.0, double fps = 24.0,
                     double bpm = 120.0);

// The increment timeline edits land on while Snap is enabled, in samples —
// the finest division of the current format that is still wide enough to aim
// at. In timecode that is whole frames, in bars|beats whole beat subdivisions.
// Zooming out steps it up through the same units rather than abandoning them.
int64_t snapStepFor(TimecodeMode mode, double samplesPerPixel,
                    double sr = 48000.0, double fps = 24.0,
                    double bpm = 120.0);

// Round a non-negative timeline position to the nearest current-format snap
// division. Kept separate from the widget so every timeline lane can use the
// same arithmetic and headless tests can cover exact boundary behavior.
int64_t snapSampleToFormat(int64_t sample, TimecodeMode mode,
                           double samplesPerPixel,
                           double sr = 48000.0, double fps = 24.0,
                           double bpm = 120.0);

// Draw the timeline widget. Caller passes everything in; the widget holds no
// state itself (immediate-mode). Interactions fire commands on the UndoStack
// and seeks on the Transport.
void drawTimeline(const document::Edit& edit,
                  editing::UndoStack& undo,
                  engine::Transport& transport,
                  PeakCache& peaks,
                  TimelineViewState& view,
                  const std::unordered_map<std::string,
                      audio::DecodedAudioAssetPtr>& assetBuffers,
                  // 58 and 30 match PTXExtractor's playlist lane and ruler.
                  // drawTimeline clamps trackHeight up if the gutter controls
                  // need more room, so this is a floor, not a guarantee.
                  float trackHeight = 58.0f,
                  float timelineHeight = 30.0f,
                  const TransientSnapshotMap& transientAnalyses = {});

// Draw the marker lane (a strip above the track rows showing markers as flags
// and regions). Returns the height it consumed (caller reserves that much
// vertical space above the tracks). Interactions: double-click adds a marker,
// drag moves, right-click opens a context menu, click seeks.
float drawMarkerLane(const document::Edit& edit,
                     editing::UndoStack& undo,
                     engine::Transport& transport,
                     TimelineViewState& view,
                     ImVec2 origin,     // top-left of the lane (screen space)
                     float totalWidth,  // available width
                     float gutterWidth, // left gutter (track-name column)
                     double scrollSamples,
                     double samplesPerPixel);

// Draw the video lane (below the audio tracks). Shows video clips as colored
// blocks with names, positioned on the timeline. Click a clip to select it;
// click empty lane to seek. Returns the height consumed — zero, drawing
// nothing, when the edit has no video tracks.
float drawVideoLane(const document::Edit& edit,
                    engine::Transport& transport,
                    TimelineViewState& view,
                    ImVec2 origin,
                    float totalWidth,
                    float gutterWidth,
                    double scrollSamples,
                    double samplesPerPixel);

} // namespace dave::gui
