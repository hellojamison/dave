// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "editing/Command.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace dave::editing {

// AddTrack: creates a new track. Undo removes it (and its clips).
class AddTrackCommand : public Command {
public:
    explicit AddTrackCommand(std::string name) : name_(std::move(name)) {}
    void perform(document::Edit& e) override { trackId_ = e.addTrack(name_); }
    void undo(document::Edit& e) override { e.removeTrack(trackId_); }
    std::string name() const override { return "Add Track"; }
    const std::string& trackId() const { return trackId_; }
private:
    std::string name_;
    std::string trackId_;
};

// AddClip: places a clip on a track. Undo removes the clip.
class AddClipCommand : public Command {
public:
    AddClipCommand(std::string trackId, document::AudioClip clip)
        : trackId_(std::move(trackId)), clip_(std::move(clip)) {}
    void perform(document::Edit& e) override { clipId_ = e.addClip(trackId_, clip_); }
    void undo(document::Edit& e) override { e.removeClip(trackId_, clipId_); }
    std::string name() const override { return "Add Clip"; }
    const std::string& clipId() const { return clipId_; }
private:
    std::string trackId_;
    document::AudioClip clip_;
    std::string clipId_;
};

// Commit a finished recording to the document. Audio I/O and hashing have
// already completed before this command is constructed, so undo/redo is a
// deterministic model edit and never observes a changing file.
//
// Undo deliberately keeps both the asset record and WAV on disk. A recorded
// performance must not become unrecoverable because the user pressed Undo;
// orphan cleanup belongs to a separate, explicitly recoverable workflow.
class CommitTakeCommand : public Command {
public:
    CommitTakeCommand(std::string trackId, document::AudioAsset asset,
                      document::AudioClip clip)
        : trackId_(std::move(trackId)), asset_(std::move(asset)),
          clip_(std::move(clip)) {}

    void perform(document::Edit& e) override {
        e.loadAsset_(asset_);
        if (clipId_.empty()) {
            clipId_ = e.addClip(trackId_, clip_);
            clip_.id = clipId_;
        } else {
            e.restoreClip_(trackId_, clip_);
        }
    }

    void undo(document::Edit& e) override {
        e.removeClip(trackId_, clipId_);
    }

    std::string name() const override { return "Commit Take"; }
    const std::string& clipId() const { return clipId_; }

private:
    std::string trackId_;
    document::AudioAsset asset_;
    document::AudioClip clip_;
    std::string clipId_;
};

// MoveClip: changes a clip's timelineStart AND optionally its track.
// Undo restores the original position and track. Used by the timeline drag
// (which can move a clip sideways and/or drop it onto another track).
class MoveClipCommand : public Command {
public:
    MoveClipCommand(std::string trackId, std::string clipId,
                    int64_t newStart, std::string newTrackId = "")
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)),
          newStart_(newStart), newTrackId_(std::move(newTrackId)) {}

    void perform(document::Edit& e) override {
        // Snapshot the original (track + position) so undo can restore it.
        auto* c = e.clip(trackId_, clipId_);
        if (c) {
            oldStart_ = c->timelineStart;
            snapshot_ = *c; // captures sourceOffset/length/fades/etc.
        }
        // If moving to a different track, remove from old and add to new.
        if (!newTrackId_.empty() && newTrackId_ != trackId_) {
            e.removeClip(trackId_, clipId_);
            // Preserve the original id so references stay stable.
            snapshot_.id = clipId_;
            snapshot_.timelineStart = newStart_;
            e.addClip(newTrackId_, snapshot_);
            // addClip assigns a NEW id; patch it back to the stable one.
            auto* moved = e.clip(newTrackId_, clipId_);
            // The clip is now the one with the new id; rename it.
            // (Find the last clip on the track and rename.)
            auto* t = e.track(newTrackId_);
            if (t && !t->clips.empty()) {
                t->clips.back().id = clipId_;
            }
            (void)moved;
        } else {
            // Same-track move: just update position.
            auto* clip = e.clip(trackId_, clipId_);
            if (clip) { clip->timelineStart = newStart_; e.notifyChanged(); }
        }
    }

    void undo(document::Edit& e) override {
        // If it moved tracks, reverse: remove from new track, restore to old.
        if (!newTrackId_.empty() && newTrackId_ != trackId_) {
            e.removeClip(newTrackId_, clipId_);
            snapshot_.id = clipId_;
            e.addClip(trackId_, snapshot_);
            auto* t = e.track(trackId_);
            if (t && !t->clips.empty()) t->clips.back().id = clipId_;
        } else {
            auto* c = e.clip(trackId_, clipId_);
            if (c) { c->timelineStart = oldStart_; e.notifyChanged(); }
        }
    }

    std::string name() const override { return "Move Clip"; }

