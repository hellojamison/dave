// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/MusicalTime.h"
#include "engine/graph/Node.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dave::engine {

// A click on every beat, accented on the bar's downbeat, following the tempo
// and meter maps. The click is synthesised — a short decaying sine ping — so no
// sample file is needed. It sums into the output like any source; it is silent
// unless enabled and the transport is rolling.
//
// Enable state is atomic so the UI can toggle it without a graph rebuild. The
// maps and rate are set once at build time (RT reads them, never mutates).
class MetronomeNode : public Node {
public:
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
        clickLen_ = static_cast<int>(0.035 * sampleRate_);  // ~35 ms ping
    }
    void setEnabled(bool enabled) {
        enabled_.store(enabled, std::memory_order_relaxed);
    }
    void setGain(float gain) { gain_.store(gain, std::memory_order_relaxed); }
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

        // Collect the beat onsets that fall in this block. A block is a handful
        // of milliseconds, so this is one or two beats — no allocation.
        constexpr int kMaxStarts = 16;
        int startOffset[kMaxStarts];
        bool startAccent[kMaxStarts];
        int starts = 0;

        document::BarsBeats bb =
            document::barsBeatsAtSample(pos, sampleRate_, tempo_, meter_);
        document::BarsBeats beat{bb.bar, bb.beat, 0};
        int64_t beatSample =
            document::sampleAtBarsBeats(beat, sampleRate_, tempo_, meter_);
        int guard = 0;
        while (beatSample < pos + n && guard++ < 4096 && starts < kMaxStarts) {
            if (beatSample >= pos) {
                startOffset[starts] = static_cast<int>(beatSample - pos);
                startAccent[starts] = beat.beat == 1;
                ++starts;
            }
            const auto sig = document::signatureAtBar(meter_, beat.bar);
            beat.beat += 1;
            if (beat.beat > std::max(1, sig.numerator)) {
                beat.bar += 1;
                beat.beat = 1;
            }
            const int64_t next =
                document::sampleAtBarsBeats(beat, sampleRate_, tempo_, meter_);
            if (next <= beatSample) break;  // never stall
            beatSample = next;
        }

        int si = 0;
        for (int i = 0; i < n; ++i) {
            if (si < starts && startOffset[si] == i) {
                active_ = clickLen_;
                phase_ = 0.0f;
                freq_ = startAccent[si] ? 1568.0f : 988.0f;  // G6 vs B5
                ++si;
            }
            float s = 0.0f;
            if (active_ > 0) {
                const float env =
                    static_cast<float>(active_) / static_cast<float>(clickLen_);
                s = std::sin(phase_) * env * env * gain;
                phase_ += 2.0f * 3.14159265358979f * freq_ /
                          static_cast<float>(sampleRate_);
                if (phase_ > 6.28318530718f) phase_ -= 6.28318530718f;
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
    int clickLen_ = 1680;  // samples (~35 ms at 48k)
    int active_ = 0;       // samples left in the current click
    float phase_ = 0.0f;
    float freq_ = 988.0f;
};

} // namespace dave::engine
