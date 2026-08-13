// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/Types.h"

#include <catch2/catch_test_macros.hpp>

using dave::document::Track;
using dave::document::trackAudible;

// Mute and solo interact, and getting the interaction wrong is the kind of bug
// that presents as "the mix went silent and I don't know why". The rule is
// asserted directly so the GUI's dimming and the engine's gain-zeroing are
// known to agree — both call this.
TEST_CASE("nothing soloed: only muted tracks are silent", "[mixer]") {
    CHECK(trackAudible(/*mute*/ false, /*solo*/ false, /*anySoloed*/ false));
    CHECK_FALSE(trackAudible(true, false, false));
}

TEST_CASE("something soloed: non-soloed tracks are silent", "[mixer]") {
    CHECK(trackAudible(false, true, true));
    CHECK_FALSE(trackAudible(false, false, true));
}

TEST_CASE("mute beats solo on the same track", "[mixer]") {
    // Otherwise a soloed track becomes impossible to mute, which is both
    // surprising and a dead end for the user.
    CHECK_FALSE(trackAudible(true, true, true));
    CHECK_FALSE(trackAudible(true, true, false));
}

TEST_CASE("a track's own solo flag is ignored when nothing is soloed",
          "[mixer]") {
    // anySoloed is computed across the edit; a stale solo flag with
    // anySoloed=false must not change audibility.
    CHECK(trackAudible(false, true, false));
}

TEST_CASE("the Track overload agrees with the raw one", "[mixer]") {
    Track t;
    t.mute = true;
    t.solo = false;
    CHECK(trackAudible(t, false) == trackAudible(true, false, false));
    CHECK_FALSE(trackAudible(t, false));

    Track audible;
    CHECK(trackAudible(audible, false));
}

TEST_CASE("tracks default to audible", "[mixer]") {
    // A newly added track that starts silent would be a miserable first
    // experience, and a defaulted field is easy to get backwards.
    Track t;
    CHECK_FALSE(t.mute);
    CHECK_FALSE(t.solo);
    CHECK(trackAudible(t, false));
}

TEST_CASE("audio tracks default to a safe mono input and are not armed",
          "[mixer][record-arm]") {
    Track t;
    CHECK_FALSE(t.recordArm);
    CHECK(t.inputChannel == 0);
    CHECK(t.inputChannelCount == 1);
}

TEST_CASE("saved track inputs clamp to a supplied live capture width",
          "[mixer][record-arm]") {
    using dave::document::clampTrackInputToCaptureChannels;
    Track t;
    t.inputChannel = 7;
    t.inputChannelCount = 4;

    CHECK(clampTrackInputToCaptureChannels(t, 2));
    CHECK(t.inputChannel == 0);
    CHECK(t.inputChannelCount == 2);
    CHECK_FALSE(clampTrackInputToCaptureChannels(t, 2));

    CHECK(clampTrackInputToCaptureChannels(t, 0));
    CHECK(t.inputChannel == 0);
    CHECK(t.inputChannelCount == 0);

    // A route repaired after the input returns always selects at least mono.
    CHECK(clampTrackInputToCaptureChannels(t, 8));
    CHECK(t.inputChannel == 0);
    CHECK(t.inputChannelCount == 1);
}