private:
    std::string trackId_;     // original track
    std::string clipId_;
    int64_t newStart_;
    std::string newTrackId_;  // empty = same track
    int64_t oldStart_ = 0;
    document::AudioClip snapshot_;
};

// RemoveClip: deletes a clip. Undo restores it (id + position preserved).
class RemoveClipCommand : public Command {
public:
    RemoveClipCommand(std::string trackId, std::string clipId)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)) {}
    void perform(document::Edit& e) override {
        // Stash the clip so undo can restore it exactly.
        if (auto* c = e.clip(trackId_, clipId_)) { snapshot_ = *c; }
        e.removeClip(trackId_, clipId_);
    }
    void undo(document::Edit& e) override {
        // Re-add; addClip assigns a NEW id, but we want the same id for stable
        // references. Patch the id back after add.
        std::string newId = e.addClip(trackId_, snapshot_);
        if (auto* c = e.clip(trackId_, newId)) { c->id = clipId_; }
    }
    std::string name() const override { return "Remove Clip"; }
private:
    std::string trackId_;
    std::string clipId_;
    document::AudioClip snapshot_;
};

// AddPlugin: appends a plugin slot to a track's effect chain. Undo removes it.
class AddPluginCommand : public Command {
public:
    AddPluginCommand(std::string trackId, document::PluginSlot slot)
        : trackId_(std::move(trackId)), slot_(std::move(slot)) {}
    void perform(document::Edit& e) override { slotId_ = e.addPlugin(trackId_, slot_); }
    void undo(document::Edit& e) override { e.removePlugin(trackId_, slotId_); }
    std::string name() const override { return "Add Plugin"; }
    const std::string& slotId() const { return slotId_; }
private:
    std::string trackId_;
    document::PluginSlot slot_;
    std::string slotId_;
};

// Split a clip at a given timeline position. Creates two clips from one:
// the left half keeps the original id+position; the right half is a new clip
// with sourceOffset adjusted. Undo removes the right clip (restoring one).
class SplitClipCommand : public Command {
public:
    SplitClipCommand(std::string trackId, std::string clipId, int64_t atSample)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)), atSample_(atSample) {}
    void perform(document::Edit& e) override {
        auto* c = e.clip(trackId_, clipId_);
        if (!c) return;
        // Left clip: trim length. Right clip: new clip starting at atSample.
        document::AudioClip right = *c;
        right.timelineStart = atSample_;
        right.sourceOffset = c->sourceOffset + (atSample_ - c->timelineStart);
        right.length = c->timelineStart + c->length - atSample_;
        c->length = atSample_ - c->timelineStart;
        e.notifyChanged();
        // Add the right clip (assigns a new id).
        rightId_ = e.addClip(trackId_, right);
    }
    void undo(document::Edit& e) override {
        if (rightId_.empty()) return;
        // Remove the right clip, restore the left clip's original length.
        auto* c = e.clip(trackId_, clipId_);
        if (c) { c->length += /* right length */ 0; } // simplified; the right
        // clip's length was the remainder. We re-merge by reading the right
        // clip before removing.
        auto* r = e.clip(trackId_, rightId_);
        if (r && c) { c->length = c->length + r->length; }
        e.removeClip(trackId_, rightId_);
        rightId_.clear();
    }
    std::string name() const override { return "Split Clip"; }
private:
    std::string trackId_;
    std::string clipId_;
    int64_t atSample_;
    std::string rightId_; // id of the right half (for undo)
};

// RemovePlugin: deletes a plugin slot. Undo restores it.
class RemovePluginCommand : public Command {
public:
    RemovePluginCommand(std::string trackId, std::string slotId)
        : trackId_(std::move(trackId)), slotId_(std::move(slotId)) {}
    void perform(document::Edit& e) override {
        // Snapshot the slot for undo.
        const auto* t = e.track(trackId_);
        if (t) for (const auto& s : t->plugins) if (s.id == slotId_) { snapshot_ = s; break; }
        const auto* b = e.bus(trackId_);
        if (b) for (const auto& s : b->plugins) if (s.id == slotId_) { snapshot_ = s; break; }
        e.removePlugin(trackId_, slotId_);
    }
    void undo(document::Edit& e) override {
        std::string newId = e.addPlugin(trackId_, snapshot_);
        if (const auto* t = e.track(trackId_)) {
            for (const auto& s : t->plugins) {
                if (s.id == newId) {
                    const_cast<document::PluginSlot&>(s).id = slotId_;
                    break;
                }
            }
        } else if (const auto* b = e.bus(trackId_)) {
            for (const auto& s : b->plugins) {
                if (s.id == newId) {
                    const_cast<document::PluginSlot&>(s).id = slotId_;
                    break;
                }
            }
        }
    }
    std::string name() const override { return "Remove Plugin"; }
private:
    std::string trackId_;
    std::string slotId_;
    document::PluginSlot snapshot_;
};

