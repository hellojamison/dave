// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/TransientNavigation.h"

#include "audio/TransientDetector.h"

#include <algorithm>
#include <limits>

namespace dave::gui {

ImGuiKeyChord transientNavigationShortcut(bool macOS,
                                          NavigationDirection direction,
                                          bool extend) noexcept {
    ImGuiKeyChord chord = ImGuiKey_Tab;
    if (direction == NavigationDirection::Previous) {
        chord |= macOS ? ImGuiMod_Alt : ImGuiMod_Ctrl;
    }
    if (extend) chord |= ImGuiMod_Shift;
    return chord;
}

ImGuiKeyChord transientNavigationToggleShortcut(bool macOS) noexcept {
    return macOS
        ? ImGuiMod_Super | ImGuiMod_Alt | ImGuiKey_Tab
        : ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_T;
}

std::vector<TransientTickPosition> cullTransientTicks(
    std::vector<TransientTickPosition> ticks,
    float minimumPixelSpacing) {
    std::sort(ticks.begin(), ticks.end(),
              [](const auto& a, const auto& b) { return a.x < b.x; });
    std::vector<TransientTickPosition> result;
    for (const auto& tick : ticks) {
        if (result.empty() ||
            tick.x - result.back().x >= std::max(0.0f, minimumPixelSpacing)) {
            result.push_back(tick);
        } else if (tick.strength > result.back().strength) {
            result.back() = tick;
        }
    }
    return result;
}

TrackLandmarks collectTrackLandmarks(
    const document::Track& track,
    TimelineLandmarkMode mode,
    const TransientSnapshotMap& analyses,
    int sensitivity) {
    TrackLandmarks result;
    if (mode == TimelineLandmarkMode::ClipBoundaries) {
        result.samples.reserve(track.clips.size() * 2);
        for (const auto& clip : track.clips) {
            if (clip.length <= 0) continue;
            result.samples.push_back(std::max<int64_t>(0, clip.timelineStart));
            if (clip.timelineStart <=
                std::numeric_limits<int64_t>::max() - clip.length) {
                result.samples.push_back(
                    std::max<int64_t>(0, clip.timelineStart + clip.length));
            }
        }
    } else {
        const float threshold =
            audio::TransientDetector::thresholdForSensitivity(sensitivity);
        for (const auto& clip : track.clips) {
            if (clip.length <= 0) continue;
            const auto analysis = analyses.find(clip.asset.sha256);
            if (analysis == analyses.end() ||
                analysis->second.status ==
                    audio::TransientAnalysisCache::Status::Missing ||
                analysis->second.status ==
                    audio::TransientAnalysisCache::Status::Pending) {
                result.analysisPending = true;
                continue;
            }
            if (analysis->second.status ==
                    audio::TransientAnalysisCache::Status::Failed ||
                !analysis->second.candidates) {
                result.analysisFailed = true;
                continue;
            }
            const int64_t sourceBegin = std::max<int64_t>(0, clip.sourceOffset);
            const int64_t sourceEnd = sourceBegin <=
                    std::numeric_limits<int64_t>::max() - clip.length
                ? sourceBegin + clip.length
                : std::numeric_limits<int64_t>::max();
            for (const auto& candidate : *analysis->second.candidates) {
                if (candidate.strength < threshold ||
                    candidate.sourceSample < sourceBegin ||
                    candidate.sourceSample >= sourceEnd) {
                    continue;
                }
                const int64_t offset = candidate.sourceSample - sourceBegin;
                if (clip.timelineStart >
                    std::numeric_limits<int64_t>::max() - offset) {
                    continue;
                }
                result.samples.push_back(
                    std::max<int64_t>(0, clip.timelineStart + offset));
            }
        }
    }

    std::sort(result.samples.begin(), result.samples.end());
    result.samples.erase(
        std::unique(result.samples.begin(), result.samples.end()),
        result.samples.end());
    return result;
}

bool findTimelineLandmark(const std::vector<int64_t>& landmarks,
                          int64_t currentSample,
                          NavigationDirection direction,
                          int64_t& destination) {
    if (direction == NavigationDirection::Next) {
        const auto found = std::upper_bound(
            landmarks.begin(), landmarks.end(), currentSample);
        if (found == landmarks.end()) return false;
        destination = *found;
        return true;
    }
    const auto found = std::lower_bound(
        landmarks.begin(), landmarks.end(), currentSample);
    if (found == landmarks.begin()) return false;
    destination = *std::prev(found);
    return true;
}

NavigationSelection applyTimelineNavigation(
    const NavigationSelection& selection,
    int64_t currentSample,
    int64_t destination,
    bool extend) {
    if (!extend) return NavigationSelection{};
    NavigationSelection result = selection;
    if (!result.active) {
        result.active = true;
        result.anchor = currentSample;
    }
    result.focus = destination;
    result.start = std::min(result.anchor, result.focus);
    result.end = std::max(result.anchor, result.focus);
    return result;
}

} // namespace dave::gui
