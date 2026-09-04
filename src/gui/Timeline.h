// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Edit.h"
#include "editing/Command.h"
#include "engine/transport/Transport.h"
#include "gui/LevelMeter.h"
#include "gui/RoutingViewModel.h"
#include "gui/TransientNavigation.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace dave::gui {

// Live gain nodes by track id, for meters. Declared here rather than in
// Mixer.h because both the mixer and the timeline headers now need it.
using TrackGainNodes =
    std::unordered_map<std::string, std::shared_ptr<engine::GainNode>>;

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
enum class AutomationTool { Pencil, Line, Curve, Eraser };
// What the expanded lane beneath a track shows: an automation envelope
// (Volume/Pan, per automationParameters) or the track's other playlists,
// stacked so a take can be picked by eye. Sends and inserts are assigned in
// the track head instead, which grows to reveal them.
enum class LaneMode { Automation, Playlists };

std::string formatAutomationDrawValue(AutomationParameter parameter,
                                      double value);

// Remove every automation point whose sample lies within the closed band
// [loSample, hiSample]. Pure so the eraser's core — what survives a swipe —
// is testable without driving the lane's geometry. Order is preserved.
std::vector<document::VolumeAutomationPoint> automationErase(
    const std::vector<document::VolumeAutomationPoint>& points,
    int64_t loSample, int64_t hiSample);

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
    // Vertical scroll of the track rows, in pixels. Managed by the timeline
    // itself rather than the host window's scroll, so the ruler, marker lane
    // and add-track button stay pinned at the top instead of sliding off.
    float verticalScroll = 0.0f;
    int selectedTrackIndex = -1;
    // Multi-track selection (Shift-click a range in the track list). Held as
    // ids so it survives reordering. selectedTrackIndex stays the "primary"
    // row that single-track views (the channel strip, the plugins panel) act
    // on; this set is the highlighted group. Reconciled each frame: when the
    // primary is not in the set, an ordinary single-select happened elsewhere
    // and the set collapses to it.
    std::unordered_set<std::string> selectedTrackIds;
    int trackSelectAnchor = -1;
    std::string selectedClipId;
    // Multi-clip selection: Shift-click adds the run between the anchor and the
    // click (with the gaps), Cmd-click toggles clips one at a time. Always
    // contains the primary `selectedClipId` too.
    std::unordered_set<std::string> selectedClipIds;
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
    // Trim state. A head trim moves timelineStart, sourceOffset and length
    // together, so all three have to be previewed together — restoring only
    // the start would slide the clip's contents inside its own box.
    int64_t dragClipOriginalOffset = 0;
    int64_t dragClipOriginalLength = 0;
    int64_t dragPreviewOffset = 0;
    int64_t dragPreviewLength = 0;
    // A trim drag can act on either clip vector, and the row alone no longer
    // says which — a track holds both kinds at once now.
    bool dragClipIsMidi = false;
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
    enum class DragKind { None, AudioClip, MidiClip, Marker, VideoClip,
                          TrimStart, TrimEnd };
    DragKind dragKind = DragKind::None;

    bool isDragging() const { return dragKind != DragKind::None; }
    bool isDragging(DragKind kind) const { return dragKind == kind; }
    bool isTrimming() const {
        return dragKind == DragKind::TrimStart || dragKind == DragKind::TrimEnd;
    }

    // ─── Requests back to the application ──────────────────────────────────
    // Widgets draw into an existing window and can't open a modal or a
    // plugin's own OS window. They record the request here and DaveApp
    // services and clears it after the frame. Shared by the timeline and the
    // mixer so both reach the picker by one path.
    enum class PluginPicker { None, AudioFx, MidiInstrument, MidiFx };
    PluginPicker requestPicker = PluginPicker::None;
    std::string requestPickerTrackId;
    std::string requestPluginEditorSlotId;       // open a plugin's editor
    // A track asking for the channel strip. The strip is opt-in, so the E
    // button on a row is how it opens — the app selects that row and shows it.
    std::string requestChannelStripTrackId;
    // A track asking to be hidden, and a request to bring every hidden one
    // back. Serviced by the application, which owns the undo stack.
    std::string requestHideTrackId;
    // A track whose eye was clicked in the list — toggled rather than set, so
    // the one control does both directions.
    std::string requestToggleHiddenTrackId;
    // A track the list asked to delete (right-click -> Delete), and the row the
    // list's context menu is currently for.
    std::string requestRemoveTrackId;
    std::string trackListContextId;
    // A track whose Solo was Cmd-clicked.
    std::string requestToggleSoloSafeTrackId;
    bool requestShowAllTracks = false;
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
    // Shared by every meter in the timeline and the mixer, so a bank of them
    // can be compared at a glance. Persisted via EditorPreferences.
    LevelMeterOptions meterOptions;
    bool meterOptionsChanged = false;
    bool snapEnabled = false;
    // Snap increment in samples. 0 means "follow the ruler's timecode grid"
    // (the format-aware default); a positive value snaps to that fixed spacing
    // instead — a preset or a custom value chosen in the toolbar.
    int64_t snapIncrementSamples = 0;
    // Visual grid-line increment in samples, chosen the same way. 0 follows the
    // timecode format; a positive value draws lines at that fixed spacing —
    // but only while they stay far enough apart to read (below that the format
    // grid is drawn instead, so the view never fills with lines).
    int64_t gridIncrementSamples = 0;
    // Vertical zoom for the track rows: Control+scroll grows or shrinks it.
    float trackHeightScale = 1.0f;
    // Per-track height multiplier (Ctrl+Up/Down on the selected track, or drag
    // its bottom edge). Absent = 1.0. View state, so resizing a row does not
    // rebuild the audio graph.
    std::unordered_map<std::string, float> trackHeightScales;
    // The track whose bottom edge is being dragged to resize, or empty.
    std::string resizingTrackId;
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
    // The range being dragged was anchored at the playhead by a Shift-click,
    // so releasing it must not move the playhead to the range's head.
    bool selectionFromPlayhead = false;
    // Which lane the selection belongs to, as a row index across both bands
    // (audio 0..n-1, then MIDI). -1 means every track: that is what a drag on
    // the ruler makes, and it is the only way to get one — a drag inside a
    // lane stays in that lane, so selecting a range on one track cannot
    // silently arm an edit on its neighbours.
    int selectionRow = -1;
    // The far row of a selection dragged across tracks; the range covers
    // every row between it and selectionRow. -1 or equal = one row.
    int selectionRowEnd = -1;
    // Where the press actually landed, before the format snap. A click that
    // turns out not to be a drag seeks here: the cursor goes where you
    // clicked, while a selection edge goes to the nearest division.
    int64_t selectionPressSample = 0;
    // Track ids whose disclosure arrow points down. Every audio, MIDI and bus
    // channel reveals one parameter-selectable automation lane below its row.
    std::unordered_set<std::string> expandedTracks;
    std::unordered_map<std::string, AutomationParameter> automationParameters;
    // Which content the expanded lane shows per track. Absent = Automation.
    std::unordered_map<std::string, LaneMode> trackLaneModes;
    // Automation editing tools are global like the pointer tools in a DAW,
    // even though their compact toggle is repeated inside each open lane.
    AutomationTool automationTool = AutomationTool::Pencil;
    std::string revealAutomationOwnerId;
    // The pointer is over something that draws its own cursor, so the system
    // one has to go. Set during the frame by whatever owns that tool and read
    // by the application afterwards; cleared each frame before drawing.
    bool wantsHiddenCursor = false;
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
    // completed stroke. Curve reads Option/Alt and Control throughout the
    // gesture so its shape and slope direction can switch live.
    bool drawingAutomation = false;
    AutomationParameter drawingAutomationParameter =
        AutomationParameter::Volume;
    AutomationTool drawingAutomationTool = AutomationTool::Pencil;
    std::string drawingAutomationOwnerId;
    // The eraser sweeps a band across the lane and removes every breakpoint
    // inside it, committing one bulk replace on release like the other tools.
    // The band is kept in samples so it survives horizontal scroll mid-drag.
    int64_t automationEraseMinSample = 0;
    int64_t automationEraseMaxSample = 0;
    document::VolumeAutomationPoint automationDrawAnchor;
    std::vector<document::VolumeAutomationPoint> automationDrawOriginal;
    std::vector<document::VolumeAutomationPoint> automationDrawStroke;
    float automationDrawLastX = 0.0f;
    float automationDrawLastY = 0.0f;
    // The curve tool's shape, as an exponent: 1 is a straight line, 2 the
    // default parabola, below 1 the mirror of a parabola. One parameter
    // instead of a boolean shape swap, because Control now sweeps it
    // continuously and a two-state flag has nothing to sweep.
    double automationDrawSteepness = 2.0;
    // Option mirrors the shape around its midpoint, moving the steep part to
    // the other end. This is the ONLY flip: Control used to do the same thing
    // by another route, which made the two modifiers indistinguishable.
    bool automationDrawFlipped = false;
    // While Control is held the curve's far end stops following the pointer
    // and the drag sweeps steepness instead. The end is latched so letting go
    // of Control does not snap the curve to wherever the mouse wandered.
    bool automationSteepnessLatched = false;
    float automationSteepnessAnchorX = 0.0f;
    double automationSteepnessAtLatch = 2.0;
    int64_t automationFrozenSample = 0;
    double automationFrozenValue = 0.0;
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
    // Audio-file drop onto the timeline. The app records the path and the drop
    // point (screen coords) on drop; the timeline — which owns the track/sample
    // geometry — resolves each to a track and a snapped sample; the app then
    // imports the WAV and places the clip. Two phases because import lives in
    // the app and layout lives here.
    struct PendingFileDrop { std::string path; float x = 0.0f; float y = 0.0f; };
    struct ResolvedFileDrop {
        std::string path;
        std::string trackId;   // empty = dropped off the lanes, make a new track
        int64_t sample = 0;
    };
    std::vector<PendingFileDrop> pendingFileDrops;
    std::vector<ResolvedFileDrop> resolvedFileDrops;
    // Live drop preview while a file is dragged over (before release). Set by
    // the app from the platform drag tracker; the timeline draws a ghost clip
    // at the track and sample under the cursor.
    bool fileDragActive = false;
    float fileDragX = 0.0f;
    float fileDragY = 0.0f;
    int64_t fileDragLengthSamples = 0;   // 0 when unknown; a marker is drawn
    // Timecode display mode for the ruler + position readout.
    TimecodeMode tcMode = TimecodeMode::MinSec;
    // Clip gain being edited in the clip context menu (dB), seeded when the
    // menu opens and committed on release so a drag is one undo step.
    float contextClipGainDb = 0.0f;
    // Editable position display state.
    bool editingPosition = false;
    char positionInput[64] = {};
    // Editable selection start/end fields in the toolbar (smaller than the main
    // counter). The buffers reflect the selection until the field is focused.
    bool editingSelStart = false;
    bool editingSelEnd = false;
    char selStartInput[32] = {};
    char selEndInput[32] = {};
    // Set the frame the editor opens so focus is grabbed once. Grabbing it
    // every frame would re-steal it, so a click outside could never land — the
    // field would refuse to close.
    bool positionEditFocusPending = false;
};