// ─── MIDI commands (RB-7) ───────────────────────────────────────────────────
// These mirror the audio commands above one for one. They are deliberately
// modelled on MoveClipCommand / AddMarkerCommand and NOT on SplitClipCommand,
// whose undo is known-broken (it double-counts the right half's length).

// Add an empty MIDI track. Undo removes it (and anything on it).
//
// Redo reuses the id minted by the first perform. The Edit's add* methods mint
// a fresh id every call, so a naive redo would produce a track that is
// identical except for its identity — orphaning the selection, the mixer
// strip, and every id-keyed engine map in one go.
class AddMidiTrackCommand : public Command {
public:
    explicit AddMidiTrackCommand(std::string name) : name_(std::move(name)) {}
    void perform(document::Edit& e) override {
        const std::string minted = e.addMidiTrack(name_);
        if (trackId_.empty()) {
            trackId_ = minted;
        } else if (auto* mt = e.midiTrack(minted)) {
            mt->id = trackId_;
        }
    }
    void undo(document::Edit& e) override { e.removeMidiTrack(trackId_); }
    std::string name() const override { return "Add MIDI Track"; }
    const std::string& trackId() const { return trackId_; }
private:
    std::string name_;
    std::string trackId_;
};

// Remove a MIDI track. Undo restores it whole — clips, instrument, effect
// chain, and crucially its position in the track list, since the list order is
// the row order the user sees.
class RemoveMidiTrackCommand : public Command {
public:
    explicit RemoveMidiTrackCommand(std::string trackId)
        : trackId_(std::move(trackId)) {}
    void perform(document::Edit& e) override {
        auto& tracks = e.midiTracksMut();
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].id == trackId_) {
                snapshot_ = tracks[i];
                index_ = i;
                break;
            }
        }
        e.removeMidiTrack(trackId_);
    }
    void undo(document::Edit& e) override {
        if (snapshot_.id.empty()) return;
        auto& tracks = e.midiTracksMut();
        const size_t at = index_ < tracks.size() ? index_ : tracks.size();
        tracks.insert(tracks.begin() + static_cast<ptrdiff_t>(at), snapshot_);
        e.notifyChanged();
    }
    std::string name() const override { return "Remove MIDI Track"; }
private:
    std::string trackId_;
    document::MidiTrack snapshot_;
    size_t index_ = 0;
};

// Import a .mid file as one MIDI track per non-empty SMF track.
//
// The command takes tracks that have ALREADY been parsed rather than a path:
// parsing lives in engine/midi/SmfReader, and editing/ must not depend on
// engine/. It also makes undo/redo cheap and deterministic — redo can't
// observe a file that changed on disk since the import.
class ImportMidiFileCommand : public Command {
public:
    ImportMidiFileCommand(std::vector<document::MidiTrack> tracks,
                          std::string displayName)
        : tracks_(std::move(tracks)), displayName_(std::move(displayName)) {}

    void perform(document::Edit& e) override {
        if (applied_) {
            // Redo: replay the exact tracks the first perform produced, ids
            // and all. Re-running the mint dance would give every track and
            // clip a new identity, so a redo would look right on screen while
            // quietly orphaning the selection and the engine's id-keyed maps.
            auto& tracks = e.midiTracksMut();
            tracks.insert(tracks.end(), tracks_.begin(), tracks_.end());
            e.notifyChanged();
            return;
        }
        trackIds_.clear();
        std::vector<document::MidiTrack> applied;
        for (const auto& src : tracks_) {
            const std::string id = e.addMidiTrack(src.name);
            auto* mt = e.midiTrack(id);
            if (mt == nullptr) continue;
            // Keep the id the Edit just minted (it's unique); take everything
            // else from the parsed track.
            mt->gain = src.gain;
            mt->pan = src.pan;
            mt->instrument = src.instrument;
            mt->plugins = src.plugins;
            for (const auto& clip : src.clips) e.addMidiClip(id, clip);
            applied.push_back(*mt);
            trackIds_.push_back(id);
        }
        tracks_ = std::move(applied);
        applied_ = true;
        e.notifyChanged();
    }

    void undo(document::Edit& e) override {
        for (const auto& id : trackIds_) e.removeMidiTrack(id);
    }

    std::string name() const override {
        return displayName_.empty() ? "Import MIDI" : ("Import " + displayName_);
    }
    const std::vector<std::string>& trackIds() const { return trackIds_; }

private:
    std::vector<document::MidiTrack> tracks_;
    std::string displayName_;
    std::vector<std::string> trackIds_;
    bool applied_ = false;
};

