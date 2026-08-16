// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/MusicalTime.h"

#include <algorithm>
#include <cmath>

namespace dave::document {
namespace {

bool isPowerOfTwo(int value) {
    return value > 0 && (value & (value - 1)) == 0;
}

double samplesPerQuarter(double sampleRate, double bpm) {
    if (!(sampleRate > 0.0) || !(bpm > 0.0)) return 0.0;
    return sampleRate * 60.0 / bpm;
}

} // namespace

std::vector<TimeSignature> normalizeMeterMap(std::vector<TimeSignature> map) {
    for (auto& signature : map) {
        signature.bar = std::max(1, signature.bar);
        signature.numerator = std::clamp(signature.numerator, 1, 64);
        // A denominator that is not a power of two has no note value, so it
        // cannot be converted to quarters at all.
        if (!isPowerOfTwo(signature.denominator) || signature.denominator > 64) {
            signature.denominator = 4;
        }
    }
    std::stable_sort(map.begin(), map.end(),
                     [](const TimeSignature& a, const TimeSignature& b) {
                         return a.bar < b.bar;
                     });
    // One per bar, last edit wins — a UI mid-edit can briefly produce two
    // changes at the same bar, and the walk below would count the bar twice.
    // std::unique would keep the FIRST of each run, which is the older edit.
    std::vector<TimeSignature> deduped;
    deduped.reserve(map.size());
    for (const auto& signature : map) {
        if (!deduped.empty() && deduped.back().bar == signature.bar) {
            deduped.back() = signature;
        } else {
            deduped.push_back(signature);
        }
    }
    map = std::move(deduped);
    if (map.empty() || map.front().bar != 1) {
        map.insert(map.begin(), TimeSignature{1, 4, 4});
    }
    return map;
}

std::vector<TempoChange> normalizeTempoMap(std::vector<TempoChange> map) {
    for (auto& change : map) {
        change.bar = std::max(1, change.bar);
        change.beat = std::max(1, change.beat);
        // A zero or negative tempo would divide by zero in every conversion.
        if (!(change.bpm > 0.0) || change.bpm > 999.0) change.bpm = 120.0;
    }
    std::stable_sort(map.begin(), map.end(),
                     [](const TempoChange& a, const TempoChange& b) {
                         if (a.bar != b.bar) return a.bar < b.bar;
                         return a.beat < b.beat;
                     });
    std::vector<TempoChange> deduped;
    deduped.reserve(map.size());
    for (const auto& change : map) {
        if (!deduped.empty() && deduped.back().bar == change.bar &&
            deduped.back().beat == change.beat) {
            deduped.back() = change;   // last edit wins
        } else {
            deduped.push_back(change);
        }
    }
    map = std::move(deduped);
    if (map.empty() || map.front().bar != 1 || map.front().beat != 1) {
        map.insert(map.begin(), TempoChange{1, 1, 120.0});
    }
    return map;
}

std::vector<TempoChange> constantTempo(double bpm) {
    return normalizeTempoMap({TempoChange{1, 1, bpm}});
}

TimeSignature signatureAtBar(const std::vector<TimeSignature>& map, int bar) {
    TimeSignature current{1, 4, 4};
    for (const auto& signature : map) {
        if (signature.bar > bar) break;
        current = signature;
    }
    return current;
}

double quartersPerBeat(const TimeSignature& signature) {
    const int denominator =
        isPowerOfTwo(signature.denominator) ? signature.denominator : 4;
    return 4.0 / static_cast<double>(denominator);
}

double quartersPerBar(const TimeSignature& signature) {
    return quartersPerBeat(signature) *
           static_cast<double>(std::max(1, signature.numerator));
}

// Quarter notes from the session start to the head of `bar`.
double quartersToBar(int bar, const std::vector<TimeSignature>& map) {
    const auto normalized = normalizeMeterMap(map);
    const int target = std::max(1, bar);
    double quarters = 0.0;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        const int from = normalized[i].bar;
        if (from >= target) break;
        // This signature runs until the next change, or until the target.
        const int until = (i + 1 < normalized.size())
            ? std::min(normalized[i + 1].bar, target) : target;
        quarters += quartersPerBar(normalized[i]) *
                    static_cast<double>(until - from);
    }
    return quarters;
}

