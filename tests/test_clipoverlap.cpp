// SPDX-License-Identifier: GPL-3.0-or-later
//
// Overlapping clips on a track must not sum: the clip painted on top (later in
// the vector) wins its overlap, the one beneath falls silent there. Two levels
// are tested — the pure interval math, and that AudioClipNode actually zeroes
// the masked samples rather than mixing them.
#include "audio/DecodedAudioAsset.h"
#include "document/ClipOverlap.h"
#include "document/Types.h"
#include "engine/nodes/AudioClipNode.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using namespace dave;

TEST_CASE("a clip is masked where a later clip overlaps it", "[clipoverlap]") {
    std::vector<document::AudioClip> clips(2);
    clips[0].timelineStart = 0;
    clips[0].length = 1000;
    clips[1].timelineStart = 600;  // laps the tail of clip 0
    clips[1].length = 1000;

    // Clip 0 (below) is masked where clip 1 covers it...
    const auto m0 = document::clipMuteIntervals(clips, 0);
    REQUIRE(m0.size() == 1);
    CHECK(m0[0].first == 600);
    CHECK(m0[0].second == 1000);
    // ...and clip 1 (on top) is masked by nothing.
    CHECK(document::clipMuteIntervals(clips, 1).empty());
}

TEST_CASE("an interior clip masks only the span it covers", "[clipoverlap]") {
    std::vector<document::AudioClip> clips(2);
    clips[0].timelineStart = 0;
    clips[0].length = 1000;
    clips[1].timelineStart = 300;  // sits entirely inside clip 0
    clips[1].length = 200;         // covers [300, 500)

    const auto m0 = document::clipMuteIntervals(clips, 0);
    REQUIRE(m0.size() == 1);
    CHECK(m0[0].first == 300);
    CHECK(m0[0].second == 500);
}

TEST_CASE("the audio node falls silent where it is masked", "[clipoverlap]") {
    auto asset = std::make_shared<audio::DecodedAudioAsset>();
    asset->channels = {std::vector<float>(1000, 0.5f),
                       std::vector<float>(1000, 0.5f)};
    engine::AudioClipNode node;
    node.setBuffer(asset);
    node.setStart(0);
    node.setLength(1000);
    node.setFades(0, 0);  // isolate the mask from the de-click fades
    node.setMuteIntervals({{600, 1000}});

    // The graph hands each clip a cleared buffer; masked samples are simply not
    // written, so they stay at zero.
    std::vector<float> outL(1000, 0.0f);
    std::vector<float> outR(1000, 0.0f);
    float* out[2] = {outL.data(), outR.data()};
    engine::TimeInfo time{};
    time.samplePos = 0;
    time.isPlaying = true;
    engine::NodeProcessContext ctx;
    ctx.numSamples = 1000;
    ctx.time = &time;
    ctx.output = engine::AudioBus{out, 2, 1000};
    node.process(ctx);

    // Before the overlap it plays at full level...
    CHECK(outL[0] == 0.5f);
    CHECK(outL[599] == 0.5f);
    // ...and across the overlap it is silent, leaving that span to the clip on
    // top rather than doubling it.
    CHECK(outL[600] == 0.0f);
    CHECK(outL[999] == 0.0f);
    CHECK(outR[800] == 0.0f);
}

TEST_CASE("per-clip gain and mute reach the output", "[clipoverlap][clipgain]") {
    auto asset = std::make_shared<audio::DecodedAudioAsset>();
    asset->channels = {std::vector<float>(100, 1.0f)};  // mono, unity samples
    engine::TimeInfo time{};
    time.isPlaying = true;
    auto levelAt50 = [&](float gain, bool muted) {
        engine::AudioClipNode node;
        node.setBuffer(asset);
        node.setStart(0);
        node.setLength(100);
        node.setFades(0, 0);
        node.setGain(gain);
        node.setMuted(muted);
        std::vector<float> outL(100, 0.0f);
        std::vector<float> outR(100, 0.0f);
        float* out[2] = {outL.data(), outR.data()};
        engine::NodeProcessContext ctx;
        ctx.numSamples = 100;
        ctx.time = &time;
        ctx.output = engine::AudioBus{out, 2, 100};
        node.process(ctx);
        return outL[50];
    };
    CHECK(levelAt50(1.0f, false) == 1.0f);   // unity
    CHECK(levelAt50(0.5f, false) == 0.5f);   // half gain scales the sample
    CHECK(levelAt50(1.0f, true) == 0.0f);    // muted is silent regardless
}

TEST_CASE("a crossfade lifts the overlap mute", "[clipoverlap][crossfade]") {
    std::vector<document::AudioClip> clips(2);
    clips[0].timelineStart = 0;
    clips[0].length = 10000;
    clips[1].timelineStart = 8000;   // overlap [8000, 10000)
    clips[1].length = 10000;

    // Without complementary fades the lower clip is muted across the overlap.
    REQUIRE(document::clipMuteIntervals(clips, 0).size() == 1);

    // Add a crossfade — A fades out into the overlap, B fades in across it —
    // and the mute stands down so both play.
    clips[0].fadeOut = 2000;
    clips[1].fadeIn = 2000;
    CHECK(document::clipMuteIntervals(clips, 0).empty());
}
