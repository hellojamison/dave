// SPDX-License-Identifier: GPL-3.0-or-later
#include "application/PunchRecording.h"

#include <algorithm>

namespace dave::application {

PunchClipRange punchClipRange(int64_t captureStartSample,
                              int64_t capturedFrames,
                              const PunchRange& punch,
                              int64_t latencyOffsetSamples) {
    PunchClipRange result;
    captureStartSample = std::max<int64_t>(0, captureStartSample);
    capturedFrames = std::max<int64_t>(0, capturedFrames);
    if (punch.open() || capturedFrames == 0) return result;

    const int64_t captureEnd = captureStartSample + capturedFrames;
    // A punch outside the captured span produces nothing rather than a clip
    // pointing at audio that was never written.
    const int64_t in = std::clamp(punch.in, captureStartSample, captureEnd);
    const int64_t out = std::clamp(punch.out, captureStartSample, captureEnd);
    result.clampedToCapture = in != punch.in || out != punch.out;
    if (out <= in) return result;

    // Where in the file the punch begins, before any latency shift.
    const int64_t sourceOffset = in - captureStartSample;
    const int64_t offset = std::max<int64_t>(0, latencyOffsetSamples);

    // Move the region earlier by the offset. Below zero there is no timeline
    // left to move into, so the remainder is taken off the front of the audio
    // instead — the region stays put and starts later in the file.
    const int64_t wantStart = in - offset;
    if (wantStart >= 0) {
        result.timelineStart = wantStart;
        result.sourceOffset = sourceOffset;
    } else {
        result.timelineStart = 0;
        result.sourceOffset = sourceOffset + (-wantStart);
    }

    if (result.sourceOffset >= capturedFrames) return result;
    const int64_t available = capturedFrames - result.sourceOffset;
    const int64_t wanted = out - in;
    result.length = std::min(wanted, available);
    result.clampedToCapture = result.clampedToCapture || result.length < wanted;
    return result;
}

std::vector<PunchRange> closePunches(std::vector<PunchRange> punches,
                                     int64_t stopSample) {
    for (auto& punch : punches) {
        if (punch.open()) punch.out = std::max(punch.in, stopSample);
    }
    // A double-tap on Record leaves an in and an out at the same sample. It is
    // a cancelled gesture, not a zero-length region.
    punches.erase(std::remove_if(punches.begin(), punches.end(),
                                 [](const PunchRange& punch) {
                                     return punch.empty();
                                 }),
                  punches.end());
    return punches;
}

} // namespace dave::application
