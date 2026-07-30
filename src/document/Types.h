// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dave::document {

// Content-addressed asset id (SHA-256 hex). Two imports of the same file
// resolve to the same id; editing a clip references the id, not a path.
struct AssetId {
    std::string sha256;
    bool operator==(const AssetId& o) const { return sha256 == o.sha256; }
    bool operator!=(const AssetId& o) const { return sha256 == o.sha256; }
    bool valid() const { return !sha256.empty(); }
};

// An audio asset: a decoded-on-demand source file. The Edit references assets
// by id; the AudioClipNode loads from `path` when the clip is first rendered.
struct AudioAsset {
    AssetId id;
    std::string path;            // resolved file on disk
    int sampleRate = 0;
    int channels = 0;
    int64_t lengthSamples = 0;
};

// A clip is a reference into an audio asset, placed on the timeline. Edits
// (move/trim/fade) mutate clip fields; the asset is never modified.
struct AudioClip {
    std::string id;              // stable across edits; assigned by Edit::addClip
    AssetId asset;
    int64_t timelineStart = 0;   // where on the timeline (samples, playback rate)
    int64_t sourceOffset = 0;    // where in the source file (samples)
    int64_t length = 0;          // how long to play (samples)
    double gain = 1.0;
    int64_t fadeIn = 0;          // fade-in length in samples
    int64_t fadeOut = 0;         // fade-out length in samples
};

// A plugin slot is one entry in a track's effect chain. It references the
// plugin by descriptor (uid + path + name) so it survives save/load, and holds
// a (runtime-only, non-serialized) instance pointer once the GraphBuilder has
// instantiated it. The chain is processed in order: clip sum -> slot[0] ->
// slot[1] -> ... -> track output.
struct PluginSlot {
    std::string id;              // stable id within the track
    std::string name;            // display name (e.g. "Melodyne")
    std::string uidString;       // VST3 class UID (identifies which plugin)
    std::string path;            // .vst3 bundle path (for reload)
    bool bypass = false;
    std::string stateBase64;     // VST3 state chunk (base64-encoded) for persistence
};

// A track holds an ordered list of clips, a plugin chain, and a gain/pan.
// RB-2: one track type (audio). MIDI tracks come later.
struct Track {
    std::string id;              // stable id
    std::string name;
    double gain = 1.0;
    double pan = 0.0;            // -1 (L) .. +1 (R)
    // Mute and solo are stored per track, but solo is only meaningful relative
    // to the other tracks: any track soloed silences every non-soloed track.
    // That comparison lives in GraphBuilder, which is the only place that sees
    // all tracks at once. See Edit::anyTrackSoloed().
    bool mute = false;
    bool solo = false;
    std::vector<AudioClip> clips;
    std::vector<PluginSlot> plugins;  // effect chain, processed in order
};

// Whether a track should be heard, given whether anything in the edit is
// soloed. Mute wins over solo, so a track that is both stays silent — that
// matches every DAW and stops a soloed track becoming impossible to mute.
//
// This is a free function rather than a Track method so it can be unit-tested
// without constructing a graph, and so the GUI's dimming and the engine's
// gain-zeroing are provably reading the same rule instead of two hand-written
// conditions that can drift apart.
inline bool trackAudible(bool mute, bool solo, bool anySoloed) {
    if (mute) return false;
    return !anySoloed || solo;
}

inline bool trackAudible(const Track& t, bool anySoloed) {
    return trackAudible(t.mute, t.solo, anySoloed);
}

// ─── Markers (RB-4) ─────────────────────────────────────────────────────────
// Marker kinds drive color and behavior (loop region loops; punch triggers
// punch-in/out; cue fires MIDI/OSC later). Custom is the escape hatch.
enum class MarkerKind { Cue, Section, Loop, Punch, CD, Custom };

// How a marker's position is specified. RB-4 fully wires Sample; the others
// are stored so we don't need a migration when SMPTE/tempo-map/clip-anchored
// resolution lands. See docs/rb4-markers-design.md.
enum class MarkerPosMode { Sample, Smpte, Musical, ClipAnchored };

// A marker is a named position (point or region) on a marker track. Regions
// have length > 0. The metadata field is free-form (cue #, scene, take, etc.)
// — it's the extension point for cue-list/show-control workflows.
struct Marker {
    std::string id;
    std::string name;
    MarkerKind kind = MarkerKind::Cue;
    MarkerPosMode posMode = MarkerPosMode::Sample;
    int64_t position = 0;        // in posMode units (samples for Sample)
    int64_t length = 0;          // 0 = point marker; >0 = region
    std::string color;           // hex "#rrggbb"; empty = default-for-kind
    std::string clipId;          // parent clip for ClipAnchored
    std::string metadata;        // free-form JSON-ish string
};

// A marker track is a category (Cues, Scenes, Loop points, etc.). The Edit
// holds a list; each can be shown/hidden independently. RB-4 ships one
// default "Markers" track and lets the user add more.
struct MarkerTrack {
    std::string id;
    std::string name;
    bool visible = true;
    std::vector<Marker> markers;
};

// ─── Video (RB-5/RB-6) ──────────────────────────────────────────────────────
// A video clip references a movie file (decoded via the LGPL FFmpeg subprocess
// — see VideoDecoder). RB-6 adds timeline placement + trimming (like audio
// clips): timelineStart, sourceOffset, length — so multiple clips can be
// arranged on a video track. The preview shows whichever clip is at the
// playhead.
struct VideoClip {
    std::string id;
    std::string path;           // movie file
    std::string name;           // display (filename)
    std::string codec;          // e.g. "h264", "dnxhd" (probed at import)
    int64_t timelineStart = 0;  // where on the timeline (samples, @ audio sr)
    int64_t sourceOffset = 0;   // where in the source to start (samples @ audio sr)
    int64_t length = 0;         // how long to play (samples @ audio sr; 0 = full)
    double fps = 0.0;           // native frame rate
    int width = 0;              // native resolution (probe at import)
    int height = 0;
    double durationSeconds = 0.0;
};

// A video track holds an ordered list of video clips. RB-6 ships one default
// video track; the model supports multiple (e.g. "Main picture" + "Alt angle").
struct VideoTrack {
    std::string id;
    std::string name;
    bool visible = true;
    std::vector<VideoClip> clips;
};

} // namespace dave::document

// Hash for AssetId so it can key an unordered_map.
namespace std {
template <>
struct hash<dave::document::AssetId> {
    size_t operator()(const dave::document::AssetId& a) const noexcept {
        return hash<string>{}(a.sha256);
    }
};
} // namespace std
