// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Fade.h"

#include <cstdint>
#include <algorithm>
#include <cmath>
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
    double gain = 1.0;           // per-clip linear gain
    bool muted = false;          // clip silenced (Cmd+M), independent of track
    int64_t fadeIn = 0;          // fade-in length in samples
    int64_t fadeOut = 0;         // fade-out length in samples
    FadeShape fadeInShape = FadeShape::Linear;
    FadeShape fadeOutShape = FadeShape::Linear;
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

// Hardware channels are zero-based in the model and displayed one-based in
// the UI. A span is deliberately device-independent: unavailable assignments
// remain in the project so reconnecting the interface restores the route.
struct HardwareChannelSpan {
    int firstChannel = 0;
    int channelCount = 2;

    bool operator==(const HardwareChannelSpan&) const = default;
};

inline constexpr const char* kMainBusId = "bus_main";

struct RouteTarget {
    enum class Kind { None, HardwareOutput, AudioTrack, Bus };

    Kind kind = Kind::Bus;
    std::string targetId = kMainBusId;
    HardwareChannelSpan hardware{};

    static RouteTarget none() {
        RouteTarget r;
        r.kind = Kind::None;
        r.targetId.clear();
        return r;
    }
    static RouteTarget hardwareOutput(int firstChannel, int channelCount) {
        RouteTarget r;
        r.kind = Kind::HardwareOutput;
        r.targetId.clear();
        r.hardware = {firstChannel, channelCount};
        return r;
    }
    static RouteTarget audioTrack(std::string id) {
        RouteTarget r;
        r.kind = Kind::AudioTrack;
        r.targetId = std::move(id);
        return r;
    }
    static RouteTarget bus(std::string id = kMainBusId) {
        RouteTarget r;
        r.kind = Kind::Bus;
        r.targetId = std::move(id);
        return r;
    }

    bool operator==(const RouteTarget&) const = default;
};

enum class SendTap { PreFader, PostFader };

// A set of clips that becomes a single clip on the timeline.
//
// Grouping does not merge the audio: the member clips stay exactly where they
// are on their own tracks, which is what keeps them playing and what makes
// ungrouping exact rather than a reconstruction. What changes is what you see
// and what you can grab — the members stop drawing individually and the group
// draws as one clip covering the range it was made from, on every track that
// has a member in it.
//
// The range is the selection the group was made from, not the union of its
// clips. Selecting four bars around three clips and grouping them gives a
// four-bar object, which is what was asked for and what makes two groups butt
// up against each other cleanly.
struct ClipGroup {
    struct Member {
        std::string trackId;
        std::string clipId;
        // Which of the track's two clip vectors the id names.
        bool midi = false;

        bool operator==(const Member& other) const {
            return trackId == other.trackId && clipId == other.clipId &&
                   midi == other.midi;
        }
    };

    std::string id;
    std::string name;
    int64_t timelineStart = 0;
    int64_t length = 0;
    std::vector<Member> members;
    // Groups inside this one. A nested group keeps its own record intact and
    // simply stops being the outermost thing over its clips, which is what
    // makes ungrouping one layer put the layer below back rather than
    // flattening everything at once.
    std::vector<std::string> childGroupIds;
    // The rows the group occupies, taken from the selection rather than
    // derived from the members. An empty group has no members and still has to
    // draw somewhere — deriving this made a group over silence invisible,
    // which looked exactly like the shortcut doing nothing.
    std::vector<std::string> trackIds;

    int64_t end() const { return timelineStart + length; }
    // One group clip per track it spans, so a group across three tracks reads
    // as three clips that move together rather than one floating over lanes it
    // does not belong to.
    bool coversTrack(const std::string& id) const {
        for (const auto& trackId : trackIds) {
            if (trackId == id) return true;
        }
        // Files written before trackIds existed only knew their members.
        if (trackIds.empty()) {
            for (const auto& member : members) {
                if (member.trackId == id) return true;
            }
        }
        return false;
    }
};

