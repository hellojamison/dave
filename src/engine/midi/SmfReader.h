// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dave::engine::midi {

// Standard MIDI File (SMF) reader. Hand-rolled for the same reason the repo
// hand-rolls SHA-256 and base64: the format is small and well-specified, and a
// dependency here would have to clear the permissive-license bar for a few
// hundred lines of parsing.
//
// The reader BAKES ticks to samples using the file's own tempo map, because
// Dave has no tempo map of its own yet and audio-for-picture work is
// sample-locked anyway. The tick-domain provenance (ppq + tempi) is returned
// alongside so a future Dave tempo map can re-conform without a re-import.

// One parsed SMF track. `notes` is sorted by startSample.
struct SmfTrack {
    std::string name;                        // from the FF 03 meta; may be empty
    std::vector<document::MidiNote> notes;
    int64_t lengthSamples = 0;               // end of track (>= last note end)
};

struct SmfFile {
    bool ok = false;
    std::string error;                       // set when !ok
    int format = 0;                          // 0 or 1 (2 is rejected)
    int ppq = 480;                           // ticks per quarter note
    std::vector<document::TempoEvent> tempi; // sorted by tick; may be empty
    std::vector<SmfTrack> tracks;            // in file order, including empty ones
    int64_t lengthSamples = 0;               // max across tracks
};

// Convert a tick position to samples through a tempo map. Pure — no I/O, no
// parser state — so the tempo arithmetic can be tested on its own. `tempi` must
// be sorted by tick; ticks before the first tempo event run at the SMF default
// of 120 bpm. Returns 0 for tick <= 0 or invalid ppq/sampleRate.
int64_t ticksToSamples(int64_t tick,
                       const std::vector<document::TempoEvent>& tempi,
                       int ppq, int sampleRate);

// Parse an in-memory SMF. Never throws and never reads out of bounds: a
// malformed file comes back with ok == false and a human-readable error, or
// (for damage confined to one track chunk) with the tracks that did parse.
SmfFile parseSmf(const uint8_t* data, size_t size, int sampleRate);

// Read and parse a .mid file from disk.
SmfFile readSmf(const std::string& path, int sampleRate);

} // namespace dave::engine::midi
