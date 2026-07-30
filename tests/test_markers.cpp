// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/Edit.h"
#include "document/MarkerCsv.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace dave::document;

namespace {
constexpr double kSr = 48000.0;

const Marker* findMarker(const Edit& edit, const std::string& name) {
    for (const auto& track : edit.markerTracks()) {
        for (const auto& m : track.markers) {
            if (m.name == name) return &m;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("Reaper CSV round-trips point markers and regions", "[markers]") {
    Edit edit;
    const std::string trackId = edit.addMarkerTrack("Markers");

    Marker point;
    point.name = "Cue A";
    point.kind = MarkerKind::Cue;
    point.position = static_cast<int64_t>(2.0 * kSr);
    edit.addMarker(trackId, point);

    Marker region;
    region.name = "Loop 1";
    region.kind = MarkerKind::Loop;
    region.position = static_cast<int64_t>(0.5 * kSr);
    region.length = static_cast<int64_t>(1.0 * kSr);
    edit.addMarker(trackId, region);

    const std::string csv = exportMarkersReaperCsv(edit, kSr);
    REQUIRE_FALSE(csv.empty());

    // Import into a fresh Edit — a round-trip through a real serialization is
    // the only way to catch seconds/samples conversion errors, which are
    // invisible as long as you only ever look at one side.
    Edit reimported;
    const std::string usedTrack =
        importMarkersReaperCsv(reimported, kSr, csv, "");
    REQUIRE_FALSE(usedTrack.empty());

    const Marker* gotPoint = findMarker(reimported, "Cue A");
    REQUIRE(gotPoint != nullptr);
    CHECK(gotPoint->position == static_cast<int64_t>(2.0 * kSr));
    CHECK(gotPoint->length == 0);

    const Marker* gotRegion = findMarker(reimported, "Loop 1");
    REQUIRE(gotRegion != nullptr);
    CHECK(gotRegion->position == static_cast<int64_t>(0.5 * kSr));
    CHECK(gotRegion->length == static_cast<int64_t>(1.0 * kSr));
}

TEST_CASE("regions import as loops and points as cues", "[markers]") {
    const std::string csv =
        "Name,Start,End,Length\n"
        "A Region,1.000000000,3.000000000,2.000000000\n"
        "A Point,5.000000000,,0.000000000\n";

    Edit edit;
    REQUIRE_FALSE(importMarkersReaperCsv(edit, kSr, csv, "").empty());

    const Marker* region = findMarker(edit, "A Region");
    REQUIRE(region != nullptr);
    CHECK(region->kind == MarkerKind::Loop);
    CHECK(region->length > 0);

    const Marker* point = findMarker(edit, "A Point");
    REQUIRE(point != nullptr);
    CHECK(point->kind == MarkerKind::Cue);
    CHECK(point->length == 0);
}

TEST_CASE("malformed CSV is rejected rather than half-imported", "[markers]") {
    Edit edit;
    // Garbage in a user-supplied file must not leave the Edit holding
    // partially-parsed markers at nonsense positions.
    CHECK(importMarkersReaperCsv(edit, kSr, "", "").empty());
    CHECK(importMarkersReaperCsv(edit, kSr, "not a csv at all\n", "").empty());
}

TEST_CASE("importing into an existing track appends", "[markers]") {
    Edit edit;
    const std::string trackId = edit.addMarkerTrack("Existing");
    Marker m;
    m.name = "Original";
    m.position = 0;
    edit.addMarker(trackId, m);

    const std::string csv =
        "Name,Start,End,Length\n"
        "Imported,1.000000000,,0.000000000\n";
    const std::string used = importMarkersReaperCsv(edit, kSr, csv, trackId);
    CHECK(used == trackId);

    // Import must not clear what's already there — losing a session's markers
    // to an import is not a recoverable mistake.
    CHECK(findMarker(edit, "Original") != nullptr);
    CHECK(findMarker(edit, "Imported") != nullptr);
    CHECK(edit.markerTracks().size() == 1);
}
