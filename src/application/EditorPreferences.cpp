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
        const auto rmsBody = root.find("meterRmsBody");
        if (rmsBody != root.end() && rmsBody->is_boolean()) {
            preferences.meterRmsBody = rmsBody->get<bool>();
        }
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
