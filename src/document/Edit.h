// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dave::document {

// Edit is the single source of truth for a project: tracks, clips, and assets.
// The UI mutates it through Commands (undoable); the engine graph is DERIVED
// from it on each change (see GraphBuilder). The Edit itself owns no audio
// buffers — it's pure metadata + asset references.
//
// IDs: tracks and clips have stable string ids (assigned on creation) so undo
// and re-derivation can refer to "the same clip" across mutations. Assets are
// content-addressed (SHA-256 of file bytes).
//
// Threading: the Edit is mutated on the UI thread only. The RT thread never
// touches it — the engine reads a derived CompiledGraph instead.
class Edit {
public:
    struct RoutingValidation {
        bool ok = true;
        std::string message;
    };

    Edit();

    // --- Assets -----------------------------------------------------------
    // Import (or re-import) a file as an audio asset. Computes the content
    // hash; if the same file is already imported, returns the existing id.
    // Probes the file header for sampleRate/channels/length. Returns an empty
    // id on failure.
    AssetId importAsset(const std::string& filePath);

    const AudioAsset* asset(const AssetId& id) const;
    const std::unordered_map<AssetId, AudioAsset>& assets() const { return assets_; }

    // --- Tracks -----------------------------------------------------------
    // Add a track with a generated stable id. Returns the id.
    std::string addTrack(const std::string& name);
    Track* track(const std::string& id);
    const Track* track(const std::string& id) const;
    bool removeTrack(const std::string& id);
    const std::vector<Track>& tracks() const { return tracks_; }
    std::vector<Track>& tracksMut() { return tracks_; }

    // --- Clips ------------------------------------------------------------
    // Add a clip to a track. Assigns the clip a stable id and returns it.
    // Returns empty string if the track doesn't exist.
    std::string addClip(const std::string& trackId, AudioClip clip);
    // Replay/load helper: inserts an already-identified clip and notifies
    // once. Commands use it on redo so graph caches never observe a temporary
    // minted id.
    bool restoreClip_(const std::string& trackId, AudioClip clip);
    AudioClip* clip(const std::string& trackId, const std::string& clipId);
    bool removeClip(const std::string& trackId, const std::string& clipId);

    // --- Plugins (effect chain) -------------------------------------------
    // Append a plugin slot to a track's chain. Assigns a stable id and returns
    // it. Returns empty string if the track doesn't exist.
    std::string addPlugin(const std::string& trackId, PluginSlot slot);
    bool removePlugin(const std::string& trackId, const std::string& slotId);

    // --- MIDI tracks (RB-7) ------------------------------------------------
    // MIDI tracks are a parallel list to audio tracks: each drives one
    // instrument plugin with its clips' notes, then an ordinary effect chain.
    const std::vector<MidiTrack>& midiTracks() const { return midiTracks_; }
    std::vector<MidiTrack>& midiTracksMut() { return midiTracks_; }
    std::string addMidiTrack(const std::string& name);
    MidiTrack* midiTrack(const std::string& id);
    const MidiTrack* midiTrack(const std::string& id) const;
    bool removeMidiTrack(const std::string& id);

    // --- Routing and buses -------------------------------------------------
    const std::vector<BusTrack>& buses() const { return buses_; }
    std::vector<BusTrack>& busesMut() { return buses_; }
    BusTrack* bus(const std::string& id);
    const BusTrack* bus(const std::string& id) const;
    const BusTrack* mainBus() const { return bus(kMainBusId); }
    std::string addBus(const std::string& name);
    bool restoreBus_(BusTrack bus, size_t index);
    bool removeBus(const std::string& id);

    // A referenced software destination cannot be removed. Main is always
    // referenced conceptually and is also protected explicitly.
    bool routeReferences(RouteTarget::Kind kind, const std::string& id) const;

    // Validate the complete immutable topology. Hardware availability is not
    // part of document validity; only the stored span shape is checked.
    RoutingValidation validateRouting() const;
    RoutingValidation validateMainOutput(const std::string& ownerId,
                                         const RouteTarget& target) const;
    RoutingValidation validateSendTarget(const std::string& ownerId,
                                         const RouteTarget& target) const;

    bool setMainOutput(const std::string& ownerId, RouteTarget target);
    bool setTrackHardwareInput(const std::string& trackId,
                               HardwareChannelSpan span);
    bool setInputMonitor(const std::string& trackId, bool enabled);
    // Applies to audio tracks, MIDI tracks, and buses. Empty restores the
    // type-specific default; malformed colors are refused.
    bool setTrackColor(const std::string& ownerId, std::string color);
    std::string addSend(const std::string& ownerId, AuxSend send);
    bool restoreSend_(const std::string& ownerId, AuxSend send, size_t index);
    bool updateSend(const std::string& ownerId, const AuxSend& send);
    bool removeSend(const std::string& ownerId, const std::string& sendId);

    // Add a MIDI clip to a track; returns the clip id (empty if track missing).
    std::string addMidiClip(const std::string& trackId, MidiClip clip);
    MidiClip* midiClip(const std::string& trackId, const std::string& clipId);
    bool removeMidiClip(const std::string& trackId, const std::string& clipId);

    // Set (or clear, by passing a default-constructed slot) the track's
    // instrument. Returns false if the track doesn't exist.
    bool setMidiInstrument(const std::string& trackId, PluginSlot slot);

