// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/SmfReader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace dave::engine::midi {

using document::MidiNote;
using document::TempoEvent;

namespace {

// A bounds-checked read cursor. Every accessor sets `bad` and returns a
// harmless value rather than reading past `end`, so the parser can run to
// completion on a truncated or hostile file and report the failure once.
struct Cursor {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    bool bad = false;

    int64_t remaining() const { return end - p; }

    uint8_t u8() {
        if (p >= end) { bad = true; return 0; }
        return *p++;
    }
    uint16_t u16() {
        const uint16_t hi = u8();
        const uint16_t lo = u8();
        return static_cast<uint16_t>((hi << 8) | lo);
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | u8();
        return v;
    }
    // Variable-length quantity: 7 bits per byte, high bit continues. The spec
    // caps these at 4 bytes; a 5th continuation byte means the stream is not
    // what it claims to be.
    int64_t vlq() {
        int64_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t b = u8();
            if (bad) return 0;
            v = (v << 7) | (b & 0x7F);
            if ((b & 0x80) == 0) return v;
        }
        bad = true;
        return 0;
    }
    void skip(int64_t n) {
        if (n < 0 || n > remaining()) { bad = true; p = end; }
        else { p += n; }
    }
};

// A note in the file's own tick domain, before the tempo map is applied.
struct TickNote {
    int64_t startTick = 0;
    int64_t endTick = 0;
    uint8_t pitch = 0;
    uint8_t velocity = 0;
    uint8_t channel = 0;
};

struct TickTrack {
    std::string name;
    std::vector<TickNote> notes;
    int64_t endTick = 0;
};

constexpr int kKeyCount = 16 * 128;  // channel × pitch

// Parse one MTrk event stream. Appends any tempo metas to `tempi` (the tempo
// map is file-global even in format 1, where it conventionally lives in track
// 0 but is not required to).
TickTrack parseTrackChunk(const uint8_t* begin, const uint8_t* chunkEnd,
                          std::vector<TempoEvent>& tempi) {
    Cursor t{begin, chunkEnd};
    TickTrack tt;
    int64_t tick = 0;
    uint8_t runningStatus = 0;
    bool endOfTrack = false;

    // Index into tt.notes of the note currently sounding on each key, or -1.
    std::vector<int> pending(kKeyCount, -1);

    while (!t.bad && t.p < t.end && !endOfTrack) {
        tick += t.vlq();
        if (t.bad) break;
        tt.endTick = std::max(tt.endTick, tick);

        // Running status: a byte with the high bit clear is the first data byte
        // of another message of the previous channel-message type.
        const uint8_t lead = t.u8();
        if (t.bad) break;
        uint8_t status;
        if (lead & 0x80) {
            status = lead;
            // System messages cancel running status; channel messages set it.
            runningStatus = (status < 0xF0) ? status : 0;
        } else {
            if (runningStatus == 0) { t.bad = true; break; }
            status = runningStatus;
            --t.p;  // `lead` belongs to the data bytes
        }

        const uint8_t kind = status & 0xF0;

        if (status == 0xFF) {  // meta event
            const uint8_t type = t.u8();
            const int64_t len = t.vlq();
            if (t.bad || len > t.remaining()) { t.bad = true; break; }
            if (type == 0x51 && len == 3) {
                const int32_t uspq = (static_cast<int32_t>(t.p[0]) << 16) |
                                     (static_cast<int32_t>(t.p[1]) << 8) |
                                     static_cast<int32_t>(t.p[2]);
                if (uspq > 0) tempi.push_back(TempoEvent{tick, uspq});
            } else if (type == 0x03 && tt.name.empty() && len > 0) {
                tt.name.assign(reinterpret_cast<const char*>(t.p),
                               static_cast<size_t>(len));
            } else if (type == 0x2F) {
                endOfTrack = true;
            }
            t.skip(len);
        } else if (status == 0xF0 || status == 0xF7) {  // sysex
            t.skip(t.vlq());
        } else if (kind == 0x90 || kind == 0x80) {
            const uint8_t pitch = t.u8() & 0x7F;
            const uint8_t velocity = t.u8() & 0x7F;
            if (t.bad) break;
            const uint8_t channel = status & 0x0F;
            const int key = channel * 128 + pitch;
            // A note-on with velocity 0 is the idiomatic note-off; files that
            // use it are the reason running status is worth supporting at all.
            const bool noteOn = (kind == 0x90) && velocity > 0;
            if (noteOn) {
                // A second note-on for a key that is already sounding ends the
                // first — one channel cannot express two overlapping notes of
                // the same pitch, so there is nothing else this can mean.
                if (pending[key] >= 0) tt.notes[pending[key]].endTick = tick;
                pending[key] = static_cast<int>(tt.notes.size());
                tt.notes.push_back(TickNote{tick, tick, pitch, velocity, channel});
            } else if (pending[key] >= 0) {
                tt.notes[pending[key]].endTick = tick;
                pending[key] = -1;
            }
        } else if (kind == 0xC0 || kind == 0xD0) {
            t.skip(1);  // program change / channel pressure
        } else if (kind >= 0x80 && kind <= 0xE0) {
            t.skip(2);  // aftertouch / control change / pitch bend
        } else {
            // 0xF1..0xFE are realtime/system-common messages that have no
            // meaning in a file and no length we could skip. Stop here rather
            // than guess; the caller resyncs to the chunk boundary.
            t.bad = true;
        }
    }

    // Notes with no matching note-off end where the track does. A file that
    // does this is broken, but dropping the note loses more than closing it.
    for (int key = 0; key < kKeyCount; ++key) {
        if (pending[key] >= 0) tt.notes[pending[key]].endTick = tt.endTick;
    }
    return tt;
}

} // namespace

