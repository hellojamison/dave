// SPDX-License-Identifier: GPL-3.0-or-later
//
// Punch recording: capture runs for as long as the transport rolls, and
// pressing Record decides which parts of it become regions.
//
// The two being separate is the whole point — deciding to keep a take a bar
// after it started is the normal case in tracking, and it only works if the
// audio was already going to disk before anyone pressed anything. What has to
// be right is the arithmetic that maps a punch on the timeline onto a slice of
// a file that started earlier.
#include "application/PunchRecording.h"

#include <catch2/catch_test_macros.hpp>

using dave::application::PunchClipRange;
using dave::application::PunchRange;
using dave::application::closePunches;
using dave::application::punchClipRange;

TEST_CASE("a punch takes the slice of the capture under it", "[punch]") {
    // Capture ran from sample 1000 for 10,000 frames; Record was pressed at
    // 3000 and again at 5000.
    const auto range = punchClipRange(1000, 10000, PunchRange{3000, 5000}, 0);
    CHECK(range.timelineStart == 3000);
    // 2000 frames into the file, which is where 3000 falls in a capture that
    // began at 1000 — not at the head of the file.
    CHECK(range.sourceOffset == 2000);
    CHECK(range.length == 2000);
    CHECK_FALSE(range.clampedToCapture);
}

TEST_CASE("latency moves the region earlier, not the audio later", "[punch]") {
    // Captured audio arrives after the sound that caused it, so the region
    // slides back by the offset while taking the same samples.
    const auto range = punchClipRange(1000, 10000, PunchRange{3000, 5000}, 500);
    CHECK(range.timelineStart == 2500);
    CHECK(range.sourceOffset == 2000);
    CHECK(range.length == 2000);
}

TEST_CASE("a latency shift past zero is absorbed by the source", "[punch]") {
    // There is no timeline before zero to slide into, so the remainder comes
    // off the front of the audio instead and the region stays put. Sliding it
    // to zero and keeping the audio would misalign the take by the overshoot.
    const auto range = punchClipRange(0, 10000, PunchRange{200, 5000}, 500);
    CHECK(range.timelineStart == 0);
    CHECK(range.sourceOffset == 500);   // 200 into the file, plus 300 overshoot
    CHECK(range.length == 4800);
}

TEST_CASE("a punch is clamped to what was actually captured", "[punch]") {
    // The transport stopped, or the writer dropped the tail: the region must
    // not point at audio the file does not contain.
    const auto tail = punchClipRange(1000, 4000, PunchRange{3000, 9000}, 0);
    CHECK(tail.timelineStart == 3000);
    CHECK(tail.sourceOffset == 2000);
    CHECK(tail.length == 2000);        // capture ends at 5000, not 9000
    CHECK(tail.clampedToCapture);

    // Entirely outside the capture is no clip at all.
    const auto outside = punchClipRange(1000, 4000, PunchRange{8000, 9000}, 0);
    CHECK(outside.length == 0);
}

TEST_CASE("an open punch and an empty capture produce nothing", "[punch]") {
    CHECK(punchClipRange(0, 10000, PunchRange{100, PunchRange::kOpen}, 0)
              .length == 0);
    CHECK(punchClipRange(0, 0, PunchRange{100, 200}, 0).length == 0);
}

TEST_CASE("stopping closes an open punch", "[punch]") {
    std::vector<PunchRange> punches{{1000, 2000}, {4000, PunchRange::kOpen}};
    const auto closed = closePunches(punches, 6000);
    REQUIRE(closed.size() == 2);
    CHECK(closed[0].out == 2000);
    CHECK(closed[1].out == 6000);
}

TEST_CASE("a double-tap on Record leaves no region", "[punch]") {
    // In and out at the same sample is a cancelled gesture, not a
    // zero-length clip on the timeline.
    std::vector<PunchRange> punches{{1000, 1000}, {2000, 3000}};
    const auto closed = closePunches(punches, 5000);
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].in == 2000);

    // Including one that was open when the transport stopped at its own start.
    const auto instant =
        closePunches({{4000, PunchRange::kOpen}}, 4000);
    CHECK(instant.empty());
}

TEST_CASE("several punches in one pass each become their own region",
          "[punch]") {
    // The reason capture and regions are separate: one continuous file, three
    // takes cut out of it.
    const std::vector<PunchRange> punches{
        {2000, 3000}, {5000, 6000}, {8000, 9000}};
    int64_t previousEnd = 0;
    for (const auto& punch : punches) {
        const auto range = punchClipRange(1000, 12000, punch, 0);
        CHECK(range.length == 1000);
        CHECK(range.timelineStart == punch.in);
        CHECK(range.sourceOffset == punch.in - 1000);
        CHECK(range.timelineStart >= previousEnd);
        previousEnd = range.timelineStart + range.length;
    }
}
