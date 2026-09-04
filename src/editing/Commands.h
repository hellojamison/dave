// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "editing/Command.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace dave::editing {

// Several edits that undo as one. Built for range delete, where trimming a
// clip on each of a few tracks (and splitting one in two) is a single user
// action and has to be a single Ctrl+Z. Sub-commands run in order and undo in
// reverse, so an add that depends on a preceding trim is unwound before it.
class CompoundCommand : public Command {
public:
    CompoundCommand(std::vector<std::unique_ptr<Command>> commands,
                    std::string label)
        : commands_(std::move(commands)), label_(std::move(label)) {}
    void perform(document::Edit& e) override {
        for (auto& command : commands_) command->perform(e);
    }
    void undo(document::Edit& e) override {
        for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) {
            (*it)->undo(e);
        }
    }
    std::string name() const override { return label_; }
    size_t size() const { return commands_.size(); }
private:
    std::vector<std::unique_ptr<Command>> commands_;
    std::string label_;
};

// AddTrack: creates a new track. Undo removes it (and its clips).
// Add a track. `Flavour` chooses only the id prefix and the default name —
// the object produced is the same either way, and nothing downstream can tell
// afterwards which one made it. It exists so a row created as a bus still
// reads as "Bus 3" in the list.
class AddTrackCommand : public Command {
public:
    enum class Flavour { Audio, Midi, Bus };

    explicit AddTrackCommand(std::string name, Flavour flavour = Flavour::Audio)
        : name_(std::move(name)), flavour_(flavour) {}

    void perform(document::Edit& e) override {
        const std::string minted =
            flavour_ == Flavour::Midi ? e.addMidiTrack(name_)
          : flavour_ == Flavour::Bus  ? e.addBus(name_)
                                      : e.addTrack(name_);
        // Redo must reuse the first id, or every graph cache and selection
        // keyed by it silently points at a track that no longer exists.
        if (trackId_.empty()) {
            trackId_ = minted;
        } else if (auto* t = e.track(minted)) {
            t->id = trackId_;
        }
    }
    void undo(document::Edit& e) override { e.removeTrack(trackId_); }
    std::string name() const override {
        return flavour_ == Flavour::Midi ? "Add MIDI Track"
             : flavour_ == Flavour::Bus  ? "Add Bus"
                                         : "Add Track";
    }
    const std::string& trackId() const { return trackId_; }
    // Retained for the bus call sites that read the new row's id.
    const std::string& busId() const { return trackId_; }
private:
    std::string name_;
    Flavour flavour_ = Flavour::Audio;
    std::string trackId_;
};

// Remove any track. Undo restores it whole — clips, instrument, chain, sends
// and its row position, since list order is the order the user sees. Audio
// tracks had no remove command at all before the merge.
class RemoveTrackCommand : public Command {
public:
    explicit RemoveTrackCommand(std::string trackId)
        : trackId_(std::move(trackId)) {}
    void perform(document::Edit& e) override {
        const auto& tracks = e.tracks();
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].id == trackId_) {
                snapshot_ = tracks[i];
                index_ = i;
                break;
            }
        }
        e.removeTrack(trackId_);
    }
    void undo(document::Edit& e) override {
        if (snapshot_.id.empty()) return;
        e.restoreTrack_(snapshot_, index_);
    }
    std::string name() const override { return "Remove Track"; }
private:
    std::string trackId_;
    document::Track snapshot_;
    size_t index_ = 0;
};

// AddClip: places a clip on a track. Undo removes the clip.
class AddClipCommand : public Command {
public:
    AddClipCommand(std::string trackId, document::AudioClip clip)
        : trackId_(std::move(trackId)), clip_(std::move(clip)) {}
    void perform(document::Edit& e) override {
        const std::string minted = e.addClip(trackId_, clip_);
        if (clipId_.empty()) {
            clipId_ = minted;
        } else if (auto* c = e.clip(trackId_, minted)) {
            c->id = clipId_;   // redo keeps the original identity
        }
    }
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
        if (auto* c = e.clip(trackId_, clipId_)) {
            oldStart_ = c->timelineStart;
            snapshot_ = *c;   // captures sourceOffset/length/fades/etc.
        }
        if (crossesTracks()) {
            e.removeClip(trackId_, clipId_);
            snapshot_.timelineStart = newStart_;
            reinsert(e, newTrackId_);
        } else if (auto* c = e.clip(trackId_, clipId_)) {
            c->timelineStart = newStart_;
            e.notifyChanged();
        }
    }

    void undo(document::Edit& e) override {
        if (crossesTracks()) {
            e.removeClip(newTrackId_, clipId_);
            snapshot_.timelineStart = oldStart_;
            reinsert(e, trackId_);
        } else if (auto* c = e.clip(trackId_, clipId_)) {
            c->timelineStart = oldStart_;
            e.notifyChanged();
        }
    }

    std::string name() const override { return "Move Clip"; }

private:
    bool crossesTracks() const {
        return !newTrackId_.empty() && newTrackId_ != trackId_;
    }
    // addClip mints a fresh id; patch the stable one back so undo/redo and any
    // selection referring to the clip keep working. Resolving the re-added clip
    // by its minted id rather than as the track's last clip is the difference
    // that matters — a track whose clips are not in insertion order used to get
    // the wrong clip renamed.
    void reinsert(document::Edit& e, const std::string& intoTrack) {
        const std::string mintedId = e.addClip(intoTrack, snapshot_);
        if (auto* c = e.clip(intoTrack, mintedId)) c->id = clipId_;
    }

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

