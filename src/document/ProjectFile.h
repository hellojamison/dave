#pragma once

#include "document/Edit.h"

#include <string>

namespace dave::document {

// Result of a save/load operation. Empty message on success.
struct ProjectResult {
    bool ok = false;
    std::string message;
};

// Serialize an Edit to a JSON string. Doesn't touch disk. Useful for testing
// and for the actual saveBundle path.
std::string serializeEdit(const Edit& edit);

// Populate an Edit from a JSON string. Clears the Edit first. Returns false
// on parse error (message filled).
ProjectResult deserializeEdit(const std::string& json, Edit& edit);

// Save the Edit as a .dave bundle at `bundlePath` (a directory). Creates the
// dir + assets/ + video/ subdirs, copies referenced media in (content-
// addressed for audio, by-name for video), writes project.json atomically.
// `copyAssets` = true copies media into the bundle (self-contained); false
// writes the bundle but leaves assets referenced by their original paths.
ProjectResult saveBundle(const std::string& bundlePath, const Edit& edit,
                         bool copyAssets = true);

// Load a .dave bundle: read project.json, rebuild the Edit, re-link asset
// paths (which are stored relative to the bundle).
ProjectResult loadBundle(const std::string& bundlePath, Edit& edit);

// Whether a path looks like a Dave bundle (ends in .dave and is a directory).
bool isDaveBundle(const std::string& path);

} // namespace dave::document
