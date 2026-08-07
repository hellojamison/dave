// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Types.h"
#include "engine/graph/Node.h"
#include "engine/plugins/PluginInstance.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace dave::engine {

// One note flattened onto the timeline: absolute sample positions, clip trim
// already applied. This is what the RT thread walks each block.
struct BakedNote {
    int64_t start = 0;   // timeline sample the note begins
    int64_t end = 0;     // timeline sample the note ends (exclusive)
    uint8_t pitch = 60;
    uint8_t velocity = 100;
    uint8_t channel = 0;
};

// A baked sequence, ready for block slicing.
//
// Two orderings, because a block has to answer two different questions — what
// starts inside it and what ends inside it — and a note's position in end
// order is not its position in start order. Both are binary-searchable, so the
// per-block cost is logarithmic in the sequence length rather than linear.
struct NoteSequence {
    std::vector<BakedNote> notes;   // sorted by start
    std::vector<int32_t> byEnd;     // indices into `notes`, sorted by end
    uint16_t usedChannels = 0;      // bit per MIDI channel that appears
    int64_t maxNoteLength = 0;      // bounds the backward scan when chasing

    bool empty() const { return notes.empty(); }
};

// Flatten one MIDI clip's notes onto the timeline, honouring its trim.
//
// The mapping is identical to AudioClipNode's: the clip shows the source
// window [sourceOffset, sourceOffset + length) at timelineStart, so a source
// position s appears at timelineStart + (s - sourceOffset). Notes starting
// outside that window don't sound; notes running past its end are cut off at
// it, exactly like audio being trimmed. A length of 0 means "to the end of the
// sequence", matching AudioClipNode::lengthSamples.
void bakeClip(const document::MidiClip& clip, std::vector<BakedNote>& out);

// Bake a whole track's clips into a sliceable sequence.
NoteSequence bakeClips(const std::vector<document::MidiClip>& clips);

// InstrumentNode — a MIDI track's note sequence driving one instrument plugin.
//
// It is a GENERATOR (zero input pins): the notes come from the document, not
// from an audio edge, and the plugin turns them into the node's output. Fusing
// the sequence and the plugin into one node is what lets MIDI land without
// touching the graph core — no event pins, no NodeProcessContext change, and
// no question about whether an events node runs before the synth that needs
// them. When first-class event pins arrive this splits in two, and the widened
// PluginInstance::process survives unchanged.
//
// Threading: setSequence/setInstrument/prepare on the UI thread (GraphBuilder
// builds a fresh node per derive, before the graph goes live); process() on
// the RT thread, where it allocates nothing.
class InstrumentNode : public Node {
public:
    InstrumentNode();

    // Generator: no audio in, one stereo out.
    int numInputPins() const override { return 0; }
    int numOutputPins() const override { return 1; }
    int channelsPerPin() const override { return 2; }

    // --- UI thread ---------------------------------------------------------
    void setInstrument(std::shared_ptr<PluginInstance> instance) {
        instance_ = std::move(instance);
    }
    const std::shared_ptr<PluginInstance>& instrument() const { return instance_; }

    void setSequence(NoteSequence sequence) { seq_ = std::move(sequence); }
    const NoteSequence& sequence() const { return seq_; }

    void prepare(double sampleRate, int maxBlock) override;

    // --- RT thread ---------------------------------------------------------
    void process(NodeProcessContext& ctx) override;

    // --- Seams for tests ---------------------------------------------------
    // The events this node sends for the block starting at `blockStart`,
    // written into `out` (capacity `maxEvents`); returns how many. This is
    // exactly what process() does minus the plugin call, which is the point:
    // note-off correctness across stops, seeks and loop wraps is testable
    // without loading a synth.
    int eventsForBlock(int64_t blockStart, int numSamples, bool playing,
                       MidiEvent* out, int maxEvents);

    // Pure slice: the note-ons and note-offs falling inside
    // [blockStart, blockStart + numSamples), ordered by sample offset with
    // offs before ons at equal offsets. Touches no state.
    static int sliceBlock(const NoteSequence& seq, int64_t blockStart,
                          int numSamples, MidiEvent* out, int maxEvents);

    // Is a (channel, pitch) currently sounding? For tests and for the
    // "did we leave a note hanging" assertion.
    bool isActive(uint8_t channel, uint8_t pitch) const {
        const int key = channel * 128 + pitch;
        return ((active_[key >> 6] >> (key & 63)) & 1ULL) != 0;
    }
    int activeNoteCount() const;

    // How many events one block can carry. Matches the capacity of
    // PluginInstance's pre-allocated event list; anything beyond is dropped.
    static constexpr int kMaxEventsPerBlock = 512;

private:
    // Note-offs for everything currently sounding, at `sampleOffset`.
    int flushActiveNotes(MidiEvent* out, int maxEvents, int32_t sampleOffset);
    // Note-offs for every pitch on every channel the sequence uses. Sent once
    // after prepare(), when the (cached, possibly reused) plugin instance may
    // still be holding notes from the graph we just replaced.
    int flushAllNotes(MidiEvent* out, int maxEvents);
    // Note-ons for notes already sounding at `at`, so dropping the playhead
    // into a held chord sounds the chord instead of silence.
    int chaseNotes(int64_t at, MidiEvent* out, int maxEvents);

    void setActive(uint8_t channel, uint8_t pitch, bool on) {
        const int key = channel * 128 + pitch;
        if (on) active_[key >> 6] |= (1ULL << (key & 63));
        else    active_[key >> 6] &= ~(1ULL << (key & 63));
    }

    std::shared_ptr<PluginInstance> instance_;
    NoteSequence seq_;

    // Which of the 16×128 (channel, pitch) keys we have sent a note-on for and
    // not yet a note-off. Tracking this is the only way a stop or a seek can
    // silence exactly what is sounding — a synth given a note-on it never sees
    // closed holds it forever, and that is a stuck note the user can only
    // clear by reloading the plugin.
    uint64_t active_[32] = {};

    // Where the previous block ended. A block that doesn't start here means
    // the transport jumped (seek, loop wrap), which invalidates every sounding
    // note.
    int64_t lastBlockEnd_ = 0;
    bool wasPlaying_ = false;
    // Set by prepare(): the graph was rebuilt, so assume the instance is dirty.
    bool needsAllNotesOff_ = true;

    // Scratch, sized once so process() never allocates.
    std::vector<MidiEvent> events_;
    std::vector<MidiEvent> sliced_;
};

} // namespace dave::engine
