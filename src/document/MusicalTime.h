// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace dave::document {

// Musical time: bars, beats and ticks, against a map of time signatures.
//
// Everything in Dave is stored in samples — clips, markers, automation. This
// converts between that and what a musician reads, which is the only reason
// the meter map exists at all. Nothing downstream stores bars.
//
// Tempo is a map too. Between changes it is constant — there are no ramps,
// so a change takes effect on its beat and holds until the next one. Clips are
// anchored in samples and do NOT move when the tempo map changes; only the
// musical reading of the timeline does. Conforming audio to a new tempo is
// time-stretching, which is a different feature.

// A meter starting at `bar` and running until the next one. There is always
// one at bar 1; normalizeMeterMap guarantees it.
struct TimeSignature {
    int bar = 1;            // 1-indexed, like the ruler
    int numerator = 4;      // beats in a bar
    int denominator = 4;    // which note gets the beat

    bool operator==(const TimeSignature& other) const {
        return bar == other.bar && numerator == other.numerator &&
               denominator == other.denominator;
    }
};

// A tempo change, anchored musically rather than in samples: it sits at a bar
// and beat, so moving a time signature moves the tempo changes after it — the
// same way a score works. There is always one at bar 1 beat 1.
struct TempoChange {
    int bar = 1;
    int beat = 1;
    double bpm = 120.0;   // quarter notes per minute

    bool operator==(const TempoChange& other) const {
        return bar == other.bar && beat == other.beat && bpm == other.bpm;
    }
};

struct BarsBeats {
    int bar = 1;    // 1-indexed
    int beat = 1;   // 1-indexed
    int tick = 0;   // 0..ticksPerBeat-1

    bool operator==(const BarsBeats& other) const {
        return bar == other.bar && beat == other.beat && tick == other.tick;
    }
};

// Ticks per beat. 960 is the usual DAW resolution and divides cleanly by 3,
// so triplets land on whole ticks.
inline constexpr int kTicksPerBeat = 960;

// A meter map that can be walked without further checking: sorted by bar,
// starting at bar 1, one entry per bar, nothing with a nonsense numerator or
// a denominator that is not a power of two.
//
// Every reader goes through this rather than trusting what it was handed —
// the map comes from a project file and from a UI that can be mid-edit.
std::vector<TimeSignature> normalizeMeterMap(std::vector<TimeSignature> map);

// Sorted by position, one per position, one at bar 1 beat 1, no tempo that
// would divide by zero. Every reader goes through this.
std::vector<TempoChange> normalizeTempoMap(std::vector<TempoChange> map);

// A one-entry map, for callers that have a single session tempo.
std::vector<TempoChange> constantTempo(double bpm);

// The tempo in force at a musical position.
double bpmAt(const std::vector<TempoChange>& tempo,
             const std::vector<TimeSignature>& meter, int bar, int beat = 1);

// The signature in force at `bar`.
TimeSignature signatureAtBar(const std::vector<TimeSignature>& map, int bar);

// Length of one beat, and of one bar, in quarter notes. A beat in 6/8 is an
// eighth, so it is half a quarter; a 6/8 bar is three quarters.
double quartersPerBeat(const TimeSignature& signature);
double quartersPerBar(const TimeSignature& signature);
// Quarter notes from the session start to the head of `bar`.
double quartersToBar(int bar, const std::vector<TimeSignature>& map);

// Sample position ↔ musical position. `bpm` is quarter notes per minute,
// which is what every tempo field in every DAW means regardless of meter.
BarsBeats barsBeatsAtSample(int64_t sample, double sampleRate,
                            const std::vector<TempoChange>& tempo,
                            const std::vector<TimeSignature>& map);
int64_t sampleAtBarsBeats(const BarsBeats& position, double sampleRate,
                          const std::vector<TempoChange>& tempo,
                          const std::vector<TimeSignature>& map);
BarsBeats barsBeatsAtSample(int64_t sample, double sampleRate, double bpm,
                            const std::vector<TimeSignature>& map);
int64_t sampleAtBarsBeats(const BarsBeats& position, double sampleRate,
                          double bpm, const std::vector<TimeSignature>& map);

// Where a bar line falls, and how long a bar and a beat are there. The ruler
// and the snap grid need these per bar, because a meter change makes them
// vary down the timeline.
int64_t sampleAtBar(int bar, double sampleRate,
                    const std::vector<TempoChange>& tempo,
                    const std::vector<TimeSignature>& map);
// Measured rather than multiplied: a tempo change inside a bar or a beat makes
// it a different length from its neighbours, and only the difference between
// its two ends is right.
double samplesPerBeatAtBar(int bar, double sampleRate,
                           const std::vector<TempoChange>& tempo,
                           const std::vector<TimeSignature>& map);
double samplesPerBarAtBar(int bar, double sampleRate,
                          const std::vector<TempoChange>& tempo,
                          const std::vector<TimeSignature>& map);
int64_t sampleAtBar(int bar, double sampleRate, double bpm,
                    const std::vector<TimeSignature>& map);
double samplesPerBeatAtBar(int bar, double sampleRate, double bpm,
                           const std::vector<TimeSignature>& map);
double samplesPerBarAtBar(int bar, double sampleRate, double bpm,
                          const std::vector<TimeSignature>& map);

} // namespace dave::document
