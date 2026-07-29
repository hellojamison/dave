// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Edit.h"
#include "editing/Command.h"
#include "engine/transport/Transport.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <string>
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

// TimelineViewState holds the user's view: horizontal scroll + zoom level.
// Timeline itself is a pure function of (Edit, viewState) and reports any
// interaction (drag, seek) back via the UndoStack + Transport.
struct TimelineViewState {
    double samplesPerPixel = 200.0;  // zoom; lower = zoomed in
    double scrollSamples = 0.0;      // leftmost visible sample
    int selectedTrackIndex = -1;
    std::string selectedClipId;
    // Transient drag state (held only while a drag is active).
    int64_t dragClipOriginalStart = 0;
    std::string dragOriginalTrackId;  // track the clip came from
    bool dragging = false;
    // Marker drag: the screen-X where the drag started, so we can show the
    // marker moving live (offset from dragClipOriginalStart) before commit.
    float markerDragStartX = 0.0f;
    // Snap-to-marker: when true, clip drags + seeks snap to nearby markers.
    bool snapToMarkers = false;
    // Selection region (click-drag on empty timeline to create).
    bool hasSelection = false;
    int64_t selectionStart = 0;
    int64_t selectionEnd = 0;
    bool isSelecting = false;
    // Inline rename state.
    bool isRenaming = false;
    int renameTrackIndex = -1;
    // Timecode display mode for the ruler + position readout.
    TimecodeMode tcMode = TimecodeMode::MinSec;
    // Editable position display state.
    bool editingPosition = false;
    char positionInput[64] = {};
};

// Format a sample position into a timecode string for the selected mode.
std::string formatTimecode(int64_t samples, TimecodeMode mode,
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
                      std::vector<std::vector<float>>>& assetBuffers,
                  float trackHeight = 80.0f,
                  float timelineHeight = 28.0f);

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
// click empty lane to seek. Returns the height consumed.
float drawVideoLane(const document::Edit& edit,
                    engine::Transport& transport,
                    TimelineViewState& view,
                    ImVec2 origin,
                    float totalWidth,
                    float gutterWidth,
                    double scrollSamples,
                    double samplesPerPixel);

} // namespace dave::gui