// One position in a channel's signal chain.
//
// Inserts, sends, the meter and the fader all sit in ONE ordered list, because
// they are all points on the same path: a send tapped before a compressor and
// one tapped after it are different sends, and the only honest way to say
// which is which is to put them in order with the compressor. Two parallel
// lists could not express "send, then insert, then send".
//
// Insert and Send name an entry in Track::plugins or Track::sends; the payload
// stays there and this says where it sits. Fader carries no id — there is
// exactly one, and it is in the list so it can be moved through it like
// anything else. A send below the Fader is a post-fader send; that is what
// post-fader now means.
//
// The fader's row is drawn as the meter, because the level arriving at the
// fader is the level worth watching and the fader's position is the one thing
// the row has to communicate. The fader control itself is not in the list —
// it sits at the bottom of the strip where it is always reachable.
struct ChainSlot {
    enum class Kind { Insert, Send, Fader };
    Kind kind = Kind::Insert;
    std::string id;
};

struct AuxSend {
    std::string id;
    RouteTarget target = RouteTarget::none();
    SendTap tap = SendTap::PostFader;
    // Linear amplitude. Zero is -infinity and is the safe default for a new
    // send; the send is also muted so neither control can surprise the user.
    double gain = 0.0;
    bool muted = true;
};

// One breakpoint in a channel's volume envelope. Values are stored in dB so
// a straight line in the lane is also a perceptually straight level change.
// The channel fader remains the trim/base gain; automation multiplies it.
struct VolumeAutomationPoint {
    std::string id;
    int64_t sample = 0;
    double db = 0.0;

    bool operator==(const VolumeAutomationPoint&) const = default;
};

inline constexpr double kMinVolumeAutomationDb = -60.0;
inline constexpr double kMaxVolumeAutomationDb = 6.0;

inline double clampVolumeAutomationDb(double db) noexcept {
    if (!std::isfinite(db)) return 0.0;
    return std::clamp(db, kMinVolumeAutomationDb, kMaxVolumeAutomationDb);
}

// Points are kept sorted by Edit. Before/after the written envelope, the
// nearest point is held. An empty envelope is unity (0 dB).
inline double volumeAutomationDbAt(
    const std::vector<VolumeAutomationPoint>& points,
    int64_t sample) noexcept {
    if (points.empty()) return 0.0;
    if (sample <= points.front().sample) return points.front().db;
    if (sample >= points.back().sample) return points.back().db;
    const auto next = std::upper_bound(
        points.begin(), points.end(), sample,
        [](int64_t position, const VolumeAutomationPoint& point) {
            return position < point.sample;
        });
    const auto previous = next - 1;
    const int64_t span = next->sample - previous->sample;
    if (span <= 0) return next->db;
    const double amount = static_cast<double>(sample - previous->sample) /
                          static_cast<double>(span);
    return previous->db + (next->db - previous->db) * amount;
}

// Pan automation owns the pan position whenever its envelope is non-empty.
// Stored values use the same normalized scale as the channel pan control:
// -1 = full left, 0 = centre, +1 = full right.
struct PanAutomationPoint {
    std::string id;
    int64_t sample = 0;
    double pan = 0.0;

    bool operator==(const PanAutomationPoint&) const = default;
};

inline double clampPanAutomation(double pan) noexcept {
    if (!std::isfinite(pan)) return 0.0;
    return std::clamp(pan, -1.0, 1.0);
}

inline double panAutomationAt(const std::vector<PanAutomationPoint>& points,
                              int64_t sample) noexcept {
    if (points.empty()) return 0.0;
    if (sample <= points.front().sample) return points.front().pan;
    if (sample >= points.back().sample) return points.back().pan;
    const auto next = std::upper_bound(
        points.begin(), points.end(), sample,
        [](int64_t position, const PanAutomationPoint& point) {
            return position < point.sample;
        });
    const auto previous = next - 1;
    const int64_t span = next->sample - previous->sample;
    if (span <= 0) return next->pan;
    const double amount = static_cast<double>(sample - previous->sample) /
                          static_cast<double>(span);
    return previous->pan + (next->pan - previous->pan) * amount;
}

