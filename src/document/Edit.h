#pragma once

#include "document/Types.h"

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
    Edit() = default;

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
    AudioClip* clip(const std::string& trackId, const std::string& clipId);
    bool removeClip(const std::string& trackId, const std::string& clipId);

    // --- Plugins (effect chain) -------------------------------------------
    // Append a plugin slot to a track's chain. Assigns a stable id and returns
    // it. Returns empty string if the track doesn't exist.
    std::string addPlugin(const std::string& trackId, PluginSlot slot);
    bool removePlugin(const std::string& trackId, const std::string& slotId);

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

    // --- Persistence helpers (load-time only; don't fire change notifications)
    void loadAsset_(AudioAsset a) { assets_.emplace(a.id, std::move(a)); }
    void loadMarkerTrack_(MarkerTrack mt) { markerTracks_.push_back(std::move(mt)); }
    void clearMarkerTracks_() { markerTracks_.clear(); }
    void clearVideoTracks_() { videoTracks_.clear(); }
    void loadVideoTrack_(VideoTrack vt) { videoTracks_.push_back(std::move(vt)); }
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
    std::vector<MarkerTrack> markerTracks_;
    std::vector<VideoTrack> videoTracks_;
    std::unordered_map<AssetId, AudioAsset> assets_;
    ChangeCallback onChange_;
    uint64_t idCounter_ = 0;
};

} // namespace dave::document
