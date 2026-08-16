// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/Edit.h"
#include "engine/GraphBuilder.h"
#include "engine/graph/Graph.h"
#include "engine/plugins/PluginHost.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
// Main is a row in the one track list now, so a test asking "how many tracks
// did I make?" has to say so. Counting user rows keeps the intent visible
// rather than burying a +1 in every expectation.
inline size_t userTracks(const dave::document::Edit& e) {
    size_t n = 0;
    for (const auto& t : e.tracks()) if (!t.isMain) ++n;
    return n;
}
} // namespace

using namespace dave;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;

document::MidiClip chordClip(int64_t timelineStart, int64_t length) {
    document::MidiClip c;
    c.timelineStart = timelineStart;
    c.length = length;
    for (uint8_t pitch : {uint8_t(60), uint8_t(64), uint8_t(67)}) {
        document::MidiNote n;
        n.startSample = 0;
        n.lengthSamples = length / 2;
        n.pitch = pitch;
        n.velocity = 110;
        c.notes.push_back(n);
    }
    return c;
}

// Counts nodes of a given typeName in a built graph, which is how these tests
// assert on the derived topology without reaching into CompiledGraph.
int countNodes(const engine::Graph& g, const std::string& typeName) {
    int count = 0;
    for (const auto& [id, node] : g.nodes()) {
        if (node->typeName() == typeName) ++count;
    }
    return count;
}

} // namespace

TEST_CASE("a MIDI track gets a gain node and a master pin", "[midigraph]") {
    document::Edit edit;
    edit.addTrack("Audio 1");
    const std::string midi = edit.addMidiTrack("Keys");

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, kSampleRate);
    REQUIRE(graph != nullptr);

    // Both tracks plus the permanent Main bus have gain nodes. A MIDI track
    // differs only in what feeds its input sum.
    CHECK(builder.trackGains().size() == 3);
    CHECK(builder.trackGains().count(midi) == 1);

    auto [compiled, err] = engine::compile(*graph, kSampleRate, kBlock);
    INFO("compile: " << (err ? err->message : std::string{}));
    REQUIRE(compiled != nullptr);
}

TEST_CASE("a MIDI track with no instrument still compiles and is silent",
          "[midigraph]") {
    // Importing a .mid and only then picking a synth is the normal order of
    // operations. The track must exist, compile, and produce silence — not
    // vanish from the graph or take the derive down with it.
    document::Edit edit;
    const std::string midi = edit.addMidiTrack("Keys");
    edit.addMidiClip(midi, chordClip(0, 48000));

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, kSampleRate);
    REQUIRE(graph != nullptr);
    CHECK(builder.instrumentNodes().empty());   // nothing to instantiate
    CHECK(builder.trackGains().count(midi) == 1);

    auto [compiled, err] = engine::compile(*graph, kSampleRate, kBlock);
    INFO("compile: " << (err ? err->message : std::string{}));
    REQUIRE(compiled != nullptr);

    std::vector<float> left(kBlock, 1.0f), right(kBlock, 1.0f);
    float* channels[2] = {left.data(), right.data()};
    engine::AudioBus out{channels, 2, kBlock};
    engine::TimeInfo time;
    time.sampleRate = kSampleRate;
    time.isPlaying = true;
    compiled->process(out, time);

    for (int i = 0; i < kBlock; ++i) {
        REQUIRE(left[i] == 0.0f);
        REQUIRE(right[i] == 0.0f);
    }
}

TEST_CASE("audio and MIDI tracks route independently into Main", "[midigraph]") {
    document::Edit edit;
    edit.addTrack("A1");
    edit.addTrack("A2");
    edit.addMidiTrack("M1");
    edit.addMidiTrack("M2");

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, kSampleRate);
    REQUIRE(graph != nullptr);
    CHECK(builder.trackGains().size() == 5);

    const auto mainGain = builder.trackGains().at(document::kMainBusId);
    engine::NodeId mainGainId = 0;
    for (const auto& [id, node] : graph->nodes()) {
        if (node == mainGain) mainGainId = id;
    }
    REQUIRE(mainGainId != 0);
    int sourcesIntoMain = 0;
    for (const auto& e : graph->edges()) {
        const auto* source = graph->node(e.srcNode);
        const auto* destination = graph->node(e.dstNode);
        if (source && source->typeName() == "gain" && destination &&
            destination->typeName() == "sum" && e.dstPin == 0) {
            ++sourcesIntoMain;
        }
    }
    CHECK(sourcesIntoMain >= 4);

    auto [compiled, err] = engine::compile(*graph, kSampleRate, kBlock);
    INFO("compile: " << (err ? err->message : std::string{}));
    REQUIRE(compiled != nullptr);
}

TEST_CASE("muting a MIDI track zeroes its gain", "[midigraph]") {
    document::Edit edit;
    const std::string midi = edit.addMidiTrack("Keys");
    edit.track(midi)->gain = 0.9;

    engine::GraphBuilder builder;
    builder.build(edit, kSampleRate);
    CHECK(builder.trackGains().at(midi)->gain() == 0.9);

    edit.track(midi)->mute = true;
    builder.build(edit, kSampleRate);
    CHECK(builder.trackGains().at(midi)->gain() == 0.0);
}

