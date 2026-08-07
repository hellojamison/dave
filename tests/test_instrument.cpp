// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/Types.h"
#include "engine/nodes/InstrumentNode.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace dave;
using engine::BakedNote;
using engine::InstrumentNode;
using engine::MidiEvent;
using engine::MidiEventType;
using engine::NoteSequence;

namespace {

document::MidiNote srcNote(int64_t start, int64_t len, uint8_t pitch,
                           uint8_t vel = 100, uint8_t chan = 0) {
    document::MidiNote n;
    n.startSample = start;
    n.lengthSamples = len;
    n.pitch = pitch;
    n.velocity = vel;
    n.channel = chan;
    return n;
}

NoteSequence sequenceOf(std::vector<BakedNote> notes) {
    document::MidiClip clip;
    clip.length = 0;   // untrimmed: the clip is the whole sequence
    for (const auto& n : notes) {
        clip.notes.push_back(srcNote(n.start, n.end - n.start, n.pitch,
                                     n.velocity, n.channel));
    }
    return engine::bakeClips({clip});
}

// Drives an InstrumentNode block by block and records what it emitted, so a
// test can talk about a whole playback pass instead of one call.
struct Rig {
    InstrumentNode node;
    std::vector<MidiEvent> buffer{InstrumentNode::kMaxEventsPerBlock};

    explicit Rig(NoteSequence seq) {
        node.setSequence(std::move(seq));
        // The first block after construction/prepare flushes every pitch on
        // every used channel, because a cached plugin instance may still be
        // holding notes from the graph this node replaced. Consume that while
        // stopped so the tests below see only real note traffic.
        run(0, 64, false);
    }

    std::vector<MidiEvent> run(int64_t blockStart, int numSamples, bool playing) {
        const int n = node.eventsForBlock(blockStart, numSamples, playing,
                                          buffer.data(),
                                          static_cast<int>(buffer.size()));
        return std::vector<MidiEvent>(buffer.begin(), buffer.begin() + n);
    }
};

int countOf(const std::vector<MidiEvent>& events, MidiEventType type) {
    return static_cast<int>(std::count_if(
        events.begin(), events.end(),
        [type](const MidiEvent& e) { return e.type == type; }));
}

bool has(const std::vector<MidiEvent>& events, MidiEventType type, uint8_t pitch,
         int32_t offset) {
    for (const auto& e : events) {
        if (e.type == type && e.pitch == pitch && e.sampleOffset == offset) {
            return true;
        }
    }
    return false;
}

} // namespace

// ─── Baking ─────────────────────────────────────────────────────────────────

TEST_CASE("baking places notes at the clip's timeline position", "[instrument]") {
    document::MidiClip clip;
    clip.timelineStart = 48000;
    clip.length = 96000;
    clip.notes = {srcNote(0, 1000, 60), srcNote(24000, 1000, 64)};

    std::vector<BakedNote> baked;
    engine::bakeClip(clip, baked);

    REQUIRE(baked.size() == 2);
    CHECK(baked[0].start == 48000);
    CHECK(baked[0].end == 49000);
    CHECK(baked[1].start == 72000);
    CHECK(baked[1].pitch == 64);
}

TEST_CASE("a head trim shifts the source window, not the notes", "[instrument]") {
    // The clip shows [24000, 72000) of the source at timeline 0, so a source
    // note at 24000 sounds at timeline 0 — the same arithmetic AudioClipNode
    // uses for sourceOffset.
    document::MidiClip clip;
    clip.timelineStart = 0;
    clip.sourceOffset = 24000;
    clip.length = 48000;
    clip.notes = {srcNote(0, 1000, 60),        // before the window: silent
                  srcNote(24000, 1000, 64),    // at the window start
                  srcNote(60000, 1000, 67)};   // inside

    std::vector<BakedNote> baked;
    engine::bakeClip(clip, baked);

    REQUIRE(baked.size() == 2);
    CHECK(baked[0].pitch == 64);
    CHECK(baked[0].start == 0);
    CHECK(baked[1].pitch == 67);
    CHECK(baked[1].start == 36000);
}

