// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <utility>

namespace dave::application {

enum class InputMode {
    Off,
    Default,
    Device,
};

struct AudioPreferences {
    // Empty means the system default output device.
    std::string outputDeviceName;

    // Recording is opt-in on first launch. Device mode requires an exact
    // device name; Default asks the platform for its current default input.
    InputMode inputMode = InputMode::Off;
    std::string inputDeviceName;

    // Positive values move a finished take earlier on the timeline to
    // compensate for measured round-trip capture latency. It is global
    // hardware calibration, not project state.
    int recordLatencyOffsetSamples = 0;

    bool operator==(const AudioPreferences&) const = default;
};

// Per-user platform location:
//   macOS:   ~/Library/Application Support/Dave/audio-preferences.json
//   Windows: %APPDATA%/Dave/audio-preferences.json
// Linux uses XDG_CONFIG_HOME (or ~/.config) for developer builds.
std::filesystem::path defaultAudioPreferencesPath();

// Small disk-backed store kept separate from AudioEngine so tests and future
// UI code can read preferences without opening or enumerating an audio device.
class AudioPreferencesStore {
public:
    explicit AudioPreferencesStore(
        std::filesystem::path path = defaultAudioPreferencesPath())
        : path_(std::move(path)) {}

    // Missing, malformed, unsupported, or incomplete files return the safe
    // first-launch defaults (input Off). Loading never throws.
    [[nodiscard]] AudioPreferences load() const noexcept;

    // Creates the parent directory when needed and replaces the JSON file.
    // Device mode without a device name is rejected rather than persisted as
    // an ambiguous request. Saving never throws.
    [[nodiscard]] bool save(const AudioPreferences& preferences) const noexcept;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace dave::application
