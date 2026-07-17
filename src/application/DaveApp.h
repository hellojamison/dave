#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dave::application {

class MainComponent;

// DaveApp is the JUCEApplication entry point. It owns the main window and
// hands shutdown back to JUCE. The app's substantive state lives in
// MainComponent.
class DaveApp : public juce::JUCEApplication {
public:
    DaveApp() = default;

    const juce::String getApplicationName() override { return "Dave"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& /*commandLine*/) override;
    void shutdown() override;

    void anotherInstanceStarted(const juce::String&) override {}

private:
    std::unique_ptr<MainComponent> mainComponent_;
    std::unique_ptr<juce::DocumentWindow> mainWindow_;
};

} // namespace dave::application