TEST_CASE("a note running past the clip's tail is cut off there", "[instrument]") {
    document::MidiClip clip;
    clip.timelineStart = 0;
    clip.length = 48000;
    clip.notes = {srcNote(24000, 96000, 60)};   // runs well past the clip end

    std::vector<BakedNote> baked;
    engine::bakeClip(clip, baked);

    REQUIRE(baked.size() == 1);
    // Shortened, not dropped: trimming a clip mid-note shortens the note the
    // same way trimming audio shortens the sound.
    CHECK(baked[0].start == 24000);
    CHECK(baked[0].end == 48000);
}

TEST_CASE("baking sorts by start and indexes by end", "[instrument]") {
    // Deliberately reversed, and with a long note that starts first but ends
    // last, so start order and end order genuinely differ.
    document::MidiClip clip;
    clip.notes = {srcNote(48000, 1000, 67), srcNote(0, 96000, 60),
                  srcNote(24000, 1000, 64)};

    NoteSequence seq = engine::bakeClips({clip});
    REQUIRE(seq.notes.size() == 3);
    CHECK(seq.notes[0].start == 0);
    CHECK(seq.notes[1].start == 24000);
    CHECK(seq.notes[2].start == 48000);

    REQUIRE(seq.byEnd.size() == 3);
    CHECK(seq.notes[seq.byEnd[0]].end == 25000);
    CHECK(seq.notes[seq.byEnd[2]].end == 96000);
    CHECK(seq.maxNoteLength == 96000);
    CHECK(seq.usedChannels == 0x0001);
}

// ─── sliceBlock ─────────────────────────────────────────────────────────────

TEST_CASE("a note gets a block-relative offset", "[instrument][slice]") {
    NoteSequence seq = sequenceOf({{100, 200, 60, 100, 0}});
    MidiEvent out[8];

    // Block [0, 512): the note-on lands at offset 100, the note-off at 200.
    const int n = InstrumentNode::sliceBlock(seq, 0, 512, out, 8);
    REQUIRE(n == 2);
    CHECK(out[0].type == MidiEventType::NoteOn);
    CHECK(out[0].sampleOffset == 100);
    CHECK(out[1].type == MidiEventType::NoteOff);
    CHECK(out[1].sampleOffset == 200);
}

TEST_CASE("a note spanning a block edge splits across the two blocks",
          "[instrument][slice]") {
    NoteSequence seq = sequenceOf({{400, 700, 60, 100, 0}});
    MidiEvent out[8];

    int n = InstrumentNode::sliceBlock(seq, 0, 512, out, 8);
    REQUIRE(n == 1);
    CHECK(out[0].type == MidiEventType::NoteOn);
    CHECK(out[0].sampleOffset == 400);

    n = InstrumentNode::sliceBlock(seq, 512, 512, out, 8);
    REQUIRE(n == 1);
    CHECK(out[0].type == MidiEventType::NoteOff);
    // 700 is 188 samples into the second block — an absolute offset here would
    // put the release 700 samples late and out of the block entirely.
    CHECK(out[0].sampleOffset == 188);
}

TEST_CASE("a note ending exactly on the block boundary belongs to the next block",
          "[instrument][slice]") {
    NoteSequence seq = sequenceOf({{0, 512, 60, 100, 0}});
    MidiEvent out[8];

    int n = InstrumentNode::sliceBlock(seq, 0, 512, out, 8);
    REQUIRE(n == 1);
    CHECK(out[0].type == MidiEventType::NoteOn);

    n = InstrumentNode::sliceBlock(seq, 512, 512, out, 8);
    REQUIRE(n == 1);
    CHECK(out[0].type == MidiEventType::NoteOff);
    CHECK(out[0].sampleOffset == 0);
}

TEST_CASE("a repeated pitch is released before it re-attacks",
          "[instrument][slice]") {
    // Two notes of the same pitch, the second starting exactly where the first
    // ends. If the attack were emitted first the synth would release the note
    // it had just been asked to play, and the repeat would go silent.
    NoteSequence seq = sequenceOf({{0, 240, 60, 100, 0}, {240, 480, 60, 100, 0}});
    MidiEvent out[8];

    const int n = InstrumentNode::sliceBlock(seq, 0, 512, out, 8);
    REQUIRE(n == 4);   // on, off, on, off — both notes fit in the block
    CHECK(out[0].type == MidiEventType::NoteOn);
    CHECK(out[0].sampleOffset == 0);
    CHECK(out[1].type == MidiEventType::NoteOff);
    CHECK(out[1].sampleOffset == 240);
    CHECK(out[2].type == MidiEventType::NoteOn);
    CHECK(out[2].sampleOffset == 240);
    CHECK(out[3].type == MidiEventType::NoteOff);
    CHECK(out[3].sampleOffset == 480);
}