namespace {

double quartersToPosition(int bar, int beat,
                          const std::vector<TimeSignature>& meter) {
    const auto signature = signatureAtBar(meter, std::max(1, bar));
    return quartersToBar(bar, meter) +
           (std::max(1, beat) - 1) * quartersPerBeat(signature);
}

// The tempo map as (quarter position, bpm), which is the axis tempo actually
// integrates along. Resolving the musical anchors once here is also what makes
// a meter change move the tempo events after it.
struct TempoSegment {
    double quarters = 0.0;
    double bpm = 120.0;
};

std::vector<TempoSegment> tempoSegments(
    const std::vector<TempoChange>& tempo,
    const std::vector<TimeSignature>& meter) {
    const auto normalized = normalizeTempoMap(tempo);
    std::vector<TempoSegment> segments;
    segments.reserve(normalized.size());
    for (const auto& change : normalized) {
        const double at = quartersToPosition(change.bar, change.beat, meter);
        // A meter change can collapse two anchors onto one position; the later
        // one wins, the same rule the maps themselves use.
        if (!segments.empty() && at <= segments.back().quarters) {
            segments.back().bpm = change.bpm;
            continue;
        }
        segments.push_back(TempoSegment{at, change.bpm});
    }
    if (segments.empty()) segments.push_back(TempoSegment{0.0, 120.0});
    segments.front().quarters = 0.0;
    return segments;
}

double samplesPerQuarterAt(double bpm, double sampleRate) {
    if (!(sampleRate > 0.0) || !(bpm > 0.0)) return 0.0;
    return sampleRate * 60.0 / bpm;
}

// Samples elapsed from the session start to `quarters`, summed segment by
// segment. Tempo is constant within a segment, so each one is a multiply.
double quartersToSamples(double quarters, double sampleRate,
                         const std::vector<TempoSegment>& segments) {
    if (!(quarters > 0.0)) return 0.0;
    double samples = 0.0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const double from = segments[i].quarters;
        if (from >= quarters) break;
        const double until = (i + 1 < segments.size())
            ? std::min(segments[i + 1].quarters, quarters) : quarters;
        samples += (until - from) *
                   samplesPerQuarterAt(segments[i].bpm, sampleRate);
    }
    return samples;
}

double samplesToQuarters(double samples, double sampleRate,
                         const std::vector<TempoSegment>& segments) {
    if (!(samples > 0.0)) return 0.0;
    double quarters = 0.0;
    double consumed = 0.0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const double perQuarter =
            samplesPerQuarterAt(segments[i].bpm, sampleRate);
        if (perQuarter <= 0.0) break;
        const bool last = i + 1 >= segments.size();
        if (!last) {
            const double segmentQuarters =
                segments[i + 1].quarters - segments[i].quarters;
            const double segmentSamples = segmentQuarters * perQuarter;
            if (samples - consumed >= segmentSamples) {
                consumed += segmentSamples;
                quarters += segmentQuarters;
                continue;
            }
        }
        quarters += (samples - consumed) / perQuarter;
        break;
    }
    return quarters;
}

} // namespace

BarsBeats barsBeatsAtSample(int64_t sample, double sampleRate,
                            const std::vector<TempoChange>& tempo,
                            const std::vector<TimeSignature>& map) {
    if (!(sampleRate > 0.0) || sample <= 0) return BarsBeats{};
    const auto normalized = normalizeMeterMap(map);
    const auto segments = tempoSegments(tempo, normalized);

    // Integer ticks from here on. The sample position came from a rounded
    // conversion, so reading it back lands a hair either side of a bar line —
    // and flooring a hair short of one puts the playhead on the last beat of
    // the previous bar. Ticks per bar is a whole number for every valid
    // signature, so this boundary is exact instead of nearly exact.
    constexpr int64_t kTicksPerQuarter = kTicksPerBeat;
    const double quarters = samplesToQuarters(static_cast<double>(sample),
                                              sampleRate, segments);
    int64_t ticks = std::llround(quarters * kTicksPerQuarter);

    int bar = 1;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        const int64_t barTicks = std::llround(
            quartersPerBar(normalized[i]) * kTicksPerQuarter);
        if (barTicks <= 0) break;
        const bool last = i + 1 >= normalized.size();
        if (!last) {
            const int64_t barsInSegment =
                normalized[i + 1].bar - normalized[i].bar;
            const int64_t segmentTicks = barTicks * barsInSegment;
            if (ticks >= segmentTicks) {
                ticks -= segmentTicks;
                bar += static_cast<int>(barsInSegment);
                continue;
            }
        }
        bar += static_cast<int>(ticks / barTicks);
        ticks %= barTicks;
        break;
    }

    const auto signature = signatureAtBar(normalized, bar);
    const int64_t beatTicks =
        std::llround(quartersPerBeat(signature) * kTicksPerQuarter);
    if (beatTicks <= 0) return BarsBeats{bar, 1, 0};
    const int64_t beat = ticks / beatTicks;
    const int64_t intoBeat = ticks % beatTicks;
    const int tick = static_cast<int>(
        std::clamp<int64_t>(intoBeat * kTicksPerBeat / beatTicks, 0,
                            kTicksPerBeat - 1));
    return BarsBeats{bar, static_cast<int>(beat) + 1, tick};
}