// Change zoom while preserving `anchorSample` at its current screen X when it
// is visible. An off-screen anchor is centred as a recovery affordance. The
// zoom is clamped to the supported range and scroll never passes sample zero,
// so the left boundary is the one case where the anchor may have to move.
void zoomAroundSample(TimelineViewState& view, double newSamplesPerPixel,
                      int64_t anchorSample);

// The curve tool's shape at `t` in 0..1. `steepness` is an exponent — 1 is a
// straight line, 2 a parabola — and `flipped` mirrors it around the midpoint,
// which moves the steep part to the other end. Both endpoints are preserved
// whatever the shape, so a curve always starts and ends where it was drawn.
double automationCurveShape(double t, double steepness, bool flipped);

// Steepness after a Control-drag of `dx` pixels from where it was latched.
// Exponential in the drag so the same gesture halves or doubles the exponent
// whichever end of the range it starts from.
double automationSteepnessForDrag(double latched, float dx);

// Format a sample position into a timecode string for the selected mode.
std::string formatTimecode(int64_t samples, TimecodeMode mode,
                           double sr = 48000.0, double fps = 24.0,
                           double bpm = 120.0,
                           const std::vector<document::TimeSignature>* meter = nullptr,
                           const std::vector<document::TempoChange>* tempo =
                               nullptr);

