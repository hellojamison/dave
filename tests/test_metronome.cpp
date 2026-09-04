// SPDX-License-Identifier: GPL-3.0-or-later
//
// The metronome node: it must emit a click exactly on beats (following the
// tempo/meter map) and stay silent when disabled, stopped, or between beats.
// The audible thing is the output energy, so that is what these assert.
#include "document/MusicalTime.h"
#include "engine/nodes/MetronomeNode.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace dave;

namespace {
// A fresh node each call so a click from one scenario can't ring into the next.
double blockEnergy(int64_t pos, bool enabled, bool playing) {
    engine::MetronomeNode node;
    node.configure(48000.0, document::constantTempo(120.0),
                   {document::TimeSignature{1, 4, 4}});
    node.setGain(0.5f);
    node.setEnabled(enabled);
    std::vector<float> outL(512, 0.0f);
    std::vector<float> outR(512, 0.0f);
    float* out[2] = {outL.data(), outR.data()};
    engine::TimeInfo time{};
    time.samplePos = pos;
    time.isPlaying = playing;
    engine::NodeProcessContext ctx;
    ctx.numSamples = 512;
    ctx.time = &time;
    ctx.output = engine::AudioBus{out, 2, 512};
    node.process(ctx);
    double energy = 0.0;
    for (float v : outL) energy += std::fabs(v);
    return energy;
}
}  // namespace

TEST_CASE("the metronome clicks on beats and is silent otherwise",
          "[metronome]") {
    // 120 bpm -> a beat every 24000 samples; the downbeat sits at sample 0.
    CHECK(blockEnergy(0, true, true) > 0.0);       // click on the downbeat
    CHECK(blockEnergy(24000, true, true) > 0.0);   // click on beat 2
    CHECK(blockEnergy(2000, true, true) == 0.0);   // between beats -> silent
    CHECK(blockEnergy(0, false, true) == 0.0);     // disabled -> silent
    CHECK(blockEnergy(0, true, false) == 0.0);     // stopped -> silent
}
