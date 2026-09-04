// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/MusicalTime.h"
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
    // Append a plugin slot to any track's chain — audio, MIDI or bus. Assigns
    // a stable id and returns it, or empty if the track doesn't exist. (These
    // used to resolve track-or-bus only, which is why a separate
    // addMidiPlugin had to exist; one track type closes that gap.)
    std::string addPlugin(const std::string& trackId, PluginSlot slot);
    bool removePlugin(const std::string& trackId, const std::string& slotId);
    // Resolve a plugin slot by id alone, across every track's chain and every
    // instrument. Slot ids are unique document-wide, and the callers that
    // bypass or open one have the id without the track it belongs to.
    const PluginSlot* pluginSlot(const std::string& slotId) const;
    // Set a slot's bypass. Covers instruments as well as inserts: both are
    // bypassable and both are addressed the same way.
    bool setPluginBypass(const std::string& slotId, bool bypass);

    // --- Creating tracks for particular content ----------------------------
    // These differ only in the id prefix, the default name and — for a bus —
    // where the new row lands. They all produce the same Track; nothing about
    // the document distinguishes them afterwards.
    std::string addMidiTrack(const std::string& name);
    // A bus is a track with no clips and no input. New buses land before Main,
    // which stays last.
    std::string addBus(const std::string& name);
    // Replay helper: re-inserts an already-identified track at a known row so
    // redo cannot mint a different id.
    bool restoreTrack_(Track track, size_t index);
    const Track* mainBus() const { return track(kMainBusId); }

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
    // Additional post-fader destinations beside the main output. Refused when
    // the route would be invalid or is already one of the track's outputs.
    bool addOutput(const std::string& ownerId, RouteTarget target);
    bool removeOutput(const std::string& ownerId, const RouteTarget& target);
    bool setTrackHardwareInput(const std::string& trackId,
                               HardwareChannelSpan span);
    bool setInputMonitor(const std::string& trackId, bool enabled);
    // Applies to audio tracks, MIDI tracks, and buses. Empty restores the
    // type-specific default; malformed colors are refused.
    bool setTrackColor(const std::string& ownerId, std::string color);
    // Hide a track from the timeline. Main is refused: it is where everything
    // ends up, and a session with no visible Main has no visible output.
    bool setTrackHidden(const std::string& trackId, bool hidden);
    // Exempt a track from other tracks' solos.
    bool setTrackSoloSafe(const std::string& trackId, bool safe);
    std::vector<VolumeAutomationPoint>* volumeAutomation(
        const std::string& ownerId);
    const std::vector<VolumeAutomationPoint>* volumeAutomation(
        const std::string& ownerId) const;
    std::string addVolumeAutomationPoint(
        const std::string& ownerId, int64_t sample, double db);
    bool restoreVolumeAutomationPoint_(
        const std::string& ownerId, VolumeAutomationPoint point, size_t index);
    bool updateVolumeAutomationPoint(
        const std::string& ownerId, VolumeAutomationPoint point);
    bool removeVolumeAutomationPoint(
        const std::string& ownerId, const std::string& pointId);
    // Replaces one complete envelope and notifies once. Gesture tools use
    // this instead of rebuilding the graph for every sampled mouse position.
    // Empty point ids are assigned here and retained by the undo command.
    bool replaceVolumeAutomation(
        const std::string& ownerId,
        std::vector<VolumeAutomationPoint> points);
    std::vector<PanAutomationPoint>* panAutomation(
        const std::string& ownerId);
    const std::vector<PanAutomationPoint>* panAutomation(
        const std::string& ownerId) const;
    std::string addPanAutomationPoint(
        const std::string& ownerId, int64_t sample, double pan);
    bool restorePanAutomationPoint_(
        const std::string& ownerId, PanAutomationPoint point, size_t index);
    bool updatePanAutomationPoint(
        const std::string& ownerId, PanAutomationPoint point);
    bool removePanAutomationPoint(
        const std::string& ownerId, const std::string& pointId);
    bool replacePanAutomation(
        const std::string& ownerId,
        std::vector<PanAutomationPoint> points);
    std::string addSend(const std::string& ownerId, AuxSend send);
    bool restoreSend_(const std::string& ownerId, AuxSend send, size_t index);
    // Reorder a send within its owner's list. Sends sum, so order is the
    // user's arrangement rather than a signal-flow constraint — which is why
    // it has to be preserved rather than derived.
    bool moveSend(const std::string& ownerId, const std::string& sendId,
                  size_t newIndex);
    // Bring a track's chain into agreement with its plugins and sends: one
    // entry per plugin and per send, exactly one Meter and one Fader, nothing
    // naming something that no longer exists. Called after every mutation that
    // could disturb it, and on load, so no other code has to remember to.
    void normalizeChain_(Track& track);
    // Same, by id — for callers that only hold one. Cheap and idempotent, so
    // the strip calls it every frame rather than trying to guess when the
    // chain might have gone stale.
    void normalizeChainFor_(const std::string& trackId);
    // Move one chain entry to another position. `from` and `to` index the
    // track's chain directly.
    bool moveChainSlot(const std::string& trackId, size_t from, size_t to);
    bool updateSend(const std::string& ownerId, const AuxSend& send);
    bool removeSend(const std::string& ownerId, const std::string& sendId);

    // Add a MIDI clip to a track; returns the clip id (empty if track missing).
    std::string addMidiClip(const std::string& trackId, MidiClip clip);
    MidiClip* midiClip(const std::string& trackId, const std::string& clipId);
    bool removeMidiClip(const std::string& trackId, const std::string& clipId);

    // Set (or clear, by passing a default-constructed slot) the track's
    // instrument. Returns false if the track doesn't exist.
    bool setMidiInstrument(const std::string& trackId, PluginSlot slot);


    // --- Clip groups --------------------------------------------------------
    // Several clips treated as one object. The clips stay where they are; a
    // group only records which of them belong together, so ungrouping is the
    // removal of that record rather than a reconstruction.
    const std::vector<ClipGroup>& clipGroups() const { return clipGroups_; }
    // Returns the new group's id, or empty when there is nothing to group:
    // fewer than two members, or members naming clips that do not exist.
    // `start` and `length` are the selection the group was made from; the
    // group draws over exactly that range rather than over its clips' extent.
    // `trackIds` are the rows the group occupies. Passing none falls back to
    // the members' tracks, which is only right when there are members.
    std::string addClipGroup(std::vector<ClipGroup::Member> members,
                             int64_t start, int64_t length,
                             std::vector<std::string> trackIds = {},
                             std::vector<std::string> childGroupIds = {},
                             std::string name = {});
    // The group this one is nested in, or null when it is the outermost.
    const ClipGroup* clipGroupParent(const std::string& groupId) const;
    // Only the outermost groups draw and only they answer a selection; the
    // ones inside them are recorded but stood in for.
    bool clipGroupIsTopLevel(const std::string& groupId) const {
        return clipGroupParent(groupId) == nullptr;
    }
    // Slide a whole group, range and members together.
    bool moveClipGroup(const std::string& groupId, int64_t deltaSamples);
    // Move a whole group down (positive) or up (negative) by that many track
    // rows, members and nested groups together. Refused — nothing moves — if
    // any member would land off the end of the track list, so a group never
    // ends up half on and half off.
    bool moveClipGroupTracks(const std::string& groupId, int rowDelta);
    // Replay helper: re-inserts an already-identified group so redo cannot
    // mint a different id.
    bool restoreClipGroup_(ClipGroup group, size_t index);
    bool removeClipGroup(const std::string& groupId);
    const ClipGroup* clipGroup(const std::string& groupId) const;
    // The group a clip belongs to, or null. A clip is in at most one.
    const ClipGroup* clipGroupContaining(const std::string& trackId,
                                         const std::string& clipId) const;
    // Drop members whose clips have gone, and any group left with fewer than
    // two. Called after clip removal so a group never names a clip that no
    // longer exists.
    void pruneClipGroups_();
    void shiftClipGroup_(const std::string& groupId, int64_t delta);
    bool clipGroupTracksFit_(const std::string& groupId, int rowDelta) const;
    void shiftClipGroupTracks_(const std::string& groupId, int rowDelta);
    int trackIndexOf_(const std::string& trackId) const;
    int64_t earliestInClipGroup_(const std::string& groupId) const;
    void loadClipGroup_(ClipGroup group) {
        clipGroups_.push_back(std::move(group));
    }
    void clearClipGroups_() { clipGroups_.clear(); }

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
    // --- Musical time -------------------------------------------------------
    // Quarter notes per minute, which is what a tempo field means in every
    // DAW regardless of meter. One value for the session: a tempo MAP would
    // require re-conforming every clip when it changed, and is its own piece
    // of work.
    // The tempo at bar 1, which is what a session with no changes has. Kept
    // as an accessor because most callers want exactly that.
    double tempoBpm() const;
    void setTempoBpm(double bpm);
    // Tempo changes, anchored musically. Always normalized: sorted, one per
    // position, one at bar 1 beat 1.
    const std::vector<TempoChange>& tempoMap() const { return tempoMap_; }
    void setTempoMap(std::vector<TempoChange> map);
    // Set or replace the tempo at a position. Bar 1 beat 1 can be changed but
    // not removed — the session would have no tempo before the first change.
    bool setTempoChange(int bar, int beat, double bpm);
    bool removeTempoChange(int bar, int beat);
    // Time signature changes, always normalized: sorted, one per bar, one at
    // bar 1. Readers can walk it without checking.
    const std::vector<TimeSignature>& meterMap() const { return meterMap_; }
    void setMeterMap(std::vector<TimeSignature> map);
    // Set or replace the signature at a bar. Bar 1 can be changed but not
    // removed — every bar before the first change would otherwise have no
    // meter at all.
    bool setTimeSignature(int bar, int numerator, int denominator);
    bool removeTimeSignature(int bar);
    // Load-time: no notification, no normalization guard.
    void loadMusicalTime_(std::vector<TempoChange> tempo,
                          std::vector<TimeSignature> map);

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
    void clearTracks_() { tracks_.clear(); }
    void loadTrack_(Track t) {
        tracks_.push_back(std::move(t));
        // A file written before the chain existed, or one hand-edited, still
        // arrives with a usable one.
        normalizeChain_(tracks_.back());
    }
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
    // Main is pinned last; new rows go ahead of it.
    std::vector<Track>::iterator mainRow_();

    // One list. Row order is the user's; Main is pinned last.
    std::vector<Track> tracks_;
    std::vector<ClipGroup> clipGroups_;
    std::vector<MarkerTrack> markerTracks_;
    std::vector<VideoTrack> videoTracks_;
    std::unordered_map<AssetId, AudioAsset> assets_;
    ChangeCallback onChange_;
    uint64_t idCounter_ = 0;
    int sampleRate_ = 48000;
    // 32-bit float. Nothing clips internally and a level set wrong is
    // recoverable exactly, which is worth more than the disk space a fixed
    // depth saves. Sessions that need 16 or 24 say so.
    int bitDepth_ = 32;
    std::vector<TempoChange> tempoMap_{TempoChange{1, 1, 120.0}};
    std::vector<TimeSignature> meterMap_{TimeSignature{1, 4, 4}};
};

} // namespace dave::document