// Channel identity colors are stored as CSS-style #RRGGBB strings so project
// JSON stays readable. Empty means "use the type's default color".
inline bool validTrackColor(const std::string& color) noexcept {
    if (color.empty()) return true;
    if (color.size() != 7 || color[0] != '#') return false;
    for (size_t i = 1; i < color.size(); ++i) {
        const char c = color[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

// ─── MIDI (RB-7) ────────────────────────────────────────────────────────────
// One note in a MIDI clip's sequence. Positions are in SOURCE samples — the
// same time base AudioClip::sourceOffset indexes into — so trimming a MIDI
// clip is arithmetically identical to trimming an audio clip and the two code
// paths can't drift apart. The conversion from the .mid file's ticks happens
// once at import (see engine/midi/SmfReader.h), against that file's own tempo
// map; Dave has no tempo map of its own yet.
struct MidiNote {
    int64_t startSample = 0;    // from the start of the source sequence
    int64_t lengthSamples = 0;
    uint8_t pitch = 60;         // 0..127
    uint8_t velocity = 100;     // 1..127 (a 0-velocity note-on is a note-off)
    uint8_t channel = 0;        // 0..15
};

// A tempo change from the source .mid file, kept in the file's own tick base.
// Dormant metadata for this milestone: nothing reads it, but storing it means a
// future Dave tempo map can re-conform an already-imported clip instead of
// forcing a re-import. See MarkerPosMode::Musical.
struct TempoEvent {
    int64_t tick = 0;                     // position in source PPQ ticks
    int32_t microsecondsPerQuarter = 500000;  // 120 bpm default
};

// A MIDI clip is a placed window onto a baked note sequence, mirroring
// AudioClip: timelineStart/sourceOffset/length have exactly the same meaning.
// Notes outside [sourceOffset, sourceOffset + length) simply don't sound.
struct MidiClip {
    std::string id;
    std::string name;            // display (usually the SMF track name)
    int64_t timelineStart = 0;   // where on the timeline (samples)
    int64_t sourceOffset = 0;    // where in the sequence (samples)
    int64_t length = 0;          // how long to play (samples)
    std::vector<MidiNote> notes; // sorted by startSample (SmfReader guarantees)
    // Provenance, for re-import and future tempo-map re-conform.
    std::string sourcePath;
    int sourcePpq = 480;
    std::vector<TempoEvent> sourceTempi;
};
// A channel: clips, a plugin chain, a gain/pan, and routing.
//
// There is deliberately ONE track type. What used to be an audio track, a MIDI
// track and a bus differ only in what they happen to contain — audio clips,
// MIDI clips plus an instrument, or neither — and every one of those is a
// property of the content, not a kind of object. A bus is simply a track with
// no clips and no hardware input that receives sends.
//
// The two clip vectors stay separate rather than becoming a variant: MIDI
// clips route through `instrument`, audio clips bypass it straight into the
// chain, and the code that handles each is genuinely different. An empty
// vector costs nothing and keeps both paths honest.
// An alternate set of clips for a track. A track plays exactly one playlist
// at a time — the active one, whose clips live in Track::clips/midiClips —
// and keeps the others here, silent, ready to be switched in. The active
// playlist's record is in the list too (for its id and name) with empty clip
// vectors, so the list is the complete roster and the track's own vectors are
// the only place a live clip ever is.
struct Playlist {
    std::string id;
    std::string name;
    std::vector<AudioClip> clips;
    std::vector<MidiClip> midiClips;
};

struct Track {
    std::string id;              // stable id
    std::string name;
    std::string color;           // #RRGGBB; empty = default audio color
    double gain = 1.0;
    std::vector<VolumeAutomationPoint> volumeAutomation;
    double pan = 0.0;            // -1 (L) .. +1 (R)
    std::vector<PanAutomationPoint> panAutomation;
    // Mute and solo are stored per track, but solo is only meaningful relative
    // to the other tracks: any track soloed silences every non-soloed track.
    // That comparison lives in GraphBuilder, which is the only place that sees
    // all tracks at once. See Edit::anySoloed().
    bool mute = false;
    bool solo = false;
    // Recording state belongs to the document so arming and routing survive
    // save/load. inputChannel is zero-based; inputChannelCount is the width of
    // the contiguous hardware-input span (1 = mono, 2 = stereo).
    bool recordArm = false;
    int inputChannel = 0;
    int inputChannelCount = 1;
    HardwareChannelSpan hardwareInput{0, 1};
    bool inputMonitor = false;
    RouteTarget mainOutput = RouteTarget::bus();
    // Further destinations fed the same post-fader signal as mainOutput — a
    // track that goes to Main and to a headphone pair at once. Validated by
    // the same rules as the main route.
    std::vector<RouteTarget> extraOutputs;
    std::vector<AuxSend> sends;
    std::vector<AudioClip> clips;
    // MIDI content. `instrument` is a dedicated field rather than "plugins[0]
    // is special": a track with no instrument is a real, representable state
    // (it is what you have between importing a .mid and choosing a synth), and
    // a convention that a vector's first element means something different is
    // the kind of thing that survives exactly until the first person inserts
    // an EQ at the top.
    std::vector<MidiClip> midiClips;
    PluginSlot instrument;            // empty uidString = no instrument yet
    // Main is the one permanent channel: it cannot be removed and its output
    // goes to hardware. A flag rather than a type, so graph construction and
    // the mixer need no special DSP path.
    bool isMain = false;
    // Hidden from the timeline. A view state, not a mute: a hidden track goes
    // on playing, recording and receiving sends exactly as before — which is
    // the whole point of hiding one rather than muting it.
    bool hidden = false;
    // Exempt from other tracks' solos. A talkback, a click, a reverb return
    // you always want to hear — soloing a guitar should not silence the
    // reverb that guitar is feeding.
    bool soloSafe = false;
    std::vector<PluginSlot> plugins;
    // Alternate takes, Pro Tools style. Empty means the track has the one
    // implicit playlist it always had; Edit materialises the roster the first
    // time a second one is asked for. See Playlist above for the invariant.
    std::vector<Playlist> playlists;
    std::string activePlaylistId;
    // The order everything above happens in. Holds one entry per plugin, one
    // per send, one Meter and one Fader; `plugins` and `sends` are storage,
    // this is sequence. Empty means "not built yet" and is filled in by
    // Edit::normalizeChain_.
    std::vector<ChainSlot> chain;  // effect chain, processed in order
};

// Whether a send taps before the fader. The chain is the truth — a send is
// pre-fader when it sits ahead of the Fader entry — with the send's own tap
// flag standing in for a track whose chain has not been built yet.
inline bool sendIsPreFader(const Track& track, const std::string& sendId) {
    for (const auto& slot : track.chain) {
        if (slot.kind == ChainSlot::Kind::Fader) return false;
        if (slot.kind == ChainSlot::Kind::Send && slot.id == sendId) return true;
    }
    for (const auto& send : track.sends) {
        if (send.id == sendId) return send.tap == SendTap::PreFader;
    }
    return false;
}

// Constrains a saved input span to a live capture device without coupling the
// document layer to AudioEngine or miniaudio. A device-less state has no valid
// channels; otherwise at least one channel remains selected and the whole span
// fits. Returns true when the route changed, which lets a caller decide whether
// the repaired value should dirty the document.
inline bool clampTrackInputToCaptureChannels(Track& track,
                                             int liveCaptureChannels) {
    const int oldChannel = track.inputChannel;
    const int oldCount = track.inputChannelCount;
    const int available = std::max(0, liveCaptureChannels);
    if (available == 0) {
        track.inputChannel = 0;
        track.inputChannelCount = 0;
    } else {
        track.inputChannelCount =
            std::clamp(track.inputChannelCount, 1, available);
        track.inputChannel = std::clamp(
            track.inputChannel, 0, available - track.inputChannelCount);
    }
    return track.inputChannel != oldChannel ||
           track.inputChannelCount != oldCount;
}

// Whether a track should be heard, given whether anything in the edit is
// soloed. Mute wins over solo, so a track that is both stays silent — that
// matches every DAW and stops a soloed track becoming impossible to mute.
//
// This is a free function rather than a Track method so it can be unit-tested
// without constructing a graph, and so the GUI's dimming and the engine's
// gain-zeroing are provably reading the same rule instead of two hand-written
// conditions that can drift apart.
inline bool trackAudible(bool mute, bool solo, bool anySoloed,
                         bool soloSafe = false) {
    // Mute still wins. Solo-safe means "another track's solo does not silence
    // me"; it does not mean "I cannot be switched off" — a safe track you
    // muted on purpose should stay off.
    if (mute) return false;
    return !anySoloed || solo || soloSafe;
}

inline bool trackAudible(const Track& t, bool anySoloed) {
    return trackAudible(t.mute, t.solo, anySoloed, t.soloSafe);
}


// Retained so existing call sites keep reading naturally while the three
// storage vectors are merged. Both name the one Track type above.
using MidiTrack = Track;
using BusTrack = Track;

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
