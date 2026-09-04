// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/MusicalTime.h"
#include "engine/graph/Node.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dave::engine {

// A click on every beat, accented on the bar's downbeat, following the tempo
// and meter maps. The click is synthesised — a short decaying tone — so no
// sample file is needed. It sums into the output like any source; it is silent
// unless enabled and the transport is rolling.
//
// Every setting is atomic so the UI can change it without a graph rebuild.
// The maps and rate are set once at build time (RT reads them, never mutates).
class MetronomeNode : public Node {
public:
    // Three synthesised voices. Beep is a clean sine ping; Wood is a short,
    // darker knock with a touch of second harmonic; Click is a bright tick.
    enum class Sound : int { Beep = 0, Wood = 1, Click = 2 };

    MetronomeNode() : Node("metronome") {}

    int numOutputPins() const override { return 1; }
    int channelsPerPin() const override { return 2; }

    // --- UI thread ---------------------------------------------------------
    void configure(double sampleRate,
                   std::vector<document::TempoChange> tempo,
                   std::vector<document::TimeSignature> meter) {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        tempo_ = std::move(tempo);
        meter_ = std::move(meter);
    }
    void setEnabled(bool enabled) {
        enabled_.store(enabled, std::memory_order_relaxed);
    }
    void setGain(float gain) { gain_.store(gain, std::memory_order_relaxed); }
    // Off: every beat clicks at the same pitch and level, downbeat included.
    void setAccent(bool accent) {
        accent_.store(accent, std::memory_order_relaxed);
    }
    // Linear multiplier on the accented downbeat (1 = same level as a beat).
    void setAccentBoost(float boost) {
        accentBoost_.store(boost, std::memory_order_relaxed);
    }
    void setSound(Sound sound) {
        sound_.store(static_cast<int>(sound), std::memory_order_relaxed);
    }
    // A softer tick halfway between beats.
    void setSubdivide(bool eighths) {
        subdivide_.store(eighths, std::memory_order_relaxed);
    }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    void prepare(double /*sampleRate*/, int /*maxBlock*/) override {}

    // --- RT thread ---------------------------------------------------------
    void process(NodeProcessContext& ctx) override {
        const int n = ctx.numSamples;
        const int outCh = ctx.output.numChannels;
        if (!enabled_.load(std::memory_order_relaxed)) return;
        const bool playing = ctx.time && ctx.time->isPlaying;
        if (!playing) { active_ = 0; return; }
        const int64_t pos = ctx.time->samplePos;
        const float gain = gain_.load(std::memory_order_relaxed);
        const bool accent = accent_.load(std::memory_order_relaxed);
        const float boost = accentBoost_.load(std::memory_order_relaxed);
        const Sound sound =
            static_cast<Sound>(sound_.load(std::memory_order_relaxed));
        const bool subdivide = subdivide_.load(std::memory_order_relaxed);

        // Collect the onsets that fall in this block. A block is a handful of
        // milliseconds, so this is one or two — no allocation.
        enum Kind : uint8_t { Beat, Downbeat, Sub };
        constexpr int kMaxStarts = 16;
        int startOffset[kMaxStarts];
        Kind startKind[kMaxStarts];
        int starts = 0;
        auto add = [&](int64_t at, Kind kind) {
            if (at >= pos && at < pos + n && starts < kMaxStarts) {
                startOffset[starts] = static_cast<int>(at - pos);
                startKind[starts] = kind;
                ++starts;
            }
        };

        document::BarsBeats bb =
            document::barsBeatsAtSample(pos, sampleRate_, tempo_, meter_);
        document::BarsBeats beat{bb.bar, bb.beat, 0};
        int64_t beatSample =
            document::sampleAtBarsBeats(beat, sampleRate_, tempo_, meter_);
        int guard = 0;
        while (beatSample < pos + n && guard++ < 4096) {
            add(beatSample, accent && beat.beat == 1 ? Downbeat : Beat);
            const auto sig = document::signatureAtBar(meter_, beat.bar);
            beat.beat += 1;
            if (beat.beat > std::max(1, sig.numerator)) {
                beat.bar += 1;
                beat.beat = 1;
            }
            const int64_t next =
                document::sampleAtBarsBeats(beat, sampleRate_, tempo_, meter_);
            if (next <= beatSample) break;  // never stall
            if (subdivide) add(beatSample + (next - beatSample) / 2, Sub);
            beatSample = next;
        }
        // Onsets were appended beat, sub, beat… which is already in time
        // order, so the per-sample walk below can consume them front to back.

        const float twoPi = 6.28318530718f;
        int si = 0;
        for (int i = 0; i < n; ++i) {
            if (si < starts && startOffset[si] == i) {
                const Kind kind = startKind[si];
                // Length, pitch and level per voice. The downbeat rings a
                // fourth higher; a subdivision is softer and a touch lower.
                float ms = 35.0f, hz = 988.0f;
                switch (sound) {
                    case Sound::Beep: ms = 35.0f; hz = 988.0f; break;
                    case Sound::Wood: ms = 14.0f; hz = 720.0f; break;
                    case Sound::Click: ms = 5.0f; hz = 3600.0f; break;
                }
                level_ = gain;
                if (kind == Downbeat) { hz *= 1.5874f; level_ *= boost; }
                if (kind == Sub) { hz *= 0.8f; level_ *= 0.55f; }
                clickLen_ = std::max(1, static_cast<int>(ms * 0.001 * sampleRate_));
                active_ = clickLen_;
                phase_ = 0.0f;
                freq_ = hz;
                ++si;
            }
            float s = 0.0f;
            if (active_ > 0) {
                const float env =
                    static_cast<float>(active_) / static_cast<float>(clickLen_);
                float tone = std::sin(phase_);
                if (sound == Sound::Wood) tone += 0.35f * std::sin(2.0f * phase_);
                // A steeper envelope for the percussive voices.
                const float shaped = sound == Sound::Beep ? env * env
                                                          : env * env * env;
                s = tone * shaped * level_;
                phase_ += twoPi * freq_ / static_cast<float>(sampleRate_);
                if (phase_ > twoPi) phase_ -= twoPi;
                --active_;
            }
            for (int c = 0; c < outCh; ++c) ctx.output.channels[c][i] = s;
        }
    }

private:
    double sampleRate_ = 48000.0;
    std::vector<document::TempoChange> tempo_;
    std::vector<document::TimeSignature> meter_;
    std::atomic<bool> enabled_{false};
    std::atomic<float> gain_{0.5f};
    std::atomic<bool> accent_{true};
    std::atomic<float> accentBoost_{2.0f};
    std::atomic<int> sound_{0};
    std::atomic<bool> subdivide_{false};
    int clickLen_ = 1680;  // samples of the current click
    int active_ = 0;       // samples left in the current click
    float phase_ = 0.0f;
    float freq_ = 988.0f;
    float level_ = 0.5f;
};

} // namespace dave::engine
