#pragma once

#include "editing/Command.h"

#include <string>

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

} // namespace dave::editing
