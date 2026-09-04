// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fallback sine synth: a MIDI track with no instrument still sounds. It
// must be audible while a note is held and silent when nothing is, and the
// graph must reach for it whenever a track has notes but no instrument.
#include "document/Edit.h"
#include "engine/GraphBuilder.h"
#include "engine/nodes/SineSynthNode.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace dave;

namespace {
double energy(engine::SineSynthNode& node, int64_t pos, int n) {
    std::vector<float> outL(static_cast<size_t>(n), 0.0f);
    std::vector<float> outR(static_cast<size_t>(n), 0.0f);
    float* out[2] = {outL.data(), outR.data()};
    engine::TimeInfo time{};
    time.samplePos = pos;
    time.isPlaying = true;
    engine::NodeProcessContext ctx;
    ctx.numSamples = n;
    ctx.time = &time;
    ctx.output = engine::AudioBus{out, 2, n};
    node.process(ctx);
    double e = 0.0;
    for (float v : outL) e += std::fabs(v);
    return e;
}
}  // namespace

TEST_CASE("the sine synth sounds while a note is held and not otherwise",
          "[midi][sinesynth]") {
    document::MidiClip clip;
    clip.timelineStart = 0;
    clip.length = 48000;
    document::MidiNote note;
    note.startSample = 1000;
    note.lengthSamples = 10000;
    note.pitch = 69;  // A4
    note.velocity = 100;
    clip.notes.push_back(note);

    engine::SineSynthNode node;
    node.setSequence(engine::bakeClips({clip}));
    node.prepare(48000.0, 512);

    // Before the note: silence. Inside it: sound.
    CHECK(energy(node, 0, 512) == 0.0);
    CHECK(energy(node, 2048, 512) > 1.0);
    // Jumping past the note's end releases it (the jump flushes the held
    // note); the release tail then decays to nothing over the following
    // contiguous blocks rather than ringing on.
    double tail = 0.0;
    for (int b = 0; b < 120; ++b) tail = energy(node, 40000 + b * 512, 512);
    CHECK(tail < 1e-3);
    CHECK(node.activeNoteCount() == 0);
}

TEST_CASE("a track with notes and no instrument gets the sine synth",
          "[midi][sinesynth][graph]") {
    document::Edit edit;
    const std::string t = edit.addTrack("Keys");
    document::MidiClip clip;
    clip.length = 48000;
    document::MidiNote note;
    note.lengthSamples = 4800;
    clip.notes.push_back(note);
    edit.addMidiClip(t, clip);

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, 48000.0, 2);
    REQUIRE(graph != nullptr);
    const auto& instruments = builder.instrumentNodes();
    REQUIRE(instruments.count(t) == 1);
    CHECK(std::dynamic_pointer_cast<engine::SineSynthNode>(
              instruments.at(t)) != nullptr);
}