    // Post-instrument effect chain, same semantics as addPlugin on audio tracks.
    std::string addMidiPlugin(const std::string& trackId, PluginSlot slot);
    bool removeMidiPlugin(const std::string& trackId, const std::string& slotId);

    // --- Solo ---------------------------------------------------------------
    // Is anything in the edit soloed? Solo is only meaningful relative to every
    // other track, so this scan belongs to the Edit rather than to each of the
    // (now several) places that need the answer — the engine's gain-zeroing and
    // the GUI's dimming have to agree, and they can only agree by asking the
    // same question. Pair with document::trackAudible().
    bool anySoloed() const;

    // --- Marker tracks -----------------------------------------------------
    // Marker tracks are category layers (Cues, Scenes, etc.). The Edit owns a
    // list; addMarkerTrack returns the new track's id.
    std::string addMarkerTrack(const std::string& name);
    bool removeMarkerTrack(const std::string& trackId);
    MarkerTrack* markerTrack(const std::string& trackId);
    const std::vector<MarkerTrack>& markerTracks() const { return markerTracks_; }

    // --- Markers -----------------------------------------------------------
    // Add a marker to a marker track. Assigns a stable id; returns it (or
    // empty string if the track doesn't exist).
    std::string addMarker(const std::string& trackId, Marker marker);
    Marker* marker(const std::string& trackId, const std::string& markerId);
    bool removeMarker(const std::string& trackId, const std::string& markerId);

    // Find the first active Loop-kind region marker (length > 0). Used by the
    // transport to sync its loop region. Returns nullptr if none.
    // (Sample-mode only for RB-4; other modes resolve later.)
    const Marker* activeLoopMarker() const;

    // --- Video (RB-6) ------------------------------------------------------
    // Video tracks hold video clips (like audio tracks hold audio clips).
    // RB-6 ships one default video track; the model supports multiple.
    const std::vector<VideoTrack>& videoTracks() const { return videoTracks_; }
    std::vector<VideoTrack>& videoTracksMut() { return videoTracks_; }
    std::string addVideoTrack(const std::string& name);
    // Add a video clip to a track; returns the clip id (empty if track missing).
    std::string addVideoClip(const std::string& trackId, VideoClip clip);
    bool removeVideoClip(const std::string& trackId, const std::string& clipId);
    // Find which video clip is active at a given timeline position (samples).
    // Returns nullptr if none. Searches the first visible video track.
    const VideoClip* videoClipAt(int64_t timelineSample) const;

    // Compute the end of all content (last clip end across audio + video +
    // marker regions). Used by the transport to auto-stop. Returns 0 if empty.
    int64_t contentEndSamples() const;

    // --- Session properties ------------------------------------------------
    // The rate every sample position in this document is expressed in. Changing
    // it does NOT resample existing audio or move clips: sample positions are
    // the document's unit, so a session recorded at 48 k and reopened at 96 k
    // would play at the wrong speed. Set it when the session is empty, or
    // accept that existing material is being reinterpreted.
    int sampleRate() const { return sampleRate_; }
    void setSampleRate(int rate) {
        if (rate <= 0 || rate == sampleRate_) return;
        sampleRate_ = rate;
        notifyChanged();
    }
    // Bits per sample for files this session writes. Nothing renders yet, so
    // today this is a stored preference waiting for an export path.
    int bitDepth() const { return bitDepth_; }
    void setBitDepth(int bits) {
        if (bits <= 0 || bits == bitDepth_) return;
        bitDepth_ = bits;
        notifyChanged();
    }
    // Load-time setters: no change notification, no equality guard.
    void loadSessionFormat_(int rate, int bits) {
        if (rate > 0) sampleRate_ = rate;
        if (bits > 0) bitDepth_ = bits;
    }

    // --- Persistence helpers (load-time only; don't fire change notifications)
    void loadAsset_(AudioAsset a) { assets_.emplace(a.id, std::move(a)); }
    void loadMarkerTrack_(MarkerTrack mt) { markerTracks_.push_back(std::move(mt)); }
    void clearMarkerTracks_() { markerTracks_.clear(); }
    void clearVideoTracks_() { videoTracks_.clear(); }
    void loadVideoTrack_(VideoTrack vt) { videoTracks_.push_back(std::move(vt)); }
    void clearMidiTracks_() { midiTracks_.clear(); }
    void loadMidiTrack_(MidiTrack mt) { midiTracks_.push_back(std::move(mt)); }
    void clearBuses_() { buses_.clear(); }
    void loadBus_(BusTrack bus) { buses_.push_back(std::move(bus)); }
    void ensureMainBus_();
    std::unordered_map<AssetId, AudioAsset>& assets() { return assets_; }

    // --- Change notification ----------------------------------------------
    // Listeners are notified after every mutation (so the engine/UI can
    // re-derive). Notifications fire on the UI thread.
    using ChangeCallback = std::function<void()>;
    void setChangeListener(ChangeCallback cb) { onChange_ = std::move(cb); }
    void notifyChanged() { if (onChange_) onChange_(); }

private:
    std::string newId(const char* prefix) const;

    std::vector<Track> tracks_;
    std::vector<MidiTrack> midiTracks_;
    std::vector<BusTrack> buses_;
    std::vector<MarkerTrack> markerTracks_;
    std::vector<VideoTrack> videoTracks_;
    std::unordered_map<AssetId, AudioAsset> assets_;
    ChangeCallback onChange_;
    uint64_t idCounter_ = 0;
    int sampleRate_ = 48000;
    int bitDepth_ = 24;
};

} // namespace dave::document
