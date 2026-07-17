#pragma once

#include "engine/graph/CompiledGraph.h"
#include "engine/nodes/SineNode.h"
#include "platform/GraphAudioSource.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace dave::application {

// MainComponent owns the audio device manager, the graph source, and the UI.
//
// In Phase 0 the UI is intentionally tiny: a header label and a play/stop
// button that toggles the sine node. The point is to prove the RT-thread
// contract end to end (UI -> atomic swap -> graph -> audio callback -> sound),
// not to look like a DAW yet.
class MainComponent : public juce::Component {
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // Build the Phase 0 graph (a single SineNode) and hand it to the source.
    void rebuildGraph();

    juce::AudioDeviceManager deviceManager_;
    juce::AudioSourcePlayer audioSourcePlayer_;
    platform::GraphAudioSource graphSource_;

    juce::Label header_;
    juce::TextButton playButton_;
    bool playing_ = false;

    // The sine node, shared with the live graph so the UI can edit its
    // parameters (RT-safe atomics) while the graph owns it.
    std::shared_ptr<engine::SineNode> sine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace dave::application
