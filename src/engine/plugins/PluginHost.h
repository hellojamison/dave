// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace dave::engine {

// A discovered VST3 plugin: its file path, display name, vendor, category
// (Fx / Instrument / etc.), and the UID needed to instantiate it.
// Multiple plugins can live in one .vst3 bundle (a factory can publish
// several classes); each gets its own PluginDescriptor.
struct PluginDescriptor {
    std::string name;        // human-readable, e.g. "Melodyne"
    std::string vendor;
    std::string category;    // "Fx", "Instrument", "Fx|EQ", etc.
    std::string subCategories;
    std::string sdkVersion;
    std::string path;        // .vst3 bundle path
    std::string uidString;   // hex UID string, e.g. "00000000000000000000000000000000"
    bool isInstrument = false;
};

// PluginHost owns plugin discovery. The scanner runs on the UI thread (VST3
// module load is NOT RT-safe). Results are cached after the first scan.
//
// Threading: scan() is main-thread-only and slow (loads every .vst3 bundle).
// The graph/RT thread never touches the host or the descriptors.
class PluginHost {
public:
    // Scan the standard VST3 directories (via VST3::Hosting::Module::getModulePaths)
    // and populate the descriptor cache. Safe to call repeatedly; returns the
    // current cache (re-scans from scratch each call for now — caching the
    // filesystem walk is a later optimization).
    std::vector<PluginDescriptor> scan();

    // Cached results from the last scan() (empty before the first scan).
    const std::vector<PluginDescriptor>& descriptors() const { return descriptors_; }

private:
    std::vector<PluginDescriptor> descriptors_;
};

} // namespace dave::engine