// Add a MIDI clip to a track. Undo removes it.
class AddMidiClipCommand : public Command {
public:
    AddMidiClipCommand(std::string trackId, document::MidiClip clip)
        : trackId_(std::move(trackId)), clip_(std::move(clip)) {}
    void perform(document::Edit& e) override {
        const std::string minted = e.addMidiClip(trackId_, clip_);
        if (clipId_.empty()) {
            clipId_ = minted;
        } else if (auto* c = e.midiClip(trackId_, minted)) {
            c->id = clipId_;   // redo keeps the original identity
        }
    }
    void undo(document::Edit& e) override { e.removeMidiClip(trackId_, clipId_); }
    std::string name() const override { return "Add MIDI Clip"; }
    const std::string& clipId() const { return clipId_; }
private:
    std::string trackId_;
    document::MidiClip clip_;
    std::string clipId_;
};

// Move a MIDI clip along the timeline and/or onto another MIDI track.
// Same shape as MoveClipCommand, including the id-patching after re-add.
class MoveMidiClipCommand : public Command {
public:
    MoveMidiClipCommand(std::string trackId, std::string clipId,
                        int64_t newStart, std::string newTrackId = "")
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)),
          newStart_(newStart), newTrackId_(std::move(newTrackId)) {}

    void perform(document::Edit& e) override {
        if (auto* c = e.midiClip(trackId_, clipId_)) {
            oldStart_ = c->timelineStart;
            snapshot_ = *c;
        }
        if (crossesTracks()) {
            e.removeMidiClip(trackId_, clipId_);
            snapshot_.timelineStart = newStart_;
            reinsert(e, newTrackId_);
        } else if (auto* c = e.midiClip(trackId_, clipId_)) {
            c->timelineStart = newStart_;
            e.notifyChanged();
        }
    }

    void undo(document::Edit& e) override {
        if (crossesTracks()) {
            e.removeMidiClip(newTrackId_, clipId_);
            snapshot_.timelineStart = oldStart_;
            reinsert(e, trackId_);
        } else if (auto* c = e.midiClip(trackId_, clipId_)) {
            c->timelineStart = oldStart_;
            e.notifyChanged();
        }
    }

    std::string name() const override { return "Move MIDI Clip"; }

private:
    bool crossesTracks() const {
        return !newTrackId_.empty() && newTrackId_ != trackId_;
    }
    // addMidiClip mints a fresh id; patch the stable one back so undo/redo and
    // any selection referring to the clip keep working.
    void reinsert(document::Edit& e, const std::string& intoTrack) {
        const std::string mintedId = e.addMidiClip(intoTrack, snapshot_);
        if (auto* c = e.midiClip(intoTrack, mintedId)) c->id = clipId_;
    }

    std::string trackId_;
    std::string clipId_;
    int64_t newStart_;
    std::string newTrackId_;
    int64_t oldStart_ = 0;
    document::MidiClip snapshot_;
};

// Trim a MIDI clip's head or tail. The caller supplies the resulting
// (timelineStart, sourceOffset, length) triple; undo restores all three, since
// a head trim moves two of them together and restoring only one would slide
// the clip's contents relative to its box.
class TrimMidiClipCommand : public Command {
public:
    TrimMidiClipCommand(std::string trackId, std::string clipId,
                        int64_t newStart, int64_t newSourceOffset, int64_t newLength)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)),
          newStart_(newStart), newSourceOffset_(newSourceOffset),
          newLength_(newLength) {}
    void perform(document::Edit& e) override {
        auto* c = e.midiClip(trackId_, clipId_);
        if (c == nullptr) return;
        oldStart_ = c->timelineStart;
        oldSourceOffset_ = c->sourceOffset;
        oldLength_ = c->length;
        c->timelineStart = newStart_;
        c->sourceOffset = newSourceOffset_;
        c->length = newLength_;
        e.notifyChanged();
    }
    void undo(document::Edit& e) override {
        auto* c = e.midiClip(trackId_, clipId_);
        if (c == nullptr) return;
        c->timelineStart = oldStart_;
        c->sourceOffset = oldSourceOffset_;
        c->length = oldLength_;
        e.notifyChanged();
    }
    std::string name() const override { return "Trim MIDI Clip"; }
private:
    std::string trackId_;
    std::string clipId_;
    int64_t newStart_, newSourceOffset_, newLength_;
    int64_t oldStart_ = 0, oldSourceOffset_ = 0, oldLength_ = 0;
};

