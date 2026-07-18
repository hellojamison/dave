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

} // namespace dave::editing