int64_t ticksToSamples(int64_t tick, const std::vector<TempoEvent>& tempi,
                       int ppq, int sampleRate) {
    if (tick <= 0 || ppq <= 0 || sampleRate <= 0) return 0;

    // Walk the tempo map accumulating real time segment by segment. Ticks
    // before the first tempo event run at the SMF default of 120 bpm.
    double seconds = 0.0;
    int64_t cursor = 0;
    int32_t uspq = 500000;
    for (const auto& te : tempi) {
        if (te.tick >= tick) break;
        if (te.tick > cursor) {
            seconds += static_cast<double>(te.tick - cursor) / ppq * uspq * 1e-6;
            cursor = te.tick;
        }
        uspq = te.microsecondsPerQuarter;
    }
    seconds += static_cast<double>(tick - cursor) / ppq * uspq * 1e-6;
    return static_cast<int64_t>(std::llround(seconds * sampleRate));
}

SmfFile parseSmf(const uint8_t* data, size_t size, int sampleRate) {
    SmfFile out;
    if (data == nullptr || size < 14) {
        out.error = "too short to be a MIDI file";
        return out;
    }
    if (std::memcmp(data, "MThd", 4) != 0) {
        out.error = "not a MIDI file (missing MThd header)";
        return out;
    }

    Cursor c{data, data + size};
    c.skip(4);
    const uint32_t headerLen = c.u32();
    const uint16_t format = c.u16();
    const uint16_t numTracks = c.u16();
    const int16_t division = static_cast<int16_t>(c.u16());
    // The header is 6 bytes today but the spec allows it to grow; skip extra.
    if (headerLen > 6) c.skip(headerLen - 6);
    if (c.bad) { out.error = "truncated MIDI header"; return out; }

    if (format == 2) {
        // Format 2 tracks are independent sequences, not a single timeline —
        // there is no correct way to lay them out on one, so refuse rather than
        // silently pile them on top of each other.
        out.error = "SMF format 2 (independent sequences) is not supported";
        return out;
    }
    if (format > 2) { out.error = "unknown SMF format"; return out; }
    out.format = format;

    // A negative division is SMPTE: absolute time, not musical time. Normalize
    // it into the PPQ representation by calling one second a "quarter" — the
    // tick rate is then fps × ticksPerFrame and everything downstream stays
    // uniform. That only works if a "quarter" is also declared to LAST one
    // second, hence the synthetic 1,000,000 µs tempo: without it the file's
    // ticks would be read against the SMF default of 500,000 and the whole
    // performance would play at double speed. The file's own tempo metas are
    // meaningless here and are dropped.
    const bool smpte = (division & 0x8000) != 0;
    if (smpte) {
        const int fps = -static_cast<int>(static_cast<int8_t>(division >> 8));
        const int ticksPerFrame = division & 0xFF;
        if (fps <= 0 || ticksPerFrame <= 0) {
            out.error = "invalid SMPTE division";
            return out;
        }
        out.ppq = fps * ticksPerFrame;
        out.tempi.push_back(TempoEvent{0, 1000000});
    } else {
        out.ppq = division & 0x7FFF;
        if (out.ppq <= 0) { out.error = "invalid division (0 ticks per quarter)"; return out; }
    }

    // Pass 1: parse every track chunk into the tick domain, collecting the
    // file-global tempo map as we go. Ticks can't be converted until the whole
    // map is known, so the conversion is a separate pass below.
    std::vector<TickTrack> tickTracks;
    std::vector<TempoEvent> tempi;
    while (tickTracks.size() < numTracks && c.remaining() >= 8) {
        const bool isTrack = std::memcmp(c.p, "MTrk", 4) == 0;
        c.skip(4);
        const uint32_t chunkLen = c.u32();
        if (c.bad || static_cast<int64_t>(chunkLen) > c.remaining()) {
            out.error = "truncated track chunk";
            return out;
        }
        const uint8_t* chunkEnd = c.p + chunkLen;
        if (isTrack) {
            tickTracks.push_back(parseTrackChunk(c.p, chunkEnd, tempi));
        }
        // Resync to the chunk boundary whatever the event stream did, so a
        // malformed track costs only that track. Unknown chunk types are
        // skipped the same way, as the spec requires.
        c.p = chunkEnd;
    }
    if (tickTracks.empty()) {
        out.error = "no track chunks found";
        return out;
    }

    if (!smpte) {
        // Stable so that two tempo events on the same tick keep file order;
        // ticksToSamples lets the last one win.
        std::stable_sort(tempi.begin(), tempi.end(),
                         [](const TempoEvent& a, const TempoEvent& b) {
                             return a.tick < b.tick;
                         });
        out.tempi = std::move(tempi);
    }

    // Pass 2: bake to samples.
    for (auto& tt : tickTracks) {
        SmfTrack st;
        st.name = std::move(tt.name);
        st.notes.reserve(tt.notes.size());
        for (const auto& tn : tt.notes) {
            MidiNote n;
            n.startSample = ticksToSamples(tn.startTick, out.tempi, out.ppq, sampleRate);
            const int64_t endSample =
                ticksToSamples(tn.endTick, out.tempi, out.ppq, sampleRate);
            // A note whose off lands on its own on — or one left open by the
            // last event in the track — has no duration to preserve. Keep it at
            // one sample so it survives to a future piano roll instead of
            // vanishing, rather than inventing a length for it.
            n.lengthSamples = std::max<int64_t>(1, endSample - n.startSample);
            n.pitch = tn.pitch;
            n.velocity = tn.velocity;
            n.channel = tn.channel;
            st.notes.push_back(n);
        }
        std::stable_sort(st.notes.begin(), st.notes.end(),
                         [](const MidiNote& a, const MidiNote& b) {
                             return a.startSample < b.startSample;
                         });
        st.lengthSamples = ticksToSamples(tt.endTick, out.tempi, out.ppq, sampleRate);
        for (const auto& n : st.notes) {
            st.lengthSamples = std::max(st.lengthSamples, n.startSample + n.lengthSamples);
        }
        out.lengthSamples = std::max(out.lengthSamples, st.lengthSamples);
        out.tracks.push_back(std::move(st));
    }

    out.ok = true;
    return out;
}

SmfFile readSmf(const std::string& path, int sampleRate) {
    SmfFile out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out.error = "cannot open " + path;
        return out;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        out.error = "empty file: " + path;
        return out;
    }
    return parseSmf(bytes.data(), bytes.size(), sampleRate);
}

} // namespace dave::engine::midi