// Parse a timecode string in `mode` back to a sample position, or -1 if it does
// not parse. The inverse of formatTimecode for the editable time fields.
int64_t parseTimecodeToSamples(
    const char* text, TimecodeMode mode, double sr, double fps,
    const std::vector<document::TempoChange>* tempo,
    const std::vector<document::TimeSignature>* meter);

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
                     double bpm = 120.0,
                     const std::vector<document::TimeSignature>* meter =
                         nullptr,
                     const std::vector<document::TempoChange>* tempo = nullptr);

// The increment timeline edits land on while Snap is enabled, in samples —
// the finest division of the current format that is still wide enough to aim
// at. In timecode that is whole frames, in bars|beats whole beat subdivisions.
// Zooming out steps it up through the same units rather than abandoning them.
int64_t snapStepFor(TimecodeMode mode, double samplesPerPixel,
                    double sr = 48000.0, double fps = 24.0,
                    double bpm = 120.0,
                    const std::vector<document::TimeSignature>* meter =
                        nullptr,
                     const std::vector<document::TempoChange>* tempo = nullptr);

// Round a non-negative timeline position to the nearest current-format snap
// division. Kept separate from the widget so every timeline lane can use the
// same arithmetic and headless tests can cover exact boundary behavior.
int64_t snapSampleToFormat(int64_t sample, TimecodeMode mode,
                           double samplesPerPixel,
                           double sr = 48000.0, double fps = 24.0,
                           double bpm = 120.0,
                           const std::vector<document::TimeSignature>* meter =
                               nullptr,
                     const std::vector<document::TempoChange>* tempo = nullptr);

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
                  const TransientSnapshotMap& transientAnalyses = {},
                  // Live post-fader levels, keyed by track id. Null draws the
                  // meters at silence rather than omitting them, so the header
                  // keeps its shape whether or not a graph is live.
                  const TrackGainNodes* gainNodes = nullptr);

