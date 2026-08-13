// SPDX-License-Identifier: GPL-3.0-or-later
#include "application/AudioPreferences.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <system_error>

namespace dave::application {

namespace {

using json = nlohmann::json;

constexpr const char* kFormat = "dave.audio-preferences/v1";

const char* inputModeString(InputMode mode) {
    switch (mode) {
        case InputMode::Off:     return "off";
        case InputMode::Default: return "default";
        case InputMode::Device:  return "device";
    }
    return "off";
}

bool parseInputMode(const std::string& value, InputMode& mode) {
    if (value == "off") {
        mode = InputMode::Off;
        return true;
    }
    if (value == "default") {
        mode = InputMode::Default;
        return true;
    }
    if (value == "device") {
        mode = InputMode::Device;
        return true;
    }
    return false;
}

std::filesystem::path environmentPath(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr && value[0] != '\0')
        ? std::filesystem::path(value)
        : std::filesystem::path{};
}

} // namespace

std::filesystem::path defaultAudioPreferencesPath() {
    std::filesystem::path base;

#if defined(_WIN32)
    base = environmentPath("APPDATA");
    if (base.empty()) base = environmentPath("LOCALAPPDATA");
#elif defined(__APPLE__)
    base = environmentPath("HOME");
    if (!base.empty()) base /= "Library/Application Support";
#else
    base = environmentPath("XDG_CONFIG_HOME");
    if (base.empty()) {
        base = environmentPath("HOME");
        if (!base.empty()) base /= ".config";
    }
#endif

    if (base.empty()) return {};
    return base / "Dave" / "audio-preferences.json";
}

AudioPreferences AudioPreferencesStore::load() const noexcept {
    const AudioPreferences safeDefaults;
    if (path_.empty()) return safeDefaults;

    try {
        std::ifstream input(path_);
        if (!input) return safeDefaults;

        json root;
        input >> root;
        if (!input || !root.is_object()) return safeDefaults;

        const auto format = root.find("format");
        const auto output = root.find("outputDeviceName");
        const auto modeValue = root.find("inputMode");
        if (format == root.end() || !format->is_string() ||
            format->get<std::string>() != kFormat ||
            output == root.end() || !output->is_string() ||
            modeValue == root.end() || !modeValue->is_string()) {
            return safeDefaults;
        }

        AudioPreferences preferences;
        preferences.outputDeviceName = output->get<std::string>();
        if (!parseInputMode(modeValue->get<std::string>(), preferences.inputMode)) {
            return safeDefaults;
        }

        if (preferences.inputMode == InputMode::Device) {
            const auto inputName = root.find("inputDeviceName");
            if (inputName == root.end() || !inputName->is_string()) {
                return safeDefaults;
            }
            preferences.inputDeviceName = inputName->get<std::string>();
            if (preferences.inputDeviceName.empty()) return safeDefaults;
        }

        preferences.recordLatencyOffsetSamples = std::clamp(
            root.value("recordLatencyOffsetSamples", 0), 0, 1000000);

        return preferences;
    } catch (...) {
        return safeDefaults;
    }
}

bool AudioPreferencesStore::save(
    const AudioPreferences& preferences) const noexcept {
    if (path_.empty() ||
        (preferences.inputMode == InputMode::Device &&
         preferences.inputDeviceName.empty())) {
        return false;
    }

    try {
        json root{
            {"format", kFormat},
            {"outputDeviceName", preferences.outputDeviceName},
            {"inputMode", inputModeString(preferences.inputMode)},
            {"recordLatencyOffsetSamples",
             std::clamp(preferences.recordLatencyOffsetSamples, 0, 1000000)},
        };
        if (preferences.inputMode == InputMode::Device) {
            root["inputDeviceName"] = preferences.inputDeviceName;
        }

        std::error_code error;
        const auto parent = path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error) return false;
        }

        auto temporaryPath = path_;
        temporaryPath += ".tmp";
        {
            std::ofstream output(temporaryPath, std::ios::trunc);
            if (!output) return false;
            output << root.dump(2) << '\n';
            output.close();
            if (!output) {
                std::filesystem::remove(temporaryPath, error);
                return false;
            }
        }

        std::filesystem::rename(temporaryPath, path_, error);
#if defined(_WIN32)
        // std::filesystem::rename does not replace an existing file on
        // Windows. Retry after removing the old preferences file.
        if (error && std::filesystem::exists(path_)) {
            error.clear();
            std::filesystem::remove(path_, error);
            if (!error) std::filesystem::rename(temporaryPath, path_, error);
        }
#endif
        if (error) {
            std::error_code ignored;
            std::filesystem::remove(temporaryPath, ignored);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace dave::application
