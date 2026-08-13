// SPDX-License-Identifier: GPL-3.0-or-later
#include "application/AudioPreferences.h"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using dave::application::AudioPreferences;
using dave::application::AudioPreferencesStore;
using dave::application::InputMode;

namespace {

struct TempPreferences {
    TempPreferences() {
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
                    ("dave_audio_preferences_" + std::to_string(suffix));
        path = directory / "nested" / "preferences.json";
    }

    ~TempPreferences() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    std::filesystem::path directory;
    std::filesystem::path path;
};

nlohmann::json readJson(const std::filesystem::path& path) {
    std::ifstream input(path);
    return nlohmann::json::parse(input);
}

} // namespace

TEST_CASE("missing audio preferences start with input off",
          "[audiopreferences]") {
    TempPreferences tmp;
    const AudioPreferences loaded = AudioPreferencesStore(tmp.path).load();

    CHECK(loaded.outputDeviceName.empty());
    CHECK(loaded.inputMode == InputMode::Off);
    CHECK(loaded.inputDeviceName.empty());
    CHECK(loaded.recordLatencyOffsetSamples == 0);
}

TEST_CASE("audio device names and all input modes round-trip exactly",
          "[audiopreferences]") {
    TempPreferences tmp;
    AudioPreferencesStore store(tmp.path);

    AudioPreferences preferences;
    preferences.outputDeviceName = "Studio Output [USB]";
    preferences.inputMode = InputMode::Device;
    preferences.inputDeviceName = "Mic Input 1 / Ünicode";
    preferences.recordLatencyOffsetSamples = 384;
    REQUIRE(store.save(preferences));
    CHECK(store.load() == preferences);
    const auto deviceJson = readJson(tmp.path);
    CHECK(deviceJson["outputDeviceName"] == preferences.outputDeviceName);
    CHECK(deviceJson["inputMode"] == "device");
    CHECK(deviceJson["inputDeviceName"] == preferences.inputDeviceName);
    CHECK(deviceJson["recordLatencyOffsetSamples"] == 384);

    preferences.inputMode = InputMode::Default;
    preferences.inputDeviceName = "must not persist outside Device mode";
    REQUIRE(store.save(preferences));
    const AudioPreferences defaultInput = store.load();
    CHECK(defaultInput.outputDeviceName == preferences.outputDeviceName);
    CHECK(defaultInput.inputMode == InputMode::Default);
    CHECK(defaultInput.inputDeviceName.empty());
    const auto defaultJson = readJson(tmp.path);
    CHECK(defaultJson["inputMode"] == "default");
    CHECK_FALSE(defaultJson.contains("inputDeviceName"));

    preferences.inputMode = InputMode::Off;
    REQUIRE(store.save(preferences));
    CHECK(store.load().inputMode == InputMode::Off);
    CHECK(readJson(tmp.path)["inputMode"] == "off");
}

TEST_CASE("invalid Device mode and corrupt JSON fail to input off",
          "[audiopreferences]") {
    TempPreferences tmp;
    AudioPreferencesStore store(tmp.path);

    AudioPreferences invalid;
    invalid.inputMode = InputMode::Device;
    CHECK_FALSE(store.save(invalid));

    std::filesystem::create_directories(tmp.path.parent_path());
    {
        std::ofstream output(tmp.path);
        output << R"({"format":"dave.audio-preferences/v1",)";
    }
    CHECK(store.load() == AudioPreferences{});

    {
        std::ofstream output(tmp.path);
        output << R"({"format":"dave.audio-preferences/v1",)"
                  R"("outputDeviceName":"Output",)"
                  R"("inputMode":"device"})";
    }
    CHECK(store.load() == AudioPreferences{});
}

TEST_CASE("the default preferences path is app-specific",
          "[audiopreferences]") {
    const auto path = dave::application::defaultAudioPreferencesPath();
    REQUIRE_FALSE(path.empty());
    CHECK(path.filename() == "audio-preferences.json");
    CHECK(path.parent_path().filename() == "Dave");
}
