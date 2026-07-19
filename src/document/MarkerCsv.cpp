#include "document/MarkerCsv.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace dave::document {

namespace {

// Split a CSV line on commas. Simple (no quoted-field handling — Reaper's
// marker format doesn't quote). Trims whitespace.
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) {
        // trim leading/trailing whitespace
        size_t a = field.find_first_not_of(" \t\r\n");
        size_t b = field.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) out.emplace_back();
        else out.push_back(field.substr(a, b - a + 1));
    }
    return out;
}

} // namespace

std::string exportMarkersReaperCsv(const Edit& edit, double sampleRate) {
    std::ostringstream out;
    out << "Name,Start,End,Length\n";
    const double inv = 1.0 / sampleRate;
    for (const auto& mt : edit.markerTracks()) {
        if (!mt.visible) continue;
        for (const auto& m : mt.markers) {
            if (m.posMode != MarkerPosMode::Sample) continue; // RB-4: Sample only
            double startSec = m.position * inv;
            if (m.length > 0) {
                double endSec = (m.position + m.length) * inv;
                out << m.name << ","
                    << startSec << ","
                    << endSec << ","
                    << (m.length * inv) << "\n";
            } else {
                // Point marker: empty End, Length 0.
                out << m.name << ","
                    << startSec << ",,"
                    << "0.000000000\n";
            }
        }
    }
    return out.str();
}

std::string importMarkersReaperCsv(Edit& edit, double sampleRate,
                                    const std::string& csv,
                                    const std::string& targetTrackId) {
    std::string trackId = targetTrackId;
    if (trackId.empty()) {
        trackId = edit.addMarkerTrack("Imported");
    } else if (edit.markerTrack(trackId) == nullptr) {
        return ""; // bad target
    }

    std::istringstream in(csv);
    std::string line;
    bool firstLine = true;
    int imported = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto fields = splitCsv(line);
        // Skip header.
        if (firstLine) {
            firstLine = false;
            if (!fields.empty() &&
                (fields[0] == "Name" || fields[0] == "name" ||
                 fields[0] == "#")) {
                continue;
            }
        }
        if (fields.size() < 4) continue;
        const std::string& name = fields[0];
        const std::string& startStr = fields[1];
        const std::string& endStr = fields[2];
        // Parse start (always required).
        double startSec = 0.0;
        try { startSec = std::stod(startStr); } catch (...) { continue; }

        Marker m;
        m.name = name.empty() ? ("Marker " + std::to_string(imported + 1)) : name;
        m.posMode = MarkerPosMode::Sample;
        m.position = static_cast<int64_t>(startSec * sampleRate);

        if (!endStr.empty()) {
            // Region.
            double endSec = startSec;
            try { endSec = std::stod(endStr); } catch (...) { continue; }
            int64_t endSamples = static_cast<int64_t>(endSec * sampleRate);
            m.length = endSamples > m.position ? (endSamples - m.position) : 0;
            m.kind = MarkerKind::Loop; // regions import as Loop (most useful default)
        } else {
            // Point marker.
            m.length = 0;
            m.kind = MarkerKind::Cue;
        }
        edit.addMarker(trackId, std::move(m));
        ++imported;
    }
    return imported > 0 ? trackId : std::string();
}

} // namespace dave::document
