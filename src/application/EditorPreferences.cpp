// SPDX-License-Identifier: GPL-3.0-or-later
#include "application/EditorPreferences.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace dave::application {

namespace {

using json = nlohmann::json;
constexpr const char* kFormat = "dave.editor-preferences/v1";

std::filesystem::path environmentPath(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0'
        ? std::filesystem::path(value) : std::filesystem::path{};
}

} // namespace

std::filesystem::path defaultEditorPreferencesPath() {
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
    return base.empty() ? std::filesystem::path{}
                        : base / "Dave" / "editor-preferences.json";
}

EditorPreferences EditorPreferencesStore::load() const noexcept {
    const EditorPreferences defaults;
    if (path_.empty()) return defaults;
    try {
        std::ifstream input(path_);
        if (!input) return defaults;
        json root;
        input >> root;
        if (!input || !root.is_object() ||
            root.value("format", std::string{}) != kFormat) {
            return defaults;
        }
        const auto enabled = root.find("transientNavigationEnabled");
        const auto ticks = root.find("showTransientTicks");
        const auto sensitivity = root.find("transientSensitivity");
        if (enabled == root.end() || !enabled->is_boolean() ||
            ticks == root.end() || !ticks->is_boolean() ||
            sensitivity == root.end() || !sensitivity->is_number_integer()) {
            return defaults;
        }
        EditorPreferences preferences;
        preferences.transientNavigationEnabled = enabled->get<bool>();
        preferences.showTransientTicks = ticks->get<bool>();
        preferences.transientSensitivity = std::clamp(
            sensitivity->get<int>(), 0, 100);
        // Added after the first release of this file, so a missing key is a
        // normal older preferences file rather than corruption — unlike the
        // fields above, it falls back on its own instead of rejecting the lot.
        const auto pattern = root.find("takeNamePattern");
        if (pattern != root.end() && pattern->is_string()) {
            auto value = pattern->get<std::string>();
            // An empty or unusable pattern would name every take the same
            // thing; the default is a better answer than a refusal here.
            if (!expandTakeNamePattern(value, TakeNameContext{}).empty()) {
                preferences.takeNamePattern = std::move(value);
            }
        }
        const auto preFader = root.find("meterPreFader");
        if (preFader != root.end() && preFader->is_boolean()) {
            preferences.meterPreFader = preFader->get<bool>();
        }
        const auto peakHold = root.find("meterPeakHoldSeconds");
        if (peakHold != root.end() && peakHold->is_number()) {
            preferences.meterPeakHoldSeconds = peakHold->get<float>();
        }
        const auto rmsBody = root.find("meterRmsBody");
        if (rmsBody != root.end() && rmsBody->is_boolean()) {
            preferences.meterRmsBody = rmsBody->get<bool>();
        }
        // Fade defaults, also added after the first release. A shape is clamped
        // to the known values so a future or corrupt id can't cast past the
        // enum.
        const auto clampShape = [](int v) {
            return static_cast<document::FadeShape>(
                std::clamp(v, 0, static_cast<int>(document::FadeShape::SCurve)));
        };
        const auto fadeInShape = root.find("defaultFadeInShape");
        if (fadeInShape != root.end() && fadeInShape->is_number_integer()) {
            preferences.defaultFadeInShape = clampShape(fadeInShape->get<int>());
        }
        const auto fadeOutShape = root.find("defaultFadeOutShape");
        if (fadeOutShape != root.end() && fadeOutShape->is_number_integer()) {
            preferences.defaultFadeOutShape =
                clampShape(fadeOutShape->get<int>());
        }
        const auto fadeMs = root.find("defaultFadeMs");
        if (fadeMs != root.end() && fadeMs->is_number_integer()) {
            preferences.defaultFadeMs = std::max(0, fadeMs->get<int>());
        }
        const auto preOn = root.find("preRollEnabled");
        if (preOn != root.end() && preOn->is_boolean())
            preferences.preRollEnabled = preOn->get<bool>();
        const auto preMs = root.find("preRollMs");
        if (preMs != root.end() && preMs->is_number_integer())
            preferences.preRollMs = std::max(0, preMs->get<int>());
        const auto postOn = root.find("postRollEnabled");
        if (postOn != root.end() && postOn->is_boolean())
            preferences.postRollEnabled = postOn->get<bool>();
        const auto postMs = root.find("postRollMs");
        if (postMs != root.end() && postMs->is_number_integer())
            preferences.postRollMs = std::max(0, postMs->get<int>());
        return preferences;
    } catch (...) {
        return defaults;
    }
}

bool EditorPreferencesStore::save(
    const EditorPreferences& preferences) const noexcept {
    if (path_.empty()) return false;
    try {
        const json root{
            {"format", kFormat},
            {"transientNavigationEnabled",
             preferences.transientNavigationEnabled},
            {"showTransientTicks", preferences.showTransientTicks},
            {"transientSensitivity",
             std::clamp(preferences.transientSensitivity, 0, 100)},
            {"takeNamePattern", preferences.takeNamePattern},
            {"meterPreFader", preferences.meterPreFader},
            {"meterRmsBody", preferences.meterRmsBody},
            {"meterPeakHoldSeconds", preferences.meterPeakHoldSeconds},
            {"defaultFadeInShape",
             static_cast<int>(preferences.defaultFadeInShape)},
            {"defaultFadeOutShape",
             static_cast<int>(preferences.defaultFadeOutShape)},
            {"defaultFadeMs", preferences.defaultFadeMs},
            {"preRollEnabled", preferences.preRollEnabled},
            {"preRollMs", preferences.preRollMs},
            {"postRollEnabled", preferences.postRollEnabled},
            {"postRollMs", preferences.postRollMs},
        };
        std::error_code error;
        const auto parent = path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error) return false;
        }
        auto temporary = path_;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) return false;
            output << root.dump(2) << '\n';
            output.close();
            if (!output) {
                std::filesystem::remove(temporary, error);
                return false;
            }
        }
        std::filesystem::rename(temporary, path_, error);
#if defined(_WIN32)
        if (error && std::filesystem::exists(path_)) {
            error.clear();
            std::filesystem::remove(path_, error);
            if (!error) std::filesystem::rename(temporary, path_, error);
        }
#endif
        if (error) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace dave::application
