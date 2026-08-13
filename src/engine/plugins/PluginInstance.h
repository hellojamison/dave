// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/Types.h"
#include "engine/plugins/PluginHost.h"

#include <memory>
#include <string>

// VST3 SDK types are intentionally NOT forward-declared here (you can't
// forward-declare them with nested-name-specifiers). All SDK-owned state is
// hidden behind the Impl struct in the .cpp, so this header stays SDK-free —
// the rest of the engine (PluginNode, graph, UI) never sees VST3 types.

namespace dave::engine {

// One MIDI event to hand a plugin for the current block. A plain POD so this
// header stays SDK-free: the translation to Steinberg's Event union happens
// inside PluginInstance.cpp, where the SDK headers already live.
//
// `sampleOffset` is BLOCK-RELATIVE (0 .. numSamples-1), which is what VST3
// wants. The caller slices absolute timeline positions down to block-relative
// ones; see InstrumentNode::sliceBlock.
enum class MidiEventType : uint8_t { NoteOff = 0, NoteOn = 1 };

struct MidiEvent {
    int32_t sampleOffset = 0;
    MidiEventType type = MidiEventType::NoteOn;
    uint8_t channel = 0;    // 0..15
    uint8_t pitch = 60;     // 0..127
    uint8_t velocity = 100; // 0..127
};

// PluginInstance wraps ONE loaded VST3 plugin: the component (audio processor)
// + controller (params/editor), with buses set up and a ProcessData ready.
//
// This is the single seam between Dave and the VST3 SDK's lifecycle rules.
// Threading:
//   - load()/prepare()/unload()/controller queries: MAIN thread only.
//   - process(): RT thread only. Calls IAudioProcessor::process with a
//     pre-built ProcessData. No allocation, no locks.
//
// We deliberately DON'T expose raw VST3 types — the graph talks to us through
// process(in, out, n). That keeps PluginNode (and the rest of the engine)
// SDK-free, and lets us add AU/CLAP instances behind the same interface later.
class PluginInstance {
public:
    PluginInstance();
    ~PluginInstance();

    PluginInstance(const PluginInstance&) = delete;
    PluginInstance& operator=(const PluginInstance&) = delete;

    // Load + initialize a plugin described by `descriptor`. Main thread.
    // Sets up 1 stereo in / 1 stereo out (the common FX case). Returns false
    // on any failure (with the reason in lastError).
    bool load(const PluginDescriptor& descriptor, double sampleRate, int maxBlock);

    // Configure for a new sample rate / block size. Main thread. Cheap if
    // unchanged.
    void prepare(double sampleRate, int maxBlock);

    // RT thread. Processes `n` samples: reads from `in` (deinterleaved, stereo),
    // writes processed audio to `out` (same layout), and delivers `numEvents`
    // MIDI events on event input bus 0. Passes `time` through to the plugin's
    // ProcessContext. No allocation: events are copied into a pre-allocated
    // list, and anything beyond its capacity is dropped rather than grown.
    //
    // `in` may be null for an instrument (a generator with no audio input);
    // pass numChannels for the OUTPUT in that case.
    void process(const float* const* in, float* const* out, int numChannels,
                 int numSamples, const MidiEvent* events, int numEvents,
                 const TimeInfo& time);

    // Audio-only overload — what every effect in the chain uses. Kept so
    // PluginNode and the existing FX path are unaffected by MIDI.
    void process(const float* const* in, float* const* out, int numChannels,
                 int numSamples, const TimeInfo& time) {
        process(in, out, numChannels, numSamples, nullptr, 0, time);
    }

    // Does the plugin have an event input bus? True for instruments and for
    // effects that take MIDI (an arpeggiator, a MIDI-triggered gate). Only
    // meaningful once loaded.
    bool acceptsMidi() const { return acceptsMidi_; }
    uint32_t latencySamples() const;
    bool consumeLatencyChange();
    bool latencyChangePending() const;

    // Unload: deactivate, terminate processor, release controller/component.
    // Main thread. Idempotent.
    void unload();

    bool isLoaded() const { return loaded_; }
    const std::string& name() const { return name_; }
    const std::string& lastError() const { return lastError_; }

    // Access the edit controller as an opaque FUnknown* (for PluginEditor to
    // call createView). Returns nullptr if no controller. The pointer is only
    // valid while the instance is loaded. Main thread.
    void* editControllerAsUnknown() const;

    // Save/restore the plugin's full state (all params + internal). getState
    // returns a base64-encoded binary chunk; setState applies it. Main thread.
    // Used by project persistence so plugin settings survive save/load.
    std::string getStateBase64() const;
    bool setStateBase64(const std::string& b64);

private:
    struct Impl; // hides SDK-owned types from the header
    std::unique_ptr<Impl> p_;
    bool loaded_ = false;
    bool acceptsMidi_ = false;
    std::string name_;
    std::string lastError_;
};

} // namespace dave::engine
