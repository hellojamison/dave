// SPDX-License-Identifier: GPL-3.0-or-later
//
// Deleting a time selection over clips trims them: a partial selection
// shortens the clip, a whole-clip selection removes it, an interior selection
// splits it in two. This is gui::trimClipsInSelection, tested at the document
// level — no ImGui context, because the decision that matters is which of the
// four cases a given (clip, range) is and what samples come out.
#include "document/Edit.h"
#include "editing/Command.h"
#include "editing/Commands.h"
#include "gui/Timeline.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace dave;

namespace {

int rowOf(const document::Edit& edit, const std::string& id) {
    const auto& tracks = edit.tracks();
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

std::string addAudioClip(document::Edit& edit, const std::string& trackId,
                         int64_t start, int64_t length, int64_t sourceOffset = 0) {
    document::AudioClip c;
    c.timelineStart = start;
    c.length = length;
    c.sourceOffset = sourceOffset;
    return edit.addClip(trackId, c);
}

// Set up a range selection on a track's row.
void select(gui::TimelineViewState& view, int row, int64_t start, int64_t end) {
    view.hasSelection = true;
    view.selectionRow = row;
    view.selectionStart = start;
    view.selectionEnd = end;
}

const document::AudioClip* clip(const document::Edit& edit,
                                const std::string& trackId,
                                const std::string& clipId) {
    const auto* t = edit.track(trackId);
    if (t == nullptr) return nullptr;
    for (const auto& c : t->clips) {
        if (c.id == clipId) return &c;
    }
    return nullptr;
}

struct Fixture {
    document::Edit edit;
    editing::UndoStack undo{edit};
    gui::TimelineViewState view;
    std::string track;
    Fixture() { track = edit.addTrack("Dialog"); }
    int row() const { return rowOf(edit, track); }
};

} // namespace

TEST_CASE("a selection over a clip's tail shortens it", "[trimrange]") {
    Fixture f;
    const std::string id = addAudioClip(f.edit, f.track, 0, 48000);
    select(f.view, f.row(), 24000, 48000);

    REQUIRE(gui::trimClipsInSelection(f.edit, f.undo, f.view));
    const auto* c = clip(f.edit, f.track, id);
    REQUIRE(c != nullptr);
    CHECK(c->timelineStart == 0);
    CHECK(c->sourceOffset == 0);
    CHECK(c->length == 24000);
    CHECK(f.undo.undoDepth() == 1);
}

TEST_CASE("a selection over a clip's head shortens it and moves the source",
          "[trimrange]") {
    Fixture f;
    // sourceOffset non-zero so a head trim has to advance it, not just move the
    // start — the bug a "shorten from the left" that forgets the source makes.
    const std::string id = addAudioClip(f.edit, f.track, 0, 48000, /*offset*/ 1000);
    select(f.view, f.row(), 0, 12000);

    REQUIRE(gui::trimClipsInSelection(f.edit, f.undo, f.view));
    const auto* c = clip(f.edit, f.track, id);
    REQUIRE(c != nullptr);
    CHECK(c->timelineStart == 12000);
    CHECK(c->sourceOffset == 13000);   // 1000 + 12000
    CHECK(c->length == 36000);
}

TEST_CASE("a selection covering the whole clip removes it", "[trimrange]") {
    Fixture f;
    const std::string id = addAudioClip(f.edit, f.track, 12000, 24000);
    select(f.view, f.row(), 0, 48000);

    REQUIRE(gui::trimClipsInSelection(f.edit, f.undo, f.view));
    CHECK(clip(f.edit, f.track, id) == nullptr);
    CHECK(f.edit.track(f.track)->clips.empty());
}

TEST_CASE("a selection inside a clip splits it in two", "[trimrange]") {
    Fixture f;
    const std::string id = addAudioClip(f.edit, f.track, 0, 48000);
    select(f.view, f.row(), 16000, 32000);

    REQUIRE(gui::trimClipsInSelection(f.edit, f.undo, f.view));
    const auto& clips = f.edit.track(f.track)->clips;
    REQUIRE(clips.size() == 2);
    // Left keeps the original id and covers [0, 16000).
    const auto* left = clip(f.edit, f.track, id);
    REQUIRE(left != nullptr);
    CHECK(left->timelineStart == 0);
    CHECK(left->length == 16000);
    // Right is a new clip covering [32000, 48000) with the source advanced.
    const document::AudioClip* right = nullptr;
    for (const auto& c : clips) if (c.id != id) right = &c;
    REQUIRE(right != nullptr);
    CHECK(right->timelineStart == 32000);
    CHECK(right->sourceOffset == 32000);
    CHECK(right->length == 16000);
    // The gap between them is exactly the deleted range.
    CHECK(right->timelineStart - (left->timelineStart + left->length) == 16000);
    // One user action, one undo step, even though it took a trim and an add.
    CHECK(f.undo.undoDepth() == 1);
}

TEST_CASE("undoing a split restores the single clip", "[trimrange]") {
    Fixture f;
    const std::string id = addAudioClip(f.edit, f.track, 0, 48000);
    select(f.view, f.row(), 16000, 32000);
    REQUIRE(gui::trimClipsInSelection(f.edit, f.undo, f.view));
    REQUIRE(f.edit.track(f.track)->clips.size() == 2);

    f.undo.undo();
    REQUIRE(f.edit.track(f.track)->clips.size() == 1);
    const auto* c = clip(f.edit, f.track, id);
    REQUIRE(c != nullptr);
    CHECK(c->timelineStart == 0);
    CHECK(c->length == 48000);

    f.undo.redo();
    CHECK(f.edit.track(f.track)->clips.size() == 2);
}

TEST_CASE("a selection touching no clip does nothing", "[trimrange]") {
    Fixture f;
    addAudioClip(f.edit, f.track, 0, 48000);
    select(f.view, f.row(), 60000, 80000);   // entirely past the clip
    CHECK_FALSE(gui::trimClipsInSelection(f.edit, f.undo, f.view));
    CHECK(f.edit.track(f.track)->clips.size() == 1);
    CHECK(f.undo.undoDepth() == 0);
}

TEST_CASE("with no selection there is nothing to trim", "[trimrange]") {
    Fixture f;
    addAudioClip(f.edit, f.track, 0, 48000);
    f.view.hasSelection = false;
    CHECK_FALSE(gui::trimClipsInSelection(f.edit, f.undo, f.view));
}

TEST_CASE("an all-tracks selection trims every track as one undo step",
          "[trimrange]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    gui::TimelineViewState view;
    const std::string a = edit.addTrack("A");
    const std::string b = edit.addTrack("B");
    const std::string ca = addAudioClip(edit, a, 0, 48000);
    const std::string cb = addAudioClip(edit, b, 0, 48000);

    // selectionRow = -1 means the range spans every track.
    view.hasSelection = true;
    view.selectionRow = -1;
    view.selectionStart = 24000;
    view.selectionEnd = 48000;

    REQUIRE(gui::trimClipsInSelection(edit, undo, view));
    CHECK(clip(edit, a, ca)->length == 24000);
    CHECK(clip(edit, b, cb)->length == 24000);
    // Two tracks trimmed, still one Ctrl+Z.
    CHECK(undo.undoDepth() == 1);

    undo.undo();
    CHECK(clip(edit, a, ca)->length == 48000);
    CHECK(clip(edit, b, cb)->length == 48000);
}

TEST_CASE("a MIDI clip trims the same way", "[trimrange][midi]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    gui::TimelineViewState view;
    const std::string t = edit.addMidiTrack("Keys");
    document::MidiClip mc;
    mc.timelineStart = 0;
    mc.length = 48000;
    document::MidiNote n; n.startSample = 0; n.lengthSamples = 1000; n.pitch = 60;
    mc.notes.push_back(n);
    const std::string id = edit.addMidiClip(t, mc);
    select(view, rowOf(edit, t), 24000, 48000);

    REQUIRE(gui::trimClipsInSelection(edit, undo, view));
    const auto* mt = edit.track(t);
    REQUIRE(mt != nullptr);
    REQUIRE(mt->midiClips.size() == 1);
    CHECK(mt->midiClips[0].id == id);
    CHECK(mt->midiClips[0].length == 24000);
}