int64_t sampleAtBarsBeats(const BarsBeats& position, double sampleRate,
                          const std::vector<TempoChange>& tempo,
                          const std::vector<TimeSignature>& map) {
    if (!(sampleRate > 0.0)) return 0;
    const auto normalized = normalizeMeterMap(map);
    const auto segments = tempoSegments(tempo, normalized);
    const int bar = std::max(1, position.bar);
    const auto signature = signatureAtBar(normalized, bar);
    const double perBeat = quartersPerBeat(signature);
    const double quarters =
        quartersToPosition(bar, position.beat, normalized) +
        (position.tick / static_cast<double>(kTicksPerBeat)) * perBeat;
    return static_cast<int64_t>(
        std::llround(quartersToSamples(quarters, sampleRate, segments)));
}

int64_t sampleAtBar(int bar, double sampleRate,
                    const std::vector<TempoChange>& tempo,
                    const std::vector<TimeSignature>& map) {
    return sampleAtBarsBeats(BarsBeats{std::max(1, bar), 1, 0}, sampleRate,
                             tempo, map);
}

double samplesPerBeatAtBar(int bar, double sampleRate,
                           const std::vector<TempoChange>& tempo,
                           const std::vector<TimeSignature>& map) {
    // Measured between the two ends rather than multiplied out: a tempo change
    // inside the beat makes it a different length from its neighbours, and
    // only the difference is right.
    const int at = std::max(1, bar);
    const int64_t start = sampleAtBarsBeats(BarsBeats{at, 1, 0}, sampleRate,
                                            tempo, map);
    const int64_t end = sampleAtBarsBeats(BarsBeats{at, 2, 0}, sampleRate,
                                          tempo, map);
    return static_cast<double>(end - start);
}

double samplesPerBarAtBar(int bar, double sampleRate,
                          const std::vector<TempoChange>& tempo,
                          const std::vector<TimeSignature>& map) {
    const int at = std::max(1, bar);
    const int64_t start = sampleAtBar(at, sampleRate, tempo, map);
    const int64_t end = sampleAtBar(at + 1, sampleRate, tempo, map);
    return static_cast<double>(end - start);
}

double bpmAt(const std::vector<TempoChange>& tempo,
             const std::vector<TimeSignature>& meter, int bar, int beat) {
    const auto normalized = normalizeMeterMap(meter);
    const auto segments = tempoSegments(tempo, normalized);
    const double at = quartersToPosition(bar, beat, normalized);
    double bpm = segments.front().bpm;
    for (const auto& segment : segments) {
        if (segment.quarters > at + 1e-9) break;
        bpm = segment.bpm;
    }
    return bpm;
}

// ─── Single-tempo overloads ────────────────────────────────────────────────

BarsBeats barsBeatsAtSample(int64_t sample, double sampleRate, double bpm,
                            const std::vector<TimeSignature>& map) {
    return barsBeatsAtSample(sample, sampleRate, constantTempo(bpm), map);
}

int64_t sampleAtBarsBeats(const BarsBeats& position, double sampleRate,
                          double bpm, const std::vector<TimeSignature>& map) {
    return sampleAtBarsBeats(position, sampleRate, constantTempo(bpm), map);
}

int64_t sampleAtBar(int bar, double sampleRate, double bpm,
                    const std::vector<TimeSignature>& map) {
    return sampleAtBar(bar, sampleRate, constantTempo(bpm), map);
}

double samplesPerBeatAtBar(int bar, double sampleRate, double bpm,
                           const std::vector<TimeSignature>& map) {
    return samplesPerBeatAtBar(bar, sampleRate, constantTempo(bpm), map);
}

double samplesPerBarAtBar(int bar, double sampleRate, double bpm,
                          const std::vector<TimeSignature>& map) {
    return samplesPerBarAtBar(bar, sampleRate, constantTempo(bpm), map);
}

} // namespace dave::document
