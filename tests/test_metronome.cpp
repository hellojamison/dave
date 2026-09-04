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

TEST_CASE("the metronome's accent and gain settings shape the click",
          "[metronome]") {
    // Render one block from a fresh node under the given settings and
    // return the dominant frequency estimate (zero crossings) and energy.
    auto render = [](bool accent, float gain, int& crossings) {
        engine::MetronomeNode node;
        node.configure(48000.0, document::constantTempo(120.0),
                       {document::TimeSignature{1, 4, 4}});
        node.setEnabled(true);
        node.setGain(gain);
        node.setAccent(accent);
        std::vector<float> outL(512, 0.0f), outR(512, 0.0f);
        float* out[2] = {outL.data(), outR.data()};
        engine::TimeInfo time{};
        time.samplePos = 0;  // the downbeat
        time.isPlaying = true;
        engine::NodeProcessContext ctx;
        ctx.numSamples = 512;
        ctx.time = &time;
        ctx.output = engine::AudioBus{out, 2, 512};
        node.process(ctx);
        crossings = 0;
        double energy = 0.0;
        for (int i = 1; i < 512; ++i) {
            if ((outL[i - 1] < 0.0f) != (outL[i] < 0.0f)) ++crossings;
            energy += std::fabs(outL[i]);
        }
        return energy;
    };
    int accented = 0, plain = 0, quiet = 0;
    const double loud = render(true, 0.5f, accented);
    render(false, 0.5f, plain);
    const double soft = render(true, 0.05f, quiet);
    // The accented downbeat rings higher (G6) than an un-accented one (B5)...
    CHECK(accented > plain);
    // ...and gain scales the click linearly.
    CHECK(soft < loud * 0.2);
}

TEST_CASE("eighth-note subdivision adds a softer tick between beats",
          "[metronome]") {
    // 120 bpm: beats at 0 and 24000, so the off-eighth sits at 12000.
    auto energyAt = [](int64_t pos, bool eighths) {
        engine::MetronomeNode node;
        node.configure(48000.0, document::constantTempo(120.0),
                       {document::TimeSignature{1, 4, 4}});
        node.setEnabled(true);
        node.setGain(0.5f);
        node.setSubdivide(eighths);
        std::vector<float> outL(512, 0.0f), outR(512, 0.0f);
        float* out[2] = {outL.data(), outR.data()};
        engine::TimeInfo time{};
        time.samplePos = pos;
        time.isPlaying = true;
        engine::NodeProcessContext ctx;
        ctx.numSamples = 512;
        ctx.time = &time;
        ctx.output = engine::AudioBus{out, 2, 512};
        node.process(ctx);
        double energy = 0.0;
        for (float v : outL) energy += std::fabs(v);
        return energy;
    };
    CHECK(energyAt(12000, false) == 0.0);
    const double sub = energyAt(12000, true);
    CHECK(sub > 0.0);
    // Softer than the beat it sits between.
    CHECK(sub < energyAt(24000, true));
}