TEST_CASE("a block with nothing in it produces nothing", "[instrument][slice]") {
    NoteSequence seq = sequenceOf({{100000, 100500, 60, 100, 0}});
    MidiEvent out[8];
    CHECK(InstrumentNode::sliceBlock(seq, 0, 512, out, 8) == 0);
    CHECK(InstrumentNode::sliceBlock(seq, 200000, 512, out, 8) == 0);
}

TEST_CASE("slicing an empty sequence is safe", "[instrument][slice]") {
    NoteSequence seq;
    MidiEvent out[8];
    CHECK(InstrumentNode::sliceBlock(seq, 0, 512, out, 8) == 0);
}

TEST_CASE("slicing never writes past the caller's capacity",
          "[instrument][slice]") {
    std::vector<BakedNote> dense;
    for (int i = 0; i < 100; ++i) {
        dense.push_back(BakedNote{i, i + 1, static_cast<uint8_t>(i), 100, 0});
    }
    NoteSequence seq = sequenceOf(dense);
    MidiEvent out[4];
    CHECK(InstrumentNode::sliceBlock(seq, 0, 512, out, 4) == 4);
}

// ─── Note-off safety ────────────────────────────────────────────────────────

TEST_CASE("the first block clears every pitch on the used channels",
          "[instrument][noteoff]") {
    // A rebuilt graph reuses the cached plugin instance, which may still be
    // sounding notes started by the node this one replaced. Nothing records
    // those, so the only safe opening move is to release everything.
    InstrumentNode node;
    node.setSequence(sequenceOf({{0, 100, 60, 100, 0}, {0, 100, 64, 100, 3}}));
    std::vector<MidiEvent> buffer(InstrumentNode::kMaxEventsPerBlock);

    const int n = node.eventsForBlock(0, 64, false, buffer.data(),
                                      static_cast<int>(buffer.size()));
    CHECK(n == 256);   // 128 pitches × 2 channels in use, and no others
    for (int i = 0; i < n; ++i) CHECK(buffer[i].type == MidiEventType::NoteOff);
}

TEST_CASE("a note plays and releases across blocks", "[instrument][noteoff]") {
    Rig rig(sequenceOf({{100, 700, 60, 100, 0}}));

    auto first = rig.run(0, 512, true);
    REQUIRE(countOf(first, MidiEventType::NoteOn) == 1);
    CHECK(rig.node.isActive(0, 60));

    auto second = rig.run(512, 512, true);
    REQUIRE(countOf(second, MidiEventType::NoteOff) == 1);
    CHECK_FALSE(rig.node.isActive(0, 60));
    CHECK(rig.node.activeNoteCount() == 0);
}

TEST_CASE("stopping mid-note releases it", "[instrument][noteoff]") {
    Rig rig(sequenceOf({{0, 100000, 60, 100, 0}}));

    rig.run(0, 512, true);
    REQUIRE(rig.node.isActive(0, 60));

    // Transport stops while the note is still held. Without an explicit
    // release the synth holds it forever and only reloading the plugin clears
    // it — the classic stuck note.
    auto stopped = rig.run(512, 512, false);
    CHECK(has(stopped, MidiEventType::NoteOff, 60, 0));
    CHECK(rig.node.activeNoteCount() == 0);
}

TEST_CASE("seeking away mid-note releases it", "[instrument][noteoff]") {
    Rig rig(sequenceOf({{0, 100000, 60, 100, 0}, {500000, 600000, 72, 100, 0}}));

    rig.run(0, 512, true);
    REQUIRE(rig.node.isActive(0, 60));

    // Jump somewhere unrelated: the block no longer continues the last one.
    auto afterSeek = rig.run(500000, 512, true);
    CHECK(has(afterSeek, MidiEventType::NoteOff, 60, 0));
    CHECK_FALSE(rig.node.isActive(0, 60));
    CHECK(rig.node.isActive(0, 72));
}

