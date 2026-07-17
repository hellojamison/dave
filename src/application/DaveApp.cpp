#include "application/DaveApp.h"
#include "application/MainComponent.h"

namespace dave::application {

void DaveApp::initialise(const juce::String& /*commandLine*/) {
    mainComponent_ = std::make_unique<MainComponent>();

    mainWindow_ = std::make_unique<juce::DocumentWindow>(
        getApplicationName(),
        juce::Colour(0xff1e1e22),
        juce::DocumentWindow::allButtons);

    mainWindow_->setContentOwned(mainComponent_.release(), true);
    mainWindow_->setResizable(false, false);
    mainWindow_->centreWithSize(480, 200);
    mainWindow_->setVisible(true);
}

void DaveApp::shutdown() {
    mainWindow_.reset();
}

} // namespace dave::application

// This macro expands to the app's main() entry point. Must appear at global
// scope, exactly once, in a .cpp (not a header).
START_JUCE_APPLICATION(dave::application::DaveApp)