TEST_CASE("soloing an audio track silences MIDI tracks too", "[midigraph]") {
    // Solo is global. If the MIDI side were scanned separately, soloing a vocal
    // would leave every synth playing underneath it.
    document::Edit edit;
    const std::string audio = edit.addTrack("Vox");
    const std::string midi = edit.addMidiTrack("Keys");
    edit.track(audio)->solo = true;

    engine::GraphBuilder builder;
    builder.build(edit, kSampleRate);
    CHECK(builder.trackGains().at(audio)->gain() > 0.0);
    CHECK(builder.trackGains().at(midi)->gain() == 0.0);
}

TEST_CASE("a bad instrument path leaves the track silent rather than failing",
          "[midigraph]") {
    document::Edit edit;
    const std::string midi = edit.addMidiTrack("Keys");
    document::PluginSlot broken;
    broken.name = "Nonexistent Synth";
    broken.uidString = "00000000000000000000000000000000";
    broken.path = "/nowhere/NoSuchPlugin.vst3";
    edit.setMidiInstrument(midi, broken);
    edit.addMidiClip(midi, chordClip(0, 48000));

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, kSampleRate);
    REQUIRE(graph != nullptr);
    CHECK(countNodes(*graph, "instrument") == 0);
    CHECK(builder.trackGains().count(midi) == 1);

    auto [compiled, err] = engine::compile(*graph, kSampleRate, kBlock);
    INFO("compile: " << (err ? err->message : std::string{}));
    REQUIRE(compiled != nullptr);
}

// ─── Real-plugin render ─────────────────────────────────────────────────────

TEST_CASE("a real VST3 instrument renders sound for the notes it is sent",
          "[midigraph][plugin]") {
    // Gated: this needs an actual synth installed, which no CI box and few
    // dev machines have. Set DAVE_TEST_INSTRUMENT to a substring of the
    // plugin's name (e.g. DAVE_TEST_INSTRUMENT="Surge XT") to run it.
    const char* wanted = std::getenv("DAVE_TEST_INSTRUMENT");
    if (wanted == nullptr || *wanted == '\0') {
        SKIP("set DAVE_TEST_INSTRUMENT=<plugin name substring> to run this");
    }

    engine::PluginHost host;
    const auto descriptors = host.scan();
    const engine::PluginDescriptor* found = nullptr;
    for (const auto& d : descriptors) {
        if (d.isInstrument && d.name.find(wanted) != std::string::npos) {
            found = &d;
            break;
        }
    }
    if (found == nullptr) {
        SKIP(std::string("no instrument matching '") + wanted + "' in " +
             std::to_string(descriptors.size()) + " scanned plugins");
    }

    document::Edit edit;
    const std::string midi = edit.addMidiTrack("Keys");
    document::PluginSlot slot;
    slot.name = found->name;
    slot.uidString = found->uidString;
    slot.path = found->path;
    edit.setMidiInstrument(midi, slot);
    edit.addMidiClip(midi, chordClip(0, 48000));

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, kSampleRate);
    REQUIRE(graph != nullptr);
    REQUIRE(builder.instrumentNodes().count(midi) == 1);

    auto [compiled, err] = engine::compile(*graph, kSampleRate, kBlock);
    INFO("compile: " << (err ? err->message : std::string{}));
    REQUIRE(compiled != nullptr);

    std::vector<float> left(kBlock), right(kBlock);
    float* channels[2] = {left.data(), right.data()};
    engine::AudioBus out{channels, 2, kBlock};

    // Sum energy over the note span and over a stretch well after the release.
    // Comparing the two is the assertion that matters: "loud during, quiet
    // after" is what "the synth is being driven and then released" sounds like,
    // and neither number alone would show it.
    double duringEnergy = 0.0;
    double afterEnergy = 0.0;
    const int64_t noteEnd = 24000;    // chordClip: notes run half the clip
    const int64_t tailStart = 96000;  // well past the release
    for (int64_t pos = 0; pos < tailStart + 48000; pos += kBlock) {
        engine::TimeInfo time;
        time.sampleRate = kSampleRate;
        time.samplePos = pos;
        time.isPlaying = true;
        compiled->process(out, time);
        double blockEnergy = 0.0;
        for (int i = 0; i < kBlock; ++i) {
            blockEnergy += double(left[i]) * left[i] + double(right[i]) * right[i];
        }
        if (pos + kBlock <= noteEnd) duringEnergy += blockEnergy;
        else if (pos >= tailStart) afterEnergy += blockEnergy;
    }

    INFO("during=" << duringEnergy << " after=" << afterEnergy);
    CHECK(duringEnergy > 0.0);
    // A release tail can ring for a while, so this is a ratio, not silence:
    // what it rules out is the note never being released at all.
    CHECK(afterEnergy < duringEnergy * 0.01);
}
