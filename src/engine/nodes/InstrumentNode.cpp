// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/nodes/InstrumentNode.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace dave::engine {

// ─── Baking ─────────────────────────────────────────────────────────────────

void bakeClip(const document::MidiClip& clip, std::vector<BakedNote>& out) {
    const int64_t winStart = clip.sourceOffset;
    const int64_t winEnd = (clip.length > 0)
        ? clip.sourceOffset + clip.length
        : std::numeric_limits<int64_t>::max();

    for (const auto& n : clip.notes) {
        if (n.startSample < winStart || n.startSample >= winEnd) continue;
        // A note running past the clip's tail is cut off there, not dropped:
        // trimming a clip mid-note should shorten the note, the same way
        // trimming audio shortens the sound rather than deleting it.
        int64_t sourceEnd = n.startSample + std::max<int64_t>(1, n.lengthSamples);
        if (sourceEnd > winEnd) sourceEnd = winEnd;

        const int64_t start = clip.timelineStart + (n.startSample - winStart);
        const int64_t end = clip.timelineStart + (sourceEnd - winStart);
        if (end <= start) continue;
        out.push_back(BakedNote{start, end, n.pitch, n.velocity, n.channel});
    }
}

NoteSequence bakeClips(const std::vector<document::MidiClip>& clips) {
    NoteSequence seq;
    for (const auto& c : clips) bakeClip(c, seq.notes);

    std::stable_sort(seq.notes.begin(), seq.notes.end(),
                     [](const BakedNote& a, const BakedNote& b) {
                         return a.start < b.start;
                     });

    seq.byEnd.resize(seq.notes.size());
    for (size_t i = 0; i < seq.notes.size(); ++i) seq.byEnd[i] = static_cast<int32_t>(i);
    std::stable_sort(seq.byEnd.begin(), seq.byEnd.end(),
                     [&seq](int32_t a, int32_t b) {
                         return seq.notes[a].end < seq.notes[b].end;
                     });

    for (const auto& n : seq.notes) {
        seq.usedChannels = static_cast<uint16_t>(seq.usedChannels |
                                                 (1u << (n.channel & 0x0F)));
        seq.maxNoteLength = std::max(seq.maxNoteLength, n.end - n.start);
    }
    return seq;
}

// ─── Node ───────────────────────────────────────────────────────────────────

InstrumentNode::InstrumentNode()
    : Node("instrument"),
      events_(kMaxEventsPerBlock),
      sliced_(kMaxEventsPerBlock) {}

void InstrumentNode::prepare(double sampleRate, int maxBlock) {
    if (instance_) instance_->prepare(sampleRate, maxBlock);
    // The GraphBuilder caches PluginInstances across re-derives, so the synth
    // this node just adopted may still be sounding notes started by the node
    // it replaced. We have no record of those, so the first block clears
    // everything the sequence could possibly have touched.
    needsAllNotesOff_ = true;
    for (auto& word : active_) word = 0;
    lastBlockEnd_ = 0;
    wasPlaying_ = false;
}

int InstrumentNode::activeNoteCount() const {
    int count = 0;
    for (uint64_t word : active_) count += std::popcount(word);
    return count;
}

int InstrumentNode::sliceBlock(const NoteSequence& seq, int64_t blockStart,
                               int numSamples, MidiEvent* out, int maxEvents) {
    if (numSamples <= 0 || maxEvents <= 0) return 0;
    const int64_t blockEnd = blockStart + numSamples;

    // First note starting at or after blockStart, and first note ending at or
    // after it. Everything this block cares about is a contiguous run from
    // each of those two cursors.
    size_t on = static_cast<size_t>(
        std::lower_bound(seq.notes.begin(), seq.notes.end(), blockStart,
                         [](const BakedNote& n, int64_t v) { return n.start < v; }) -
        seq.notes.begin());
    size_t off = static_cast<size_t>(
        std::lower_bound(seq.byEnd.begin(), seq.byEnd.end(), blockStart,
                         [&seq](int32_t idx, int64_t v) {
                             return seq.notes[idx].end < v;
                         }) -
        seq.byEnd.begin());

    int count = 0;
    while (count < maxEvents) {
        const bool haveOn = on < seq.notes.size() && seq.notes[on].start < blockEnd;
        const bool haveOff = off < seq.byEnd.size() &&
                             seq.notes[seq.byEnd[off]].end < blockEnd;
        if (!haveOn && !haveOff) break;

        // Offs win ties. A note that ends exactly where the next one of the
        // same pitch begins must be released first, or the synth sees the
        // release after the attack and the repeated note dies instead of
        // re-articulating.
        const bool takeOff = haveOff &&
            (!haveOn || seq.notes[seq.byEnd[off]].end <= seq.notes[on].start);

        if (takeOff) {
            const BakedNote& n = seq.notes[seq.byEnd[off]];
            out[count++] = MidiEvent{static_cast<int32_t>(n.end - blockStart),
                                     MidiEventType::NoteOff, n.channel, n.pitch, 0};
            ++off;
        } else {
            const BakedNote& n = seq.notes[on];
            out[count++] = MidiEvent{static_cast<int32_t>(n.start - blockStart),
                                     MidiEventType::NoteOn, n.channel, n.pitch,
                                     n.velocity};
            ++on;
        }
    }
    return count;
}