TEST_CASE("a loop wrap releases the notes held at the loop end",
          "[instrument][noteoff]") {
    // A note held across the loop end. Wrapping back to the loop start is a
    // discontinuity like any other seek: the held note has to be released, or
    // it accumulates one stuck voice per lap.
    Rig rig(sequenceOf({{0, 96000, 60, 100, 0}}));

    rig.run(0, 512, true);
    rig.run(512, 512, true);
    REQUIRE(rig.node.isActive(0, 60));

    auto wrapped = rig.run(0, 512, true);   // loop start
    CHECK(has(wrapped, MidiEventType::NoteOff, 60, 0));
    // ...and the note that spans the loop start sounds again.
    CHECK(countOf(wrapped, MidiEventType::NoteOn) == 1);
    CHECK(rig.node.isActive(0, 60));
}

TEST_CASE("seeking past a note's start does not send an orphan release",
          "[instrument][noteoff]") {
    // Two notes. We seek into the gap after the first has ended, so its
    // note-off falls inside no block we ever played. Sending it anyway would
    // release whatever the synth happens to be playing on that pitch.
    Rig rig(sequenceOf({{0, 1000, 60, 100, 0}, {2000, 3000, 64, 100, 0}}));

    auto blk = rig.run(1500, 512, true);   // starts after note 60 has ended
    CHECK(countOf(blk, MidiEventType::NoteOff) == 0);
    CHECK_FALSE(rig.node.isActive(0, 60));
    // The block does reach note 64's start, so exactly that one is sounding.
    CHECK(rig.node.activeNoteCount() == 1);
    CHECK(rig.node.isActive(0, 64));
}

TEST_CASE("dropping the playhead into a held note sounds it",
          "[instrument][noteoff]") {
    // A four-bar pad. Starting playback in the middle of it should give you
    // the pad, not silence until the next note — which is what a naive
    // "only what starts in this block" slice would do.
    Rig rig(sequenceOf({{0, 192000, 60, 100, 0}}));

    auto blk = rig.run(96000, 512, true);
    CHECK(has(blk, MidiEventType::NoteOn, 60, 0));
    CHECK(rig.node.isActive(0, 60));

    // And it is still released at the right place.
    auto tail = rig.run(191744, 512, true);
    CHECK(has(tail, MidiEventType::NoteOff, 60, 256));
    CHECK(rig.node.activeNoteCount() == 0);
}

TEST_CASE("a note chased into is not retriggered on the following block",
          "[instrument][noteoff]") {
    Rig rig(sequenceOf({{0, 192000, 60, 100, 0}}));
    rig.run(96000, 512, true);
    auto next = rig.run(96512, 512, true);
    CHECK(next.empty());
    CHECK(rig.node.isActive(0, 60));
}

TEST_CASE("staying stopped emits nothing", "[instrument][noteoff]") {
    Rig rig(sequenceOf({{0, 100000, 60, 100, 0}}));
    CHECK(rig.run(0, 512, false).empty());
    CHECK(rig.run(512, 512, false).empty());
    CHECK(rig.node.activeNoteCount() == 0);
}

TEST_CASE("channels are preserved end to end", "[instrument]") {
    Rig rig(sequenceOf({{0, 1000, 60, 100, 0}, {0, 1000, 60, 100, 9}}));

    auto blk = rig.run(0, 512, true);
    REQUIRE(countOf(blk, MidiEventType::NoteOn) == 2);
    CHECK(rig.node.isActive(0, 60));
    // Same pitch, different channel: two independent notes, not one.
    CHECK(rig.node.isActive(9, 60));
    CHECK(rig.node.activeNoteCount() == 2);
}

TEST_CASE("velocity survives to the event", "[instrument]") {
    Rig rig(sequenceOf({{0, 1000, 60, 127, 0}, {0, 1000, 64, 1, 0}}));
    auto blk = rig.run(0, 512, true);
    REQUIRE(blk.size() == 2);
    for (const auto& e : blk) {
        if (e.pitch == 60) CHECK(e.velocity == 127);
        if (e.pitch == 64) CHECK(e.velocity == 1);
    }
}

TEST_CASE("a track with no notes never emits anything", "[instrument]") {
    Rig rig(NoteSequence{});
    CHECK(rig.run(0, 512, true).empty());
    CHECK(rig.run(512, 512, true).empty());
    CHECK(rig.node.activeNoteCount() == 0);
}