// Split a MIDI clip at a timeline position into two clips that between them
// cover exactly what the original covered.
//
// Written from scratch rather than copied from SplitClipCommand, in two ways
// that matter:
//   - a cut outside the clip is refused. SplitClipCommand computes both halves
//     by subtraction without checking, so splitting past a clip's end gives the
//     right half a NEGATIVE length and makes the left half longer than the
//     original — reachable today by right-clicking a clip with the playhead
//     parked somewhere else.
//   - undo restores a snapshotted length instead of reconstructing it by adding
//     the two halves back together. The reconstruction depends on state this
//     command doesn't own; the snapshot depends on nothing.
class SplitMidiClipCommand : public Command {
public:
    SplitMidiClipCommand(std::string trackId, std::string clipId, int64_t atSample)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)),
          atSample_(atSample) {}

    void perform(document::Edit& e) override {
        auto* c = e.midiClip(trackId_, clipId_);
        if (c == nullptr) return;
        // A cut at or outside the clip's own bounds would produce an empty
        // half; refuse rather than leave a zero-length clip on the timeline.
        if (atSample_ <= c->timelineStart ||
            atSample_ >= c->timelineStart + c->length) {
            return;
        }
        originalLength_ = c->length;

        document::MidiClip right = *c;
        right.timelineStart = atSample_;
        right.sourceOffset = c->sourceOffset + (atSample_ - c->timelineStart);
        right.length = c->timelineStart + c->length - atSample_;
        c->length = atSample_ - c->timelineStart;
        e.notifyChanged();

        // Notes are shared by both halves and filtered by each one's window at
        // playback (see InstrumentNode::bakeClip), so nothing has to be split
        // note by note — and a note straddling the cut is trimmed on the left
        // and re-attacked on the right, which is what a split should sound like.
        const std::string minted = e.addMidiClip(trackId_, right);
        if (rightId_.empty()) rightId_ = minted;
        else if (auto* r = e.midiClip(trackId_, minted)) r->id = rightId_;
    }

    void undo(document::Edit& e) override {
        if (rightId_.empty()) return;
        e.removeMidiClip(trackId_, rightId_);
        if (auto* c = e.midiClip(trackId_, clipId_)) {
            c->length = originalLength_;
            e.notifyChanged();
        }
    }

    std::string name() const override { return "Split MIDI Clip"; }
    const std::string& rightId() const { return rightId_; }

private:
    std::string trackId_;
    std::string clipId_;
    int64_t atSample_;
    int64_t originalLength_ = 0;
    std::string rightId_;
};

// Duplicate a MIDI clip, placing the copy immediately after the original.
class DuplicateMidiClipCommand : public Command {
public:
    DuplicateMidiClipCommand(std::string trackId, std::string clipId)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)) {}

    void perform(document::Edit& e) override {
        const auto* source = e.midiClip(trackId_, clipId_);
        if (source == nullptr) return;
        document::MidiClip copy = *source;
        copy.timelineStart = source->timelineStart + source->length;
        const std::string minted = e.addMidiClip(trackId_, copy);
        if (copyId_.empty()) copyId_ = minted;
        else if (auto* c = e.midiClip(trackId_, minted)) c->id = copyId_;
    }

    void undo(document::Edit& e) override {
        if (!copyId_.empty()) e.removeMidiClip(trackId_, copyId_);
    }

    std::string name() const override { return "Duplicate MIDI Clip"; }
    const std::string& copyId() const { return copyId_; }

private:
    std::string trackId_;
    std::string clipId_;
    std::string copyId_;
};

// Remove a MIDI clip. Undo restores it, notes and id included.
class RemoveMidiClipCommand : public Command {
public:
    RemoveMidiClipCommand(std::string trackId, std::string clipId)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)) {}
    void perform(document::Edit& e) override {
        if (auto* c = e.midiClip(trackId_, clipId_)) snapshot_ = *c;
        e.removeMidiClip(trackId_, clipId_);
    }
    void undo(document::Edit& e) override {
        const std::string mintedId = e.addMidiClip(trackId_, snapshot_);
        if (auto* c = e.midiClip(trackId_, mintedId)) c->id = clipId_;
    }
    std::string name() const override { return "Remove MIDI Clip"; }
private:
    std::string trackId_;
    std::string clipId_;
    document::MidiClip snapshot_;
};

// Set (or clear) a MIDI track's instrument. Undo restores the previous slot —
// including its saved state, so undoing an instrument swap doesn't silently
// reset the synth you had dialled in.
class SetMidiInstrumentCommand : public Command {
public:
    SetMidiInstrumentCommand(std::string trackId, document::PluginSlot slot)
        : trackId_(std::move(trackId)), slot_(std::move(slot)) {}
    void perform(document::Edit& e) override {
        if (const auto* mt = e.midiTrack(trackId_)) previous_ = mt->instrument;
        // On redo, reuse the id minted the first time so the GraphBuilder's
        // instance cache and any open editor window still resolve.
        e.setMidiInstrument(trackId_, slot_);
        if (const auto* mt = e.midiTrack(trackId_)) slot_ = mt->instrument;
    }
    void undo(document::Edit& e) override {
        e.setMidiInstrument(trackId_, previous_);
    }
    std::string name() const override {
        return slot_.uidString.empty() ? "Clear Instrument" : "Set Instrument";
    }
    const document::PluginSlot& slot() const { return slot_; }
private:
    std::string trackId_;
    document::PluginSlot slot_;
    document::PluginSlot previous_;
};

