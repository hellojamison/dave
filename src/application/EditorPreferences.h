// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "application/TakeNaming.h"

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
