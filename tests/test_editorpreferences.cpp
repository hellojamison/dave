// SPDX-License-Identifier: GPL-3.0-or-later
#include "application/EditorPreferences.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace dave;

namespace {

struct TemporaryPreferences {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("dave-editor-preferences-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path path = directory / "editor-preferences.json";
    ~TemporaryPreferences() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

} // namespace

TEST_CASE("editor preferences default safely and round trip globally",
          "[transient][preferences]") {
    TemporaryPreferences temporary;
    application::EditorPreferencesStore store(temporary.path);
    REQUIRE(store.load() == application::EditorPreferences{});

    application::EditorPreferences preferences;
    preferences.transientNavigationEnabled = true;
    preferences.showTransientTicks = true;
    preferences.transientSensitivity = 73;
    // Fade defaults ride the same round trip; the whole-struct == below covers
    // them, so a shape or length that failed to serialize would fail here.
    preferences.defaultFadeInShape = document::FadeShape::EqualPower;
    preferences.defaultFadeOutShape = document::FadeShape::SCurve;
    preferences.defaultFadeMs = 42;
    preferences.preRollEnabled = true;
    preferences.preRollMs = 1500;
    preferences.postRollEnabled = true;
    preferences.postRollMs = 3000;
    preferences.metronomeGainDb = -12;
    preferences.metronomeAccent = false;
    preferences.metronomeOnlyWhenRecording = true;
    preferences.metronomeAccentDb = 9;
    preferences.metronomeSound = 2;
    preferences.metronomeEighths = true;
    preferences.metronomeCountInBars = 2;
    REQUIRE(store.save(preferences));
    REQUIRE(store.load() == preferences);
}

TEST_CASE("malformed and unsupported editor preferences return defaults",
          "[transient][preferences]") {
    TemporaryPreferences temporary;
    std::filesystem::create_directories(temporary.directory);
    {
        std::ofstream output(temporary.path);
        output << "not json";
    }
    application::EditorPreferencesStore store(temporary.path);
    REQUIRE(store.load() == application::EditorPreferences{});

    {
        std::ofstream output(temporary.path);
        output << R"({"format":"dave.editor-preferences/v2","transientNavigationEnabled":true,"showTransientTicks":true,"transientSensitivity":99})";
    }
    REQUIRE(store.load() == application::EditorPreferences{});
}

TEST_CASE("editor preference sensitivity is clamped on load and save",
          "[transient][preferences]") {
    TemporaryPreferences temporary;
    application::EditorPreferencesStore store(temporary.path);
    application::EditorPreferences preferences;
    preferences.transientSensitivity = 999;
    REQUIRE(store.save(preferences));
    REQUIRE(store.load().transientSensitivity == 100);
}

TEST_CASE("the meter peak hold round-trips through preferences",
          "[preferences]") {
    TemporaryPreferences temporary;
    application::EditorPreferencesStore store(temporary.path);

    application::EditorPreferences preferences;
    // A user who never opens the preference keeps the shipped behaviour.
    CHECK(preferences.meterPeakHoldSeconds < 0.0f);

    preferences.meterPeakHoldSeconds = 2.0f;
    REQUIRE(store.save(preferences));
    CHECK(store.load().meterPeakHoldSeconds == 2.0f);

    // Including the sentinel — a "hold until cleared" that came back as zero
    // would silently turn every marker into a second peak line.
    preferences.meterPeakHoldSeconds = -1.0f;
    REQUIRE(store.save(preferences));
    CHECK(store.load().meterPeakHoldSeconds == -1.0f);
}