// Append a plugin to a MIDI track's post-instrument effect chain.
class AddMidiPluginCommand : public Command {
public:
    AddMidiPluginCommand(std::string trackId, document::PluginSlot slot)
        : trackId_(std::move(trackId)), slot_(std::move(slot)) {}
    void perform(document::Edit& e) override {
        const std::string minted = e.addMidiPlugin(trackId_, slot_);
        if (slotId_.empty()) {
            slotId_ = minted;
        } else if (auto* mt = e.midiTrack(trackId_)) {
            // Redo keeps the original slot id so the GraphBuilder's plugin
            // instance cache hands back the same loaded plugin.
            for (auto& s : mt->plugins) {
                if (s.id == minted) { s.id = slotId_; break; }
            }
        }
    }
    void undo(document::Edit& e) override { e.removeMidiPlugin(trackId_, slotId_); }
    std::string name() const override { return "Add Plugin"; }
    const std::string& slotId() const { return slotId_; }
private:
    std::string trackId_;
    document::PluginSlot slot_;
    std::string slotId_;
};

// Remove a plugin from a MIDI track's effect chain. Undo restores it (at the
// end of the chain — chain order beyond append/remove has no UI yet).
class RemoveMidiPluginCommand : public Command {
public:
    RemoveMidiPluginCommand(std::string trackId, std::string slotId)
        : trackId_(std::move(trackId)), slotId_(std::move(slotId)) {}
    void perform(document::Edit& e) override {
        if (const auto* mt = e.midiTrack(trackId_)) {
            for (const auto& s : mt->plugins) {
                if (s.id == slotId_) { snapshot_ = s; break; }
            }
        }
        e.removeMidiPlugin(trackId_, slotId_);
    }
    void undo(document::Edit& e) override {
        const std::string mintedId = e.addMidiPlugin(trackId_, snapshot_);
        if (auto* mt = e.midiTrack(trackId_)) {
            for (auto& s : mt->plugins) {
                if (s.id == mintedId) { s.id = slotId_; break; }
            }
        }
    }
    std::string name() const override { return "Remove Plugin"; }
private:
    std::string trackId_;
    std::string slotId_;
    document::PluginSlot snapshot_;
};

// ─── Marker commands (RB-4) ─────────────────────────────────────────────────

// Add a marker track. Undo removes it.
class AddMarkerTrackCommand : public Command {
public:
    explicit AddMarkerTrackCommand(std::string name) : name_(std::move(name)) {}
    void perform(document::Edit& e) override { trackId_ = e.addMarkerTrack(name_); }
    void undo(document::Edit& e) override { e.removeMarkerTrack(trackId_); }
    std::string name() const override { return "Add Marker Track"; }
    const std::string& trackId() const { return trackId_; }
private:
    std::string name_;
    std::string trackId_;
};

// Add a marker to a marker track. Undo removes it.
class AddMarkerCommand : public Command {
public:
    AddMarkerCommand(std::string trackId, document::Marker marker)
        : trackId_(std::move(trackId)), marker_(std::move(marker)) {}
    void perform(document::Edit& e) override { markerId_ = e.addMarker(trackId_, marker_); }
    void undo(document::Edit& e) override { e.removeMarker(trackId_, markerId_); }
    std::string name() const override { return "Add Marker"; }
    const std::string& markerId() const { return markerId_; }
private:
    std::string trackId_;
    document::Marker marker_;
    std::string markerId_;
};

// Move a marker (position and/or length). Undo restores both.
class MoveMarkerCommand : public Command {
public:
    MoveMarkerCommand(std::string trackId, std::string markerId,
                      int64_t newPosition, int64_t newLength)
        : trackId_(std::move(trackId)), markerId_(std::move(markerId)),
          newPosition_(newPosition), newLength_(newLength) {}
    void perform(document::Edit& e) override {
        auto* m = e.marker(trackId_, markerId_);
        if (m) { oldPosition_ = m->position; oldLength_ = m->length;
                 m->position = newPosition_; m->length = newLength_; e.notifyChanged(); }
    }
    void undo(document::Edit& e) override {
        auto* m = e.marker(trackId_, markerId_);
        if (m) { m->position = oldPosition_; m->length = oldLength_; e.notifyChanged(); }
    }
    std::string name() const override { return "Move Marker"; }
private:
    std::string trackId_;
    std::string markerId_;
    int64_t newPosition_;
    int64_t newLength_;
    int64_t oldPosition_ = 0;
    int64_t oldLength_ = 0;
};

