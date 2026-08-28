// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Types.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace dave::document {

// The half-open timeline intervals of `clips[index]` that a clip drawn on top
// of it covers. "On top" is later in the vector — the order the timeline paints,
// last one over the rest — so overlapping clips don't sum in playback: the top
// clip wins its overlap and the one beneath falls silent there.
//
// Intervals are returned in clip order and may abut or overlap each other; a
// caller only ever tests membership, so that is harmless. Empty when nothing
// covers the clip.
inline std::vector<std::pair<std::int64_t, std::int64_t>> clipMuteIntervals(
    const std::vector<AudioClip>& clips, std::size_t index) {
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    if (index >= clips.size()) return out;
    const std::int64_t start = clips[index].timelineStart;
    const std::int64_t end = start + clips[index].length;
    for (std::size_t j = index + 1; j < clips.size(); ++j) {
        const std::int64_t js = clips[j].timelineStart;
        const std::int64_t je = js + clips[j].length;
        const std::int64_t overlapStart = std::max(start, js);
        const std::int64_t overlapEnd = std::min(end, je);
        if (overlapEnd > overlapStart) {
            out.emplace_back(overlapStart, overlapEnd);
        }
    }
    return out;
}

} // namespace dave::document
