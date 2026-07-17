#pragma once

#include <cstdint>

namespace dave::engine {

// Sample position / transport time, threaded through the graph each block.
// Written by the Transport (UI thread sets play state; RT thread advances the
// sample counter). Nodes that care about time (AudioClipNode) read it; nodes
// that don't (SineNode) ignore it.
struct TimeInfo {
    double sampleRate = 48000.0;
    int64_t samplePos = 0;        // absolute timeline position, in samples
    double bpm = 120.0;
    double ppqPosition = 0.0;     // musical position (derived from tempo map later)
    bool isPlaying = false;
    bool isRecording = false;
    bool isLooping = false;
    int64_t loopStart = 0;
    int64_t loopEnd = 0;
};

// A non-owning view onto a multi-channel block of audio. Pointers are into
// buffers owned by CompiledGraph (allocated in compile(), freed in dtor). The
// RT thread never allocates.
//
// Layout is **non-interleaved** (planar): channels[channel][sample].
struct AudioBus {
    float* const* channels = nullptr;
    int numChannels = 0;
    int numSamples = 0;

    bool isValid() const { return channels != nullptr && numChannels > 0; }

    // Zero every channel (used to clear a buffer before summing into it).
    void clear() {
        for (int c = 0; c < numChannels; ++c) {
            for (int i = 0; i < numSamples; ++i) {
                channels[c][i] = 0.0f;
            }
        }
    }
};

} // namespace dave::engine
