// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/nodes/InstrumentNode.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace dave::engine {

// The instrument a MIDI track has until it is given one: a small polyphonic
// sine so notes are audible the moment a clip lands, rather than a silent
// row that looks broken. Built on InstrumentNode's event slicing, so it
// chases, seeks and stops exactly the way a plugin would — it just renders
// the events itself instead of handing them to one.
class SineSynthNode : public InstrumentNode {
public:
    void prepare(double sampleRate, int maxBlock) override {
        InstrumentNode::prepare(sampleRate, maxBlock);
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (auto& v : voices_) v = Voice{};
    }

    void process(NodeProcessContext& ctx) override {
        const TimeInfo time = (ctx.time != nullptr) ? *ctx.time : TimeInfo{};
        const int n = ctx.numSamples;
        const int count = eventsForBlock(time.samplePos, n, time.isPlaying,
                                         events_.data(),
                                         static_cast<int>(events_.size()));
        const int outCh = ctx.output.numChannels;
        int ei = 0;
        for (int i = 0; i < n; ++i) {
            while (ei < count && events_[ei].sampleOffset <= i) {
                apply(events_[ei]);
                ++ei;
            }
            float s = 0.0f;
            for (auto& v : voices_) {
                if (v.level <= 0.0f && !v.held) continue;
                // A short attack and a longer release, so notes neither
                // click on nor cut off dead.
                const float target = v.held ? v.velocity : 0.0f;
                v.level += (target - v.level) * (v.held ? attack_ : release_);
                if (!v.held && v.level < 1e-4f) { v.level = 0.0f; continue; }
                s += std::sin(v.phase) * v.level * 0.18f;
                v.phase += v.step;
                if (v.phase > kTwoPi) v.phase -= kTwoPi;
            }
            for (int c = 0; c < outCh; ++c) ctx.output.channels[c][i] = s;
        }
        // Events past the block's end (none expected) still apply.
        for (; ei < count; ++ei) apply(events_[ei]);
    }

private:
    static constexpr float kTwoPi = 6.28318530718f;
    struct Voice {
        bool held = false;
        uint8_t pitch = 0;
        uint8_t channel = 0;
        float velocity = 0.0f;
        float level = 0.0f;
        float phase = 0.0f;
        float step = 0.0f;
    };

    void apply(const MidiEvent& e) {
        if (e.type == MidiEventType::NoteOn) {
            Voice* slot = nullptr;
            for (auto& v : voices_) {
                if (!v.held && v.level <= 0.0f) { slot = &v; break; }
            }
            if (slot == nullptr) {
                // Steal the quietest.
                slot = &voices_[0];
                for (auto& v : voices_) if (v.level < slot->level) slot = &v;
            }
            slot->held = true;
            slot->pitch = e.pitch;
            slot->channel = e.channel;
            slot->velocity = static_cast<float>(e.velocity) / 127.0f;
            slot->phase = 0.0f;
            const double hz = 440.0 * std::pow(2.0, (e.pitch - 69) / 12.0);
            slot->step = static_cast<float>(kTwoPi * hz / sampleRate_);
        } else if (e.type == MidiEventType::NoteOff) {
            for (auto& v : voices_) {
                if (v.held && v.pitch == e.pitch && v.channel == e.channel) {
                    v.held = false;
                }
            }
        }
    }

    double sampleRate_ = 48000.0;
    // Per-sample one-pole coefficients: ~3 ms attack, ~60 ms release at 48k.
    float attack_ = 0.007f;
    float release_ = 0.00035f;
    std::array<Voice, 16> voices_{};
    std::array<MidiEvent, kMaxEventsPerBlock> events_{};
};

} // namespace dave::engine