// Split a clip at a given timeline position into two clips that between them
// cover exactly what the original covered. Modelled on SplitMidiClipCommand,
// which was written from scratch because this command's original undo was
// broken: it rebuilt the left half's length by adding the right half back,
// and it computed both halves by subtraction without checking that the cut
// fell inside the clip. A cut past a clip's end therefore gave the right half
// a negative length — reachable by right-clicking a clip with the playhead
// parked elsewhere.
class SplitClipCommand : public Command {
public:
    SplitClipCommand(std::string trackId, std::string clipId, int64_t atSample)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)),
          atSample_(atSample) {}

    void perform(document::Edit& e) override {
        auto* c = e.clip(trackId_, clipId_);
        if (c == nullptr) return;
        // A cut at or outside the clip's own bounds would produce an empty
        // half; refuse rather than leave a zero-length clip on the timeline.
        if (atSample_ <= c->timelineStart ||
            atSample_ >= c->timelineStart + c->length) {
            return;
        }
        originalLength_ = c->length;

        document::AudioClip right = *c;
        right.timelineStart = atSample_;
        right.sourceOffset = c->sourceOffset + (atSample_ - c->timelineStart);
        right.length = c->timelineStart + c->length - atSample_;
        c->length = atSample_ - c->timelineStart;
        e.notifyChanged();

        const std::string minted = e.addClip(trackId_, right);
        if (rightId_.empty()) rightId_ = minted;
        else if (auto* r = e.clip(trackId_, minted)) r->id = rightId_;
    }

    void undo(document::Edit& e) override {
        if (rightId_.empty()) return;
        e.removeClip(trackId_, rightId_);
        if (auto* c = e.clip(trackId_, clipId_)) {
            c->length = originalLength_;
            e.notifyChanged();
        }
    }

    std::string name() const override { return "Split Clip"; }
    const std::string& rightId() const { return rightId_; }

private:
    std::string trackId_;
    std::string clipId_;
    int64_t atSample_;
    int64_t originalLength_ = 0;
    std::string rightId_;
};

// Trim an audio clip's head or tail. The caller supplies the resulting
// (timelineStart, sourceOffset, length) triple; undo restores all three, since
// a head trim moves two of them together and restoring only one would slide
// the clip's contents relative to its box.
//
// Bounds are the gesture's business, not this command's: the timeline clamps
// the drag so a trim can't invert the clip or run past its source, and a
// scripted caller that wants a specific triple gets exactly that triple.
class TrimClipCommand : public Command {
public:
    TrimClipCommand(std::string trackId, std::string clipId,
                    int64_t newStart, int64_t newSourceOffset, int64_t newLength)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)),
          newStart_(newStart), newSourceOffset_(newSourceOffset),
          newLength_(newLength) {}
    void perform(document::Edit& e) override {
        auto* c = e.clip(trackId_, clipId_);
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
        auto* c = e.clip(trackId_, clipId_);
        if (c == nullptr) return;
        c->timelineStart = oldStart_;
        c->sourceOffset = oldSourceOffset_;
        c->length = oldLength_;
        e.notifyChanged();
    }
    std::string name() const override { return "Trim Clip"; }
private:
    std::string trackId_;
    std::string clipId_;
    int64_t newStart_, newSourceOffset_, newLength_;
    int64_t oldStart_ = 0, oldSourceOffset_ = 0, oldLength_ = 0;
};

// Set a clip's fades — both lengths and their curve shapes at once. The caller
// passes the whole fade state (copying the side it isn't touching straight from
// the clip), so one command serves a fade-in, a fade-out, or a preset applied
// to both, and a single undo puts every field back.
class SetClipFadeCommand : public Command {
public:
    SetClipFadeCommand(std::string trackId, std::string clipId, int64_t fadeIn,
                       document::FadeShape fadeInShape, int64_t fadeOut,
                       document::FadeShape fadeOutShape)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)),
          fadeIn_(fadeIn), fadeOut_(fadeOut), fadeInShape_(fadeInShape),
          fadeOutShape_(fadeOutShape) {}
    void perform(document::Edit& e) override {
        auto* c = e.clip(trackId_, clipId_);
        if (c == nullptr) return;
        oldFadeIn_ = c->fadeIn;
        oldFadeOut_ = c->fadeOut;
        oldFadeInShape_ = c->fadeInShape;
        oldFadeOutShape_ = c->fadeOutShape;
        c->fadeIn = fadeIn_;
        c->fadeOut = fadeOut_;
        c->fadeInShape = fadeInShape_;
        c->fadeOutShape = fadeOutShape_;
        e.notifyChanged();
    }
    void undo(document::Edit& e) override {
        auto* c = e.clip(trackId_, clipId_);
        if (c == nullptr) return;
        c->fadeIn = oldFadeIn_;
        c->fadeOut = oldFadeOut_;
        c->fadeInShape = oldFadeInShape_;
        c->fadeOutShape = oldFadeOutShape_;
        e.notifyChanged();
    }
    std::string name() const override { return "Set Fade"; }
private:
    std::string trackId_;
    std::string clipId_;
    int64_t fadeIn_, fadeOut_;
    document::FadeShape fadeInShape_, fadeOutShape_;
    int64_t oldFadeIn_ = 0, oldFadeOut_ = 0;
    document::FadeShape oldFadeInShape_ = document::FadeShape::Linear;
    document::FadeShape oldFadeOutShape_ = document::FadeShape::Linear;
};

