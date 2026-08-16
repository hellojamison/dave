// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace dave::application {

// Punch recording, the way a desk does it: capture runs for as long as the
// transport rolls, and pressing Record decides which parts of it become
// regions on the timeline.
//
// The two are separate on purpose. Deciding to keep a take a bar after it
// started is the normal case in tracking, and it is only possible if the
// audio was already going to disk before anyone pressed anything.

// One in/out pair against the timeline. `out` is exclusive; while a punch is
// still open it is kOpen and the transport stopping closes it.
struct PunchRange {
    static constexpr int64_t kOpen = -1;
    int64_t in = 0;
    int64_t out = kOpen;

    bool open() const { return out == kOpen; }
    bool empty() const { return !open() && out <= in; }
};

// Where a punch lands on the timeline and which part of the take file it
// takes. The take covers [captureStart, captureStart + capturedFrames).
struct PunchClipRange {
    int64_t timelineStart = 0;
    int64_t sourceOffset = 0;
    int64_t length = 0;
    // The punch asked for audio the capture does not contain — the transport
    // stopped before the punch closed, or the writer dropped the tail.
    bool clampedToCapture = false;
};

// The clip one punch produces.
//
// `latencyOffsetSamples` moves the region EARLIER on the timeline, because
// captured audio arrives that much after the sound that caused it. Where the
// shift would push the region before zero it is absorbed into the source
// offset instead, so the audio still lines up with what the performer heard.
PunchClipRange punchClipRange(int64_t captureStartSample,
                              int64_t capturedFrames,
                              const PunchRange& punch,
                              int64_t latencyOffsetSamples);

// Close any still-open punch at `stopSample` and drop the ones that captured
// nothing — a double-tap on Record leaves a zero-length punch that should not
// become a clip.
std::vector<PunchRange> closePunches(std::vector<PunchRange> punches,
                                     int64_t stopSample);

} // namespace dave::application
