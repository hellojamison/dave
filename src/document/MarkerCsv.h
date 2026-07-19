#pragma once

#include "document/Edit.h"

#include <string>

namespace dave::document {

// ─── Reaper region/marker CSV interchange ───────────────────────────────────
// Format (de-facto standard, also used by OverMarker workflows):
//   Header:    Name,Start,End,Length
//   Region:    Region 1,0.500000000,1.500000000,1.000000000
//   Marker:    Marker A,2.000000000,,0.000000000
// Times are in SECONDS (decimal, double). Regions have an End; point markers
// have an empty End field and Length 0.
//
// Sample rate is needed to convert seconds <-> samples. We assume a single
// project sample rate (passed in) — the standard case.

// Export all visible markers across all marker tracks to the Reaper CSV
// format. Returns the CSV string. (sampleRate converts samples -> seconds.)
std::string exportMarkersReaperCsv(const Edit& edit, double sampleRate);

// Import a Reaper CSV into a marker track. Creates the track if
// `targetTrackId` is empty; otherwise appends to it. Returns the track id
// used (empty on parse failure). Regions become Loop-kind markers; point
// markers become Cue-kind. Existing markers are NOT cleared.
std::string importMarkersReaperCsv(Edit& edit, double sampleRate,
                                    const std::string& csv,
                                    const std::string& targetTrackId = "");

} // namespace dave::document