// Set an audio clip's per-clip gain. Undo restores the old value.
class SetClipGainCommand : public Command {
public:
    SetClipGainCommand(std::string trackId, std::string clipId, double gain)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)),
          gain_(gain) {}
    void perform(document::Edit& e) override {
        auto* c = e.clip(trackId_, clipId_);
        if (c == nullptr) return;
        oldGain_ = c->gain;
        c->gain = gain_;
        e.notifyChanged();
    }
    void undo(document::Edit& e) override {
        if (auto* c = e.clip(trackId_, clipId_)) {
            c->gain = oldGain_;
            e.notifyChanged();
        }
    }
    std::string name() const override { return "Clip Gain"; }
private:
    std::string trackId_;
    std::string clipId_;
    double gain_, oldGain_ = 1.0;
};

// Set an audio clip's mute. The caller passes the desired state (a toggle
// passes the negation of what it read), so undo is a plain restore.
class SetClipMuteCommand : public Command {
public:
    SetClipMuteCommand(std::string trackId, std::string clipId, bool muted)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)),
          muted_(muted) {}
    void perform(document::Edit& e) override {
        auto* c = e.clip(trackId_, clipId_);
        if (c == nullptr) return;
        oldMuted_ = c->muted;
        c->muted = muted_;
        e.notifyChanged();
    }
    void undo(document::Edit& e) override {
        if (auto* c = e.clip(trackId_, clipId_)) {
            c->muted = oldMuted_;
            e.notifyChanged();
        }
    }
    std::string name() const override { return muted_ ? "Mute Clip"
                                                      : "Unmute Clip"; }
private:
    std::string trackId_;
    std::string clipId_;
    bool muted_, oldMuted_ = false;
};

// Copy a clip onto a track at a position — Option-dragging leaves the original
// where it was and drops a copy at the release. Undo removes the copy.
class CopyClipToCommand : public Command {
public:
    CopyClipToCommand(std::string srcTrack, std::string srcClip,
                      std::string dstTrack, int64_t position)
        : srcTrack_(std::move(srcTrack)), srcClip_(std::move(srcClip)),
          dstTrack_(std::move(dstTrack)), position_(position) {}
    void perform(document::Edit& e) override {
        const auto* src = e.clip(srcTrack_, srcClip_);
        if (src == nullptr) return;
        document::AudioClip copy = *src;
        copy.timelineStart = position_;
        const std::string minted = e.addClip(dstTrack_, copy);
        if (copyId_.empty()) copyId_ = minted;
        else if (auto* c = e.clip(dstTrack_, minted)) c->id = copyId_;
    }
    void undo(document::Edit& e) override {
        if (!copyId_.empty()) e.removeClip(dstTrack_, copyId_);
    }
    std::string name() const override { return "Copy Clip"; }
    const std::string& copyId() const { return copyId_; }
private:
    std::string srcTrack_, srcClip_, dstTrack_;
    int64_t position_;
    std::string copyId_;
};

// The MIDI twin of CopyClipToCommand.
class CopyMidiClipToCommand : public Command {
public:
    CopyMidiClipToCommand(std::string srcTrack, std::string srcClip,
                          std::string dstTrack, int64_t position)
        : srcTrack_(std::move(srcTrack)), srcClip_(std::move(srcClip)),
          dstTrack_(std::move(dstTrack)), position_(position) {}
    void perform(document::Edit& e) override {
        const auto* src = e.midiClip(srcTrack_, srcClip_);
        if (src == nullptr) return;
        document::MidiClip copy = *src;
        copy.timelineStart = position_;
        const std::string minted = e.addMidiClip(dstTrack_, copy);
        if (copyId_.empty()) copyId_ = minted;
        else if (auto* c = e.midiClip(dstTrack_, minted)) c->id = copyId_;
    }
    void undo(document::Edit& e) override {
        if (!copyId_.empty()) e.removeMidiClip(dstTrack_, copyId_);
    }
    std::string name() const override { return "Copy Clip"; }
private:
    std::string srcTrack_, srcClip_, dstTrack_;
    int64_t position_;
    std::string copyId_;
};

// Duplicate an audio clip, placing the copy immediately after the original so
// a repeated Cmd+D lays down a run of clips end to end.
class DuplicateClipCommand : public Command {
public:
    DuplicateClipCommand(std::string trackId, std::string clipId)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)) {}

    void perform(document::Edit& e) override {
        const auto* source = e.clip(trackId_, clipId_);
        if (source == nullptr) return;
        document::AudioClip copy = *source;
        copy.timelineStart = source->timelineStart + source->length;
        const std::string minted = e.addClip(trackId_, copy);
        if (copyId_.empty()) copyId_ = minted;
        else if (auto* c = e.clip(trackId_, minted)) c->id = copyId_;
    }

    void undo(document::Edit& e) override {
        if (!copyId_.empty()) e.removeClip(trackId_, copyId_);
    }

    std::string name() const override { return "Duplicate Clip"; }
    const std::string& copyId() const { return copyId_; }

private:
    std::string trackId_;
    std::string clipId_;
    std::string copyId_;
};