int InstrumentNode::flushActiveNotes(MidiEvent* out, int maxEvents,
                                     int32_t sampleOffset) {
    int count = 0;
    for (int word = 0; word < 32; ++word) {
        uint64_t bits = active_[word];
        while (bits != 0 && count < maxEvents) {
            const int bit = std::countr_zero(bits);
            bits &= bits - 1;
            const int key = word * 64 + bit;
            out[count++] = MidiEvent{sampleOffset, MidiEventType::NoteOff,
                                     static_cast<uint8_t>(key / 128),
                                     static_cast<uint8_t>(key % 128), 0};
        }
        // Whatever we could not fit is dropped rather than remembered: leaving
        // the bit set would replay the same note-off next block forever if the
        // capacity is genuinely too small, and 512 covers 4× the worst case.
        active_[word] = 0;
    }
    return count;
}

int InstrumentNode::flushAllNotes(MidiEvent* out, int maxEvents) {
    int count = 0;
    // Only the channels this sequence actually uses. A track with one channel
    // costs 128 events; the alternative — all 16 channels — is 2048, four
    // times the event capacity, and would silently truncate.
    for (int channel = 0; channel < 16; ++channel) {
        if ((seq_.usedChannels & (1u << channel)) == 0) continue;
        for (int pitch = 0; pitch < 128 && count < maxEvents; ++pitch) {
            out[count++] = MidiEvent{0, MidiEventType::NoteOff,
                                     static_cast<uint8_t>(channel),
                                     static_cast<uint8_t>(pitch), 0};
        }
    }
    for (auto& word : active_) word = 0;
    return count;
}

int InstrumentNode::chaseNotes(int64_t at, MidiEvent* out, int maxEvents) {
    if (seq_.notes.empty() || maxEvents <= 0) return 0;

    // Notes that started before `at` and have not ended. Scanning backwards
    // from the first note starting after `at` and stopping once we are further
    // back than the longest note in the sequence bounds the work — no note
    // that began earlier than that can still be sounding.
    size_t i = static_cast<size_t>(
        std::lower_bound(seq_.notes.begin(), seq_.notes.end(), at,
                         [](const BakedNote& n, int64_t v) { return n.start < v; }) -
        seq_.notes.begin());
    const int64_t earliest = at - seq_.maxNoteLength;

    int count = 0;
    while (i > 0 && count < maxEvents) {
        --i;
        const BakedNote& n = seq_.notes[i];
        if (n.start < earliest) break;
        if (n.end <= at) continue;
        if (isActive(n.channel, n.pitch)) continue;
        out[count++] = MidiEvent{0, MidiEventType::NoteOn, n.channel, n.pitch,
                                 n.velocity};
        setActive(n.channel, n.pitch, true);
    }
    return count;
}

int InstrumentNode::eventsForBlock(int64_t blockStart, int numSamples,
                                   bool playing, MidiEvent* out, int maxEvents) {
    int count = 0;

    // A jump is any block that doesn't continue the last one: a seek, a loop
    // wrap, the first block after a graph rebuild, or the moment play starts
    // (the playhead may have been parked inside a held chord all along).
    const bool jumped = needsAllNotesOff_ || (blockStart != lastBlockEnd_) ||
                        (playing && !wasPlaying_);

    if (needsAllNotesOff_) {
        count += flushAllNotes(out + count, maxEvents - count);
        needsAllNotesOff_ = false;
    } else if (jumped || (!playing && wasPlaying_)) {
        count += flushActiveNotes(out + count, maxEvents - count, 0);
    }

    // Stopped: the position still tracks the playhead so the next block after
    // a scrub is not mistaken for a seek, but nothing sounds.
    if (!playing) {
        lastBlockEnd_ = blockStart;
        wasPlaying_ = false;
        return count;
    }

    if (jumped) count += chaseNotes(blockStart, out + count, maxEvents - count);

    // The block's own note starts and ends, filtered against what is actually
    // sounding: after a seek past a note's start we must not send its orphan
    // note-off, and a re-attack of a still-held pitch needs its release first.
    const int numSliced = sliceBlock(seq_, blockStart, numSamples, sliced_.data(),
                                     static_cast<int>(sliced_.size()));
    for (int i = 0; i < numSliced && count < maxEvents; ++i) {
        const MidiEvent& e = sliced_[i];
        if (e.type == MidiEventType::NoteOn) {
            if (isActive(e.channel, e.pitch) && count < maxEvents) {
                out[count++] = MidiEvent{e.sampleOffset, MidiEventType::NoteOff,
                                         e.channel, e.pitch, 0};
            }
            if (count >= maxEvents) break;
            out[count++] = e;
            setActive(e.channel, e.pitch, true);
        } else {
            if (!isActive(e.channel, e.pitch)) continue;
            out[count++] = e;
            setActive(e.channel, e.pitch, false);
        }
    }

    lastBlockEnd_ = blockStart + numSamples;
    wasPlaying_ = true;
    return count;
}

void InstrumentNode::process(NodeProcessContext& ctx) {
    if (!instance_ || !instance_->isLoaded()) return;

    const TimeInfo time = (ctx.time != nullptr) ? *ctx.time : TimeInfo{};
    const int numEvents = eventsForBlock(time.samplePos, ctx.numSamples,
                                         time.isPlaying, events_.data(),
                                         static_cast<int>(events_.size()));

    // No audio input: an instrument is driven by events. PluginInstance feeds
    // the plugin silence if it declared an input bus anyway.
    instance_->process(nullptr, ctx.output.channels, ctx.output.numChannels,
                       ctx.numSamples, events_.data(), numEvents, time);
}

} // namespace dave::engine
