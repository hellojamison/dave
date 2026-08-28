// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "application/TakeNaming.h"
#include "document/Fade.h"

#include <filesystem>
#include <string>
#include <utility>

namespace dave::application {

struct EditorPreferences {
    bool transientNavigationEnabled = false;
    bool showTransientTicks = false;
    int transientSensitivity = 50;
    // Filename pattern for recorded takes. See TakeNaming.h for the tokens.
    std::string takeNamePattern = kDefaultTakeNamePattern;
    // Meter source and bar body, shared by every meter (see LevelMeter.h).
    bool meterPreFader = false;
    bool meterRmsBody = true;
    // Seconds the peak marker holds. Negative holds until cleared, which is
    // the shipped behaviour and so the default.
    float meterPeakHoldSeconds = -1.0f;
    // Whether the channel strip's meter sits below the fader (see LevelMeter.h).
    bool meterBelowFader = false;
    // Fade defaults. The F key applies these shapes to the fade it makes from
    // the selection, and this length to a whole-clip selection (click a clip,
    // press F, and it top-and-tails at this length); it is also the auto
    // de-click fade put on imported clips. Milliseconds, at the session rate.
    document::FadeShape defaultFadeInShape = document::FadeShape::Linear;
    document::FadeShape defaultFadeOutShape = document::FadeShape::Linear;
    int defaultFadeMs = 10;
    // Pre-roll rolls playback in this far ahead of the cursor before the start
    // point; post-roll is the tail kept rolling past it. Milliseconds.
    bool preRollEnabled = false;
    int preRollMs = 2000;
    bool postRollEnabled = false;
    int postRollMs = 2000;

    bool operator==(const EditorPreferences&) const = default;
};

std::filesystem::path defaultEditorPreferencesPath();

class EditorPreferencesStore {
public:
    explicit EditorPreferencesStore(
        std::filesystem::path path = defaultEditorPreferencesPath())
        : path_(std::move(path)) {}

    [[nodiscard]] EditorPreferences load() const noexcept;
    [[nodiscard]] bool save(const EditorPreferences& preferences) const noexcept;
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace dave::application
