#pragma once

#include "engine/graph/CompiledGraph.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace dave::platform {

// GraphAudioSource adapts a dave::engine::CompiledGraph to JUCE's
// AudioSourceChannelInfo callback. This is the single seam between the host
// audio device and our node graph.
//
// Threading:
// - getNextAudioBlock() runs on the RT audio thread.
// - setGraph() runs on the UI thread and publishes a new CompiledGraph via an
//   atomic pointer; the RT thread picks it up at the next block boundary.
//   Ownership of the old graph is transferred back to the UI thread for
//   deletion (see pendingDisposal_).
//
// RT rules respected: no allocation, no locks, a single relaxed atomic load.
class GraphAudioSource : public juce::AudioSource {
public:
    GraphAudioSource() = default;
    ~GraphAudioSource() override;

    // --- UI thread ---------------------------------------------------------
    // Publish a new compiled graph. The previous graph (if any) is scheduled
    // for deletion on the UI thread via releaseResources() / the destructor.
    void setGraph(std::unique_ptr<engine::CompiledGraph> graph, double sampleRate, int maxBlock);

    // --- AudioSource (called by JUCE) -------------------------------------
    void prepareToPlay(int /*samplesPerBlockExpected*/, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;

private:
    // Current graph the RT thread is rendering. Owned here.
    std::unique_ptr<engine::CompiledGraph> graph_;
    // Graph swapped out from under the RT thread; delete on the UI thread.
    std::unique_ptr<engine::CompiledGraph> pendingDisposal_;

    double sampleRate_ = 44100.0;
    std::atomic<bool> prepared_{false};
};

} // namespace dave::platform