// Remove a marker. Undo restores it (id + position + length preserved).
class RemoveMarkerCommand : public Command {
public:
    RemoveMarkerCommand(std::string trackId, std::string markerId)
        : trackId_(std::move(trackId)), markerId_(std::move(markerId)) {}
    void perform(document::Edit& e) override {
        if (auto* m = e.marker(trackId_, markerId_)) snapshot_ = *m;
        e.removeMarker(trackId_, markerId_);
    }
    void undo(document::Edit& e) override {
        std::string newId = e.addMarker(trackId_, snapshot_);
        if (auto* m = e.marker(trackId_, newId)) m->id = markerId_;
    }
    std::string name() const override { return "Remove Marker"; }
private:
    std::string trackId_;
    std::string markerId_;
    document::Marker snapshot_;
};

// ─── Routing commands (Phase 4) ────────────────────────────────────────────

inline document::RouteTarget routeForOwner(const document::Edit& edit,
                                           const std::string& id) {
    if (const auto* track = edit.track(id)) return track->mainOutput;
    if (const auto* track = edit.midiTrack(id)) return track->mainOutput;
    if (const auto* bus = edit.bus(id)) return bus->mainOutput;
    return document::RouteTarget::none();
}

inline std::string colorForOwner(const document::Edit& edit,
                                 const std::string& id) {
    if (const auto* track = edit.track(id)) return track->color;
    if (const auto* track = edit.midiTrack(id)) return track->color;
    if (const auto* bus = edit.bus(id)) return bus->color;
    return {};
}

class SetTrackColorCommand : public Command {
public:
    SetTrackColorCommand(std::string ownerId, std::string color)
        : ownerId_(std::move(ownerId)), color_(std::move(color)) {}
    void perform(document::Edit& edit) override {
        previous_ = colorForOwner(edit, ownerId_);
        edit.setTrackColor(ownerId_, color_);
    }
    void undo(document::Edit& edit) override {
        edit.setTrackColor(ownerId_, previous_);
    }
    std::string name() const override { return "Set Track Color"; }
private:
    std::string ownerId_;
    std::string color_;
    std::string previous_;
};

class SetMainRouteCommand : public Command {
public:
    SetMainRouteCommand(std::string ownerId, document::RouteTarget target)
        : ownerId_(std::move(ownerId)), target_(std::move(target)) {}
    void perform(document::Edit& edit) override {
        previous_ = routeForOwner(edit, ownerId_);
        edit.setMainOutput(ownerId_, target_);
    }
    void undo(document::Edit& edit) override {
        edit.setMainOutput(ownerId_, previous_);
    }
    std::string name() const override { return "Set Main Route"; }
private:
    std::string ownerId_;
    document::RouteTarget target_;
    document::RouteTarget previous_ = document::RouteTarget::none();
};

class SetInputMonitorCommand : public Command {
public:
    SetInputMonitorCommand(std::string trackId, bool enabled)
        : trackId_(std::move(trackId)), enabled_(enabled) {}
    void perform(document::Edit& edit) override {
        if (const auto* track = edit.track(trackId_)) previous_ = track->inputMonitor;
        edit.setInputMonitor(trackId_, enabled_);
    }
    void undo(document::Edit& edit) override {
        edit.setInputMonitor(trackId_, previous_);
    }
    std::string name() const override { return "Set Input Monitor"; }
private:
    std::string trackId_;
    bool enabled_ = false;
    bool previous_ = false;
};

class SetHardwareInputCommand : public Command {
public:
    SetHardwareInputCommand(std::string trackId,
                            document::HardwareChannelSpan input)
        : trackId_(std::move(trackId)), input_(input) {}
    void perform(document::Edit& edit) override {
        if (const auto* track = edit.track(trackId_)) previous_ = track->hardwareInput;
        edit.setTrackHardwareInput(trackId_, input_);
    }
    void undo(document::Edit& edit) override {
        edit.setTrackHardwareInput(trackId_, previous_);
    }
    std::string name() const override { return "Set Hardware Input"; }
private:
    std::string trackId_;
    document::HardwareChannelSpan input_;
    document::HardwareChannelSpan previous_;
};

