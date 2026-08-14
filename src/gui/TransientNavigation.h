// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/TransientAnalysisCache.h"
#include "document/Types.h"

#include <cstdint>
#include <imgui.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace dave::gui {

enum class TimelineLandmarkMode { ClipBoundaries, Transients };
enum class NavigationDirection { Previous, Next };

ImGuiKeyChord transientNavigationShortcut(bool macOS,
                                          NavigationDirection direction,
                                          bool extend) noexcept;
ImGuiKeyChord transientNavigationToggleShortcut(bool macOS) noexcept;

using TransientSnapshotMap = std::unordered_map<
    std::string, audio::TransientAnalysisCache::Snapshot>;

struct TrackLandmarks {
    std::vector<int64_t> samples;
    bool analysisPending = false;
    bool analysisFailed = false;
};

struct TransientTickPosition {
    float x = 0.0f;
    float strength = 0.0f;
    bool operator==(const TransientTickPosition&) const = default;
};

std::vector<TransientTickPosition> cullTransientTicks(
    std::vector<TransientTickPosition> ticks,
    float minimumPixelSpacing = 3.0f);

TrackLandmarks collectTrackLandmarks(
    const document::Track& track,
    TimelineLandmarkMode mode,
    const TransientSnapshotMap& analyses,
    int sensitivity);

bool findTimelineLandmark(const std::vector<int64_t>& landmarks,
                          int64_t currentSample,
                          NavigationDirection direction,
                          int64_t& destination);

struct NavigationSelection {
    bool active = false;
    int64_t anchor = 0;
    int64_t focus = 0;
    int64_t start = 0;
    int64_t end = 0;
};

// Navigation is view state, not a document edit. An unmodified move collapses
// the range; an extending move preserves its explicit anchor and lets focus
// cross it so repeated forward/backward gestures contract naturally.
NavigationSelection applyTimelineNavigation(
    const NavigationSelection& selection,
    int64_t currentSample,
    int64_t destination,
    bool extend);

} // namespace dave::gui