// RemovePlugin: deletes a plugin slot. Undo restores it.
class RemovePluginCommand : public Command {
public:
    RemovePluginCommand(std::string trackId, std::string slotId)
        : trackId_(std::move(trackId)), slotId_(std::move(slotId)) {}
    void perform(document::Edit& e) override {
        // Snapshot the slot for undo.
        if (const auto* t = e.track(trackId_)) {
            for (const auto& s : t->plugins) {
                if (s.id == slotId_) { snapshot_ = s; break; }
            }
        }
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
            auto& tracks = e.tracksMut();
            // Ahead of Main, which is always the last row.
            const auto main = std::find_if(
                tracks.begin(), tracks.end(),
                [](const document::Track& t) { return t.isMain; });
            tracks.insert(main, tracks_.begin(), tracks_.end());
            e.notifyChanged();
            return;
        }
        trackIds_.clear();
        std::vector<document::MidiTrack> applied;
        for (const auto& src : tracks_) {
            const std::string id = e.addMidiTrack(src.name);
            auto* mt = e.track(id);
            if (mt == nullptr) continue;
            // Keep the id the Edit just minted (it's unique); take everything
            // else from the parsed track.
            mt->gain = src.gain;
            mt->pan = src.pan;
            mt->instrument = src.instrument;
            mt->plugins = src.plugins;
            for (const auto& clip : src.midiClips) e.addMidiClip(id, clip);
            applied.push_back(*mt);
            trackIds_.push_back(id);
        }
        tracks_ = std::move(applied);
        applied_ = true;
        e.notifyChanged();
    }

    void undo(document::Edit& e) override {
        for (const auto& id : trackIds_) e.removeTrack(id);
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
        if (const auto* mt = e.track(trackId_)) previous_ = mt->instrument;
        // On redo, reuse the id minted the first time so the GraphBuilder's
        // instance cache and any open editor window still resolve.
        e.setMidiInstrument(trackId_, slot_);
        if (const auto* mt = e.track(trackId_)) slot_ = mt->instrument;
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
    return document::RouteTarget::none();
}

