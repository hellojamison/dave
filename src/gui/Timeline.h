#pragma once

#include "document/Edit.h"
#include "editing/Command.h"
#include "engine/transport/Transport.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dave::gui {

// Peak cache: min/max per "bucket" of samples, used to draw waveforms cheaply
// at a given zoom level. Computed once per asset, cached by asset id + bucket
// size. For RB-2 this is a single resolution; a mipmap (multiple bucket sizes)
// comes later for smooth zoom.
struct PeakBucket { float min; float max; };

class PeakCache {
public:
    // Get peaks for an asset at `samplesPerPixel` resolution. Computes and
    // caches on first request; returns cached thereafter.
    const std::vector<PeakBucket>& get(const std::string& assetId,
                                       const std::vector<std::vector<float>>& buffer,
                                       int samplesPerPixel);

private:
    // key: assetId + samplesPerPixel
    std::unordered_map<std::string, std::vector<PeakBucket>> cache_;
    std::string key(const std::string& id, int spp) const { return id + ":" + std::to_string(spp); }
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
    bool dragging = false;
};

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

} // namespace dave::gui
