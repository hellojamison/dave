#include "application/MainComponent.h"

namespace dave::application {

MainComponent::MainComponent() {
    // Use CharPointer_UTF8 so the em-dash is interpreted correctly (a raw
    // const char* with bytes > 127 trips JUCE's ASCII assertion).
    header_.setText(juce::CharPointer_UTF8("Dave \xe2\x80\x94 Phase 0"),
                    juce::dontSendNotification);
    header_.setFont(juce::Font(28.0f, juce::Font::bold));
    header_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(header_);

    playButton_.setButtonText("Play 440 Hz");
    playButton_.onClick = [this] {
        playing_ = !playing_;
        if (sine_) {
            // UI-thread parameter edit: RT-safe atomic store inside SineNode.
            sine_->setGain(playing_ ? 0.2 : 0.0);
        }
        playButton_.setButtonText(playing_ ? "Stop" : "Play 440 Hz");
    };
    addAndMakeVisible(playButton_);

    setSize(480, 200);

    // --- Audio device setup ----------------------------------------------
    // Open the default output device at 48k, stereo. This is the same call
    // path the full DAW will use; Phase 0 just skips the device picker UI.
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.sampleRate = 48000.0;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = true;
    setup.outputChannels = 2;

    deviceManager_.initialise(0, 2, nullptr, true, {}, &setup);

    // Positive confirmation the device opened — this is the proof the RT path
    // (device -> player -> graph source -> compiled graph -> sine node) is live.
    auto* dev = deviceManager_.getCurrentAudioDevice();
    if (dev != nullptr) {
        juce::Logger::writeToLog("Dave: audio device \"" + dev->getName()
            + "\" opened @ " + juce::String(dev->getCurrentSampleRate()) + " Hz, "
            + juce::String(dev->getCurrentBufferSizeSamples()) + " samples/block, "
            + juce::String(dev->getActiveOutputChannels().countNumberOfSetBits()) + " out ch");
    } else {
        juce::Logger::writeToLog("Dave: no audio device available");
    }

    // Wire: AudioDeviceManager -> AudioSourcePlayer -> GraphAudioSource ->
    // CompiledGraph -> SineNode.
    rebuildGraph();
    audioSourcePlayer_.setSource(&graphSource_);
    deviceManager_.addAudioCallback(&audioSourcePlayer_);
}

MainComponent::~MainComponent() {
    deviceManager_.removeAudioCallback(&audioSourcePlayer_);
    audioSourcePlayer_.setSource(nullptr);
}

void MainComponent::rebuildGraph() {
    // Build on the UI thread, then publish. The source prepares the graph and
    // swaps it in atomically.
    sine_ = std::make_shared<engine::SineNode>();
    sine_->setFrequency(440.0);
    sine_->setGain(0.0); // start silent; the button turns it on

    auto graph = std::make_unique<engine::CompiledGraph>();
    graph->addNode(sine_); // shared with sine_ so the UI can edit params

    graphSource_.setGraph(std::move(graph), 48000.0, 256);
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff1e1e22));
}

void MainComponent::resized() {
    auto r = getLocalBounds().reduced(24);
    header_.setBounds(r.removeFromTop(60));
    playButton_.setBounds(r.withSizeKeepingCentre(180, 40));
}

} // namespace dave::application
