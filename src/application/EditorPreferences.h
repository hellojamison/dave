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
    // Crossfade defaults: the shape both sides take across an overlap (equal
    // power is the constant-power default), and the length of the auto-fade on
    // the clip's free edge when F crossfades an overlap.
    document::FadeShape defaultCrossfadeShape = document::FadeShape::EqualPower;
    int defaultCrossfadeMs = 20;
    // Pre-roll rolls playback in this far ahead of the cursor before the start
    // point; post-roll is the tail kept rolling past it. Milliseconds.
    bool preRollEnabled = false;
    int preRollMs = 2000;
    bool postRollEnabled = false;
    int postRollMs = 2000;
    // Metronome: click level in dB, whether the downbeat is accented, and
    // whether it clicks only while recording (silent on plain playback).
    int metronomeGainDb = -6;
    bool metronomeAccent = true;
    bool metronomeOnlyWhenRecording = false;
    // How much louder the accented downbeat is, in dB.
    int metronomeAccentDb = 6;
    // 0 = Beep (sine ping), 1 = Wood (short block), 2 = Click (tick).
    int metronomeSound = 0;
    // A softer tick on the off-eighths between beats.
    bool metronomeEighths = false;
    // Bars of click before a punch from a standing start; 0 = none.
    int metronomeCountInBars = 0;

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
