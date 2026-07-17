#include "platform/GraphAudioSource.h"

namespace dave::platform {

GraphAudioSource::~GraphAudioSource() {
    // Both graphs are unique_ptr; destruction happens here on the UI thread.
    // (releaseResources is also called by JUCE on teardown, but this is the
    // final safety net.)
}

void GraphAudioSource::setGraph(std::unique_ptr<engine::CompiledGraph> graph,
                                double sampleRate, int maxBlock) {
    // Called on the UI thread. Prepare the new graph first so the RT thread
    // sees a fully-prepared CompiledGraph the instant the pointer swaps.
    if (graph)
        graph->prepareAll(sampleRate, maxBlock);

    // Hold the previous graph for disposal off the RT thread. JUCE guarantees
    // prepareToPlay/releaseResources calls are not concurrent with
    // getNextAudioBlock, but we keep explicit ownership control regardless.
    pendingDisposal_ = std::move(graph_);
    graph_ = std::move(graph);
}

void GraphAudioSource::prepareToPlay(int /*samplesPerBlockExpected*/, double sampleRate) {
    sampleRate_ = sampleRate;
    prepared_.store(true, std::memory_order_release);
}

void GraphAudioSource::releaseResources() {
    prepared_.store(false, std::memory_order_release);
    // Drop any graph swapped out while running. Done on the UI thread.
    pendingDisposal_.reset();
}

void GraphAudioSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) {
    // --- RT thread --------------------------------------------------------
    // Fill the buffer with silence first so a no-graph state is quiet.
    info.clearActiveBufferRegion();

    auto* graph = graph_.get();
    if (graph == nullptr || graph->empty())
        return;

    // Map JUCE's buffer view onto our ProcessContext. No allocation: we only
    // borrow the channel pointers already owned by the JUCE buffer.
    auto& buffer = *info.buffer;
    engine::TimeInfo time;
    time.sampleRate = sampleRate_;

    engine::ProcessContext ctx;
    ctx.numSamples = info.numSamples;
    ctx.sampleRate = sampleRate_;
    ctx.inChannels = buffer.getArrayOfReadPointers();
    ctx.outChannels = buffer.getArrayOfWritePointers();
    ctx.numInChannels = buffer.getNumChannels();
    ctx.numOutChannels = buffer.getNumChannels();
    ctx.time = &time;

    graph->process(ctx);
}

} // namespace dave::platform
