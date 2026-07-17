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

// MoveClip: changes a clip's timelineStart. Undo restores the old position.
// Used by drag; each drag can emit one MoveClip on mouse-up (transactional).
class MoveClipCommand : public Command {
public:
    MoveClipCommand(std::string trackId, std::string clipId, int64_t newStart)
        : trackId_(std::move(trackId)), clipId_(std::move(clipId)), newStart_(newStart) {}
    void perform(document::Edit& e) override {
        auto* c = e.clip(trackId_, clipId_);
        if (c) { oldStart_ = c->timelineStart; c->timelineStart = newStart_; e.notifyChanged(); }
    }
    void undo(document::Edit& e) override {
        auto* c = e.clip(trackId_, clipId_);
        if (c) { c->timelineStart = oldStart_; e.notifyChanged(); }
    }
    std::string name() const override { return "Move Clip"; }
private:
    std::string trackId_;
    std::string clipId_;
    int64_t newStart_;
    int64_t oldStart_ = 0;
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