inline std::string colorForOwner(const document::Edit& edit,
                                 const std::string& id) {
    if (const auto* track = edit.track(id)) return track->color;
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

class AddVolumeAutomationPointCommand : public Command {
public:
    AddVolumeAutomationPointCommand(std::string ownerId, int64_t sample,
                                    double db)
        : ownerId_(std::move(ownerId)) {
        point_.sample = sample;
        point_.db = db;
    }
    void perform(document::Edit& edit) override {
        if (point_.id.empty()) {
            point_.id = edit.addVolumeAutomationPoint(
                ownerId_, point_.sample, point_.db);
        } else {
            edit.restoreVolumeAutomationPoint_(ownerId_, point_, index_);
        }
    }
    void undo(document::Edit& edit) override {
        const auto* points = edit.volumeAutomation(ownerId_);
        if (points != nullptr) {
            const auto found = std::find_if(
                points->begin(), points->end(), [&](const auto& point) {
                    return point.id == point_.id;
                });
            if (found != points->end()) {
                index_ = static_cast<size_t>(found - points->begin());
                point_ = *found;
            }
        }
        edit.removeVolumeAutomationPoint(ownerId_, point_.id);
    }
    std::string name() const override { return "Add Volume Automation Point"; }
private:
    std::string ownerId_;
    document::VolumeAutomationPoint point_;
    size_t index_ = 0;
};

class MoveVolumeAutomationPointCommand : public Command {
public:
    MoveVolumeAutomationPointCommand(
        std::string ownerId, document::VolumeAutomationPoint point)
        : ownerId_(std::move(ownerId)), point_(std::move(point)) {}
    void perform(document::Edit& edit) override {
        const auto* points = edit.volumeAutomation(ownerId_);
        if (points != nullptr) {
            const auto found = std::find_if(
                points->begin(), points->end(), [&](const auto& existing) {
                    return existing.id == point_.id;
                });
            if (found != points->end()) previous_ = *found;
        }
        edit.updateVolumeAutomationPoint(ownerId_, point_);
    }
    void undo(document::Edit& edit) override {
        edit.updateVolumeAutomationPoint(ownerId_, previous_);
    }
    std::string name() const override { return "Move Volume Automation Point"; }
private:
    std::string ownerId_;
    document::VolumeAutomationPoint point_;
    document::VolumeAutomationPoint previous_;
};

class RemoveVolumeAutomationPointCommand : public Command {
public:
    RemoveVolumeAutomationPointCommand(std::string ownerId,
                                       std::string pointId)
        : ownerId_(std::move(ownerId)), pointId_(std::move(pointId)) {}
    void perform(document::Edit& edit) override {
        const auto* points = edit.volumeAutomation(ownerId_);
        if (points != nullptr) {
            const auto found = std::find_if(
                points->begin(), points->end(), [&](const auto& point) {
                    return point.id == pointId_;
                });
            if (found != points->end()) {
                index_ = static_cast<size_t>(found - points->begin());
                point_ = *found;
            }
        }
        edit.removeVolumeAutomationPoint(ownerId_, pointId_);
    }
    void undo(document::Edit& edit) override {
        edit.restoreVolumeAutomationPoint_(ownerId_, point_, index_);
    }
    std::string name() const override {
        return "Remove Volume Automation Point";
    }
private:
    std::string ownerId_;
    std::string pointId_;
    document::VolumeAutomationPoint point_;
    size_t index_ = 0;
};

class ReplaceVolumeAutomationCommand : public Command {
public:
    ReplaceVolumeAutomationCommand(
        std::string ownerId,
        std::vector<document::VolumeAutomationPoint> points)
        : ownerId_(std::move(ownerId)), after_(std::move(points)) {}
    void perform(document::Edit& edit) override {
        if (!initialized_) {
            if (const auto* points = edit.volumeAutomation(ownerId_)) {
                before_ = *points;
            }
            initialized_ = true;
        }
        if (edit.replaceVolumeAutomation(ownerId_, after_)) {
            if (const auto* points = edit.volumeAutomation(ownerId_)) {
                after_ = *points;
            }
        }
    }
    void undo(document::Edit& edit) override {
        edit.replaceVolumeAutomation(ownerId_, before_);
    }
    std::string name() const override { return "Draw Volume Automation"; }
private:
    std::string ownerId_;
    std::vector<document::VolumeAutomationPoint> before_;
    std::vector<document::VolumeAutomationPoint> after_;
    bool initialized_ = false;
};

class AddPanAutomationPointCommand : public Command {
public:
    AddPanAutomationPointCommand(std::string ownerId, int64_t sample,
                                 double pan)
        : ownerId_(std::move(ownerId)) {
        point_.sample = sample;
        point_.pan = pan;
    }
    void perform(document::Edit& edit) override {
        if (point_.id.empty()) {
            point_.id = edit.addPanAutomationPoint(
                ownerId_, point_.sample, point_.pan);
        } else {
            edit.restorePanAutomationPoint_(ownerId_, point_, index_);
        }
    }
    void undo(document::Edit& edit) override {
        const auto* points = edit.panAutomation(ownerId_);
        if (points != nullptr) {
            const auto found = std::find_if(
                points->begin(), points->end(), [&](const auto& point) {
                    return point.id == point_.id;
                });
            if (found != points->end()) {
                index_ = static_cast<size_t>(found - points->begin());
                point_ = *found;
            }
        }
        edit.removePanAutomationPoint(ownerId_, point_.id);
    }
    std::string name() const override { return "Add Pan Automation Point"; }
private:
    std::string ownerId_;
    document::PanAutomationPoint point_;
    size_t index_ = 0;
};

class MovePanAutomationPointCommand : public Command {
public:
    MovePanAutomationPointCommand(
        std::string ownerId, document::PanAutomationPoint point)
        : ownerId_(std::move(ownerId)), point_(std::move(point)) {}
    void perform(document::Edit& edit) override {
        const auto* points = edit.panAutomation(ownerId_);
        if (points != nullptr) {
            const auto found = std::find_if(
                points->begin(), points->end(), [&](const auto& existing) {
                    return existing.id == point_.id;
                });
            if (found != points->end()) previous_ = *found;
        }
        edit.updatePanAutomationPoint(ownerId_, point_);
    }
    void undo(document::Edit& edit) override {
        edit.updatePanAutomationPoint(ownerId_, previous_);
    }
    std::string name() const override { return "Move Pan Automation Point"; }
private:
    std::string ownerId_;
    document::PanAutomationPoint point_;
    document::PanAutomationPoint previous_;
};

class RemovePanAutomationPointCommand : public Command {
public:
    RemovePanAutomationPointCommand(std::string ownerId,
                                    std::string pointId)
        : ownerId_(std::move(ownerId)), pointId_(std::move(pointId)) {}
    void perform(document::Edit& edit) override {
        const auto* points = edit.panAutomation(ownerId_);
        if (points != nullptr) {
            const auto found = std::find_if(
                points->begin(), points->end(), [&](const auto& point) {
                    return point.id == pointId_;
                });
            if (found != points->end()) {
                index_ = static_cast<size_t>(found - points->begin());
                point_ = *found;
            }
        }
        edit.removePanAutomationPoint(ownerId_, pointId_);
    }
    void undo(document::Edit& edit) override {
        edit.restorePanAutomationPoint_(ownerId_, point_, index_);
    }
    std::string name() const override { return "Remove Pan Automation Point"; }
private:
    std::string ownerId_;
    std::string pointId_;
    document::PanAutomationPoint point_;
    size_t index_ = 0;
};

class ReplacePanAutomationCommand : public Command {
public:
    ReplacePanAutomationCommand(
        std::string ownerId,
        std::vector<document::PanAutomationPoint> points)
        : ownerId_(std::move(ownerId)), after_(std::move(points)) {}
    void perform(document::Edit& edit) override {
        if (!initialized_) {
            if (const auto* points = edit.panAutomation(ownerId_)) {
                before_ = *points;
            }
            initialized_ = true;
        }
        if (edit.replacePanAutomation(ownerId_, after_)) {
            if (const auto* points = edit.panAutomation(ownerId_)) {
                after_ = *points;
            }
        }
    }
    void undo(document::Edit& edit) override {
        edit.replacePanAutomation(ownerId_, before_);
    }
    std::string name() const override { return "Draw Pan Automation"; }
private:
    std::string ownerId_;
    std::vector<document::PanAutomationPoint> before_;
    std::vector<document::PanAutomationPoint> after_;
    bool initialized_ = false;
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

// ─── Playlists ─────────────────────────────────────────────────────────────

class AddPlaylistCommand : public Command {
public:
    AddPlaylistCommand(std::string trackId, std::string name,
                       bool duplicateActive)
        : trackId_(std::move(trackId)), name_(std::move(name)),
          duplicate_(duplicateActive) {}
    void perform(document::Edit& e) override {
        if (playlistId_.empty()) {
            playlistId_ = e.addPlaylist(trackId_, name_, duplicate_);
        } else {
            e.restorePlaylist_(trackId_, snapshot_, index_);
        }
    }
    void undo(document::Edit& e) override {
        // Keep the record so redo puts back the same id (and, for a
        // duplicate, the same copied clips).
        if (const auto* t = e.track(trackId_)) {
            for (size_t i = 0; i < t->playlists.size(); ++i) {
                if (t->playlists[i].id == playlistId_) {
                    snapshot_ = t->playlists[i];
                    index_ = i;
                }
            }
        }
        e.removePlaylist(trackId_, playlistId_);
    }
    std::string name() const override {
        return duplicate_ ? "Duplicate Playlist" : "New Playlist";
    }
    const std::string& playlistId() const { return playlistId_; }
private:
    std::string trackId_;
    std::string name_;
    bool duplicate_ = false;
    std::string playlistId_;
    document::Playlist snapshot_;
    size_t index_ = 0;
};

class SwitchPlaylistCommand : public Command {
public:
    SwitchPlaylistCommand(std::string trackId, std::string playlistId)
        : trackId_(std::move(trackId)), playlistId_(std::move(playlistId)) {}
    void perform(document::Edit& e) override {
        if (const auto* t = e.track(trackId_)) previous_ = t->activePlaylistId;
        applied_ = e.switchPlaylist(trackId_, playlistId_);
        // The first switch materialises the roster, which names the
        // previously-implicit playlist; read the id back for undo.
        if (applied_ && previous_.empty()) {
            if (const auto* t = e.track(trackId_)) {
                for (const auto& p : t->playlists) {
                    if (p.id != playlistId_) { previous_ = p.id; break; }
                }
            }
        }
    }
    void undo(document::Edit& e) override {
        if (applied_) e.switchPlaylist(trackId_, previous_);
    }
    std::string name() const override { return "Switch Playlist"; }
private:
    std::string trackId_;
    std::string playlistId_;
    std::string previous_;
    bool applied_ = false;
};

class RenamePlaylistCommand : public Command {
public:
    RenamePlaylistCommand(std::string trackId, std::string playlistId,
                          std::string name)
        : trackId_(std::move(trackId)), playlistId_(std::move(playlistId)),
          name_(std::move(name)) {}
    void perform(document::Edit& e) override {
        if (const auto* p = e.playlist(trackId_, playlistId_)) {
            previous_ = p->name;
        }
        applied_ = e.renamePlaylist(trackId_, playlistId_, name_);
    }
    void undo(document::Edit& e) override {
        if (applied_) e.renamePlaylist(trackId_, playlistId_, previous_);
    }
    std::string name() const override { return "Rename Playlist"; }
private:
    std::string trackId_;
    std::string playlistId_;
    std::string name_;
    std::string previous_;
    bool applied_ = false;
};

class RemovePlaylistCommand : public Command {
public:
    RemovePlaylistCommand(std::string trackId, std::string playlistId)
        : trackId_(std::move(trackId)), playlistId_(std::move(playlistId)) {}
    void perform(document::Edit& e) override {
        if (const auto* t = e.track(trackId_)) {
            for (size_t i = 0; i < t->playlists.size(); ++i) {
                if (t->playlists[i].id == playlistId_) {
                    snapshot_ = t->playlists[i];
                    index_ = i;
                }
            }
        }
        applied_ = e.removePlaylist(trackId_, playlistId_);
    }
    void undo(document::Edit& e) override {
        if (applied_) e.restorePlaylist_(trackId_, snapshot_, index_);
    }
    std::string name() const override { return "Delete Playlist"; }
private:
    std::string trackId_;
    std::string playlistId_;
    document::Playlist snapshot_;
    size_t index_ = 0;
    bool applied_ = false;
};

class AddOutputCommand : public Command {
public:
    AddOutputCommand(std::string ownerId, document::RouteTarget target)
        : ownerId_(std::move(ownerId)), target_(std::move(target)) {}
    void perform(document::Edit& edit) override {
        applied_ = edit.addOutput(ownerId_, target_);
    }
    void undo(document::Edit& edit) override {
        if (applied_) edit.removeOutput(ownerId_, target_);
    }
    std::string name() const override { return "Add Output"; }
private:
    std::string ownerId_;
    document::RouteTarget target_;
    bool applied_ = false;
};

class RemoveOutputCommand : public Command {
public:
    RemoveOutputCommand(std::string ownerId, document::RouteTarget target)
        : ownerId_(std::move(ownerId)), target_(std::move(target)) {}
    void perform(document::Edit& edit) override {
        applied_ = edit.removeOutput(ownerId_, target_);
    }
    void undo(document::Edit& edit) override {
        if (applied_) edit.addOutput(ownerId_, target_);
    }
    std::string name() const override { return "Remove Output"; }
private:
    std::string ownerId_;
    document::RouteTarget target_;
    bool applied_ = false;
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
        return nullptr;
    }
    std::string ownerId_;
    document::AuxSend send_;
    size_t index_ = 0;
};

// Bypass or enable a plugin. Undoable like every other chain edit — bypass is
// a mix decision, and one you make while comparing, which is exactly when you
// want to take it back.
class SetPluginBypassCommand : public Command {
public:
    SetPluginBypassCommand(std::string slotId, bool bypass)
        : slotId_(std::move(slotId)), bypass_(bypass) {}

    void perform(document::Edit& e) override {
        if (const auto* slot = e.pluginSlot(slotId_)) previous_ = slot->bypass;
        applied_ = e.setPluginBypass(slotId_, bypass_);
    }

    void undo(document::Edit& e) override {
        if (applied_) e.setPluginBypass(slotId_, previous_);
    }

    std::string name() const override {
        return bypass_ ? "Bypass Plugin" : "Enable Plugin";
    }

private:
    std::string slotId_;
    bool bypass_ = false;
    bool previous_ = false;
    bool applied_ = false;
};

// Hide or show a track. Undoable because it is easy to hit by accident and
// the track is then not on screen to put back.
class SetTrackHiddenCommand : public Command {
public:
    SetTrackHiddenCommand(std::string trackId, bool hidden)
        : trackId_(std::move(trackId)), hidden_(hidden) {}

    void perform(document::Edit& e) override {
        if (const auto* t = e.track(trackId_)) previous_ = t->hidden;
        applied_ = e.setTrackHidden(trackId_, hidden_);
    }
    void undo(document::Edit& e) override {
        if (applied_) e.setTrackHidden(trackId_, previous_);
    }
    std::string name() const override {
        return hidden_ ? "Hide Track" : "Show Track";
    }

private:
    std::string trackId_;
    bool hidden_ = false;
    bool previous_ = false;
    bool applied_ = false;
};

// Exempt a track from other tracks' solos. Undoable like the other per-track
// switches — it changes what you hear, and Cmd-clicking a button you meant to
// click plainly is easy to do.
class SetTrackSoloSafeCommand : public Command {
public:
    SetTrackSoloSafeCommand(std::string trackId, bool safe)
        : trackId_(std::move(trackId)), safe_(safe) {}

    void perform(document::Edit& e) override {
        if (const auto* t = e.track(trackId_)) previous_ = t->soloSafe;
        applied_ = e.setTrackSoloSafe(trackId_, safe_);
    }
    void undo(document::Edit& e) override {
        if (applied_) e.setTrackSoloSafe(trackId_, previous_);
    }
    std::string name() const override { return "Solo Safe"; }

private:
    std::string trackId_;
    bool safe_ = false;
    bool previous_ = false;
    bool applied_ = false;
};

// Put every hidden track back. One command, so an accidental hide-several is
// one undo rather than several.
class ShowAllTracksCommand : public Command {
public:
    void perform(document::Edit& e) override {
        hiddenIds_.clear();
        for (const auto& track : e.tracks()) {
            if (track.hidden) hiddenIds_.push_back(track.id);
        }
        for (const auto& id : hiddenIds_) e.setTrackHidden(id, false);
    }
    void undo(document::Edit& e) override {
        for (const auto& id : hiddenIds_) e.setTrackHidden(id, true);
    }
    std::string name() const override { return "Show All Tracks"; }

private:
    std::vector<std::string> hiddenIds_;
};

// ─── Clip groups ────────────────────────────────────────────────────────────

// Group the given clips. The clips themselves are untouched — a group is a
// record held beside them — so undo is the removal of that record and nothing
// has to be reconstructed.
class GroupClipsCommand : public Command {
public:
    GroupClipsCommand(std::vector<document::ClipGroup::Member> members,
                      int64_t start, int64_t length,
                      std::vector<std::string> trackIds = {},
                      std::vector<std::string> childGroupIds = {},
                      std::string name = {})
        : members_(std::move(members)), start_(start), length_(length),
          trackIds_(std::move(trackIds)),
          childGroupIds_(std::move(childGroupIds)), name_(std::move(name)) {}

    void perform(document::Edit& e) override {
        if (groupId_.empty()) {
            groupId_ = e.addClipGroup(members_, start_, length_, trackIds_,
                                      childGroupIds_, name_);
            if (const auto* group = e.clipGroup(groupId_)) snapshot_ = *group;
        } else {
            // Redo reuses the first id, so anything holding it still resolves.
            e.restoreClipGroup_(snapshot_, index_);
        }
    }

    void undo(document::Edit& e) override {
        if (groupId_.empty()) return;
        const auto& groups = e.clipGroups();
        for (size_t i = 0; i < groups.size(); ++i) {
            if (groups[i].id == groupId_) { index_ = i; break; }
        }
        e.removeClipGroup(groupId_);
    }

    std::string name() const override { return "Group Clips"; }
    const std::string& groupId() const { return groupId_; }

private:
    std::vector<document::ClipGroup::Member> members_;
    int64_t start_ = 0;
    int64_t length_ = 0;
    std::vector<std::string> trackIds_;
    std::vector<std::string> childGroupIds_;
    std::string name_;
    std::string groupId_;
    document::ClipGroup snapshot_;
    size_t index_ = 0;
};

// Ungroup. Undo puts the group back exactly, including its position in the
// list, because the clips never moved.
class UngroupClipsCommand : public Command {
public:
    explicit UngroupClipsCommand(std::string groupId)
        : groupId_(std::move(groupId)) {}

    void perform(document::Edit& e) override {
        const auto& groups = e.clipGroups();
        for (size_t i = 0; i < groups.size(); ++i) {
            if (groups[i].id != groupId_) continue;
            snapshot_ = groups[i];
            index_ = i;
            break;
        }
        removed_ = e.removeClipGroup(groupId_);
    }

    void undo(document::Edit& e) override {
        if (removed_) e.restoreClipGroup_(snapshot_, index_);
    }

    std::string name() const override { return "Ungroup Clips"; }

private:
    std::string groupId_;
    document::ClipGroup snapshot_;
    size_t index_ = 0;
    bool removed_ = false;
};

// Move a group: its range and every clip in it, together. One command, so it
// moves and un-moves as one thing — a command per member would let an undo
// stop halfway and tear the group apart on the timeline.
//
// Members keep their tracks. A group can span tracks, and dragging one across
// lanes would have to decide what happens to a member with no lane to land in;
// sliding in time has no such question.
class MoveClipGroupCommand : public Command {
public:
    // rowDelta moves the group across tracks (positive = down) as well as
    // along the timeline; a row move the document refuses is dropped and the
    // time move still happens.
    MoveClipGroupCommand(std::string groupId, int64_t deltaSamples,
                         int rowDelta = 0)
        : groupId_(std::move(groupId)), delta_(deltaSamples),
          rowDelta_(rowDelta) {}

    void perform(document::Edit& e) override {
        const auto* before = e.clipGroup(groupId_);
        const int64_t start = before != nullptr ? before->timelineStart : 0;
        applied_ = e.moveClipGroup(groupId_, delta_);
        if (applied_) {
            // The clamp may have shortened the move; undo has to reverse what
            // actually happened, not what was asked for.
            const auto* after = e.clipGroup(groupId_);
            if (after != nullptr) delta_ = after->timelineStart - start;
        }
        rowsApplied_ = rowDelta_ != 0 &&
                       e.moveClipGroupTracks(groupId_, rowDelta_);
    }

    void undo(document::Edit& e) override {
        if (rowsApplied_) e.moveClipGroupTracks(groupId_, -rowDelta_);
        if (applied_) e.moveClipGroup(groupId_, -delta_);
    }

    std::string name() const override { return "Move Clip Group"; }

private:
    std::string groupId_;
    int64_t delta_ = 0;
    int rowDelta_ = 0;
    bool applied_ = false;
    bool rowsApplied_ = false;
};

// Set or replace the time signature at a bar. Undoable because it moves every
// bar line after it — an accidental change to bar 1 renumbers the whole
// session, and the only way back is the exact map that was there before.
class SetTimeSignatureCommand : public Command {
public:
    SetTimeSignatureCommand(int bar, int numerator, int denominator)
        : bar_(bar), numerator_(numerator), denominator_(denominator) {}

    void perform(document::Edit& e) override {
        before_ = e.meterMap();
        applied_ = e.setTimeSignature(bar_, numerator_, denominator_);
    }
    void undo(document::Edit& e) override {
        if (applied_) e.setMeterMap(before_);
    }
    std::string name() const override { return "Set Time Signature"; }

private:
    int bar_ = 1;
    int numerator_ = 4;
    int denominator_ = 4;
    std::vector<document::TimeSignature> before_;
    bool applied_ = false;
};

class RemoveTimeSignatureCommand : public Command {
public:
    explicit RemoveTimeSignatureCommand(int bar) : bar_(bar) {}
    void perform(document::Edit& e) override {
        before_ = e.meterMap();
        applied_ = e.removeTimeSignature(bar_);
    }
    void undo(document::Edit& e) override {
        if (applied_) e.setMeterMap(before_);
    }
    std::string name() const override { return "Remove Time Signature"; }

private:
    int bar_ = 1;
    std::vector<document::TimeSignature> before_;
    bool applied_ = false;
};

// Tempo moves every bar line after it, so it gets the same treatment as a
// meter change: undo restores the whole map, because a change at bar 1
// re-times the entire session.
class SetTempoCommand : public Command {
public:
    explicit SetTempoCommand(double bpm) : bar_(1), beat_(1), bpm_(bpm) {}
    SetTempoCommand(int bar, int beat, double bpm)
        : bar_(bar), beat_(beat), bpm_(bpm) {}

    void perform(document::Edit& e) override {
        before_ = e.tempoMap();
        applied_ = e.setTempoChange(bar_, beat_, bpm_);
    }
    void undo(document::Edit& e) override {
        if (applied_) e.setTempoMap(before_);
    }
    std::string name() const override { return "Set Tempo"; }

private:
    int bar_ = 1;
    int beat_ = 1;
    double bpm_ = 120.0;
    std::vector<document::TempoChange> before_;
    bool applied_ = false;
};

class RemoveTempoCommand : public Command {
public:
    RemoveTempoCommand(int bar, int beat) : bar_(bar), beat_(beat) {}
    void perform(document::Edit& e) override {
        before_ = e.tempoMap();
        applied_ = e.removeTempoChange(bar_, beat_);
    }
    void undo(document::Edit& e) override {
        if (applied_) e.setTempoMap(before_);
    }
    std::string name() const override { return "Remove Tempo Change"; }

private:
    int bar_ = 1;
    int beat_ = 1;
    std::vector<document::TempoChange> before_;
    bool applied_ = false;
};

// Move one entry of a channel's chain — an insert, a send, the meter or the
// fader. One command because there is one list: the two it replaced
// (SetMeterTapCommand, ReorderSendCommand) were the same move expressed twice
// because the meter and the sends used to live in separate orderings.
class MoveChainSlotCommand : public Command {
public:
    MoveChainSlotCommand(std::string trackId, size_t from, size_t to)
        : trackId_(std::move(trackId)), from_(from), to_(to) {}

    void perform(document::Edit& e) override {
        moved_ = e.moveChainSlot(trackId_, from_, to_);
    }

    void undo(document::Edit& e) override {
        // Back to where it came from, which after the shift is `from_` — the
        // entries it passed have closed up behind it.
        if (moved_) e.moveChainSlot(trackId_, to_, from_);
    }

    std::string name() const override { return "Reorder Chain"; }

private:
    std::string trackId_;
    size_t from_ = 0;
    size_t to_ = 0;
    bool moved_ = false;
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
        return nullptr;
    }
    std::string ownerId_;
    std::string sendId_;
    document::AuxSend snapshot_;
    size_t index_ = 0;
};

} // namespace dave::editing