class AddSendCommand : public Command {
public:
    AddSendCommand(std::string ownerId, document::AuxSend send)
        : ownerId_(std::move(ownerId)), send_(std::move(send)) {}
    void perform(document::Edit& edit) override {
        if (send_.id.empty()) send_.id = edit.addSend(ownerId_, send_);
        else edit.restoreSend_(ownerId_, send_, index_);
    }
    void undo(document::Edit& edit) override {
        if (const auto* sends = ownerSends(edit)) {
            for (size_t i = 0; i < sends->size(); ++i) {
                if ((*sends)[i].id == send_.id) { index_ = i; break; }
            }
        }
        edit.removeSend(ownerId_, send_.id);
    }
    std::string name() const override { return "Add Send"; }
    const std::string& sendId() const { return send_.id; }
private:
    const std::vector<document::AuxSend>* ownerSends(
        const document::Edit& edit) const {
        if (const auto* track = edit.track(ownerId_)) return &track->sends;
        if (const auto* track = edit.midiTrack(ownerId_)) return &track->sends;
        if (const auto* bus = edit.bus(ownerId_)) return &bus->sends;
        return nullptr;
    }
    std::string ownerId_;
    document::AuxSend send_;
    size_t index_ = 0;
};

class AddBusCommand : public Command {
public:
    explicit AddBusCommand(std::string name) : name_(std::move(name)) {}
    void perform(document::Edit& edit) override {
        if (snapshot_.id.empty()) {
            const std::string id = edit.addBus(name_);
            if (const auto* bus = edit.bus(id)) snapshot_ = *bus;
        } else {
            edit.restoreBus_(snapshot_, index_);
        }
    }
    void undo(document::Edit& edit) override {
        const auto& buses = edit.buses();
        for (size_t i = 0; i < buses.size(); ++i) {
            if (buses[i].id == snapshot_.id) { index_ = i; break; }
        }
        edit.removeBus(snapshot_.id);
    }
    std::string name() const override { return "Add Bus"; }
    const std::string& busId() const { return snapshot_.id; }
private:
    std::string name_;
    document::BusTrack snapshot_;
    size_t index_ = 0;
};

class UpdateSendCommand : public Command {
public:
    UpdateSendCommand(std::string ownerId, document::AuxSend send)
        : ownerId_(std::move(ownerId)), send_(std::move(send)) {}
    void perform(document::Edit& edit) override {
        if (const auto* list = sends(edit)) {
            for (const auto& candidate : *list) {
                if (candidate.id == send_.id) { previous_ = candidate; break; }
            }
        }
        edit.updateSend(ownerId_, send_);
    }
    void undo(document::Edit& edit) override { edit.updateSend(ownerId_, previous_); }
    std::string name() const override { return "Update Send"; }
private:
    const std::vector<document::AuxSend>* sends(const document::Edit& edit) const {
        if (const auto* track = edit.track(ownerId_)) return &track->sends;
        if (const auto* track = edit.midiTrack(ownerId_)) return &track->sends;
        if (const auto* bus = edit.bus(ownerId_)) return &bus->sends;
        return nullptr;
    }
    std::string ownerId_;
    document::AuxSend send_;
    document::AuxSend previous_;
};

class RemoveSendCommand : public Command {
public:
    RemoveSendCommand(std::string ownerId, std::string sendId)
        : ownerId_(std::move(ownerId)), sendId_(std::move(sendId)) {}
    void perform(document::Edit& edit) override {
        if (const auto* list = sends(edit)) {
            for (size_t i = 0; i < list->size(); ++i) {
                if ((*list)[i].id == sendId_) { snapshot_ = (*list)[i]; index_ = i; break; }
            }
        }
        edit.removeSend(ownerId_, sendId_);
    }
    void undo(document::Edit& edit) override {
        edit.restoreSend_(ownerId_, snapshot_, index_);
    }
    std::string name() const override { return "Remove Send"; }
private:
    const std::vector<document::AuxSend>* sends(const document::Edit& edit) const {
        if (const auto* track = edit.track(ownerId_)) return &track->sends;
        if (const auto* track = edit.midiTrack(ownerId_)) return &track->sends;
        if (const auto* bus = edit.bus(ownerId_)) return &bus->sends;
        return nullptr;
    }
    std::string ownerId_;
    std::string sendId_;
    document::AuxSend snapshot_;
    size_t index_ = 0;
};

class RemoveBusCommand : public Command {
public:
    explicit RemoveBusCommand(std::string id) : id_(std::move(id)) {}
    void perform(document::Edit& edit) override {
        const auto& buses = edit.buses();
        for (size_t i = 0; i < buses.size(); ++i) {
            if (buses[i].id == id_) { snapshot_ = buses[i]; index_ = i; break; }
        }
        edit.removeBus(id_);
    }
    void undo(document::Edit& edit) override {
        edit.restoreBus_(snapshot_, index_);
    }
    std::string name() const override { return "Remove Bus"; }
private:
    std::string id_;
    document::BusTrack snapshot_;
    size_t index_ = 0;
};

} // namespace dave::editing
