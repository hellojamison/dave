// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace dave::application {

struct PlaybackRange {
    int64_t start = 0;
    int64_t stop = -1;
};

// A selection changes playback only when playback starts. Looping uses the
// transport's existing loop range instead of a one-shot stop.
inline PlaybackRange playbackRange(int64_t position, bool hasSelection,
                                   int64_t selectionStart, int64_t selectionEnd,
                                   bool loopEnabled, int64_t postRollSamples) {
    PlaybackRange range{position, -1};
    if (!hasSelection || selectionStart == selectionEnd) return range;
    range.start = std::min(selectionStart, selectionEnd);
    if (!loopEnabled) {
        const int64_t end = std::max(selectionStart, selectionEnd);
        const int64_t post = std::max<int64_t>(0, postRollSamples);
        range.stop = end > std::numeric_limits<int64_t>::max() - post
            ? std::numeric_limits<int64_t>::max() : end + post;
    }
    return range;
}

} // namespace dave::application