// Duplicate whatever clip the timeline currently has selected, placing the
// copy immediately after it. Dispatches on which clip vector actually holds
// the selected id: a track carries audio and MIDI clips at once now, so the
// row cannot answer that on its own.
//
// This lives here rather than in the key handler because DaveApp is not in the
// test target — the dispatch is the part that can be got wrong, so it belongs
// somewhere a test can reach it. Returns false when nothing is selected.
// The clips the timeline selection covers, as group members.
//
// A clip counts when it OVERLAPS the range rather than when it is contained by
// it: a selection dragged roughly around three clips is the gesture people
// actually make, and requiring full containment would silently drop the ones
// whose tails stick out. The selection's lane scopes it — a range dragged on
// one track cannot group its neighbours' clips, which is the same rule every
// other selection edit follows.
std::vector<document::ClipGroup::Member> clipsInSelection(
    const document::Edit& edit, const TimelineViewState& view);

// The same, for a range the caller already has. `row` scopes it to one track,
// or -1 for every track.
std::vector<document::ClipGroup::Member> clipsInRange(
    const document::Edit& edit, int64_t start, int64_t end, int row);

// Group whatever the selection covers, or ungroup the group under it. Return
// false when there is nothing to do, so the shortcut can stay quiet.
bool groupSelectedClips(document::Edit& edit, editing::UndoStack& undo,
                        TimelineViewState& view);
bool ungroupSelectedClips(document::Edit& edit, editing::UndoStack& undo,
                          TimelineViewState& view);

// Point the timeline selection at one clip's extent, on its own row. Clicking
// a clip is how you say "this one" — and the range edits (group, delete,
// loop) all read the selection, so leaving it untouched meant a clicked clip
// was selected for some purposes and not for others.
// The playhead goes to the head of the selection, the same as it does when a
// dragged range finishes — a selection made by clicking and one made by
// dragging should leave the cursor in the same place.
void selectClipRange(TimelineViewState& view, engine::Transport& transport,
                     int row, int64_t start, int64_t length);

// Delete every automation point inside the timeline selection, on the lane
// the selected track currently shows. Returns false when there is nothing to
// act on — no selection, no open lane, or no points inside it — so the caller
// can let the key fall through to whatever else Delete might mean.
//
// Here rather than in the key handler for the usual reason: DaveApp is not in
// the test target, and deciding WHICH points are inside a range is the part
// worth testing.
bool deleteAutomationInSelection(document::Edit& edit,
                                 editing::UndoStack& undo,
                                 TimelineViewState& view);

// Delete the time range the user has selected out of the clips it covers,
// trimming rather than removing: a selection over a clip's head or tail
// shortens it, a selection covering the whole clip removes it, and a selection
// inside a clip splits it in two around the gap. Audio and MIDI clips both.
// Acts on the selection's own row, or on every track when the selection spans
// them. Returns false when there is no range or nothing under it.
bool trimClipsInSelection(document::Edit& edit,
                          editing::UndoStack& undo,
                          TimelineViewState& view);

// The points that survive removing everything in [start, end] inclusive.
// Exposed because the range arithmetic is the whole of the decision.
template <typename Point>
std::vector<Point> pointsOutsideRange(const std::vector<Point>& points,
                                      int64_t start, int64_t end) {
    std::vector<Point> kept;
    kept.reserve(points.size());
    for (const auto& point : points) {
        if (point.sample >= start && point.sample <= end) continue;
        kept.push_back(point);
    }
    return kept;
}

bool duplicateSelectedClip(const document::Edit& edit,
                           editing::UndoStack& undo,
                           TimelineViewState& view);

// Toggle the mute of the selected audio clip (Cmd+M). Returns false when no
// audio clip is selected. Here, not in the key handler, so it is testable.
bool toggleSelectedClipMute(const document::Edit& edit,
                            editing::UndoStack& undo, TimelineViewState& view);

// Update the multi-clip selection for a click on `clipId` on track `trackIndex`.
// shift extends a run from the anchor (current primary) to the click, gaps and
// all; cmd toggles the single clip; neither selects it alone. Here, not in the
// handler, so the range/toggle arithmetic is testable.
void applyClipSelection(TimelineViewState& view, const document::Edit& edit,
                        int trackIndex, const std::string& clipId, bool shift,
                        bool cmd);

// Scale the selected track's row height by `factor` (Ctrl+Up/Down), clamped.
// Per-track and view-only, so it neither rebuilds the graph nor needs undo.
bool adjustSelectedTrackHeight(const document::Edit& edit,
                               TimelineViewState& view, float factor);

// Trim the selected audio clip to the selection: `keepAfter` (A key) cuts
// everything before the selection's start; otherwise (S key) everything after
// its end. Returns false when there is nothing to cut.
bool trimSelectedClipToSelection(const document::Edit& edit,
                                 editing::UndoStack& undo, TimelineViewState& view,
                                 bool keepAfter);

// Fade the selected audio clip to the playhead: `fadeIn` (D key) fades in up to
// it, otherwise (G key) fades out from it. Returns false when the playhead is
// not inside the selected clip.
bool fadeSelectedClipToPlayhead(const document::Edit& edit,
                                editing::UndoStack& undo, TimelineViewState& view,
                                int64_t playhead, bool fadeIn,
                                document::FadeShape shape);

// Fade the audio clips the timeline selection covers. A fade always grows from
// a clip edge, so the selection sets its length: a range that reaches a clip's
// head becomes a fade-in ending where the selection ends, one that reaches its
// tail a fade-out starting where the selection begins, and an interior range
// fades from whichever edge is nearer. A selection covering a whole clip — what
// a single click leaves behind — instead drops `defaultFadeSamples` on both
// ends, so "click a clip, press F" is a one-key top-and-tail. `inShape`/
// `outShape` are the preset curves to stamp on. Batches into one undo step when
// it touches several clips. Returns false when there is no range, or nothing
// audio under it — so the shortcut can report why nothing happened. MIDI clips
// are skipped (no fades).
//
// Here rather than in the key handler because DaveApp is not in the test
// target, and mapping a range onto per-clip fade lengths is the part to test.
bool createFadeFromSelection(const document::Edit& edit,
                             editing::UndoStack& undo, TimelineViewState& view,
                             document::FadeShape inShape,
                             document::FadeShape outShape,
                             int64_t defaultFadeSamples);

// F on a selected clip that overlaps a same-track neighbour crossfades the two
// over their overlap (both take `crossShape`) and fades the selected clip's free
// edge with `freeFadeSamples`/`freeShape`. Returns false when the selected clip
// has no clean edge overlap, so the caller falls back to the normal fade.
bool createCrossfadeForSelectedClip(const document::Edit& edit,
                                    editing::UndoStack& undo,
                                    TimelineViewState& view,
                                    document::FadeShape crossShape,
                                    int64_t freeFadeSamples,
                                    document::FadeShape freeShape);

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
